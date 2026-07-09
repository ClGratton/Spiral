#pragma once

#include "Engine/Assets/AssetHandle.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    enum class AssetType
    {
        Unknown,
        Mesh,
        Material,
        Texture,
        Scene,
        Shader,
        Script,
        Audio
    };

    const char* ToString(AssetType type);
    AssetType ParseAssetType(std::string_view value);

    struct AssetMetadata
    {
        AssetHandle Handle = kInvalidAssetHandle;
        AssetType Type = AssetType::Unknown;
        std::string SourcePath;
        std::string Name;
    };

    class AssetRegistry
    {
    public:
        AssetHandle RegisterAsset(AssetType type, std::string sourcePath, std::string name = {});
        bool RegisterAsset(const AssetMetadata& metadata);
        bool Contains(AssetHandle handle) const;
        const AssetMetadata* GetAsset(AssetHandle handle) const;
        AssetHandle FindAssetByPath(AssetType type, std::string_view sourcePath) const;
        const std::vector<AssetMetadata>& GetAssets() const { return m_Assets; }
        void Clear();

        bool SaveToFile(const std::filesystem::path& path) const;
        bool LoadFromFile(const std::filesystem::path& path);

        static AssetHandle GenerateStableHandle(AssetType type, std::string_view sourcePath);
        static std::string NormalizeSourcePath(std::string_view sourcePath);

    private:
        const AssetMetadata* FindByPath(AssetType type, std::string_view normalizedSourcePath) const;

    private:
        std::vector<AssetMetadata> m_Assets;
    };
}
