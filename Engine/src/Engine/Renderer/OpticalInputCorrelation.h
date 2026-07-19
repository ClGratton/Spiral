#pragma once

#include "Engine/Core/Base.h"

#include <string>

namespace Engine
{
    // A renderer-owned visible marker token. Optical hardware, not this token,
    // is the only admissible source for panel or click-to-photon observations.
    struct OpticalResponseMarker
    {
        static constexpr u32 SchemaVersion = 1;
        std::string MarkerId;
        u64 ApplicationFrameIndex = 0;
        u64 InputFrameIndex = 0;
        u64 InputQpcTick = 0;
        bool HighContrast = true;
    };
}
