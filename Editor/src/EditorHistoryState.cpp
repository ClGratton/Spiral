#include "EditorHistoryState.h"

#include "History/HistoryStore.h"

#include <cstddef>

// Layout tripwire. EditorHistoryStatesEqual compares component fields by hand;
// a field added to one of these types and forgotten there would make two
// different states compare equal and silently drop an edit from the history.
// On the reference toolchain (x86-64 libstdc++) a size change fails the build
// here, which is the prompt to extend the comparison below and update the
// number. Other toolchains skip the check (their std::string differs) but run
// the same comparison, and Tests/src/EditorHistoryStateTests.cpp mutates every
// field the comparison names.
#if defined(__GLIBCXX__) && defined(__x86_64__) && defined(__linux__)
static_assert(sizeof(Engine::CameraComponent) == 28, "CameraComponent changed: update EditorHistoryStatesEqual");
static_assert(sizeof(Engine::LightComponent) == 48, "LightComponent changed: update EditorHistoryStatesEqual");
static_assert(sizeof(Engine::MeshRendererComponent) == 56, "MeshRendererComponent changed: update EditorHistoryStatesEqual");
static_assert(sizeof(Engine::TransformComponent) == 72, "TransformComponent changed: update EditorHistoryStatesEqual");
static_assert(sizeof(Engine::AssetMetadata) == 120, "AssetMetadata changed: update EditorHistoryStatesEqual");
static_assert(sizeof(Engine::MaterialAsset) == 184, "MaterialAsset changed: update EditorHistoryStatesEqual");
static_assert(sizeof(Engine::MaterialTextureSet) == 48, "MaterialTextureSet changed: update EditorHistoryStatesEqual");
static_assert(sizeof(Engine::MaterialTextureSamplerSet) == 24, "MaterialTextureSamplerSet changed: update EditorHistoryStatesEqual");
static_assert(sizeof(Engine::Math::WorldGridPolicy) == 32, "WorldGridPolicy changed: update EditorHistoryStatesEqual");
#endif

namespace
{
    using EditorHistory::EstimateStringHeapBytes;

    bool SameVec3(const Engine::Math::Vec3& left, const Engine::Math::Vec3& right)
    {
        return left.X == right.X && left.Y == right.Y && left.Z == right.Z;
    }

    bool SamePosition(const Engine::Math::SectorLocalPosition& left, const Engine::Math::SectorLocalPosition& right)
    {
        return left.Sector == right.Sector && left.Local.X == right.Local.X && left.Local.Y == right.Local.Y
            && left.Local.Z == right.Local.Z;
    }

    bool SameTransform(const Engine::TransformComponent& left, const Engine::TransformComponent& right)
    {
        return SamePosition(left.GetPosition(), right.GetPosition())
            && SameVec3(left.RotationDegrees, right.RotationDegrees) && SameVec3(left.Scale, right.Scale);
    }

    bool SameCamera(const Engine::CameraComponent& left, const Engine::CameraComponent& right)
    {
        return left.Primary == right.Primary && left.Projection.VerticalFovDegrees == right.Projection.VerticalFovDegrees
            && left.Projection.NearClip == right.Projection.NearClip && left.Projection.FarClip == right.Projection.FarClip
            && SameVec3(left.BackgroundColor, right.BackgroundColor);
    }

    bool SameLight(const Engine::LightComponent& left, const Engine::LightComponent& right)
    {
        return left.Type == right.Type && SameVec3(left.Color, right.Color)
            && left.PhotometricValue == right.PhotometricValue && left.PhotometricUnit == right.PhotometricUnit
            && left.Range == right.Range && left.InnerConeDegrees == right.InnerConeDegrees
            && left.OuterConeDegrees == right.OuterConeDegrees && left.CastsShadows == right.CastsShadows;
    }

