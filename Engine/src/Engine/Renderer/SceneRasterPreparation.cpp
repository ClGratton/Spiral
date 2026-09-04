#include "Engine/Renderer/SceneRasterPreparation.h"

#include "Engine/Renderer/Renderer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine
{
    bool BuildSceneNormalTransform(const Math::Vec3& scale,
        const Math::Vec3& rotationDegrees, Math::Mat4& outTransform)
    {
        const float values[] { scale.X, scale.Y, scale.Z,
            rotationDegrees.X, rotationDegrees.Y, rotationDegrees.Z };
        for (float value : values)
            if (!std::isfinite(value))
                return false;
        if (scale.X == 0.0f || scale.Y == 0.0f || scale.Z == 0.0f)
            return false;

        const Math::Mat4 inverseScale = Math::Scale(
            { 1.0f / scale.X, 1.0f / scale.Y, 1.0f / scale.Z });
        const Math::Mat4 rotation = Math::RotationYawPitchRoll(
            Math::DegreesToRadians(rotationDegrees.Y),
            Math::DegreesToRadians(rotationDegrees.X),
            Math::DegreesToRadians(rotationDegrees.Z));
        const Math::Mat4 candidate = Math::Multiply(inverseScale, rotation);
        for (float value : candidate.Values)
            if (!std::isfinite(value))
                return false;
        outTransform = candidate;
        return true;
    }

    SceneRasterFrame PrepareSceneRasterFrame(const SceneRenderSnapshot& snapshot, size_t viewIndex)
    {
        SceneRasterFrame frame;
        frame.SnapshotFrameIndex = snapshot.FrameIndex;
        if (viewIndex >= snapshot.Views.size() || !snapshot.Views[viewIndex].Camera.Valid)
            return frame;

        const CameraView& view = snapshot.Views[viewIndex].Camera;
        Math::SectorLocalPosition translationOriginPosition;
        if (view.HasCanonicalTranslationOrigin)
        {
            translationOriginPosition = view.TranslationOriginPosition;
        }
        else if (!Math::TryDecomposeWorldPosition(
            view.TranslationOrigin,
            snapshot.WorldGridPolicy,
            translationOriginPosition))
        {
            return frame;
        }

        frame.TranslationOrigin = view.TranslationOrigin;
        frame.HasValidView = true;
        SceneMaterialRow defaultRow;
        defaultRow.Material.Name = "Default/Error Material";
        frame.MaterialRows.push_back(defaultRow);

        std::vector<AssetHandle> uniqueMaterialAssets;
        uniqueMaterialAssets.reserve(snapshot.Meshes.size());
        for (const SceneRenderMesh& mesh : snapshot.Meshes)
            if (mesh.MaterialAsset != kInvalidAssetHandle)
                uniqueMaterialAssets.push_back(mesh.MaterialAsset);
        std::sort(uniqueMaterialAssets.begin(), uniqueMaterialAssets.end());
        uniqueMaterialAssets.erase(std::unique(uniqueMaterialAssets.begin(), uniqueMaterialAssets.end()), uniqueMaterialAssets.end());
        if (uniqueMaterialAssets.size() >= std::numeric_limits<u32>::max())
            return {};

        std::vector<std::pair<AssetHandle, u32>> materialIds;
        materialIds.reserve(uniqueMaterialAssets.size());
        bool generationInitialized = false;
        u64 catalogGeneration = 0;
        for (AssetHandle asset : uniqueMaterialAssets)
        {
            MaterialAsset material;
            u64 resolvedGeneration = 0;
            std::string ignoredError;
            const bool resolved = Renderer::ResolvePublishedMaterialAsset(
                asset, material, resolvedGeneration, ignoredError);
            if (!generationInitialized)
            {
                generationInitialized = true;
                catalogGeneration = resolvedGeneration;
            }
            else if (catalogGeneration != resolvedGeneration)
            {
                return {};
            }
            u32 materialId = 0;
            if (resolved)
            {
                materialId = static_cast<u32>(frame.MaterialRows.size());
                frame.MaterialRows.push_back({ materialId, asset, std::move(material), resolvedGeneration, false });
            }
            materialIds.emplace_back(asset, materialId);
        }
        frame.MaterialCatalogGeneration = generationInitialized
            ? catalogGeneration : Renderer::GetPublishedArtifactResolverGeneration();
        frame.MaterialRows[0].CatalogGeneration = frame.MaterialCatalogGeneration;
        if (Renderer::GetPublishedArtifactResolverGeneration() != frame.MaterialCatalogGeneration)
            return {};
        frame.Instances.reserve(snapshot.Meshes.size());

        for (const SceneRenderMesh& mesh : snapshot.Meshes)
        {
            SceneRasterInstance instance;
            instance.SourceEntity = mesh.SourceEntity;
            instance.MeshAsset = mesh.MeshAsset;
            instance.MaterialAsset = mesh.MaterialAsset;
            const auto material = std::lower_bound(materialIds.begin(), materialIds.end(), mesh.MaterialAsset,
                [](const std::pair<AssetHandle, u32>& entry, AssetHandle value) { return entry.first < value; });
            instance.MaterialId = material != materialIds.end() && material->first == mesh.MaterialAsset
                ? material->second : 0;
            instance.Position = mesh.Transform.Position;
            instance.TranslationOrigin = view.TranslationOrigin;
            instance.TranslationOriginPosition = translationOriginPosition;
            Math::DVec3 relativePosition;
            if (!Math::TryGetSectorLocalRelativePosition(
                mesh.Transform.Position,
                translationOriginPosition,
                snapshot.WorldGridPolicy,
                relativePosition))
            {
                return {};
            }
            instance.CameraRelativePosition = {
                static_cast<float>(relativePosition.X),
                static_cast<float>(relativePosition.Y),
                static_cast<float>(relativePosition.Z)
            };

            const Math::Mat4 scale = Math::Scale(mesh.Transform.Scale);
            const Math::Mat4 rotation = Math::RotationYawPitchRoll(
                Math::DegreesToRadians(mesh.Transform.RotationDegrees.Y),
                Math::DegreesToRadians(mesh.Transform.RotationDegrees.X),
                Math::DegreesToRadians(mesh.Transform.RotationDegrees.Z));
            const Math::Mat4 translation = Math::Translation(instance.CameraRelativePosition);
            instance.CameraRelativeModel = Math::Multiply(Math::Multiply(scale, rotation), translation);
            instance.ModelViewProjection = Math::Multiply(instance.CameraRelativeModel, view.ViewProjection);
            if (!BuildSceneNormalTransform(mesh.Transform.Scale,
                mesh.Transform.RotationDegrees, instance.NormalTransform))
                return {};
            frame.Instances.push_back(instance);
        }

        return frame;
    }
}
