#pragma once

#include "HistoryLabel.h"
#include "HistoryStore.h"

#include "Engine/Core/Base.h"

#include <string>
#include <string_view>

namespace EditorHistory
{
    // One id per editable property the Inspector, the project settings and the
    // typed controls can change. The value doubles as EditGestureKey::Property,
    // so two widgets never share a gesture, and selects the label wording.
    enum class EditProperty : Engine::u32
    {
        None = 0,
        EntityName,
        TransformPosition,
        TransformRotation,
        TransformScale,
        CameraPrimary,
        CameraVerticalFov,
        CameraNearClip,
        CameraFarClip,
        CameraBackgroundColor,
        LightType,
        LightColor,
        LightPhotometricValue,
        LightRange,
        LightInnerCone,
        LightOuterCone,
        LightCastsShadows,
        MeshRendererMeshName,
        MeshRendererMeshAsset,
        MeshRendererMaterialAsset,
        MeshRendererVisible,
        MeshRendererCastsShadows,
        MaterialName,
        MaterialShadingModel,
        MaterialAlphaMode,
        MaterialTwoSided,
        MaterialBaseColor,
        MaterialMetallic,
        MaterialRoughness,
        MaterialNormalScale,
        MaterialOcclusionStrength,
        MaterialEmissiveColor,
        MaterialEmissiveStrength,
        MaterialAlphaCutoff,
        // Six texture slots in MaterialTextureSlot order, then their samplers.
        MaterialTextureBaseColor,
        MaterialTextureNormal,
        MaterialTextureOrm,
        MaterialTextureEmissive,
        MaterialTextureOpacity,
        MaterialTextureCallistoControl,
        MaterialSamplerBaseColor,
        MaterialSamplerNormal,
        MaterialSamplerOrm,
        MaterialSamplerEmissive,
        MaterialSamplerOpacity,
        MaterialSamplerCallistoControl,
        MaterialDiffuseFresnel,
        MaterialRetroreflection,
        MaterialDiffuseFalloff,
        MaterialRetroreflectionFalloff,
        MaterialSmoothTerminator,
        ColorExposureMode,
        ColorManualExposure,
        ColorAperture,
        ColorShutter,
        ColorIso,
        ColorSaturation,
        ColorContrast,
        Count
    };

    constexpr Engine::u32 ToPropertyId(EditProperty property)
    {
        return static_cast<Engine::u32>(property);
    }

    // The component and the readable property label an edit is named after, for
    // example { "Light", "Casts Shadows" }. Transform properties and the entity
    // name have no component wording (they use the verbs Move, Rotate, Scale and
    // Rename); None and Count return empty strings.
    struct EditPropertyInfo
    {
        const char* Component = "";
        const char* Property = "";
    };

    EditPropertyInfo DescribeEditProperty(EditProperty property);

    // Names an edit of one property: Rename / Move / Rotate / Scale for the
    // entity name and the three transform controls, otherwise
    // "Edit <Component>.<Property> of" with the target after it, so the
    // display reads "Edit Light.Color of Sun". Target is the entity name (the
    // material name for a material property, "Project" for the colour pipeline).
    HistoryLabel MakeEditLabel(EditProperty property, std::string_view target, HistorySource source = HistorySource::User);

    // A transform edit that changed more than one part, or a typed pose edit:
    // "Move", "Rotate" or "Scale" when exactly one part changed, otherwise
    // "Edit Transform of". Nothing changed yields "Edit Transform of".
    HistoryLabel MakeTransformEditLabel(std::string_view target, bool moved, bool rotated, bool scaled,
        HistorySource source = HistorySource::User);

    // "Edit <Component> of <target>" for an edit that changed several
    // properties of one component (a typed light or mesh-renderer patch).
    HistoryLabel MakeComponentEditLabel(std::string_view component, std::string_view target,
        HistorySource source = HistorySource::User);

    // "Add <component> to <target>".
    HistoryLabel MakeAddComponentLabel(std::string_view component, std::string_view target,
        HistorySource source = HistorySource::User);

    // "Create <name>", "Delete <name>", "Place <asset>".
    HistoryLabel MakeCreateLabel(std::string_view name, HistorySource source = HistorySource::User);
    HistoryLabel MakeDeleteLabel(std::string_view name, HistorySource source = HistorySource::User);
    HistoryLabel MakePlaceLabel(std::string_view assetName, HistorySource source = HistorySource::User);

    // The Edit menu text for one direction. Label is what the menu item shows
    // ("Undo: Move Cube", or "Undo (nothing to undo)" when unavailable); Reason
    // is the sentence for the tooltip and the status text, empty when the
    // command is available ("Undo unavailable: Fab import changed the project").
    struct HistoryCommandText
    {
        bool Enabled = false;
        std::string Label;
        std::string Reason;
    };

    // topEntry is the entry the command would undo (TopUndo) or redo (TopRedo),
    // null when none; baseReason is HistoryStore::BaseReason().
    HistoryCommandText DescribeUndoCommand(const HistoryAvailability& availability, const HistoryEntryInfo* topEntry,
        std::string_view baseReason);
    HistoryCommandText DescribeRedoCommand(const HistoryAvailability& availability, const HistoryEntryInfo* topEntry);
}
