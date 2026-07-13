#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

#include <cstddef>
#include <vector>

namespace Engine
{
    struct SceneRasterInstance
    {
        EntityId SourceEntity = kInvalidEntityId;
        AssetHandle MeshAsset = kInvalidAssetHandle;
        AssetHandle MaterialAsset = kInvalidAssetHandle;
        Math::DVec3 WorldPosition;
        Math::DVec3 TranslationOrigin;
        Math::Vec3 CameraRelativePosition;
        Math::Mat4 CameraRelativeModel;
        Math::Mat4 ModelViewProjection;
    };

    struct SceneRasterFrame
    {
        u64 SnapshotFrameIndex = 0;
        Math::DVec3 TranslationOrigin;
        bool HasValidView = false;
        u32 IssuedDrawCount = 0;
        std::vector<SceneRasterInstance> Instances;
    };

    SceneRasterFrame PrepareSceneRasterFrame(const SceneRenderSnapshot& snapshot, size_t viewIndex = 0);
}
