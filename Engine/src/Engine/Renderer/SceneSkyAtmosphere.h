#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

#include <cstddef>
#include <limits>
#include <string>

namespace Engine
{
    inline constexpr float kBasicSkyTurbidity = 3.0f;
    inline constexpr float kBasicSkyGroundAlbedo = 0.1f;
    inline constexpr float kBasicSunAngularRadiusDegrees = 0.266f;
    inline constexpr float kBasicSunAngularRadiusRadians =
        kBasicSunAngularRadiusDegrees * 0.01745329251994329577f;
    inline constexpr double kBasicSkyReferenceSunIlluminanceLux = 100000.0;

    struct SceneSkyPerezCoefficients
    {
        float A = 0.0f;
        float B = 0.0f;
        float C = 0.0f;
        float D = 0.0f;
        float E = 0.0f;

        bool operator==(const SceneSkyPerezCoefficients&) const = default;
    };

    // Immutable per-view inputs for the first analytic daytime sky. The model
    // is deliberately fixed to an Earth-like baseline until the authorable
    // environment and production-atmosphere work in Phase 8.
    struct SceneSkyAtmosphereFrame
    {
        bool Enabled = false;
        EntityId SunEntity = kInvalidEntityId;
        u32 SunLightIndex = std::numeric_limits<u32>::max();
        float Turbidity = kBasicSkyTurbidity;
        float GroundAlbedo = kBasicSkyGroundAlbedo;
        float SunAngularRadiusRadians = kBasicSunAngularRadiusRadians;
        float SunZenithRadians = 0.0f;
        float TanHalfVerticalFov = 0.0f;
        float AspectRatio = 0.0f;
        Math::Vec3 SurfaceToSunWorld;
        Math::Vec3 SurfaceToSunView;
        Math::Vec3 ViewUp;
        Math::Vec3 SunRadiance;
        Math::Vec3 GroundRadiance;
        Math::Vec3 UpperDiffuseIrradiance;
        Math::Vec3 LowerDiffuseIrradiance;
        float ZenithChromaticityX = 0.0f;
        float ZenithChromaticityY = 0.0f;
        float ZenithLuminanceCdPerSquareMeter = 0.0f;
        SceneSkyPerezCoefficients LuminancePerez;
        SceneSkyPerezCoefficients ChromaticityXPerez;
        SceneSkyPerezCoefficients ChromaticityYPerez;
    };

    // Exact b0/space0 payload consumed by SkyAtmosphere.hlsl.
    struct SceneSkyAtmosphereGpuConstants
    {
        float ProjectionAndExposure[4] {};
        float SunDirectionAndCosRadius[4] {};
        float ViewUpAndEnabled[4] {};
        float ZenithxyYAndSunTheta[4] {};
        float LuminancePerezABCD[4] {};
        float ChromaticityXPerezABCD[4] {};
        float ChromaticityYPerezABCD[4] {};
        float PerezE[4] {};
        float SunRadiance[4] {};
        float GroundRadiance[4] {};
    };

    static_assert(sizeof(SceneSkyAtmosphereGpuConstants) == 160);

    bool TryPrepareSceneSkyAtmosphere(const SceneRenderSnapshot& snapshot,
        size_t viewIndex, SceneSkyAtmosphereFrame& outFrame,
        std::string& outError);
    bool IsValidSceneSkyAtmosphereFrame(const SceneSkyAtmosphereFrame& frame);
    bool TryBuildSceneSkyAtmosphereGpuConstants(
        const SceneSkyAtmosphereFrame& frame, float preExposure,
        SceneSkyAtmosphereGpuConstants& outConstants, std::string& outError);
}
