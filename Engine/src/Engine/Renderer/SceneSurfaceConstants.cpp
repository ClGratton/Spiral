#include "Engine/Renderer/SceneSurfaceConstants.h"

#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/TextureRuntimePublication.h"

#include <cmath>
#include <cstring>

namespace Engine
{
    namespace
    {
        bool IsFiniteMatrix(const float (&matrix)[16])
        {
            for (float value : matrix)
                if (!std::isfinite(value))
                    return false;
            return true;
        }

        bool IsFiniteMaterial(const MaterialAsset& material)
        {
            const float values[] {
                material.BaseColor.X, material.BaseColor.Y, material.BaseColor.Z,
                material.Metallic, material.Roughness, material.NormalScale,
                material.OcclusionStrength, material.EmissiveColor.X,
                material.EmissiveColor.Y, material.EmissiveColor.Z,
                material.EmissiveStrength, material.AlphaCutoff,
                material.DiffuseFresnelIntensity,
                material.RetroreflectionIntensity,
                material.DiffuseFresnelFalloff,
                material.RetroreflectionFalloff,
                material.SmoothTerminator
            };
            for (float value : values)
                if (!std::isfinite(value))
                    return false;
            const bool baseColorValid = material.BaseColor.X >= 0.0f && material.BaseColor.X <= 1.0f
                && material.BaseColor.Y >= 0.0f && material.BaseColor.Y <= 1.0f
                && material.BaseColor.Z >= 0.0f && material.BaseColor.Z <= 1.0f;
            const bool emissiveProductFinite = std::isfinite(
                material.EmissiveColor.X * material.EmissiveStrength)
                && std::isfinite(material.EmissiveColor.Y * material.EmissiveStrength)
                && std::isfinite(material.EmissiveColor.Z * material.EmissiveStrength);
            const bool shadingModelValid = material.ShadingModel == MaterialShadingModel::Standard
                || material.ShadingModel == MaterialShadingModel::Unlit;
            const bool alphaModeValid = material.AlphaMode == MaterialAlphaMode::Opaque
                || material.AlphaMode == MaterialAlphaMode::Mask
                || material.AlphaMode == MaterialAlphaMode::Blend;
            return baseColorValid && emissiveProductFinite
                && shadingModelValid && alphaModeValid;
        }
    }

    bool TryBuildSceneSurfaceConstants(
        const SceneRasterInstance& instance,
        const MaterialTextureBindingSet& bindings,
        bool materialErrorRow,
        SceneSurfaceConstants& outConstants)
    {
        SceneSurfaceConstants constants;
        std::memcpy(constants.ViewProjection, instance.ModelViewProjection.Values, sizeof(constants.ViewProjection));
        std::memcpy(constants.NormalTransform, instance.NormalTransform.Values, sizeof(constants.NormalTransform));
        std::memcpy(constants.ModelView, instance.ModelView.Values, sizeof(constants.ModelView));
        std::memcpy(constants.NormalViewTransform, instance.NormalViewTransform.Values, sizeof(constants.NormalViewTransform));
        if (!IsFiniteMatrix(constants.ViewProjection)
            || !IsFiniteMatrix(constants.NormalTransform)
            || !IsFiniteMatrix(constants.ModelView)
            || !IsFiniteMatrix(constants.NormalViewTransform)
            || !IsFiniteMaterial(bindings.Material))
            return false;
        constants.BaseColorAndAlphaCutoff[0] = bindings.Material.BaseColor.X;
        constants.BaseColorAndAlphaCutoff[1] = bindings.Material.BaseColor.Y;
        constants.BaseColorAndAlphaCutoff[2] = bindings.Material.BaseColor.Z;
        constants.BaseColorAndAlphaCutoff[3] = bindings.Material.AlphaCutoff;
        constants.EmissiveAndStrength[0] = bindings.Material.EmissiveColor.X;
        constants.EmissiveAndStrength[1] = bindings.Material.EmissiveColor.Y;
        constants.EmissiveAndStrength[2] = bindings.Material.EmissiveColor.Z;
        constants.EmissiveAndStrength[3] = bindings.Material.EmissiveStrength;
        constants.SurfaceFactors[0] = bindings.Material.Metallic;
        constants.SurfaceFactors[1] = bindings.Material.Roughness;
        constants.SurfaceFactors[2] = bindings.Material.NormalScale;
        constants.SurfaceFactors[3] = bindings.Material.OcclusionStrength;
        constants.CallistoFactors[0] = bindings.Material.DiffuseFresnelIntensity;
        constants.CallistoFactors[1] = bindings.Material.RetroreflectionIntensity;
        constants.CallistoFactors[2] = bindings.Material.DiffuseFresnelFalloff;
        constants.CallistoFactors[3] = bindings.Material.RetroreflectionFalloff;
        for (size_t index = 0; index < 4; ++index)
            constants.TextureIndices0[index] = bindings.Handles[index].Index;
        constants.TextureIndices1[0] = bindings.Handles[4].Index;
        constants.TextureIndices1[1] = bindings.Handles[5].Index;
        constants.TextureState[0] = bindings.DeclaredMask;
        constants.TextureState[1] = bindings.ErrorMask;
        constants.TextureState[2] = static_cast<u32>(bindings.Material.AlphaMode);
        constants.TextureState[3] = static_cast<u32>(bindings.Material.ShadingModel);
        constants.MaterialState[0] = instance.MaterialId;
        constants.MaterialState[1] = materialErrorRow || instance.MaterialId == 0 ? 1u : 0u;
        outConstants = constants;
        return true;
    }
}
