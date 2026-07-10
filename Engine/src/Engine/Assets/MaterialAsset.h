#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Math/Math.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    enum class MaterialShadingModel
    {
        Standard,
        Unlit
    };

    enum class MaterialAlphaMode
    {
        Opaque,
        Mask,
        Blend
    };

    enum class MaterialTextureSlot
    {
        BaseColor,
        Normal,
        Orm,
        Emissive,
        Opacity,
        CallistoControl
    };

    const char* ToString(MaterialShadingModel model);
    const char* ToString(MaterialAlphaMode mode);
    const char* ToString(MaterialTextureSlot slot);
    MaterialShadingModel ParseMaterialShadingModel(std::string_view value);
    MaterialAlphaMode ParseMaterialAlphaMode(std::string_view value);

    struct MaterialTextureSet
    {
        AssetHandle BaseColor = kInvalidAssetHandle;
        AssetHandle Normal = kInvalidAssetHandle;
        AssetHandle Orm = kInvalidAssetHandle;
        AssetHandle Emissive = kInvalidAssetHandle;
        AssetHandle Opacity = kInvalidAssetHandle;
        AssetHandle CallistoControl = kInvalidAssetHandle;
    };

    struct MaterialAsset
    {
        std::string Name = "Material";
        MaterialShadingModel ShadingModel = MaterialShadingModel::Standard;
        MaterialAlphaMode AlphaMode = MaterialAlphaMode::Opaque;
        bool TwoSided = false;

        Math::Vec3 BaseColor = { 1.0f, 1.0f, 1.0f };
        float Metallic = 0.0f;
        float Roughness = 0.5f;
        float NormalScale = 1.0f;
        float OcclusionStrength = 1.0f;
        Math::Vec3 EmissiveColor = { 0.0f, 0.0f, 0.0f };
        float EmissiveStrength = 0.0f;
        float AlphaCutoff = 0.5f;

        float DiffuseFresnelIntensity = 1.0f;
        float RetroreflectionIntensity = 1.0f;
        float DiffuseFresnelFalloff = 0.75f;
        float RetroreflectionFalloff = 0.75f;
        float SmoothTerminator = 0.0f;

        MaterialTextureSet Textures;

        AssetHandle& GetTexture(MaterialTextureSlot slot);
        const AssetHandle& GetTexture(MaterialTextureSlot slot) const;
        void ClampValues();

        bool SaveToFile(const std::filesystem::path& path) const;
        static bool LoadFromFile(const std::filesystem::path& path, MaterialAsset& outMaterial);
    };

    class MaterialLibrary
    {
    public:
        bool Set(AssetHandle handle, MaterialAsset material);
        MaterialAsset* Get(AssetHandle handle);
        const MaterialAsset* Get(AssetHandle handle) const;
        bool Save(AssetHandle handle, const std::filesystem::path& path) const;
        bool Load(AssetHandle handle, const std::filesystem::path& path);

    private:
        struct Entry
        {
            AssetHandle Handle = kInvalidAssetHandle;
            MaterialAsset Material;
        };

        std::vector<Entry> m_Entries;
    };
}
