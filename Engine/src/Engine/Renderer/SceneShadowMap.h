#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Scene/Entity.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace Engine
{
    struct SceneRasterFrame;

    inline constexpr u32 kSceneShadowMapResolution = 1024;

    struct SceneObjectBounds
    {
        Math::Vec3 Minimum;
        Math::Vec3 Maximum;
    };

    enum class SceneShadowCasterMode : u32
    {
        Opaque = 0,
        AlphaTested = 1,
        ConservativeError = 2,
        Excluded = 3
    };

    struct SceneShadowCaster
    {
        size_t InstanceIndex = 0;
        SceneShadowCasterMode Mode = SceneShadowCasterMode::Opaque;
        Math::Mat4 ModelToShadowClip;
    };

    // One current-frame, primary-directional shadow view. Later cascades and
    // atlases extend this bounded publication instead of hiding extra resource
    // policy or unbounded light loops in the shader.
    struct SceneShadowMapFrame
    {
        bool Enabled = false;
        u32 Resolution = 0;
        u32 PrimaryLightIndex = std::numeric_limits<u32>::max();
        EntityId PrimaryLightEntity = kInvalidEntityId;
        u32 EligibleDirectionalLightCount = 0;
        u32 OpaqueCasterCount = 0;
        u32 AlphaTestedCasterCount = 0;
        u32 ConservativeErrorCasterCount = 0;
        u32 ComponentExcludedCount = 0;
        u32 BlendExcludedCount = 0;
        float WorldUnitsPerTexel = 0.0f;
        float ConstantDepthBias = 0.0f;
        float SlopeDepthBias = 0.0f;
        Math::Mat4 CameraRelativeToShadowClip;
        std::vector<SceneShadowCaster> Casters;
    };

    bool TryPrepareSceneShadowMap(const SceneRasterFrame& frame,
        const std::vector<SceneObjectBounds>& objectBounds, u32 resolution,
        SceneShadowMapFrame& outShadow, std::string& outError);
}
