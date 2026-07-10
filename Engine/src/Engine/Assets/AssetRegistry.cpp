#include "Engine/Assets/AssetRegistry.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace Engine
{
    namespace
    {
        constexpr int kAssetRegistryFormatVersion = 1;
        constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
        constexpr u64 kFnvPrime = 1099511628211ull;

        std::string MakePathKey(AssetType type, std::string_view normalizedSourcePath)
        {
            std::string key = ToString(type);
            key += ':';
            key += normalizedSourcePath;
            return key;
        }

        AssetHandle HashString(std::string_view value)
        {
            u64 hash = kFnvOffsetBasis;
            for (char character : value)
            {
                hash ^= static_cast<unsigned char>(character);
                hash *= kFnvPrime;
            }

            return hash == kInvalidAssetHandle ? 1 : hash;
        }

        std::string BuildDefaultName(std::string_view normalizedSourcePath)
        {
            const std::filesystem::path path { std::string(normalizedSourcePath) };
            const std::string stem = path.stem().string();
            return stem.empty() ? std::string(normalizedSourcePath) : stem;
        }
    }

    const char* ToString(AssetType type)
    {
        switch (type)
        {
            case AssetType::Mesh: return "Mesh";
            case AssetType::Material: return "Material";
            case AssetType::Texture: return "Texture";
            case AssetType::Scene: return "Scene";
            case AssetType::Shader: return "Shader";
            case AssetType::Script: return "Script";
            case AssetType::Audio: return "Audio";
            case AssetType::Unknown: return "Unknown";
        }

        return "Unknown";
    }

    AssetType ParseAssetType(std::string_view value)
    {
        if (value == "Mesh")
            return AssetType::Mesh;
        if (value == "Material")
            return AssetType::Material;
        if (value == "Texture")
            return AssetType::Texture;
        if (value == "Scene")
            return AssetType::Scene;
        if (value == "Shader")
            return AssetType::Shader;
        if (value == "Script")
            return AssetType::Script;
        if (value == "Audio")
            return AssetType::Audio;

        return AssetType::Unknown;
    }

    AssetHandle AssetRegistry::RegisterAsset(AssetType type, std::string sourcePath, std::string name)
    {
        const std::string normalizedSourcePath = NormalizeSourcePath(sourcePath);
        if (normalizedSourcePath.empty())
            return kInvalidAssetHandle;

        if (const AssetMetadata* existing = FindByPath(type, normalizedSourcePath))
            return existing->Handle;

        AssetMetadata metadata;
        metadata.Type = type;
        metadata.SourcePath = normalizedSourcePath;
        metadata.Name = name.empty() ? BuildDefaultName(normalizedSourcePath) : std::move(name);
        metadata.Handle = GenerateStableHandle(type, normalizedSourcePath);

        AssetHandle candidate = metadata.Handle;
        u32 collisionSalt = 1;
        while (const AssetMetadata* existing = GetAsset(candidate))
        {
            if (existing->Type == metadata.Type && existing->SourcePath == metadata.SourcePath)
                return existing->Handle;

            candidate = HashString(MakePathKey(type, normalizedSourcePath) + "#" + std::to_string(collisionSalt++));
        }

        metadata.Handle = candidate;
        m_Assets.push_back(std::move(metadata));
        return candidate;
    }

    bool AssetRegistry::RegisterAsset(const AssetMetadata& metadata)
    {
        AssetMetadata normalized = metadata;
        normalized.SourcePath = NormalizeSourcePath(normalized.SourcePath);
        if (normalized.Handle == kInvalidAssetHandle || normalized.SourcePath.empty())
            return false;

        if (const AssetMetadata* existing = GetAsset(normalized.Handle))
            return existing->Type == normalized.Type && existing->SourcePath == normalized.SourcePath;

        if (FindByPath(normalized.Type, normalized.SourcePath))
            return false;

        if (normalized.Name.empty())
            normalized.Name = BuildDefaultName(normalized.SourcePath);

        m_Assets.push_back(std::move(normalized));
        return true;
    }

    bool AssetRegistry::Contains(AssetHandle handle) const
    {
        return GetAsset(handle) != nullptr;
    }

    AssetMetadata* AssetRegistry::GetAsset(AssetHandle handle)
    {
        const auto it = std::find_if(m_Assets.begin(), m_Assets.end(), [handle](const AssetMetadata& metadata)
        {
            return metadata.Handle == handle;
        });

        return it == m_Assets.end() ? nullptr : &(*it);
    }

    const AssetMetadata* AssetRegistry::GetAsset(AssetHandle handle) const
    {
        const auto it = std::find_if(m_Assets.begin(), m_Assets.end(), [handle](const AssetMetadata& metadata)
        {
            return metadata.Handle == handle;
        });

        return it == m_Assets.end() ? nullptr : &(*it);
    }

    bool AssetRegistry::SetAssetName(AssetHandle handle, std::string name)
    {
        AssetMetadata* metadata = GetAsset(handle);
        if (!metadata)
            return false;

        metadata->Name = std::move(name);
        return true;
    }

    AssetHandle AssetRegistry::FindAssetByPath(AssetType type, std::string_view sourcePath) const
    {
        const std::string normalizedSourcePath = NormalizeSourcePath(sourcePath);
        const AssetMetadata* metadata = FindByPath(type, normalizedSourcePath);
        return metadata ? metadata->Handle : kInvalidAssetHandle;
    }

    void AssetRegistry::Clear()
    {
        m_Assets.clear();
    }

    bool AssetRegistry::SaveToFile(const std::filesystem::path& path) const
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);

        if (error)
        {
            Log::Error("Could not create asset registry directory: ", parent.string(), " (", error.message(), ")");
            return false;
        }

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output)
        {
            Log::Error("Could not open asset registry for writing: ", path.string());
            return false;
        }

        output << "SpiralAssetRegistry " << kAssetRegistryFormatVersion << '\n';
        for (const AssetMetadata& metadata : m_Assets)
        {
            output << "Asset " << metadata.Handle
                << ' ' << ToString(metadata.Type)
                << ' ' << std::quoted(metadata.SourcePath)
                << ' ' << std::quoted(metadata.Name) << '\n';
        }

        return true;
    }

    bool AssetRegistry::LoadFromFile(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        if (!input)
        {
            Log::Error("Could not open asset registry for reading: ", path.string());
            return false;
        }

        std::string magic;
        int version = 0;
        input >> magic >> version;
        if (magic != "SpiralAssetRegistry" || version != kAssetRegistryFormatVersion)
        {
            Log::Error("Unsupported asset registry format: ", path.string());
            return false;
        }

        std::vector<AssetMetadata> loadedAssets;
        std::string line;
        std::getline(input, line);
        while (std::getline(input, line))
        {
            if (line.empty())
                continue;

            std::istringstream stream(line);
            std::string key;
            stream >> key;
            if (key != "Asset")
                continue;

            std::string type;
            AssetMetadata metadata;
            if (!(stream >> metadata.Handle >> type >> std::quoted(metadata.SourcePath) >> std::quoted(metadata.Name)))
                return false;

            metadata.Type = ParseAssetType(type);
            metadata.SourcePath = NormalizeSourcePath(metadata.SourcePath);
            if (metadata.Handle == kInvalidAssetHandle || metadata.SourcePath.empty())
                return false;

            const auto duplicateHandle = std::find_if(loadedAssets.begin(), loadedAssets.end(), [&metadata](const AssetMetadata& existing)
            {
                return existing.Handle == metadata.Handle;
            });
            if (duplicateHandle != loadedAssets.end())
                return false;

            const auto duplicatePath = std::find_if(loadedAssets.begin(), loadedAssets.end(), [&metadata](const AssetMetadata& existing)
            {
                return existing.Type == metadata.Type && existing.SourcePath == metadata.SourcePath;
            });
            if (duplicatePath != loadedAssets.end())
                return false;

            loadedAssets.push_back(std::move(metadata));
        }

        m_Assets = std::move(loadedAssets);
        return true;
    }

    AssetHandle AssetRegistry::GenerateStableHandle(AssetType type, std::string_view sourcePath)
    {
        const std::string normalizedSourcePath = NormalizeSourcePath(sourcePath);
        if (normalizedSourcePath.empty())
            return kInvalidAssetHandle;

        return HashString(MakePathKey(type, normalizedSourcePath));
    }

    std::string AssetRegistry::NormalizeSourcePath(std::string_view sourcePath)
    {
        std::string normalized(sourcePath);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');

        while (!normalized.empty() && std::isspace(static_cast<unsigned char>(normalized.back())))
            normalized.pop_back();

        while (!normalized.empty() && std::isspace(static_cast<unsigned char>(normalized.front())))
            normalized.erase(normalized.begin());

        while (normalized.rfind("./", 0) == 0)
            normalized.erase(0, 2);

        const bool isAbsolutePath = std::filesystem::path(normalized).is_absolute();
        while (!isAbsolutePath && !normalized.empty() && normalized.front() == '/')
            normalized.erase(normalized.begin());

        return normalized;
    }

    const AssetMetadata* AssetRegistry::FindByPath(AssetType type, std::string_view normalizedSourcePath) const
    {
        const auto it = std::find_if(m_Assets.begin(), m_Assets.end(), [type, normalizedSourcePath](const AssetMetadata& metadata)
        {
            return metadata.Type == type && metadata.SourcePath == normalizedSourcePath;
        });

        return it == m_Assets.end() ? nullptr : &(*it);
    }
}
