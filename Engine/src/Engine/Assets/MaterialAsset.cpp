#include "Engine/Assets/MaterialAsset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

namespace Engine
{
    namespace
    {
        constexpr int kLegacyMaterialAssetFormatVersion = 1;
        constexpr int kMaterialAssetFormatVersion = 2;

        constexpr std::array<MaterialTextureSlot, 6> kMaterialTextureSlots {{
            MaterialTextureSlot::BaseColor,
            MaterialTextureSlot::Normal,
            MaterialTextureSlot::Orm,
            MaterialTextureSlot::Emissive,
            MaterialTextureSlot::Opacity,
            MaterialTextureSlot::CallistoControl
        }};

        bool TryParseMaterialTextureSlot(std::string_view value, MaterialTextureSlot& outSlot)
        {
            const auto slot = std::find_if(kMaterialTextureSlots.begin(), kMaterialTextureSlots.end(),
                [value](MaterialTextureSlot candidate) { return value == ToString(candidate); });
            if (slot == kMaterialTextureSlots.end())
                return false;
            outSlot = *slot;
            return true;
        }

        size_t MaterialTextureSlotIndex(MaterialTextureSlot slot)
        {
            return static_cast<size_t>(std::distance(kMaterialTextureSlots.begin(),
                std::find(kMaterialTextureSlots.begin(), kMaterialTextureSlots.end(), slot)));
        }

        float ClampNonNegative(float value)
        {
            return std::max(value, 0.0f);
        }

        bool ParseBool(std::string_view value, bool& outValue)
        {
            if (value == "true")
            {
                outValue = true;
                return true;
            }
            if (value == "false")
            {
                outValue = false;
                return true;
            }

            return false;
        }
    }

    const char* ToString(MaterialShadingModel model)
    {
        switch (model)
        {
            case MaterialShadingModel::Standard: return "Standard";
            case MaterialShadingModel::Unlit: return "Unlit";
        }

        return "Standard";
    }

    const char* ToString(MaterialAlphaMode mode)
    {
        switch (mode)
        {
            case MaterialAlphaMode::Opaque: return "Opaque";
            case MaterialAlphaMode::Mask: return "Mask";
            case MaterialAlphaMode::Blend: return "Blend";
        }

        return "Opaque";
    }

    const char* ToString(MaterialTextureSlot slot)
    {
        switch (slot)
        {
            case MaterialTextureSlot::BaseColor: return "BaseColor";
            case MaterialTextureSlot::Normal: return "Normal";
            case MaterialTextureSlot::Orm: return "Orm";
            case MaterialTextureSlot::Emissive: return "Emissive";
            case MaterialTextureSlot::Opacity: return "Opacity";
            case MaterialTextureSlot::CallistoControl: return "CallistoControl";
        }

        return "BaseColor";
    }

    const char* ToString(MaterialTextureSampler sampler)
    {
        switch (sampler)
        {
            case MaterialTextureSampler::LinearWrap: return "LinearWrap";
            case MaterialTextureSampler::LinearClamp: return "LinearClamp";
            case MaterialTextureSampler::PointWrap: return "PointWrap";
            case MaterialTextureSampler::PointClamp: return "PointClamp";
        }

        return "LinearWrap";
    }

    bool TryParseMaterialTextureSampler(
        std::string_view value, MaterialTextureSampler& outSampler)
    {
        const MaterialTextureSampler samplers[] = {
            MaterialTextureSampler::LinearWrap,
            MaterialTextureSampler::LinearClamp,
            MaterialTextureSampler::PointWrap,
            MaterialTextureSampler::PointClamp
        };
        const auto sampler = std::find_if(std::begin(samplers), std::end(samplers),
            [value](MaterialTextureSampler candidate) { return value == ToString(candidate); });
        if (sampler == std::end(samplers))
            return false;
        outSampler = *sampler;
        return true;
    }

    MaterialShadingModel ParseMaterialShadingModel(std::string_view value)
    {
        return value == "Unlit" ? MaterialShadingModel::Unlit : MaterialShadingModel::Standard;
    }

