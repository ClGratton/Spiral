#pragma once

#include "Engine/Core/Base.h"

#include <cmath>

namespace Engine
{
    constexpr double kMinimumManualExposureEV100 = -16.0;
    constexpr double kMaximumManualExposureEV100 = 16.0;

    struct RendererColorPipelineSettings
    {
        double ManualExposureEV100 = 0.0;

        bool operator==(const RendererColorPipelineSettings&) const = default;
    };

    inline bool IsValidRendererColorPipelineSettings(const RendererColorPipelineSettings& settings)
    {
        return std::isfinite(settings.ManualExposureEV100)
            && settings.ManualExposureEV100 >= kMinimumManualExposureEV100
            && settings.ManualExposureEV100 <= kMaximumManualExposureEV100;
    }

    inline double ManualExposureScale(const RendererColorPipelineSettings& settings)
    {
        return std::exp2(-settings.ManualExposureEV100);
    }
}
