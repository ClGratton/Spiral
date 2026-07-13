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
}