    MaterialAlphaMode ParseMaterialAlphaMode(std::string_view value)
    {
        if (value == "Mask")
            return MaterialAlphaMode::Mask;
        if (value == "Blend")
            return MaterialAlphaMode::Blend;

        return MaterialAlphaMode::Opaque;
    }

    bool IsValidMaterialSurface(const MaterialSurface& surface)
    {
        const float values[] {
            surface.BaseColor.X, surface.BaseColor.Y, surface.BaseColor.Z,
            surface.Metallic, surface.Roughness
        };
        return std::all_of(std::begin(values), std::end(values), [](float value)
        {
            return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
        });
    }

    MaterialSurface GetMaterialSurface(const MaterialAsset& material)
    {
        return { material.BaseColor, material.Metallic, material.Roughness };
    }

    bool TrySetMaterialSurface(MaterialAsset& material, const MaterialSurface& surface)
    {
        if (!IsValidMaterialSurface(surface))
            return false;
        material.BaseColor = surface.BaseColor;
        material.Metallic = surface.Metallic;
        material.Roughness = surface.Roughness;
        return true;
    }

    bool IsValidMaterialAssetValues(const MaterialAsset& material)
    {
        const auto finiteRange = [](float value, float minimum, float maximum)
        {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        };
        const auto finiteNonNegative = [](float value)
        {
            return std::isfinite(value) && value >= 0.0f;
        };
        const auto samplerValid = [](MaterialTextureSampler sampler)
        {
            return sampler == MaterialTextureSampler::LinearWrap
                || sampler == MaterialTextureSampler::LinearClamp
                || sampler == MaterialTextureSampler::PointWrap
                || sampler == MaterialTextureSampler::PointClamp;
        };

        const bool enumsValid = (material.ShadingModel == MaterialShadingModel::Standard
                || material.ShadingModel == MaterialShadingModel::Unlit)
            && (material.AlphaMode == MaterialAlphaMode::Opaque
                || material.AlphaMode == MaterialAlphaMode::Mask
                || material.AlphaMode == MaterialAlphaMode::Blend)
            && samplerValid(material.Samplers.BaseColor)
            && samplerValid(material.Samplers.Normal)
            && samplerValid(material.Samplers.Orm)
            && samplerValid(material.Samplers.Emissive)
            && samplerValid(material.Samplers.Opacity)
            && samplerValid(material.Samplers.CallistoControl);
        const bool emissiveValid = finiteNonNegative(material.EmissiveColor.X)
            && finiteNonNegative(material.EmissiveColor.Y)
            && finiteNonNegative(material.EmissiveColor.Z)
            && finiteNonNegative(material.EmissiveStrength)
            && std::isfinite(material.EmissiveColor.X * material.EmissiveStrength)
            && std::isfinite(material.EmissiveColor.Y * material.EmissiveStrength)
            && std::isfinite(material.EmissiveColor.Z * material.EmissiveStrength);
        return enumsValid && IsValidMaterialSurface(GetMaterialSurface(material))
            && finiteRange(material.NormalScale, 0.0f, 4.0f)
            && finiteRange(material.OcclusionStrength, 0.0f, 1.0f)
            && emissiveValid
            && finiteRange(material.AlphaCutoff, 0.0f, 1.0f)
            && finiteRange(material.DiffuseFresnelIntensity, 0.0f, 256.0f)
            && finiteRange(material.RetroreflectionIntensity, 0.0f, 256.0f)
            && finiteRange(material.DiffuseFresnelFalloff, 0.0f, 1.0f)
            && finiteRange(material.RetroreflectionFalloff, 0.0f, 1.0f)
            && finiteRange(material.SmoothTerminator, -1.0f, 1.0f);
    }

