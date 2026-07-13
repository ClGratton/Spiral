#pragma once

#include "Engine/RHI/Device.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Engine
{
    struct RendererAdapterCandidateDiagnostics
    {
        std::size_t CandidateIndex = 0;
        std::string Name;
        bool Accepted = false;
        bool Selected = false;
        std::vector<std::string> Fallbacks;
        std::vector<std::string> RejectionReasons;
    };

    struct RendererCapabilityReasonDiagnostics
    {
        std::vector<std::string> SelectedFallbacks;
        std::vector<RendererAdapterCandidateDiagnostics> AdapterCandidates;
    };

    RendererCapabilityReasonDiagnostics BuildRendererCapabilityReasonDiagnostics(
        const RHI::DeviceCapabilities& capabilities);
}
