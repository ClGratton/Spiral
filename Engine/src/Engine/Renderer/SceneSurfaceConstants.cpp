#include "Engine/Renderer/SceneSurfaceConstants.h"

#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/TextureRuntimePublication.h"

#include <cstring>

namespace Engine
{
    SceneSurfaceConstants BuildSceneSurfaceConstants(
        const SceneRasterInstance& instance,
        const MaterialTextureBindingSet& bindings,
        bool materialErrorRow)
    {
        SceneSurfaceConstants constants;
        std::memcpy(constants.ViewProjection, instance.ModelViewProjection.Values, sizeof(constants.ViewProjection));
        std::memcpy(constants.NormalTransform, instance.NormalTransform.Values, sizeof(constants.NormalTransform));
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
        constants.MaterialState[1] = materialErrorRow ? 1u : 0u;
        return constants;
    }
}
