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

    // Physical files participate in filesystem watching and reimport. Immutable
    // package identities are logical provenance and must never be resolved as paths.
    enum class AssetSourcePolicy
    {
        PhysicalFile,
        ImmutablePackage
    };

    const char* ToString(AssetSourcePolicy policy);

    struct AssetGeneration
    {
        AssetSourcePolicy SourcePolicy = AssetSourcePolicy::PhysicalFile;
        std::string CookedRoot;
    };

    struct AssetMetadata
    {
        AssetHandle Handle = kInvalidAssetHandle;
        AssetType Type = AssetType::Unknown;
        std::string SourcePath;
        std::string Name;
        AssetSourcePolicy SourcePolicy = AssetSourcePolicy::PhysicalFile;
        std::string CookedRoot;
    };

    class AssetRegistry
    {
    public:
        AssetHandle RegisterAsset(AssetType type, std::string sourcePath, std::string name = {});
        bool RegisterAsset(const AssetMetadata& metadata);
        bool RemoveAsset(AssetHandle handle);
        bool Contains(AssetHandle handle) const;
        AssetMetadata* GetAsset(AssetHandle handle);
        const AssetMetadata* GetAsset(AssetHandle handle) const;
        bool SetAssetName(AssetHandle handle, std::string name);
        // This gate is intended for an unpublished candidate registry. Published
        // renderer catalogs retain their copied registry and are never mutated.
        bool CompareAndSwapAssetGeneration(AssetHandle handle,
            const AssetGeneration& expected, const AssetGeneration& replacement);
        // Runtime-only anchor for project-relative immutable cooked roots. Loads
        // derive it from the registry file; newly staged registries set it before
        // resolver capture. It is copied with the registry and never serialized.
        bool SetCookedArtifactBasePath(const std::filesystem::path& path);
        const std::filesystem::path& GetCookedArtifactBasePath() const { return m_CookedArtifactBasePath; }
        AssetHandle FindAssetByPath(AssetType type, std::string_view sourcePath) const;
        const std::vector<AssetMetadata>& GetAssets() const { return m_Assets; }
        void Clear();

        bool SaveToFile(const std::filesystem::path& path) const;
        bool LoadFromFile(const std::filesystem::path& path);

        static AssetHandle GenerateStableHandle(AssetType type, std::string_view sourcePath);
        static std::string NormalizeSourcePath(std::string_view sourcePath);
        // Empty means the legacy mutable cooked location. A nonempty root must be
        // a canonical project-relative portable path before it can be published.
        static bool IsValidCookedRoot(std::string_view cookedRoot);

    private:
        const AssetMetadata* FindByPath(AssetType type, std::string_view normalizedSourcePath) const;

    private:
        std::vector<AssetMetadata> m_Assets;
        std::filesystem::path m_CookedArtifactBasePath;
    };
}