    AssetHandle& MaterialAsset::GetTexture(MaterialTextureSlot slot)
    {
        switch (slot)
        {
            case MaterialTextureSlot::BaseColor: return Textures.BaseColor;
            case MaterialTextureSlot::Normal: return Textures.Normal;
            case MaterialTextureSlot::Orm: return Textures.Orm;
            case MaterialTextureSlot::Emissive: return Textures.Emissive;
            case MaterialTextureSlot::Opacity: return Textures.Opacity;
            case MaterialTextureSlot::CallistoControl: return Textures.CallistoControl;
        }

        return Textures.BaseColor;
    }

    const AssetHandle& MaterialAsset::GetTexture(MaterialTextureSlot slot) const
    {
        switch (slot)
        {
            case MaterialTextureSlot::BaseColor: return Textures.BaseColor;
            case MaterialTextureSlot::Normal: return Textures.Normal;
            case MaterialTextureSlot::Orm: return Textures.Orm;
            case MaterialTextureSlot::Emissive: return Textures.Emissive;
            case MaterialTextureSlot::Opacity: return Textures.Opacity;
            case MaterialTextureSlot::CallistoControl: return Textures.CallistoControl;
        }

        return Textures.BaseColor;
    }

    MaterialTextureSampler& MaterialAsset::GetSampler(MaterialTextureSlot slot)
    {
        switch (slot)
        {
            case MaterialTextureSlot::BaseColor: return Samplers.BaseColor;
            case MaterialTextureSlot::Normal: return Samplers.Normal;
            case MaterialTextureSlot::Orm: return Samplers.Orm;
            case MaterialTextureSlot::Emissive: return Samplers.Emissive;
            case MaterialTextureSlot::Opacity: return Samplers.Opacity;
            case MaterialTextureSlot::CallistoControl: return Samplers.CallistoControl;
        }

        return Samplers.BaseColor;
    }

    const MaterialTextureSampler& MaterialAsset::GetSampler(MaterialTextureSlot slot) const
    {
        switch (slot)
        {
            case MaterialTextureSlot::BaseColor: return Samplers.BaseColor;
            case MaterialTextureSlot::Normal: return Samplers.Normal;
            case MaterialTextureSlot::Orm: return Samplers.Orm;
            case MaterialTextureSlot::Emissive: return Samplers.Emissive;
            case MaterialTextureSlot::Opacity: return Samplers.Opacity;
            case MaterialTextureSlot::CallistoControl: return Samplers.CallistoControl;
        }

        return Samplers.BaseColor;
    }

    void MaterialAsset::ClampValues()
    {
        BaseColor.X = ClampNonNegative(BaseColor.X);
        BaseColor.Y = ClampNonNegative(BaseColor.Y);
        BaseColor.Z = ClampNonNegative(BaseColor.Z);
        Metallic = std::clamp(Metallic, 0.0f, 1.0f);
        Roughness = std::clamp(Roughness, 0.0f, 1.0f);
        NormalScale = std::clamp(NormalScale, 0.0f, 4.0f);
        OcclusionStrength = std::clamp(OcclusionStrength, 0.0f, 1.0f);
        EmissiveColor.X = ClampNonNegative(EmissiveColor.X);
        EmissiveColor.Y = ClampNonNegative(EmissiveColor.Y);
        EmissiveColor.Z = ClampNonNegative(EmissiveColor.Z);
        EmissiveStrength = ClampNonNegative(EmissiveStrength);
        AlphaCutoff = std::clamp(AlphaCutoff, 0.0f, 1.0f);
        DiffuseFresnelIntensity = std::clamp(DiffuseFresnelIntensity, 0.0f, 256.0f);
        RetroreflectionIntensity = std::clamp(RetroreflectionIntensity, 0.0f, 256.0f);
        DiffuseFresnelFalloff = std::clamp(DiffuseFresnelFalloff, 0.0f, 1.0f);
        RetroreflectionFalloff = std::clamp(RetroreflectionFalloff, 0.0f, 1.0f);
        SmoothTerminator = std::clamp(SmoothTerminator, -1.0f, 1.0f);
    }

