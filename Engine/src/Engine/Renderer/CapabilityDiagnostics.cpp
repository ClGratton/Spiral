#include "Engine/Renderer/CapabilityDiagnostics.h"

#include <utility>

namespace Engine
{
    RendererCapabilityReasonDiagnostics BuildRendererCapabilityReasonDiagnostics(
        const RHI::DeviceCapabilities& capabilities)
    {
        RendererCapabilityReasonDiagnostics diagnostics;
        diagnostics.SelectedFallbacks = capabilities.Fallbacks;
        diagnostics.AdapterCandidates.reserve(capabilities.AdapterSelection.Evaluations.size());
        for (const RHI::AdapterEvaluation& evaluation : capabilities.AdapterSelection.Evaluations)
        {
            if (evaluation.CandidateIndex >= capabilities.AdapterCandidates.size())
                continue;

            RendererAdapterCandidateDiagnostics candidate;
            candidate.CandidateIndex = evaluation.CandidateIndex;
            candidate.Name = capabilities.AdapterCandidates[evaluation.CandidateIndex].Identity.Name;
            candidate.Accepted = evaluation.Accepted;
            candidate.Selected = evaluation.CandidateIndex == capabilities.AdapterSelection.SelectedIndex;
            candidate.Fallbacks = evaluation.Fallbacks;
            candidate.RejectionReasons = evaluation.RejectionReasons;
            diagnostics.AdapterCandidates.push_back(std::move(candidate));
        }
        return diagnostics;
    }

    RHI::CapabilityGroupState BuildFrameTimingCapabilityGroup(
        const RHI::DeviceCapabilities& capabilities, bool gpuTimingConsumerImplemented)
    {
        RHI::CapabilityGroupState group;
        group.Group = RHI::CapabilityGroupId::Phase3FrameTimingV1;
        group.ProfileName = "Phase 3 Frame Timing V1";
        group.PreferredPath = RHI::CapabilityPath::GpuTimestamps;

        const RHI::CapabilityState& timestamps = capabilities.GetFeature(RHI::DeviceFeature::Timestamps);
        if (timestamps.IsUsable() && gpuTimingConsumerImplemented)
        {
            group.SelectedPath = RHI::CapabilityPath::GpuTimestamps;
            group.Implemented = true;
            return group;
        }

        group.SelectedPath = RHI::CapabilityPath::CpuSteadyClock;
        group.Implemented = true;
        if (timestamps.IsUsable())
        {
            group.UnsupportedReasons.emplace_back(
                "Native RHI timestamps are usable, but whole-frame/pass instrumentation is not implemented yet");
            group.Fallbacks.emplace_back(
                "Renderer GPU timing consumer is unavailable; selected portable CPU steady-clock timing");
        }
        else
        {
            group.UnsupportedReasons.emplace_back(timestamps.Detail.empty()
                ? "GPU timestamps are not usable on the active device profile"
                : timestamps.Detail);
            group.Fallbacks.emplace_back("GPU timestamps are unavailable; selected portable CPU steady-clock timing");
        }
        return group;
    }
}
