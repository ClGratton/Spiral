#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Entity.h"

#include <vector>

namespace Engine
{
    struct SceneRenderTransform
    {
        Math::SectorLocalPosition Position;
        Math::Vec3 RotationDegrees;
        Math::Vec3 Scale = { 1.0f, 1.0f, 1.0f };
    };

    struct SceneRenderMesh
    {
        EntityId SourceEntity = kInvalidEntityId;
        SceneRenderTransform Transform;
        AssetHandle MeshAsset = kInvalidAssetHandle;
        AssetHandle MaterialAsset = kInvalidAssetHandle;
        bool CastsShadows = true;
    };

    struct SceneRenderLight
    {
        EntityId SourceEntity = kInvalidEntityId;
        SceneRenderTransform Transform;
        LightType Type = LightType::Directional;
        Math::Vec3 Color = { 1.0f, 1.0f, 1.0f };
        float Intensity = 1.0f;
        float Range = 10.0f;
        float InnerConeDegrees = 25.0f;
        float OuterConeDegrees = 45.0f;
        bool CastsShadows = true;
    };

    struct SceneRenderCamera
    {
        EntityId SourceEntity = kInvalidEntityId;
        SceneRenderTransform Transform;
        CameraProjection Projection;
        Math::Vec3 BackgroundColor = { 0.08f, 0.09f, 0.10f };
        bool Main = false;
    };

    struct SceneRenderView
    {
        CameraView Camera;
    };

    struct SceneRenderSnapshot
    {
        u64 FrameIndex = 0;
        EntityId MainCameraEntity = kInvalidEntityId;
        Math::WorldGridPolicy WorldGridPolicy;
        std::vector<SceneRenderMesh> Meshes;
        std::vector<SceneRenderLight> Lights;
        std::vector<SceneRenderCamera> Cameras;
        std::vector<SceneRenderView> Views;
    };
}
