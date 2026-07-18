#include "Engine/Renderer/Renderer.h"

#include <cmath>
#include <set>

namespace Engine
{
    bool BuildRendererGpuTimingPublication(
        const std::vector<RenderGraph::RawTimestampScope>& scopes,
        RendererGpuTimingPublication& publication, std::string* error)
    {
        const auto fail = [&](std::string message)
        {
            publication = {};
            if (error) *error = std::move(message);
            return false;
        };
        publication = {};
        if (scopes.empty()) return fail("GPU timing publication requires at least one scope.");

        publication.FrameIndex = scopes.front().FrameIndex;
        publication.Status = RendererTimingStatus::Ready;
        publication.Passes.reserve(scopes.size());
        const double period = scopes.front().PeriodNanoseconds;
        if (!std::isfinite(period) || period <= 0.0)
            return fail("GPU timing publication requires a positive finite timestamp period.");

        std::set<std::string> labels;
        for (const RenderGraph::RawTimestampScope& scope : scopes)
        {
            if (scope.FrameIndex != publication.FrameIndex || scope.PassLabel.empty()
                || !labels.insert(scope.PassLabel).second || !scope.Token.IsValid()
                || scope.EffectiveQueue != RHI::QueueType::Graphics
                || scope.Start.Generation == 0 || scope.Start.Generation != scope.End.Generation
                || !std::isfinite(scope.PeriodNanoseconds) || scope.PeriodNanoseconds != period)
                return fail("GPU timing scopes do not share one exact frame, label set, generation, token, and Graphics clock.");

            RendererPassTiming pass;
            pass.Name = scope.PassLabel;
            if (scope.Start.Status == RHI::QueryResultStatus::Ready
                && scope.End.Status == RHI::QueryResultStatus::Ready)
            {
                if (scope.End.Value < scope.Start.Value)
                    pass.GpuStatus = RendererTimingStatus::Disjoint;
                else
                {
                    pass.GpuStatus = RendererTimingStatus::Ready;
                    pass.GpuMilliseconds = static_cast<double>(scope.End.Value - scope.Start.Value)
                        * period / 1'000'000.0;
                }
            }
            else if (scope.Start.Status == RHI::QueryResultStatus::Disjoint
                || scope.End.Status == RHI::QueryResultStatus::Disjoint)
                pass.GpuStatus = RendererTimingStatus::Disjoint;
            else if (scope.Start.Status == RHI::QueryResultStatus::Pending
                || scope.End.Status == RHI::QueryResultStatus::Pending)
                pass.GpuStatus = RendererTimingStatus::Pending;
            else
                pass.GpuStatus = RendererTimingStatus::Unavailable;

            if (pass.GpuStatus == RendererTimingStatus::Disjoint)
                publication.Status = RendererTimingStatus::Disjoint;
            else if (publication.Status != RendererTimingStatus::Disjoint
                && pass.GpuStatus == RendererTimingStatus::Pending)
                publication.Status = RendererTimingStatus::Pending;
            else if (publication.Status == RendererTimingStatus::Ready
                && pass.GpuStatus == RendererTimingStatus::Unavailable)
                publication.Status = RendererTimingStatus::Unavailable;
            publication.Passes.emplace_back(std::move(pass));
        }

        if (publication.Status == RendererTimingStatus::Ready)
        {
            const RenderGraph::RawTimestampScope& first = scopes.front();
            const RenderGraph::RawTimestampScope& last = scopes.back();
            if (last.End.Value < first.Start.Value)
                publication.Status = RendererTimingStatus::Disjoint;
            else
                publication.GpuMilliseconds = static_cast<double>(last.End.Value - first.Start.Value)
                    * period / 1'000'000.0;
        }
        return true;
    }

    bool ApplyRendererGpuTimingPublication(RendererFrameTiming& timing,
        const RendererGpuTimingPublication& publication, std::string* error)
    {
        if (timing.FrameIndex != publication.FrameIndex)
        {
            if (error) *error = "GPU timing publication frame identity does not match the retained renderer frame.";
            return false;
        }
        if (publication.Passes.empty())
        {
            if (error) *error = "GPU timing publication has no named passes.";
            return false;
        }
        timing.GpuStatus = publication.Status;
        timing.GpuMilliseconds = publication.Status == RendererTimingStatus::Ready
            ? publication.GpuMilliseconds : 0.0;
        timing.Passes.insert(timing.Passes.end(), publication.Passes.begin(), publication.Passes.end());
        return true;
    }

