#include "Engine/Renderer/SceneSurfaceConstants.h"

#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/SceneShadowMap.h"
#include "Engine/Renderer/TextureRuntimePublication.h"

#include <cmath>
#include <cstring>
#include <limits>

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
            || !IsValidMaterialAssetValues(bindings.Material))
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

    bool TryApplySceneShadowMapConstants(const SceneRasterInstance& instance,
        const SceneShadowMapFrame& shadow, SceneShadowCasterMode casterMode,
        SceneSurfaceConstants& inOutConstants)
    {
        if (shadow.Resolution == 0
            || (shadow.Enabled
                && (shadow.PrimaryLightIndex == std::numeric_limits<u32>::max()
                    || !(shadow.WorldUnitsPerTexel > 0.0f)
                    || !(shadow.ConstantDepthBias > 0.0f)
                    || !(shadow.SlopeDepthBias >= shadow.ConstantDepthBias))))
            return false;
        const Math::Mat4 modelToShadow = shadow.Enabled
            ? Math::Multiply(instance.CameraRelativeModel,
                shadow.CameraRelativeToShadowClip)
            : Math::Mat4::Identity();
        for (float value : modelToShadow.Values)
            if (!std::isfinite(value))
                return false;
        SceneSurfaceConstants candidate = inOutConstants;
        std::memcpy(candidate.ShadowViewProjection, modelToShadow.Values,
            sizeof(candidate.ShadowViewProjection));
        candidate.ShadowParameters[0] = 1.0f / static_cast<float>(shadow.Resolution);
        candidate.ShadowParameters[1] = shadow.Enabled ? shadow.ConstantDepthBias : 0.0f;
        candidate.ShadowParameters[2] = shadow.Enabled ? shadow.SlopeDepthBias : 0.0f;
        candidate.ShadowParameters[3] = shadow.Enabled ? shadow.WorldUnitsPerTexel : 0.0f;
        candidate.ShadowState[0] = shadow.Enabled ? 1u : 0u;
        candidate.ShadowState[1] = shadow.Enabled
            ? shadow.PrimaryLightIndex : std::numeric_limits<u32>::max();
        candidate.ShadowState[2] = shadow.Resolution;
        candidate.ShadowState[3] = static_cast<u32>(casterMode);
        if (!IsFiniteMatrix(candidate.ShadowViewProjection)
            || !std::isfinite(candidate.ShadowParameters[0])
            || !std::isfinite(candidate.ShadowParameters[1])
            || !std::isfinite(candidate.ShadowParameters[2])
            || !std::isfinite(candidate.ShadowParameters[3]))
            return false;
        inOutConstants = candidate;
        return true;
    }
}