    bool SameMeshRenderer(const Engine::MeshRendererComponent& left, const Engine::MeshRendererComponent& right)
    {
        return left.MeshAsset == right.MeshAsset && left.MaterialAsset == right.MaterialAsset
            && left.MeshName == right.MeshName && left.Visible == right.Visible
            && left.CastsShadows == right.CastsShadows;
    }

    template <class Component, class Compare>
    bool SameOptional(const std::optional<Component>& left, const std::optional<Component>& right, Compare compare)
    {
        if (left.has_value() != right.has_value())
            return false;
        return !left.has_value() || compare(*left, *right);
    }

    bool SameWorldGridPolicy(const Engine::Math::WorldGridPolicy& left, const Engine::Math::WorldGridPolicy& right)
    {
        return left.Version == right.Version && left.SectorExtent == right.SectorExtent
            && left.OriginHysteresis == right.OriginHysteresis && left.OriginMode == right.OriginMode;
    }

    bool SameScene(const Engine::Scene& left, const Engine::Scene& right)
    {
        if (left.GetName() != right.GetName() || !SameWorldGridPolicy(left.GetWorldGridPolicy(), right.GetWorldGridPolicy())
            || left.GetMainCameraEntity() != right.GetMainCameraEntity())
            return false;
        const std::vector<Engine::SceneEntity>& leftEntities = left.GetEntities();
        const std::vector<Engine::SceneEntity>& rightEntities = right.GetEntities();
        if (leftEntities.size() != rightEntities.size())
            return false;
        for (size_t index = 0; index < leftEntities.size(); ++index)
        {
            const Engine::SceneEntity& a = leftEntities[index];
            const Engine::SceneEntity& b = rightEntities[index];
            if (a.EntityHandle != b.EntityHandle || a.Name != b.Name || !SameTransform(a.Transform, b.Transform)
                || !SameOptional(a.Camera, b.Camera, SameCamera) || !SameOptional(a.Light, b.Light, SameLight)
                || !SameOptional(a.MeshRenderer, b.MeshRenderer, SameMeshRenderer))
                return false;
        }
        return true;
    }

    bool SameRegistry(const Engine::AssetRegistry& left, const Engine::AssetRegistry& right)
    {
        const std::vector<Engine::AssetMetadata>& a = left.GetAssets();
        const std::vector<Engine::AssetMetadata>& b = right.GetAssets();
        if (a.size() != b.size() || left.GetCookedArtifactBasePath() != right.GetCookedArtifactBasePath())
            return false;
        for (size_t index = 0; index < a.size(); ++index)
        {
            if (a[index].Handle != b[index].Handle || a[index].Type != b[index].Type
                || a[index].SourcePath != b[index].SourcePath || a[index].Name != b[index].Name
                || a[index].SourcePolicy != b[index].SourcePolicy || a[index].CookedRoot != b[index].CookedRoot)
                return false;
        }
        return true;
    }

    bool SameMaterial(const Engine::MaterialAsset& a, const Engine::MaterialAsset& b)
    {
        using Engine::MaterialTextureSlot;
        constexpr MaterialTextureSlot slots[] = { MaterialTextureSlot::BaseColor, MaterialTextureSlot::Normal,
            MaterialTextureSlot::Orm, MaterialTextureSlot::Emissive, MaterialTextureSlot::Opacity,
            MaterialTextureSlot::CallistoControl };
        for (MaterialTextureSlot slot : slots)
        {
            if (a.GetTexture(slot) != b.GetTexture(slot) || a.GetSampler(slot) != b.GetSampler(slot))
                return false;
        }
        return a.Name == b.Name && a.ShadingModel == b.ShadingModel && a.AlphaMode == b.AlphaMode
            && a.TwoSided == b.TwoSided && SameVec3(a.BaseColor, b.BaseColor) && a.Metallic == b.Metallic
            && a.Roughness == b.Roughness && a.NormalScale == b.NormalScale
            && a.OcclusionStrength == b.OcclusionStrength && SameVec3(a.EmissiveColor, b.EmissiveColor)
            && a.EmissiveStrength == b.EmissiveStrength && a.AlphaCutoff == b.AlphaCutoff
            && a.DiffuseFresnelIntensity == b.DiffuseFresnelIntensity
            && a.RetroreflectionIntensity == b.RetroreflectionIntensity
            && a.DiffuseFresnelFalloff == b.DiffuseFresnelFalloff
            && a.RetroreflectionFalloff == b.RetroreflectionFalloff && a.SmoothTerminator == b.SmoothTerminator;
    }