    bool ApplyRendererInputSample(RendererFrameTiming& timing,
        const RendererInputSample& sample, std::string* error)
    {
        const auto fail = [error](const char* message)
        {
            if (error)
                *error = message;
            return false;
        };

        if (sample.FrameIndex != timing.FrameIndex)
            return fail("input sample frame does not match timing frame");
        if (!std::isfinite(sample.MillisecondsFromFrameStart) || sample.MillisecondsFromFrameStart < 0.0)
            return fail("input sample offset is not finite and nonnegative");
        if (timing.InputSample)
            return fail("timing frame already has an input sample");

        bool hasFrameStart = false;
        for (const RendererFrameLifecycleEvent& event : timing.Lifecycle)
        {
            if (!std::isfinite(event.MillisecondsFromFrameStart) || event.MillisecondsFromFrameStart < 0.0)
                return fail("timing lifecycle contains an invalid offset");
            if (event.Phase == RendererFrameLifecyclePhase::FrameStart)
                hasFrameStart = true;
            if (event.Phase == RendererFrameLifecyclePhase::InputSample)
                return fail("timing lifecycle already contains an input sample");
            if (event.Phase == RendererFrameLifecyclePhase::InputSimulation)
                return fail("input simulation was recorded before the input sample");
            if (sample.MillisecondsFromFrameStart < event.MillisecondsFromFrameStart)
                return fail("input sample precedes an existing lifecycle event");
        }
        if (!hasFrameStart)
            return fail("input sample requires a frame-start lifecycle event");

        timing.Lifecycle.push_back({ RendererFrameLifecyclePhase::InputSample,
            sample.MillisecondsFromFrameStart, sample.QpcTick });
        timing.InputSample = sample;
        return true;
    }

    bool RefreshRendererInputLatencyIntervals(RendererFrameTiming& timing, std::string* error)
    {
        const auto fail = [error](const char* message)
        {
            if (error)
                *error = message;
            return false;
        };
        if (!timing.InputSample || timing.InputSample->FrameIndex != timing.FrameIndex
            || !std::isfinite(timing.InputSample->MillisecondsFromFrameStart)
            || timing.InputSample->MillisecondsFromFrameStart < 0.0)
            return fail("input latency intervals require one valid same-frame sample");

        const double sample = timing.InputSample->MillisecondsFromFrameStart;
        std::optional<double> simulation;
        std::optional<double> submission;
        std::optional<double> present;
        size_t sampleEvents = 0;
        for (const RendererFrameLifecycleEvent& event : timing.Lifecycle)
        {
            if (!std::isfinite(event.MillisecondsFromFrameStart) || event.MillisecondsFromFrameStart < 0.0)
                return fail("input latency lifecycle contains an invalid offset");
            if (event.Phase == RendererFrameLifecyclePhase::InputSample)
            {
                ++sampleEvents;
                if (event.MillisecondsFromFrameStart != sample || event.QpcTick != timing.InputSample->QpcTick)
                    return fail("input latency sample lifecycle does not match its source record");
            }
            const auto capture = [&](std::optional<double>& target) -> bool
            {
                if (target || event.MillisecondsFromFrameStart < sample)
                    return false;
                target = event.MillisecondsFromFrameStart - sample;
                return true;
            };
            if (event.Phase == RendererFrameLifecyclePhase::InputSimulation && !capture(simulation))
                return fail("input latency simulation endpoint is duplicate or precedes the sample");
            if (event.Phase == RendererFrameLifecyclePhase::RenderSubmission && !capture(submission))
                return fail("input latency submission endpoint is duplicate or precedes the sample");
            if (event.Phase == RendererFrameLifecyclePhase::PresentEnd && !capture(present))
                return fail("input latency present endpoint is duplicate or precedes the sample");
        }
        if (sampleEvents != 1)
            return fail("input latency intervals require one exact sample lifecycle event");
        if ((submission && !simulation) || (present && !submission)
            || (simulation && submission && *submission < *simulation)
            || (submission && present && *present < *submission))
            return fail("input latency endpoints are missing or out of order");

        timing.InputLatencySourceFrameIndex = timing.FrameIndex;
        timing.InputToSimulationMilliseconds = simulation;
        timing.InputToRenderSubmissionMilliseconds = submission;
        timing.InputToPresentMilliseconds = present;
        return true;
    }
}
