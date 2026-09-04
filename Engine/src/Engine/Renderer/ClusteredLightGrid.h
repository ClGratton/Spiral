#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Engine
{
    // Keeps the later global per-pixel loop explicitly bounded. Exceeding the
    // cap rejects the complete candidate grid instead of silently dropping a
    // light or publishing an unbounded shader workload.
    inline constexpr u32 kMaximumDirectionalLightCount = 16;

    struct ClusteredLightGridConfig
    {
        u32 TileSizePixels = 64;
        u32 DepthSliceCount = 16;
        u32 MaximumLocalLightsPerCluster = 64;
    };

    struct ClusteredLightRecord
    {
        EntityId SourceEntity = kInvalidEntityId;
        LightType Type = LightType::Directional;
        Math::Vec3 ViewPosition;
        Math::Vec3 WorldDirection;
        Math::Vec3 ViewDirection;
        Math::Vec3 Color = { 1.0f, 1.0f, 1.0f };
        double PhotometricValue = 0.0;
        LightPhotometricUnit PhotometricUnit = LightPhotometricUnit::Lux;
        float Range = 0.0f;
        float InnerConeCosine = 1.0f;
        float OuterConeCosine = 1.0f;
        bool CastsShadows = false;
    };

    // CPU reference/prototype for the production Forward+ grid. A bounded
    // directional set stays in one compact global list; only bounded local-light
    // references occupy the screen/depth cluster CSR arrays.
    struct ClusteredLightGrid
    {
        u32 ViewportWidth = 0;
        u32 ViewportHeight = 0;
        u32 TileSizePixels = 0;
        u32 TileCountX = 0;
        u32 TileCountY = 0;
        u32 DepthSliceCount = 0;
        float NearClip = 0.0f;
        float FarClip = 0.0f;
        u32 MaximumLocalLightsPerCluster = 0;
        u32 OverflowedLocalLightReferences = 0;
        std::vector<ClusteredLightRecord> Lights;
        std::vector<u32> GlobalLightIndices;
        std::vector<u32> ClusterOffsets;
        std::vector<u32> LocalLightIndices;

        size_t GetClusterCount() const;
        size_t GetClusterIndex(u32 tileX, u32 tileY, u32 depthSlice) const;
        u32 SelectDepthSlice(float viewDepth) const;
    };

    bool BuildClusteredLightGrid(const SceneRenderSnapshot& snapshot,
        size_t viewIndex, u32 viewportWidth, u32 viewportHeight,
        const ClusteredLightGridConfig& config,
        ClusteredLightGrid& outGrid, std::string& outError);
}