    // The library has no enumeration of its own; the registry names every
    // material asset, which is the set the Editor can ever address.
    bool SameMaterials(const EditorHistoryState& first, const EditorHistoryState& second)
    {
        for (const Engine::AssetMetadata& metadata : first.AssetRegistry.GetAssets())
        {
            if (metadata.Type != Engine::AssetType::Material)
                continue;
            const Engine::MaterialAsset* a = first.MaterialLibrary.Get(metadata.Handle);
            const Engine::MaterialAsset* b = second.MaterialLibrary.Get(metadata.Handle);
            if ((a == nullptr) != (b == nullptr) || (a && !SameMaterial(*a, *b)))
                return false;
        }
        return true;
    }
}

Engine::u64 EstimateEditorHistoryStateBytes(const EditorHistoryState& state)
{
    Engine::u64 bytes = sizeof(EditorHistoryState) + EstimateStringHeapBytes(state.Scene.GetName());
    const std::vector<Engine::SceneEntity>& entities = state.Scene.GetEntities();
    bytes += static_cast<Engine::u64>(entities.capacity()) * sizeof(Engine::SceneEntity);
    for (const Engine::SceneEntity& entity : entities)
    {
        bytes += EstimateStringHeapBytes(entity.Name);
        if (entity.MeshRenderer)
            bytes += EstimateStringHeapBytes(entity.MeshRenderer->MeshName);
    }
    const std::vector<Engine::AssetMetadata>& assets = state.AssetRegistry.GetAssets();
    bytes += static_cast<Engine::u64>(assets.capacity()) * sizeof(Engine::AssetMetadata);
    for (const Engine::AssetMetadata& metadata : assets)
    {
        bytes += EstimateStringHeapBytes(metadata.SourcePath) + EstimateStringHeapBytes(metadata.Name)
            + EstimateStringHeapBytes(metadata.CookedRoot);
        if (metadata.Type == Engine::AssetType::Material)
        {
            if (const Engine::MaterialAsset* material = state.MaterialLibrary.Get(metadata.Handle))
                bytes += sizeof(Engine::MaterialAsset) + sizeof(Engine::AssetHandle)
                    + EstimateStringHeapBytes(material->Name);
        }
    }
    return bytes;
}

bool EditorHistoryStatesEqual(const EditorHistoryState& first, const EditorHistoryState& second)
{
    return first.ProjectColorPipelineSettings == second.ProjectColorPipelineSettings
        && SameScene(first.Scene, second.Scene) && SameRegistry(first.AssetRegistry, second.AssetRegistry)
        && SameMaterials(first, second) && SameMaterials(second, first);
}

bool EditorHistoryCameraEdited(const EditorHistoryState& before, const EditorHistoryState& after)
{
    const Engine::Entity cameraBefore = before.Scene.GetMainCameraEntity();
    const Engine::Entity cameraAfter = after.Scene.GetMainCameraEntity();
    if (cameraBefore != cameraAfter)
        return true;
    const Engine::SceneEntity* a = before.Scene.TryGetEntity(cameraBefore);
    const Engine::SceneEntity* b = after.Scene.TryGetEntity(cameraAfter);
    if ((a == nullptr) != (b == nullptr))
        return true;
    if (!a)
        return false;
    return !SameTransform(a->Transform, b->Transform) || !SameOptional(a->Camera, b->Camera, SameCamera);
}

