#pragma once

#include "Engine/Core/Base.h"

#include <cmath>
#include <string_view>

namespace Engine
{
    constexpr double kMinimumManualExposureEV100 = -16.0;
    constexpr double kMaximumManualExposureEV100 = 16.0;
    constexpr double kMinimumCameraApertureFNumber = 0.7;
    constexpr double kMaximumCameraApertureFNumber = 64.0;
    constexpr double kMinimumCameraShutterSeconds = 1.0 / 8000.0;
    constexpr double kMaximumCameraShutterSeconds = 60.0;
    constexpr double kMinimumCameraISO = 1.0;
    constexpr double kMaximumCameraISO = 102400.0;
    constexpr double kMinimumPostToneMapSaturation = 0.0;
    constexpr double kMaximumPostToneMapSaturation = 2.0;
    constexpr double kMinimumPostToneMapContrast = 0.0;
    constexpr double kMaximumPostToneMapContrast = 2.0;

    enum class RendererExposureMode
    {
        ManualEV100,
        CameraCalibration
    };

    inline const char* ToString(RendererExposureMode mode)
    {
        switch (mode)
        {
            case RendererExposureMode::ManualEV100: return "ManualEV100";
            case RendererExposureMode::CameraCalibration: return "CameraCalibration";
        }
        return "ManualEV100";
    }

    inline bool ParseRendererExposureMode(std::string_view text, RendererExposureMode& outMode)
    {
        if (text == "ManualEV100")
        {
            outMode = RendererExposureMode::ManualEV100;
            return true;
        }
        if (text == "CameraCalibration")
        {
            outMode = RendererExposureMode::CameraCalibration;
            return true;
        }
        return false;
    }

    struct RendererColorPipelineSettings
    {
        double ManualExposureEV100 = 0.0;
        double PostToneMapSaturation = 1.0;
        double PostToneMapContrast = 1.0;
        RendererExposureMode ExposureMode = RendererExposureMode::ManualEV100;
        double CameraApertureFNumber = 1.0;
        double CameraShutterSeconds = 1.0;
        double CameraISO = 100.0;

        bool operator==(const RendererColorPipelineSettings&) const = default;
    };

    inline double CameraCalibrationExposureEV100(double apertureFNumber, double shutterSeconds, double iso)
    {
        return std::log2((apertureFNumber * apertureFNumber) / shutterSeconds * (100.0 / iso));
    }

    inline double EffectiveExposureEV100(const RendererColorPipelineSettings& settings)
    {
        if (settings.ExposureMode == RendererExposureMode::CameraCalibration)
            return CameraCalibrationExposureEV100(
                settings.CameraApertureFNumber, settings.CameraShutterSeconds, settings.CameraISO);
        return settings.ManualExposureEV100;
    }

    inline bool IsValidRendererColorPipelineSettings(const RendererColorPipelineSettings& settings)
    {
        return std::isfinite(settings.ManualExposureEV100)
            && settings.ManualExposureEV100 >= kMinimumManualExposureEV100
            && settings.ManualExposureEV100 <= kMaximumManualExposureEV100
            && (settings.ExposureMode == RendererExposureMode::ManualEV100
                || settings.ExposureMode == RendererExposureMode::CameraCalibration)
            && std::isfinite(settings.CameraApertureFNumber)
            && settings.CameraApertureFNumber >= kMinimumCameraApertureFNumber
            && settings.CameraApertureFNumber <= kMaximumCameraApertureFNumber
            && std::isfinite(settings.CameraShutterSeconds)
            && settings.CameraShutterSeconds >= kMinimumCameraShutterSeconds
            && settings.CameraShutterSeconds <= kMaximumCameraShutterSeconds
            && std::isfinite(settings.CameraISO)
            && settings.CameraISO >= kMinimumCameraISO
            && settings.CameraISO <= kMaximumCameraISO
            && std::isfinite(EffectiveExposureEV100(settings))
            && EffectiveExposureEV100(settings) >= kMinimumManualExposureEV100
            && EffectiveExposureEV100(settings) <= kMaximumManualExposureEV100
            && std::isfinite(settings.PostToneMapSaturation)
            && settings.PostToneMapSaturation >= kMinimumPostToneMapSaturation
            && settings.PostToneMapSaturation <= kMaximumPostToneMapSaturation
            && std::isfinite(settings.PostToneMapContrast)
            && settings.PostToneMapContrast >= kMinimumPostToneMapContrast
            && settings.PostToneMapContrast <= kMaximumPostToneMapContrast;
    }

    inline double ManualExposureScale(const RendererColorPipelineSettings& settings)
    {
        return std::exp2(-EffectiveExposureEV100(settings));
    }
}
