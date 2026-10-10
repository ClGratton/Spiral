#include "HistoryNaming.h"

#include <array>
#include <initializer_list>

namespace EditorHistory
{
    namespace
    {
        constexpr EditPropertyInfo kInfo[] = {
            /* None */ { "", "" },
            /* EntityName */ { "", "" },
            /* TransformPosition */ { "", "" },
            /* TransformRotation */ { "", "" },
            /* TransformScale */ { "", "" },
            /* CameraPrimary */ { "Camera", "Primary" },
            /* CameraVerticalFov */ { "Camera", "Vertical FOV" },
            /* CameraNearClip */ { "Camera", "Near Clip" },
            /* CameraFarClip */ { "Camera", "Far Clip" },
            /* CameraBackgroundColor */ { "Camera", "Background Color" },
            /* LightType */ { "Light", "Type" },
            /* LightColor */ { "Light", "Color" },
            /* LightPhotometricValue */ { "Light", "Intensity" },
            /* LightRange */ { "Light", "Range" },
            /* LightInnerCone */ { "Light", "Inner Cone" },
            /* LightOuterCone */ { "Light", "Outer Cone" },
            /* LightCastsShadows */ { "Light", "Casts Shadows" },
            /* MeshRendererMeshName */ { "Mesh Renderer", "Mesh Name" },
            /* MeshRendererMeshAsset */ { "Mesh Renderer", "Mesh Asset" },
            /* MeshRendererMaterialAsset */ { "Mesh Renderer", "Material Asset" },
            /* MeshRendererVisible */ { "Mesh Renderer", "Visible" },
            /* MeshRendererCastsShadows */ { "Mesh Renderer", "Casts Shadows" },
            /* MaterialName */ { "Material", "Name" },
            /* MaterialShadingModel */ { "Material", "Shading Model" },
            /* MaterialAlphaMode */ { "Material", "Alpha Mode" },
            /* MaterialTwoSided */ { "Material", "Two Sided" },
            /* MaterialBaseColor */ { "Material", "Base Color" },
            /* MaterialMetallic */ { "Material", "Metallic" },
            /* MaterialRoughness */ { "Material", "Roughness" },
            /* MaterialNormalScale */ { "Material", "Normal Scale" },
            /* MaterialOcclusionStrength */ { "Material", "Occlusion Strength" },
            /* MaterialEmissiveColor */ { "Material", "Emissive Color" },
            /* MaterialEmissiveStrength */ { "Material", "Emissive Strength" },
            /* MaterialAlphaCutoff */ { "Material", "Alpha Cutoff" },
            /* MaterialTextureBaseColor */ { "Material", "Base Color Texture" },
            /* MaterialTextureNormal */ { "Material", "Normal Texture" },
            /* MaterialTextureOrm */ { "Material", "Orm Texture" },
            /* MaterialTextureEmissive */ { "Material", "Emissive Texture" },
            /* MaterialTextureOpacity */ { "Material", "Opacity Texture" },
            /* MaterialTextureCallistoControl */ { "Material", "Callisto Control Texture" },
            /* MaterialSamplerBaseColor */ { "Material", "Base Color Sampling" },
            /* MaterialSamplerNormal */ { "Material", "Normal Sampling" },
            /* MaterialSamplerOrm */ { "Material", "Orm Sampling" },
            /* MaterialSamplerEmissive */ { "Material", "Emissive Sampling" },
            /* MaterialSamplerOpacity */ { "Material", "Opacity Sampling" },
            /* MaterialSamplerCallistoControl */ { "Material", "Callisto Control Sampling" },
            /* MaterialDiffuseFresnel */ { "Material", "Diffuse Fresnel" },
            /* MaterialRetroreflection */ { "Material", "Retroreflection" },
            /* MaterialDiffuseFalloff */ { "Material", "Diffuse Falloff" },
            /* MaterialRetroreflectionFalloff */ { "Material", "Retroreflection Falloff" },
            /* MaterialSmoothTerminator */ { "Material", "Smooth Terminator" },
            /* ColorExposureMode */ { "Color Pipeline", "Exposure Mode" },
            /* ColorManualExposure */ { "Color Pipeline", "Manual EV100" },
            /* ColorAperture */ { "Color Pipeline", "Aperture" },
            /* ColorShutter */ { "Color Pipeline", "Shutter" },
            /* ColorIso */ { "Color Pipeline", "ISO" },
            /* ColorSaturation */ { "Color Pipeline", "Saturation" },
            /* ColorContrast */ { "Color Pipeline", "Contrast" },
        };
        static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == static_cast<size_t>(EditProperty::Count),
            "every EditProperty needs a label row");

