#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
    constexpr float kMaximumFiniteRgba16Float = 65504.0f;

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

    // Scene-linear radiance is pre-exposed before entering RGBA16F. Saturation
    // at the finite-half limit intentionally preserves the current neutral
    // tone mapper's displayed result (its input ceiling is much lower), not an
    // unbounded HDR value. Tone mapping never applies exposure a second time.
    struct ScenePreExposureState
    {
        double EffectiveExposureEV100 = 0.0;
        float Scale = 1.0f;
        float MaximumSceneLinearChannel = kMaximumFiniteRgba16Float;

        bool operator==(const ScenePreExposureState&) const = default;
    };

    inline bool TryResolveScenePreExposure(const RendererColorPipelineSettings& settings,
        ScenePreExposureState& outState)
    {
        if (!IsValidRendererColorPipelineSettings(settings))
            return false;
        const double effectiveEV100 = EffectiveExposureEV100(settings);
        const double scale = std::exp2(-effectiveEV100);
        const double maximumInput = static_cast<double>(kMaximumFiniteRgba16Float) / scale;
        if (!std::isfinite(scale) || scale <= 0.0
            || scale > std::numeric_limits<float>::max()
            || !std::isfinite(maximumInput) || maximumInput <= 0.0
            || maximumInput > std::numeric_limits<float>::max())
            return false;
        outState = {
            effectiveEV100,
            static_cast<float>(scale),
            static_cast<float>(maximumInput)
        };
        return true;
    }

    inline bool IsValidScenePreExposureState(const ScenePreExposureState& state)
    {
        if (!std::isfinite(state.EffectiveExposureEV100)
            || state.EffectiveExposureEV100 < kMinimumManualExposureEV100
            || state.EffectiveExposureEV100 > kMaximumManualExposureEV100)
            return false;
        const double expectedScale = std::exp2(-state.EffectiveExposureEV100);
        const double expectedMaximum =
            static_cast<double>(kMaximumFiniteRgba16Float) / expectedScale;
        return std::isfinite(state.Scale) && state.Scale > 0.0f
            && std::isfinite(state.MaximumSceneLinearChannel)
            && state.MaximumSceneLinearChannel > 0.0f
            && state.Scale == static_cast<float>(expectedScale)
            && state.MaximumSceneLinearChannel == static_cast<float>(expectedMaximum);
    }

    inline bool TryPreExposeSceneLinear(const Math::Vec3& sceneLinear,
        const ScenePreExposureState& state, Math::Vec3& outPreExposed)
    {
        if (!std::isfinite(sceneLinear.X) || !std::isfinite(sceneLinear.Y)
            || !std::isfinite(sceneLinear.Z)
            || !IsValidScenePreExposureState(state))
            return false;
        const auto convert = [&state](float channel)
        {
            const double nonnegative = std::max(static_cast<double>(channel), 0.0);
            const double boundedInput = std::min(nonnegative,
                static_cast<double>(state.MaximumSceneLinearChannel));
            return static_cast<float>(std::min(
                boundedInput * static_cast<double>(state.Scale),
                static_cast<double>(kMaximumFiniteRgba16Float)));
        };
        outPreExposed = {
            convert(sceneLinear.X),
            convert(sceneLinear.Y),
            convert(sceneLinear.Z)
        };
        return true;
    }

    struct PhotometricLightReadout
    {
        LightType Type = LightType::Directional;
        double Value = 0.0;
        LightPhotometricUnit Unit = LightPhotometricUnit::Lux;
        double EffectiveExposureEV100 = 0.0;
        double ExposureScale = 1.0;
    };

    inline bool TryBuildPhotometricLightReadout(const LightComponent& light,
        const RendererColorPipelineSettings& settings, PhotometricLightReadout& outReadout)
    {
        if (!IsValidLightComponent(light) || !IsValidRendererColorPipelineSettings(settings))
            return false;
        outReadout = {
            light.Type,
            light.PhotometricValue,
            light.PhotometricUnit,
            EffectiveExposureEV100(settings),
            ManualExposureScale(settings)
        };
        return true;
    }
}
