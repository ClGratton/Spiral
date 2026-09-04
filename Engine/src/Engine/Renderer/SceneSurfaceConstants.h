#pragma once

#include "Engine/Core/Base.h"

namespace Engine
{
    struct MaterialTextureBindingSet;
    struct SceneRasterInstance;

    // Exact b0/space0 payload shared by the production D3D12/Vulkan Scene path.
    // It is one 256-byte hardware constant-buffer block per visible instance.
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
        // x is the stable frame-local material row. y marks row zero as the
        // deterministic default/error row; z/w are reserved and remain zero.
        u32 MaterialState[4] {};
    };

    static_assert(sizeof(SceneSurfaceConstants) == 256);

    SceneSurfaceConstants BuildSceneSurfaceConstants(
        const SceneRasterInstance& instance,
        const MaterialTextureBindingSet& bindings,
        bool materialErrorRow);
}