        std::string Join(std::initializer_list<std::string_view> parts)
        {
            std::string text;
            for (std::string_view part : parts)
                text += part;
            return text;
        }

        std::string DirectionReason(std::string_view direction, std::string_view detail)
        {
            return Join({ direction, " unavailable: ", detail });
        }
    }

    EditPropertyInfo DescribeEditProperty(EditProperty property)
    {
        const size_t index = static_cast<size_t>(property);
        if (index >= static_cast<size_t>(EditProperty::Count))
            return {};
        return kInfo[index];
    }

    HistoryLabel MakeEditLabel(EditProperty property, std::string_view target, HistorySource source)
    {
        switch (property)
        {
        case EditProperty::EntityName: return MakeHistoryLabel("Rename", target, source);
        case EditProperty::TransformPosition: return MakeHistoryLabel("Move", target, source);
        case EditProperty::TransformRotation: return MakeHistoryLabel("Rotate", target, source);
        case EditProperty::TransformScale: return MakeHistoryLabel("Scale", target, source);
        default: break;
        }
        const EditPropertyInfo info = DescribeEditProperty(property);
        if (info.Component[0] == '\0')
            return MakeHistoryLabel("Edit", target, source);
        return MakeHistoryLabel(Join({ "Edit ", info.Component, ".", info.Property, " of" }), target, source);
    }

    HistoryLabel MakeTransformEditLabel(std::string_view target, bool moved, bool rotated, bool scaled, HistorySource source)
    {
        const int changed = (moved ? 1 : 0) + (rotated ? 1 : 0) + (scaled ? 1 : 0);
        if (changed == 1)
        {
            return MakeEditLabel(moved ? EditProperty::TransformPosition
                                       : (rotated ? EditProperty::TransformRotation : EditProperty::TransformScale),
                target, source);
        }
        return MakeHistoryLabel("Edit Transform of", target, source);
    }

    HistoryLabel MakeComponentEditLabel(std::string_view component, std::string_view target, HistorySource source)
    {
        return MakeHistoryLabel(Join({ "Edit ", component, " of" }), target, source);
    }

    HistoryLabel MakeAddComponentLabel(std::string_view component, std::string_view target, HistorySource source)
    {
        return MakeHistoryLabel(Join({ "Add ", component, " to" }), target, source);
    }

    HistoryLabel MakeCreateLabel(std::string_view name, HistorySource source)
    {
        return MakeHistoryLabel("Create", name, source);
    }

    HistoryLabel MakeDeleteLabel(std::string_view name, HistorySource source)
    {
        return MakeHistoryLabel("Delete", name, source);
    }

    HistoryLabel MakePlaceLabel(std::string_view assetName, HistorySource source)
    {
        return MakeHistoryLabel("Place", assetName, source);
    }

    HistoryCommandText DescribeUndoCommand(const HistoryAvailability& availability, const HistoryEntryInfo* topEntry,
        std::string_view baseReason)
    {
        HistoryCommandText text;
        if (availability.Enabled && topEntry)
        {
            text.Enabled = true;
            text.Label = "Undo: " + topEntry->Label.Display();
            return text;
        }
        text.Label = availability.MenuLabel.empty() ? "Undo" : availability.MenuLabel;
        switch (availability.Block)
        {
        case HistoryBlock::UndoBarrier:
            text.Reason = DirectionReason("Undo", baseReason);
            break;
        case HistoryBlock::GestureOpen:
        case HistoryBlock::TransactionOpen:
            text.Reason = DirectionReason("Undo", "finish the current edit");
            break;
        case HistoryBlock::NothingToUndo:
            text.Reason = baseReason.empty() ? DirectionReason("Undo", "nothing to undo") : DirectionReason("Undo", baseReason);
            break;
        default:
            text.Reason = DirectionReason("Undo", "nothing to undo");
            break;
        }
        return text;
    }

    HistoryCommandText DescribeRedoCommand(const HistoryAvailability& availability, const HistoryEntryInfo* topEntry)
    {
        HistoryCommandText text;
        if (availability.Enabled && topEntry)
        {
            text.Enabled = true;
            text.Label = "Redo: " + topEntry->Label.Display();
            return text;
        }
        text.Label = availability.MenuLabel.empty() ? "Redo" : availability.MenuLabel;
        if (availability.Block == HistoryBlock::GestureOpen || availability.Block == HistoryBlock::TransactionOpen)
            text.Reason = DirectionReason("Redo", "finish the current edit");
        else
            text.Reason = DirectionReason("Redo", "nothing to redo");
        return text;
    }
}
