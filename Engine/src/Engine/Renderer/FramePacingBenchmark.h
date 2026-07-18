#pragma once

#include "Engine/Renderer/Renderer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace Engine
{
    // This artifact intentionally records engine observations only. Unknown
    // presentation/display/input fields stay unknown until an external runner
    // supplies measured evidence; they are never inferred from Present.
    struct FramePacingBenchmarkCondition
    {
        static constexpr u32 SchemaVersion = 5;
        std::string RunId = "unavailable";
        u32 ProcessId = 0;
        std::string ExecutablePath = "unavailable";
        u64 QpcFrequency = 0;
        std::string Backend;
        std::string Adapter;
        std::string AdapterStableId;
        double TargetFramesPerSecond = 0.0;
        u32 WarmupFrames = 0;
        ResolvedFramePacingPolicy Policy;
        std::string PresentationMode = "unknown";
        std::string SyncMode = "unknown";
        std::string VrrMode = "unknown";
        std::string TearingMode = "unknown";
    };

    struct FramePacingBenchmarkAttachmentReadiness
    {
        static constexpr u32 SchemaVersion = 1;
        std::string RunId;
        u32 ProcessId = 0;
        std::string ExecutablePath;
        u64 QpcFrequency = 0;
        u64 QpcTick = 0;
        std::string BenchmarkArtifactPath;
        FramePacingBenchmarkCondition Condition;
    };

    struct FramePacingBenchmarkSummary
    {
        size_t SampleCount = 0;
        double StartToStartP50Milliseconds = 0.0;
        double StartToStartP95Milliseconds = 0.0;
        double StartToStartP99Milliseconds = 0.0;
        double CpuActiveP50Milliseconds = 0.0;
        double CpuActiveP95Milliseconds = 0.0;
        double CpuActiveP99Milliseconds = 0.0;
        double IntentionalWaitP50Milliseconds = 0.0;
        double IntentionalWaitP95Milliseconds = 0.0;
        double IntentionalWaitP99Milliseconds = 0.0;
        // A low is 1000 / worst-tail frametime: 1% uses p99, 0.1% uses p99.9.
        double OnePercentLowFramesPerSecond = 0.0;
        double PointOnePercentLowFramesPerSecond = 0.0;
        size_t DeadlineMissCount = 0;
        double DeadlineOvershootP99Milliseconds = 0.0;
    };

    struct FramePacingBenchmarkSnapshot
    {
        FramePacingBenchmarkCondition Condition;
        std::vector<RendererFrameTiming> Frames;
        FramePacingBenchmarkSummary Summary;
    };

    class FramePacingBenchmarkCapture
    {
    public:
        explicit FramePacingBenchmarkCapture(size_t capacity = 4096) : m_Capacity(std::max<size_t>(1, capacity)) {}

        void Begin(FramePacingBenchmarkCondition condition)
        {
            m_Condition = std::move(condition);
            m_Frames.clear();
            Publish();
        }

        void Record(const RendererFrameTiming& timing)
        {
            if (m_Frames.size() == m_Capacity)
                m_Frames.erase(m_Frames.begin());
            m_Frames.push_back(timing);
            UpdateGpuHeadroom(m_Frames.back());
            Publish();
        }

        bool AmendGpuTiming(const RendererGpuTimingPublication& publication)
        {
            const auto found = std::find_if(m_Frames.begin(), m_Frames.end(), [&](const RendererFrameTiming& timing)
                { return timing.FrameIndex == publication.FrameIndex; });
            if (found == m_Frames.end() || !ApplyRendererGpuTimingPublication(*found, publication))
                return false;
            UpdateGpuHeadroom(*found);
            Publish();
            return true;
        }

        bool AmendEffectiveLimitingSource(u64 cadenceFrameIndex, RendererEffectiveLimitingSource source,
            const std::optional<u64>& sourceFrameIndex)
        {
            const auto found = std::find_if(m_Frames.begin(), m_Frames.end(), [&](const RendererFrameTiming& timing)
                { return timing.FrameIndex == cadenceFrameIndex; });
            if (found == m_Frames.end())
                return false;
            found->EffectiveLimitingSource = source;
            found->EffectiveLimitingSourceFrameIndex = sourceFrameIndex;
            Publish();
            return true;
        }

        std::shared_ptr<const FramePacingBenchmarkSnapshot> GetSnapshot() const { return m_Snapshot; }

        static double Percentile(const std::vector<double>& values, double percentile)
        {
            if (values.empty()) return 0.0;
            std::vector<double> sorted = values;
            std::sort(sorted.begin(), sorted.end());
            const size_t index = static_cast<size_t>(std::ceil(percentile * static_cast<double>(sorted.size()))) - 1;
            return sorted[std::min(index, sorted.size() - 1)];
        }

        static bool WriteArtifacts(const FramePacingBenchmarkSnapshot& snapshot, const std::filesystem::path& directory, std::string& error)
        {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec) { error = "could not create benchmark directory"; return false; }
            const std::string csv = Csv(snapshot);
            const std::string json = Json(snapshot);
            return WriteAtomically(directory / "frame-pacing-benchmark.csv", csv, error)
                && WriteAtomically(directory / "frame-pacing-benchmark.json", json, error);
        }

        static bool WriteAttachmentReadiness(const FramePacingBenchmarkAttachmentReadiness& readiness,
            const std::filesystem::path& path, std::string& error)
        {
            if (readiness.RunId.empty() || readiness.ProcessId == 0 || readiness.ExecutablePath.empty()
                || readiness.QpcFrequency == 0 || readiness.QpcTick == 0 || readiness.BenchmarkArtifactPath.empty())
            {
                error = "attachment readiness identity is incomplete";
                return false;
            }
            std::error_code directoryError;
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path(), directoryError);
                if (directoryError)
                {
                    error = "could not create attachment readiness directory";
                    return false;
                }
            }
            std::ostringstream out;
            out << "{\n  \"schema\":" << FramePacingBenchmarkAttachmentReadiness::SchemaVersion
                << ",\n  \"runId\":\"" << Escape(readiness.RunId)
                << "\",\n  \"processId\":" << readiness.ProcessId
                << ",\n  \"executablePath\":\"" << Escape(readiness.ExecutablePath)
                << "\",\n  \"qpcFrequency\":" << readiness.QpcFrequency
                << ",\n  \"qpcTick\":" << readiness.QpcTick
                << ",\n  \"benchmarkArtifactPath\":\"" << Escape(readiness.BenchmarkArtifactPath)
                << "\",\n  \"condition\":{\"backend\":\"" << Escape(readiness.Condition.Backend)
                << "\",\"targetFps\":" << readiness.Condition.TargetFramesPerSecond
                << ",\"candidate\":\"" << ToString(readiness.Condition.Policy.Candidate)
                << "\",\"presentationMode\":\"" << Escape(readiness.Condition.PresentationMode)
                << "\",\"sync\":\"" << Escape(readiness.Condition.SyncMode)
                << "\",\"vrr\":\"" << Escape(readiness.Condition.VrrMode)
                << "\",\"tearing\":\"" << Escape(readiness.Condition.TearingMode) << "\"}\n}\n";
            return WriteAtomically(path, out.str(), error);
        }

        static bool IsValidAttachmentRelease(const FramePacingBenchmarkAttachmentReadiness& readiness,
            std::string_view release, std::string& error)
        {
            const std::string expected = "schema=1\nrunId=" + readiness.RunId + "\npid=" + std::to_string(readiness.ProcessId) + "\n";
            if (release != expected)
            {
                error = "attachment release does not match the published schema/run/process identity";
                return false;
            }
            return true;
        }

    private:
        static std::string Escape(std::string_view value)
        {
            std::string result;
            for (const unsigned char c : value)
            {
                if (c == '"') result += "\\\"";
                else if (c == '\\') result += "\\\\";
                else if (c == '\b') result += "\\b";
                else if (c == '\f') result += "\\f";
                else if (c == '\n') result += "\\n";
                else if (c == '\r') result += "\\r";
                else if (c == '\t') result += "\\t";
                else if (c < 0x20)
                {
                    constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result += hex[(c >> 4) & 0xf];
                    result += hex[c & 0xf];
                }
                else result += static_cast<char>(c);
            }
            return result;
        }

        static std::string CsvEscape(std::string_view value)
        {
            std::string result = "\"";
            for (const char c : value) { if (c == '"') result += "\"\""; else result += c; }
            return result + '"';
        }

        static const char* PhaseName(RendererFrameLifecyclePhase phase)
        {
            switch (phase) { case RendererFrameLifecyclePhase::FrameStart: return "FrameStart"; case RendererFrameLifecyclePhase::InputSample: return "InputSample"; case RendererFrameLifecyclePhase::InputSimulation: return "InputSimulation"; case RendererFrameLifecyclePhase::RenderSubmission: return "RenderSubmission"; case RendererFrameLifecyclePhase::PresentBegin: return "PresentBegin"; case RendererFrameLifecyclePhase::PresentEnd: return "PresentEnd"; case RendererFrameLifecyclePhase::IntentionalPacingWait: return "IntentionalPacingWait"; case RendererFrameLifecyclePhase::GpuCompletionObservation: return "GpuCompletionObservation"; } return "Unknown";
        }
        static const char* WaitName(RendererFrameWaitKind kind)
        {
            switch (kind) { case RendererFrameWaitKind::IntentionalPacing: return "IntentionalPacing"; case RendererFrameWaitKind::MandatoryDxgiFrameLatency: return "MandatoryDxgiFrameLatency"; case RendererFrameWaitKind::MandatoryVulkanAcquire: return "MandatoryVulkanAcquire"; case RendererFrameWaitKind::MandatoryVulkanFence: return "MandatoryVulkanFence"; } return "Unknown";
        }

        static double Overshoot(const RendererFrameWaitTiming& wait)
        {
            return wait.DeadlineMissed ? std::max(0.0, wait.ActualReleaseMilliseconds - wait.RequestedDeadlineMilliseconds) : 0.0;
        }

        static void UpdateGpuHeadroom(RendererFrameTiming& timing)
        {
            timing.GpuHeadroomMilliseconds.reset();
            if (timing.GpuStatus != RendererTimingStatus::Ready || !std::isfinite(timing.GpuMilliseconds)
                || timing.GpuMilliseconds < 0.0 || !timing.FramePacingPolicy.SmoothTargetFramesPerSecond)
                return;
            const double target = *timing.FramePacingPolicy.SmoothTargetFramesPerSecond;
            if (!std::isfinite(target) || target <= 0.0)
                return;
            timing.GpuHeadroomMilliseconds = 1000.0 / target - timing.GpuMilliseconds;
        }

        static std::string GpuDurationValue(const RendererFrameTiming& timing)
        {
            return timing.GpuStatus == RendererTimingStatus::Ready && std::isfinite(timing.GpuMilliseconds)
                ? std::to_string(timing.GpuMilliseconds) : "unavailable";
        }

        static std::string GpuHeadroomValue(const RendererFrameTiming& timing)
        {
            return timing.GpuHeadroomMilliseconds && std::isfinite(*timing.GpuHeadroomMilliseconds)
                ? std::to_string(*timing.GpuHeadroomMilliseconds) : "unavailable";
        }

        static std::string OptionalValue(const std::optional<double>& value)
        {
            return value && std::isfinite(*value) ? std::to_string(*value) : "unavailable";
        }

        static FramePacingBenchmarkSummary Summarize(const std::vector<RendererFrameTiming>& frames)
        {
            std::vector<double> cadence, active, intentional, overshoots;
            size_t misses = 0;
            for (const RendererFrameTiming& frame : frames)
            {
                if (frame.StartToStartMilliseconds > 0.0) cadence.push_back(frame.StartToStartMilliseconds);
                active.push_back(frame.CpuActiveMilliseconds);
                intentional.push_back(frame.IntentionalPacingMilliseconds);
                for (const RendererFrameWaitTiming& wait : frame.Waits)
                    if (wait.Kind == RendererFrameWaitKind::IntentionalPacing && wait.DeadlineMissed)
                    { ++misses; overshoots.push_back(Overshoot(wait)); }
            }
            FramePacingBenchmarkSummary summary;
            summary.SampleCount = cadence.size();
            summary.StartToStartP50Milliseconds = Percentile(cadence, .50);
            summary.StartToStartP95Milliseconds = Percentile(cadence, .95);
            summary.StartToStartP99Milliseconds = Percentile(cadence, .99);
            summary.CpuActiveP50Milliseconds = Percentile(active, .50); summary.CpuActiveP95Milliseconds = Percentile(active, .95); summary.CpuActiveP99Milliseconds = Percentile(active, .99);
            summary.IntentionalWaitP50Milliseconds = Percentile(intentional, .50); summary.IntentionalWaitP95Milliseconds = Percentile(intentional, .95); summary.IntentionalWaitP99Milliseconds = Percentile(intentional, .99);
            const double one = Percentile(cadence, .99), pointOne = Percentile(cadence, .999);
            summary.OnePercentLowFramesPerSecond = one > 0.0 ? 1000.0 / one : 0.0;
            summary.PointOnePercentLowFramesPerSecond = pointOne > 0.0 ? 1000.0 / pointOne : 0.0;
            summary.DeadlineMissCount = misses;
            summary.DeadlineOvershootP99Milliseconds = Percentile(overshoots, .99);
            return summary;
        }

        static std::string Csv(const FramePacingBenchmarkSnapshot& snapshot)
        {
            std::ostringstream out; out << std::setprecision(12);
            out << "schema,runId,processId,executablePath,qpcFrequency,backend,adapter,adapterStableId,targetFps,effectiveTargetFps,warmupFrames,projectMode,runtimeOverride,mode,candidate,behavior,presentationMode,sync,vrr,tearing,frame,startToStartMs,cpuTotalMs,cpuActiveMs,intentionalWaitMs,gpuCompleteFrame,cadencePreviousFrame,limitingSource,limitingSourceFrame,inputLatencySourceFrame,inputToSimulationMs,inputToSubmitMs,inputToPresentMs,lifecycleJson,waitsJson,display,replacementDrop,inputLatency,gpuTimingStatus,gpuDurationMs,gpuHeadroom\n";
            for (const RendererFrameTiming& f : snapshot.Frames)
            {
                std::ostringstream lifecycle, waits;
                lifecycle << std::setprecision(12) << '[';
                waits << std::setprecision(12) << '[';
                for (size_t i=0;i<f.Lifecycle.size();++i) { if(i) lifecycle << ','; lifecycle << "{\"phase\":\"" << PhaseName(f.Lifecycle[i].Phase) << "\",\"ms\":" << f.Lifecycle[i].MillisecondsFromFrameStart << ",\"qpc\":" << f.Lifecycle[i].QpcTick << '}'; }
                for (size_t i=0;i<f.Waits.size();++i) { if(i) waits << ','; const auto& w=f.Waits[i]; waits << "{\"kind\":\"" << WaitName(w.Kind) << "\",\"applied\":" << (w.Applied?"true":"false") << ",\"ms\":" << w.Milliseconds << ",\"candidate\":\"" << ToString(w.Candidate) << "\",\"deadlineMissed\":" << (w.DeadlineMissed?"true":"false") << ",\"requestedDeadlineMs\":" << w.RequestedDeadlineMilliseconds << ",\"actualReleaseMs\":" << w.ActualReleaseMilliseconds << '}'; }
                lifecycle << ']'; waits << ']';
                out << FramePacingBenchmarkCondition::SchemaVersion << ',' << CsvEscape(snapshot.Condition.RunId) << ',' << snapshot.Condition.ProcessId << ',' << CsvEscape(snapshot.Condition.ExecutablePath) << ',' << snapshot.Condition.QpcFrequency << ',' << CsvEscape(snapshot.Condition.Backend) << ',' << CsvEscape(snapshot.Condition.Adapter)
                    << ',' << CsvEscape(snapshot.Condition.AdapterStableId) << ',' << snapshot.Condition.TargetFramesPerSecond << ','
                    << (snapshot.Condition.Policy.SmoothTargetFramesPerSecond ? std::to_string(*snapshot.Condition.Policy.SmoothTargetFramesPerSecond) : "unavailable")
                    << ',' << snapshot.Condition.WarmupFrames << ',' << CsvEscape(ToString(snapshot.Condition.Policy.ProjectMode)) << ','
                    << CsvEscape(ToString(snapshot.Condition.Policy.RuntimeOverride)) << ',' << CsvEscape(ToString(f.FramePacingPolicy.EffectiveMode))
                    << ',' << CsvEscape(ToString(f.FramePacingPolicy.Candidate)) << ',' << CsvEscape(f.FramePacingPolicy.Behavior)
                    << ',' << CsvEscape(snapshot.Condition.PresentationMode) << ',' << CsvEscape(snapshot.Condition.SyncMode)
                    << ',' << CsvEscape(snapshot.Condition.VrrMode) << ',' << CsvEscape(snapshot.Condition.TearingMode) << ',' << f.FrameIndex << ',' << f.StartToStartMilliseconds << ','
                    << f.CpuMilliseconds << ',' << f.CpuActiveMilliseconds << ',' << f.IntentionalPacingMilliseconds << ',' << (f.HasGpuCompletionObservation ? std::to_string(f.LastGpuCompletionObservedFrameIndex) : "unavailable")
                    << ',' << (f.CadencePreviousFrameIndex ? std::to_string(*f.CadencePreviousFrameIndex) : "unavailable")
                    << ',' << CsvEscape(ToString(f.EffectiveLimitingSource))
                    << ',' << (f.EffectiveLimitingSourceFrameIndex ? std::to_string(*f.EffectiveLimitingSourceFrameIndex) : "unavailable")
                    << ',' << (f.InputLatencySourceFrameIndex ? std::to_string(*f.InputLatencySourceFrameIndex) : "unavailable")
                    << ',' << OptionalValue(f.InputToSimulationMilliseconds)
                    << ',' << OptionalValue(f.InputToRenderSubmissionMilliseconds)
                    << ',' << OptionalValue(f.InputToPresentMilliseconds)
                    << ',' << CsvEscape(lifecycle.str()) << ',' << CsvEscape(waits.str()) << ",unavailable,unavailable,unavailable,"
                    << ToString(f.GpuStatus) << ',' << GpuDurationValue(f) << ',' << GpuHeadroomValue(f) << '\n';
            }
            return out.str();
        }

        static std::string Json(const FramePacingBenchmarkSnapshot& s)
        {
            std::ostringstream out; out << std::setprecision(12);
            out << "{\n  \"schema\":" << FramePacingBenchmarkCondition::SchemaVersion << ",\n  \"condition\":{\"runId\":\"" << Escape(s.Condition.RunId) << "\",\"processId\":" << s.Condition.ProcessId << ",\"executablePath\":\"" << Escape(s.Condition.ExecutablePath) << "\",\"qpcFrequency\":" << s.Condition.QpcFrequency << ",\"backend\":\"" << Escape(s.Condition.Backend) << "\",\"adapter\":\"" << Escape(s.Condition.Adapter)
                << "\",\"adapterStableId\":\"" << Escape(s.Condition.AdapterStableId) << "\",\"targetFps\":" << s.Condition.TargetFramesPerSecond << ",\"warmupFrames\":" << s.Condition.WarmupFrames
                << ",\"projectMode\":\"" << ToString(s.Condition.Policy.ProjectMode) << "\",\"runtimeOverride\":\"" << ToString(s.Condition.Policy.RuntimeOverride)
                << "\",\"mode\":\"" << ToString(s.Condition.Policy.EffectiveMode) << "\",\"candidate\":\"" << ToString(s.Condition.Policy.Candidate)
                << "\",\"behavior\":\"" << Escape(s.Condition.Policy.Behavior) << "\",\"effectiveTargetFps\":"
                << (s.Condition.Policy.SmoothTargetFramesPerSecond ? std::to_string(*s.Condition.Policy.SmoothTargetFramesPerSecond) : "null")
                << ",\"presentationMode\":\"" << Escape(s.Condition.PresentationMode) << "\",\"sync\":\"" << Escape(s.Condition.SyncMode) << "\",\"vrr\":\"" << Escape(s.Condition.VrrMode) << "\",\"tearing\":\"" << Escape(s.Condition.TearingMode)
                << "\"},\n  \"summary\":{\"samples\":" << s.Summary.SampleCount << ",\"p50Ms\":" << s.Summary.StartToStartP50Milliseconds << ",\"p95Ms\":" << s.Summary.StartToStartP95Milliseconds << ",\"p99Ms\":" << s.Summary.StartToStartP99Milliseconds << ",\"cpuActiveP50Ms\":" << s.Summary.CpuActiveP50Milliseconds << ",\"cpuActiveP95Ms\":" << s.Summary.CpuActiveP95Milliseconds << ",\"cpuActiveP99Ms\":" << s.Summary.CpuActiveP99Milliseconds << ",\"intentionalWaitP50Ms\":" << s.Summary.IntentionalWaitP50Milliseconds << ",\"intentionalWaitP95Ms\":" << s.Summary.IntentionalWaitP95Milliseconds << ",\"intentionalWaitP99Ms\":" << s.Summary.IntentionalWaitP99Milliseconds << ",\"onePercentLowFps\":" << s.Summary.OnePercentLowFramesPerSecond << ",\"pointOnePercentLowFps\":" << s.Summary.PointOnePercentLowFramesPerSecond << ",\"deadlineMisses\":" << s.Summary.DeadlineMissCount << ",\"deadlineOvershootP99Ms\":" << s.Summary.DeadlineOvershootP99Milliseconds << "},\n  \"frames\":[\n";
            for (size_t i = 0; i < s.Frames.size(); ++i) { const auto& f = s.Frames[i]; const std::string gpuDuration = GpuDurationValue(f), gpuHeadroom = GpuHeadroomValue(f); out << "    {\"frame\":" << f.FrameIndex << ",\"startToStartMs\":" << f.StartToStartMilliseconds << ",\"cpuTotalMs\":" << f.CpuMilliseconds << ",\"cpuActiveMs\":" << f.CpuActiveMilliseconds << ",\"intentionalWaitMs\":" << f.IntentionalPacingMilliseconds << ",\"cadencePreviousFrame\":" << (f.CadencePreviousFrameIndex ? std::to_string(*f.CadencePreviousFrameIndex) : "null") << ",\"limitingSource\":\"" << ToString(f.EffectiveLimitingSource) << "\",\"limitingSourceFrame\":" << (f.EffectiveLimitingSourceFrameIndex ? std::to_string(*f.EffectiveLimitingSourceFrameIndex) : "null") << ",\"inputLatencySourceFrame\":" << (f.InputLatencySourceFrameIndex ? std::to_string(*f.InputLatencySourceFrameIndex) : "null") << ",\"inputToSimulationMs\":" << (f.InputToSimulationMilliseconds ? std::to_string(*f.InputToSimulationMilliseconds) : "null") << ",\"inputToSubmitMs\":" << (f.InputToRenderSubmissionMilliseconds ? std::to_string(*f.InputToRenderSubmissionMilliseconds) : "null") << ",\"inputToPresentMs\":" << (f.InputToPresentMilliseconds ? std::to_string(*f.InputToPresentMilliseconds) : "null") << ",\"inputToDisplay\":\"unavailable\",\"clickToPhoton\":\"unavailable\",\"lifecycle\":["; for(size_t e=0;e<f.Lifecycle.size();++e){if(e)out<<',';out<<"{\"phase\":\""<<PhaseName(f.Lifecycle[e].Phase)<<"\",\"ms\":"<<f.Lifecycle[e].MillisecondsFromFrameStart<<",\"qpc\":"<<f.Lifecycle[e].QpcTick<<'}';} out<<"],\"waits\":["; for(size_t w=0;w<f.Waits.size();++w){if(w)out<<',';const auto& x=f.Waits[w];out<<"{\"kind\":\""<<WaitName(x.Kind)<<"\",\"applied\":"<<(x.Applied?"true":"false")<<",\"ms\":"<<x.Milliseconds<<",\"candidate\":\""<<ToString(x.Candidate)<<"\",\"deadlineMissed\":"<<(x.DeadlineMissed?"true":"false")<<",\"requestedDeadlineMs\":"<<x.RequestedDeadlineMilliseconds<<",\"actualReleaseMs\":"<<x.ActualReleaseMilliseconds<<'}';} out<<"],\"gpuCompletionFrame\":"<<(f.HasGpuCompletionObservation?std::to_string(f.LastGpuCompletionObservedFrameIndex):"null")<<",\"display\":\"unavailable\",\"replacementDrop\":\"unavailable\",\"inputLatency\":\"unavailable\",\"gpuTimingStatus\":\""<<ToString(f.GpuStatus)<<"\",\"gpuDurationMs\":"<<(gpuDuration == "unavailable" ? "\"unavailable\"" : gpuDuration)<<",\"gpuHeadroom\":"<<(gpuHeadroom == "unavailable" ? "\"unavailable\"" : gpuHeadroom)<<'}' << (i + 1 == s.Frames.size() ? "\n" : ",\n"); }
            return out.str() + "  ]\n}\n";
        }

        static bool WriteAtomically(const std::filesystem::path& path, const std::string& content, std::string& error)
        {
            const std::filesystem::path temp = path.string() + ".tmp";
            { std::ofstream out(temp, std::ios::binary | std::ios::trunc); out << content; if (!out) { error = "could not write benchmark artifact"; return false; } }
            std::error_code ec; std::filesystem::rename(temp, path, ec);
            if (ec) { std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(temp, path, ec); }
            if (ec) { error = "could not publish benchmark artifact"; return false; }
            return true;
        }

        void Publish()
        {
            auto snapshot = std::make_shared<FramePacingBenchmarkSnapshot>();
            snapshot->Condition = m_Condition; snapshot->Frames = m_Frames; snapshot->Summary = Summarize(m_Frames); m_Snapshot = std::move(snapshot);
        }

        size_t m_Capacity;
        FramePacingBenchmarkCondition m_Condition;
        std::vector<RendererFrameTiming> m_Frames;
        std::shared_ptr<const FramePacingBenchmarkSnapshot> m_Snapshot;
    };
}