EditorHistory::HistoryLabel DescribeTransformEdit(std::string_view target,
    const Engine::Math::SectorLocalPosition& beforePosition, const Engine::TransformComponent& before,
    const Engine::Math::SectorLocalPosition& afterPosition, const Engine::TransformComponent& after,
    EditorHistory::HistorySource source)
{
    return EditorHistory::MakeTransformEditLabel(target, !SamePosition(beforePosition, afterPosition),
        !SameVec3(before.RotationDegrees, after.RotationDegrees), !SameVec3(before.Scale, after.Scale), source);
}

EditorHistory::HistoryLabel DescribeLightEdit(std::string_view target, const Engine::LightComponent& before,
    const Engine::LightComponent& after, EditorHistory::HistorySource source)
{
    using EditorHistory::EditProperty;
    EditProperty only = EditProperty::None;
    int changed = 0;
    const auto note = [&](bool differs, EditProperty property)
    {
        if (differs)
        {
            only = property;
            ++changed;
        }
    };
    // The photometric unit follows the light type, so it belongs to the type group.
    note(before.Type != after.Type || before.PhotometricUnit != after.PhotometricUnit, EditProperty::LightType);
    note(!SameVec3(before.Color, after.Color), EditProperty::LightColor);
    note(before.PhotometricValue != after.PhotometricValue, EditProperty::LightPhotometricValue);
    note(before.Range != after.Range, EditProperty::LightRange);
    note(before.InnerConeDegrees != after.InnerConeDegrees, EditProperty::LightInnerCone);
    note(before.OuterConeDegrees != after.OuterConeDegrees, EditProperty::LightOuterCone);
    note(before.CastsShadows != after.CastsShadows, EditProperty::LightCastsShadows);
    if (changed == 1)
        return EditorHistory::MakeEditLabel(only, target, source);
    return EditorHistory::MakeComponentEditLabel("Light", target, source);
}

EditorHistory::HistoryLabel DescribeColorPipelineEdit(const Engine::RendererColorPipelineSettings& before,
    const Engine::RendererColorPipelineSettings& after, EditorHistory::HistorySource source)
{
    using EditorHistory::EditProperty;
    EditProperty only = EditProperty::None;
    int changed = 0;
    const auto note = [&](bool differs, EditProperty property)
    {
        if (differs)
        {
            only = property;
            ++changed;
        }
    };
    note(before.ExposureMode != after.ExposureMode, EditProperty::ColorExposureMode);
    note(before.ManualExposureEV100 != after.ManualExposureEV100, EditProperty::ColorManualExposure);
    note(before.CameraApertureFNumber != after.CameraApertureFNumber, EditProperty::ColorAperture);
    note(before.CameraShutterSeconds != after.CameraShutterSeconds, EditProperty::ColorShutter);
    note(before.CameraISO != after.CameraISO, EditProperty::ColorIso);
    note(before.PostToneMapSaturation != after.PostToneMapSaturation, EditProperty::ColorSaturation);
    note(before.PostToneMapContrast != after.PostToneMapContrast, EditProperty::ColorContrast);
    if (changed == 1)
        return EditorHistory::MakeEditLabel(only, "Project", source);
    return EditorHistory::MakeComponentEditLabel("Color Pipeline", "Project", source);
}

EditorHistory::HistoryLabel DescribeMeshRendererFlagsEdit(std::string_view target, bool beforeVisible,
    bool beforeCastsShadows, bool afterVisible, bool afterCastsShadows, EditorHistory::HistorySource source)
{
    using EditorHistory::EditProperty;
    const bool visible = beforeVisible != afterVisible;
    const bool shadows = beforeCastsShadows != afterCastsShadows;
    if (visible && !shadows)
        return EditorHistory::MakeEditLabel(EditProperty::MeshRendererVisible, target, source);
    if (shadows && !visible)
        return EditorHistory::MakeEditLabel(EditProperty::MeshRendererCastsShadows, target, source);
    return EditorHistory::MakeComponentEditLabel("Mesh Renderer", target, source);
}