    bool MaterialAsset::SaveToFile(const std::filesystem::path& path) const
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);
        if (error)
            return false;

        MaterialAsset material = *this;
        material.ClampValues();

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output)
            return false;

        output << "SpiralMaterial " << kMaterialAssetFormatVersion << '\n';
        output << "Name " << std::quoted(material.Name) << '\n';
        output << "ShadingModel " << ToString(material.ShadingModel) << '\n';
        output << "AlphaMode " << ToString(material.AlphaMode) << '\n';
        output << "TwoSided " << (material.TwoSided ? "true" : "false") << '\n';
        output << "BaseColor " << material.BaseColor.X << ' ' << material.BaseColor.Y << ' ' << material.BaseColor.Z << '\n';
        output << "Metallic " << material.Metallic << '\n';
        output << "Roughness " << material.Roughness << '\n';
        output << "NormalScale " << material.NormalScale << '\n';
        output << "OcclusionStrength " << material.OcclusionStrength << '\n';
        output << "EmissiveColor " << material.EmissiveColor.X << ' ' << material.EmissiveColor.Y << ' ' << material.EmissiveColor.Z << '\n';
        output << "EmissiveStrength " << material.EmissiveStrength << '\n';
        output << "AlphaCutoff " << material.AlphaCutoff << '\n';
        output << "DiffuseFresnelIntensity " << material.DiffuseFresnelIntensity << '\n';
        output << "RetroreflectionIntensity " << material.RetroreflectionIntensity << '\n';
        output << "DiffuseFresnelFalloff " << material.DiffuseFresnelFalloff << '\n';
        output << "RetroreflectionFalloff " << material.RetroreflectionFalloff << '\n';
        output << "SmoothTerminator " << material.SmoothTerminator << '\n';

        for (MaterialTextureSlot slot : kMaterialTextureSlots)
            output << "Texture " << ToString(slot) << ' ' << material.GetTexture(slot) << '\n';
        for (MaterialTextureSlot slot : kMaterialTextureSlots)
            output << "Sampler " << ToString(slot) << ' ' << ToString(material.GetSampler(slot)) << '\n';

        return static_cast<bool>(output);
    }

    bool MaterialAsset::LoadFromFile(const std::filesystem::path& path, MaterialAsset& outMaterial)
    {
        std::ifstream input(path);
        if (!input)
            return false;

        std::string magic;
        int version = 0;
        if (!(input >> magic >> version) || magic != "SpiralMaterial"
            || (version != kLegacyMaterialAssetFormatVersion
                && version != kMaterialAssetFormatVersion))
            return false;

        MaterialAsset loaded;
        std::array<bool, kMaterialTextureSlots.size()> samplerSeen {};
        std::string line;
        std::getline(input, line);
        while (std::getline(input, line))
        {
            if (line.empty())
                continue;

            std::istringstream stream(line);
            std::string key;
            stream >> key;
            if (key == "Name")
            {
                if (!(stream >> std::quoted(loaded.Name)))
                    return false;
            }
            else if (key == "ShadingModel")
            {
                std::string value;
                if (!(stream >> value) || (value != "Standard" && value != "Unlit"))
                    return false;
                loaded.ShadingModel = ParseMaterialShadingModel(value);
            }
            else if (key == "AlphaMode")
            {
                std::string value;
                if (!(stream >> value) || (value != "Opaque" && value != "Mask" && value != "Blend"))
                    return false;
                loaded.AlphaMode = ParseMaterialAlphaMode(value);
            }
            else if (key == "TwoSided")
            {
                std::string value;
                if (!(stream >> value) || !ParseBool(value, loaded.TwoSided))
                    return false;
            }
            else if (key == "BaseColor")
            {
                if (!(stream >> loaded.BaseColor.X >> loaded.BaseColor.Y >> loaded.BaseColor.Z))
                    return false;
            }
            else if (key == "Metallic")
            {
                if (!(stream >> loaded.Metallic))
                    return false;
            }
            else if (key == "Roughness")
            {
                if (!(stream >> loaded.Roughness))
                    return false;
            }
            else if (key == "NormalScale")
            {
                if (!(stream >> loaded.NormalScale))
                    return false;
            }
            else if (key == "OcclusionStrength")
            {
                if (!(stream >> loaded.OcclusionStrength))
                    return false;
            }
            else if (key == "EmissiveColor")
            {
                if (!(stream >> loaded.EmissiveColor.X >> loaded.EmissiveColor.Y >> loaded.EmissiveColor.Z))
                    return false;
            }
            else if (key == "EmissiveStrength")
            {
                if (!(stream >> loaded.EmissiveStrength))
                    return false;
            }
            else if (key == "AlphaCutoff")
            {
                if (!(stream >> loaded.AlphaCutoff))
                    return false;
            }
            else if (key == "DiffuseFresnelIntensity")
            {
                if (!(stream >> loaded.DiffuseFresnelIntensity))
                    return false;
            }
            else if (key == "RetroreflectionIntensity")
            {
                if (!(stream >> loaded.RetroreflectionIntensity))
                    return false;
            }
            else if (key == "DiffuseFresnelFalloff")
            {
                if (!(stream >> loaded.DiffuseFresnelFalloff))
                    return false;
            }
            else if (key == "RetroreflectionFalloff")
            {
                if (!(stream >> loaded.RetroreflectionFalloff))
                    return false;
            }
            else if (key == "SmoothTerminator")
            {
                if (!(stream >> loaded.SmoothTerminator))
                    return false;
            }
            else if (key == "Texture")
            {
                std::string slotName;
                AssetHandle handle = kInvalidAssetHandle;
                if (!(stream >> slotName >> handle))
                    return false;

                MaterialTextureSlot slot;
                if (!TryParseMaterialTextureSlot(slotName, slot))
                    return false;

                loaded.GetTexture(slot) = handle;
            }
            else if (key == "Sampler")
            {
                if (version != kMaterialAssetFormatVersion)
                    return false;
                std::string slotName;
                std::string samplerName;
                MaterialTextureSlot slot;
                MaterialTextureSampler sampler;
                if (!(stream >> slotName >> samplerName)
                    || !TryParseMaterialTextureSlot(slotName, slot)
                    || !TryParseMaterialTextureSampler(samplerName, sampler))
                    return false;
                const size_t slotIndex = MaterialTextureSlotIndex(slot);
                if (slotIndex >= samplerSeen.size() || samplerSeen[slotIndex])
                    return false;
                samplerSeen[slotIndex] = true;
                loaded.GetSampler(slot) = sampler;
            }
            else
            {
                return false;
            }
        }

        if (version == kMaterialAssetFormatVersion
            && !std::all_of(samplerSeen.begin(), samplerSeen.end(), [](bool seen) { return seen; }))
            return false;
        loaded.ClampValues();
        outMaterial = std::move(loaded);
        return true;
    }

    bool MaterialLibrary::Set(AssetHandle handle, MaterialAsset material)
    {
        if (handle == kInvalidAssetHandle)
            return false;

        material.ClampValues();
        if (MaterialAsset* existing = Get(handle))
        {
            *existing = std::move(material);
            return true;
        }

        m_Entries.push_back({ handle, std::move(material) });
        return true;
    }

    MaterialAsset* MaterialLibrary::Get(AssetHandle handle)
    {
        const auto it = std::find_if(m_Entries.begin(), m_Entries.end(), [handle](const Entry& entry)
        {
            return entry.Handle == handle;
        });

        return it == m_Entries.end() ? nullptr : &it->Material;
    }

    const MaterialAsset* MaterialLibrary::Get(AssetHandle handle) const
    {
        return const_cast<MaterialLibrary*>(this)->Get(handle);
    }

    bool MaterialLibrary::Save(AssetHandle handle, const std::filesystem::path& path) const
    {
        const MaterialAsset* material = Get(handle);
        return material && material->SaveToFile(path);
    }

    bool MaterialLibrary::Load(AssetHandle handle, const std::filesystem::path& path)
    {
        MaterialAsset material;
        if (!MaterialAsset::LoadFromFile(path, material))
            return false;

        return Set(handle, std::move(material));
    }
}
