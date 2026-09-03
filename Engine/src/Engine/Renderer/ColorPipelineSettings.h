#pragma once

#include "Engine/Core/Base.h"

#include <cmath>

namespace Engine
{
    constexpr double kMinimumManualExposureEV100 = -16.0;
    constexpr double kMaximumManualExposureEV100 = 16.0;
    constexpr double kMinimumPostToneMapSaturation = 0.0;
    constexpr double kMaximumPostToneMapSaturation = 2.0;
    constexpr double kMinimumPostToneMapContrast = 0.0;
    constexpr double kMaximumPostToneMapContrast = 2.0;

    struct RendererColorPipelineSettings
    {
        double ManualExposureEV100 = 0.0;
        double PostToneMapSaturation = 1.0;
        double PostToneMapContrast = 1.0;

        bool operator==(const RendererColorPipelineSettings&) const = default;
    };

    inline bool IsValidRendererColorPipelineSettings(const RendererColorPipelineSettings& settings)
    {
        return std::isfinite(settings.ManualExposureEV100)
            && settings.ManualExposureEV100 >= kMinimumManualExposureEV100
            && settings.ManualExposureEV100 <= kMaximumManualExposureEV100
            && std::isfinite(settings.PostToneMapSaturation)
            && settings.PostToneMapSaturation >= kMinimumPostToneMapSaturation
            && settings.PostToneMapSaturation <= kMaximumPostToneMapSaturation
            && std::isfinite(settings.PostToneMapContrast)
            && settings.PostToneMapContrast >= kMinimumPostToneMapContrast
            && settings.PostToneMapContrast <= kMaximumPostToneMapContrast;
    }

    inline double ManualExposureScale(const RendererColorPipelineSettings& settings)
    {
        return std::exp2(-settings.ManualExposureEV100);
    }
}
