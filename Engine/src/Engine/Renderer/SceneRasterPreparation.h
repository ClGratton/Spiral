#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/Renderer/ClusteredLightGrid.h"
#include "Engine/Renderer/SceneSkyAtmosphere.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Engine
{
    class ArtifactResolverSnapshot;

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
        Math::Mat4 NormalTransform;
        Math::Mat4 ModelView;
        Math::Mat4 NormalViewTransform;
        bool CastsShadows = true;
        // Frame-local row zero is always the default/error material and never
        // aliases a persistent asset, entity, or future visibility ID.
        u32 MaterialId = 0;
    };

    struct SceneMaterialRow
    {
        u32 Id = 0;
        AssetHandle SourceAsset = kInvalidAssetHandle;
        MaterialAsset Material;
        u64 CatalogGeneration = 0;
        bool IsError = true;
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
        ClusteredLightGrid LightGrid;
        SceneSkyAtmosphereFrame SkyAtmosphere;
        // Exact immutable asset/material generation used by preparation and
        // every later GPU resolution for this frame.
        Ref<const ArtifactResolverSnapshot> ArtifactResolvers;
        u64 MaterialCatalogGeneration = 0;
        std::vector<SceneMaterialRow> MaterialRows;
        std::vector<SceneRasterInstance> Instances;
    };

    bool BuildSceneNormalTransform(const Math::Vec3& scale,
        const Math::Vec3& rotationDegrees, Math::Mat4& outTransform);
    SceneRasterFrame PrepareSceneRasterFrame(const SceneRenderSnapshot& snapshot, size_t viewIndex = 0);
}
