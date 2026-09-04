#pragma once

#include "Engine/Core/Base.h"

namespace Engine
{
    struct MaterialTextureBindingSet;
    struct SceneRasterInstance;
    struct SceneDebugVisualizationSettings;
    struct SceneSkyAtmosphereFrame;
    struct SceneShadowMapFrame;
    enum class SceneShadowCasterMode : u32;

    // Exact b0/space0 payload shared by the production D3D12/Vulkan Scene path.
    // The reflected payload exactly fills one 512-byte hardware constant-buffer
    // block per visible instance.
    struct SceneSurfaceConstants
    {
        float ViewProjection[16] {};
        float NormalTransform[16] {};
        float BaseColorAndAlphaCutoff[4] {};
        float EmissiveAndStrength[4] {};
        float SurfaceFactors[4] {};
        float CallistoFactors[4] {};
        u32 TextureIndices0[4] {};
        u32 TextureIndices1[4] {};
        u32 TextureState[4] {};
        // x is the frame-local material row; y marks row zero as the
        // deterministic default/error row; z is SceneDebugView; w is the
        // deterministic 32-bit fold of the persistent MaterialAsset handle
        // used as the stable material-identity visualization input.
        u32 MaterialState[4] {};
        float ModelView[16] {};
        float NormalViewTransform[16] {};
        float ShadowViewProjection[16] {};
        // x=1/resolution, y=constant depth bias, z=slope depth bias,
        // w=world units per texel.
        float ShadowParameters[4] {};
        // x=enabled, y=primary light record, z=resolution, w=caster mode.
        u32 ShadowState[4] {};
        // RGB diffuse irradiance for upward/downward-facing world normals.
        // Upper.w is the enabled flag; Lower.w is reserved zero.
        float SkyIrradianceUpper[4] {};
        float SkyIrradianceLower[4] {};
    };

    static_assert(sizeof(SceneSurfaceConstants) == 512);

    bool TryBuildSceneSurfaceConstants(
        const SceneRasterInstance& instance,
        const MaterialTextureBindingSet& bindings,
        bool materialErrorRow,
        SceneSurfaceConstants& outConstants);
    bool TryApplySceneShadowMapConstants(const SceneRasterInstance& instance,
        const SceneShadowMapFrame& shadow, SceneShadowCasterMode casterMode,
        SceneSurfaceConstants& inOutConstants);
    bool TryApplySceneSkyIrradianceConstants(
        const SceneSkyAtmosphereFrame& sky,
        SceneSurfaceConstants& inOutConstants);
    bool TryApplySceneDebugVisualizationConstants(
        const SceneRasterInstance& instance,
        const SceneDebugVisualizationSettings& settings,
        SceneSurfaceConstants& inOutConstants);
}
