#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Engine
{
    struct SceneRasterInstance
    {
        EntityId SourceEntity = kInvalidEntityId;
        AssetHandle MeshAsset = kInvalidAssetHandle;
        AssetHandle MaterialAsset = kInvalidAssetHandle;
        Math::SectorLocalPosition Position;
        Math::DVec3 TranslationOrigin;
        Math::SectorLocalPosition TranslationOriginPosition;
        Math::Vec3 CameraRelativePosition;
        Math::Mat4 CameraRelativeModel;
        Math::Mat4 ModelViewProjection;
    };

    struct SceneRasterFrame
    {
        enum class Availability
        {
            Ready,
            ShaderPipelinePending,
            ShaderPipelineUnavailable
        };

        u64 SnapshotFrameIndex = 0;
        Math::DVec3 TranslationOrigin;
        bool HasValidView = false;
        u32 IssuedDrawCount = 0;
        Availability RasterAvailability = Availability::Ready;
        std::string Diagnostic;
        std::vector<SceneRasterInstance> Instances;
    };

    SceneRasterFrame PrepareSceneRasterFrame(const SceneRenderSnapshot& snapshot, size_t viewIndex = 0);
}
