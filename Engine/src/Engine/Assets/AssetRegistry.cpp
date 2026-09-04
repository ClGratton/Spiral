#include "Engine/Assets/AssetRegistry.h"

#include "Engine/Core/AtomicFile.h"
#include "Engine/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <fstream>
#include <locale>
#include <sstream>
#include <system_error>

namespace Engine
{
    namespace
    {
        constexpr int kAssetRegistryFormatVersion = 2;
        constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
        constexpr u64 kFnvPrime = 1099511628211ull;

        bool IsAssetTypeValid(AssetType type)
        {
            switch (type)
            {
                case AssetType::Mesh:
                case AssetType::Material:
                case AssetType::Texture:
                case AssetType::Scene:
                case AssetType::Shader:
                case AssetType::Script:
                case AssetType::Audio:
                    return true;
                case AssetType::Unknown:
                    return false;
            }

            return false;
        }

        bool IsSourcePolicyValid(AssetSourcePolicy policy)
        {
            switch (policy)
            {
                case AssetSourcePolicy::PhysicalFile:
                case AssetSourcePolicy::ImmutablePackage:
                    return true;
            }

            return false;
        }

        bool ParseSourcePolicy(std::string_view value, AssetSourcePolicy& outPolicy)
        {
            if (value == "PhysicalFile")
            {
                outPolicy = AssetSourcePolicy::PhysicalFile;
                return true;
            }
            if (value == "ImmutablePackage")
            {
                outPolicy = AssetSourcePolicy::ImmutablePackage;
                return true;
            }

            return false;
        }

        bool IsSafeText(std::string_view value)
        {
            return std::none_of(value.begin(), value.end(), [](unsigned char character)
            {
                return character < 0x20 || character == 0x7f;
            });
        }

        bool IsWindowsReservedName(std::string_view segment)
        {
            const size_t period = segment.find('.');
            std::string stem(segment.substr(0, period));
            std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char character)
            {
                return character >= 'A' && character <= 'Z'
                    ? static_cast<char>(character + ('a' - 'A')) : static_cast<char>(character);
            });
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
                return true;
            return stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9'
                && (stem.starts_with("com") || stem.starts_with("lpt"));
        }

        bool IsGenerationValid(AssetSourcePolicy policy, std::string_view cookedRoot)
        {
            return IsSourcePolicyValid(policy) && AssetRegistry::IsValidCookedRoot(cookedRoot)
                && (policy != AssetSourcePolicy::ImmutablePackage || !cookedRoot.empty());
        }

        bool IsStreamExhausted(std::istream& stream)
        {
            stream >> std::ws;
            return stream.eof();
        }

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

        bool NormalizeAndValidateMetadata(AssetMetadata& metadata, bool fillDefaultName)
        {
            if (!IsAssetTypeValid(metadata.Type)
                || !IsGenerationValid(metadata.SourcePolicy, metadata.CookedRoot)
                || metadata.Handle == kInvalidAssetHandle || metadata.SourcePath.empty()
                || !IsSafeText(metadata.SourcePath) || !IsSafeText(metadata.Name)
                || !IsSafeText(metadata.CookedRoot))
                return false;

            if (metadata.SourcePolicy == AssetSourcePolicy::PhysicalFile)
                metadata.SourcePath = AssetRegistry::NormalizeSourcePath(metadata.SourcePath);
            if (metadata.SourcePath.empty())
                return false;

            if (fillDefaultName && metadata.Name.empty())
                metadata.Name = BuildDefaultName(metadata.SourcePath);
            return true;
        }

        bool HasDuplicate(const std::vector<AssetMetadata>& assets, const AssetMetadata& metadata)
        {
            return std::any_of(assets.begin(), assets.end(), [&metadata](const AssetMetadata& existing)
            {
                return existing.Handle == metadata.Handle
                    || (existing.Type == metadata.Type && existing.SourcePath == metadata.SourcePath);
            });
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

    const char* ToString(AssetSourcePolicy policy)
    {
        switch (policy)
        {
            case AssetSourcePolicy::PhysicalFile: return "PhysicalFile";
            case AssetSourcePolicy::ImmutablePackage: return "ImmutablePackage";
        }

        return "Unknown";
    }

    AssetHandle AssetRegistry::RegisterAsset(AssetType type, std::string sourcePath, std::string name)
    {
        if (!IsAssetTypeValid(type))
            return kInvalidAssetHandle;

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
        if (!NormalizeAndValidateMetadata(normalized, true))
            return false;

        if (const AssetMetadata* existing = GetAsset(normalized.Handle))
            return existing->Type == normalized.Type && existing->SourcePath == normalized.SourcePath
                && existing->SourcePolicy == normalized.SourcePolicy
                && existing->CookedRoot == normalized.CookedRoot;

        if (FindByPath(normalized.Type, normalized.SourcePath))
            return false;

        m_Assets.push_back(std::move(normalized));
        return true;
    }

    bool AssetRegistry::RemoveAsset(AssetHandle handle)
    {
        const auto it = std::find_if(m_Assets.begin(), m_Assets.end(), [handle](const AssetMetadata& candidate)
        {
            return candidate.Handle == handle;
        });
        if (it == m_Assets.end())
            return false;

        m_Assets.erase(it);
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

    bool AssetRegistry::CompareAndSwapAssetGeneration(AssetHandle handle,
        const AssetGeneration& expected, const AssetGeneration& replacement)
    {
        AssetMetadata* metadata = GetAsset(handle);
        if (!metadata || !IsGenerationValid(expected.SourcePolicy, expected.CookedRoot)
            || !IsGenerationValid(replacement.SourcePolicy, replacement.CookedRoot)
            || expected.SourcePolicy != replacement.SourcePolicy
            || metadata->SourcePolicy != expected.SourcePolicy
            || metadata->CookedRoot != expected.CookedRoot)
            return false;

        std::string replacementRoot = replacement.CookedRoot;
        metadata->CookedRoot.swap(replacementRoot);
        metadata->SourcePolicy = replacement.SourcePolicy;
        return true;
    }

    bool AssetRegistry::SetCookedArtifactBasePath(const std::filesystem::path& path)
    {
        if (path.empty())
            return false;
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
        if (error || absolute.empty() || !absolute.is_absolute())
            return false;
        m_CookedArtifactBasePath = absolute;
        return true;
    }

    AssetHandle AssetRegistry::FindAssetByPath(AssetType type, std::string_view sourcePath) const
    {
        const std::string normalizedSourcePath = NormalizeSourcePath(sourcePath);
        const auto it = std::find_if(m_Assets.begin(), m_Assets.end(),
            [type, sourcePath, &normalizedSourcePath](const AssetMetadata& metadata)
        {
            if (metadata.Type != type)
                return false;
            return metadata.SourcePolicy == AssetSourcePolicy::ImmutablePackage
                ? std::string_view(metadata.SourcePath) == sourcePath
                : metadata.SourcePath == normalizedSourcePath;
        });
        return it == m_Assets.end() ? kInvalidAssetHandle : it->Handle;
    }

    void AssetRegistry::Clear()
    {
        m_Assets.clear();
        m_CookedArtifactBasePath.clear();
    }

    bool AssetRegistry::SaveToFile(const std::filesystem::path& path) const
    {
        std::vector<AssetMetadata> sortedAssets;
        sortedAssets.reserve(m_Assets.size());
        for (const AssetMetadata& metadata : m_Assets)
        {
            AssetMetadata normalized = metadata;
            if (!NormalizeAndValidateMetadata(normalized, false)
                || normalized.SourcePath != metadata.SourcePath
                || HasDuplicate(sortedAssets, normalized))
            {
                Log::Error("Could not save invalid asset registry metadata: ", path.string());
                return false;
            }
            sortedAssets.push_back(std::move(normalized));
        }
        std::sort(sortedAssets.begin(), sortedAssets.end(), [](const AssetMetadata& left, const AssetMetadata& right)
        {
            return left.Handle < right.Handle;
        });

        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);

        if (error)
        {
            Log::Error("Could not create asset registry directory: ", parent.string(), " (", error.message(), ")");
            return false;
        }

        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << "SpiralAssetRegistry " << kAssetRegistryFormatVersion << '\n';
        for (const AssetMetadata& metadata : sortedAssets)
        {
            output << "Asset " << metadata.Handle
                << ' ' << ToString(metadata.Type)
                << ' ' << ToString(metadata.SourcePolicy)
                << ' ' << std::quoted(metadata.SourcePath)
                << ' ' << std::quoted(metadata.Name)
                << ' ' << std::quoted(metadata.CookedRoot) << '\n';
        }

        std::string writeError;
        if (!output || !WriteFileAtomically(path, output.str(), writeError))
        {
            Log::Error("Could not atomically save asset registry: ", path.string(), " (", writeError, ")");
            return false;
        }
        return true;
    }

    bool AssetRegistry::LoadFromFile(const std::filesystem::path& path)
    {
        std::error_code pathError;
        const std::filesystem::path absolutePath = std::filesystem::absolute(path, pathError).lexically_normal();
        if (pathError || absolutePath.empty() || !absolutePath.is_absolute())
        {
            Log::Error("Could not resolve asset registry storage base: ", path.string());
            return false;
        }
        const std::filesystem::path loadedArtifactBase = absolutePath.parent_path();

        std::ifstream input(absolutePath);
        if (!input)
        {
            Log::Error("Could not open asset registry for reading: ", path.string());
            return false;
        }

        std::string headerLine;
        if (!std::getline(input, headerLine))
        {
            Log::Error("Unsupported asset registry format: ", path.string());
            return false;
        }
        std::istringstream header(headerLine);
        std::string magic;
        int version = 0;
        if (!(header >> magic >> version) || magic != "SpiralAssetRegistry"
            || (version != 1 && version != kAssetRegistryFormatVersion)
            || !IsStreamExhausted(header))
        {
            Log::Error("Unsupported asset registry format: ", path.string());
            return false;
        }

        std::vector<AssetMetadata> loadedAssets;
        std::string line;
        while (std::getline(input, line))
        {
            const bool whitespaceOnly = std::all_of(line.begin(), line.end(), [](unsigned char character)
            {
                return std::isspace(character) != 0;
            });
            if (whitespaceOnly)
                continue;

            std::istringstream stream(line);
            std::string key;
            stream >> key;
            if (key != "Asset")
                return false;

            std::string type;
            std::string sourcePolicy;
            AssetMetadata metadata;
            if (!(stream >> metadata.Handle >> type))
                return false;
            metadata.Type = ParseAssetType(type);

            if (version == 1)
            {
                metadata.SourcePolicy = AssetSourcePolicy::PhysicalFile;
                if (!(stream >> std::quoted(metadata.SourcePath) >> std::quoted(metadata.Name)))
                    return false;
            }
            else if (!(stream >> sourcePolicy)
                || !ParseSourcePolicy(sourcePolicy, metadata.SourcePolicy)
                || !(stream >> std::quoted(metadata.SourcePath) >> std::quoted(metadata.Name)
                    >> std::quoted(metadata.CookedRoot)))
                return false;

            const std::string serializedSourcePath = metadata.SourcePath;
            if (!IsStreamExhausted(stream) || !NormalizeAndValidateMetadata(metadata, false)
                || (version == kAssetRegistryFormatVersion
                    && metadata.SourcePolicy == AssetSourcePolicy::PhysicalFile
                    && metadata.SourcePath != serializedSourcePath)
                || HasDuplicate(loadedAssets, metadata))
                return false;
            if (version == kAssetRegistryFormatVersion)
            {
                std::ostringstream canonicalLine;
                canonicalLine.imbue(std::locale::classic());
                canonicalLine << "Asset " << metadata.Handle
                    << ' ' << ToString(metadata.Type)
                    << ' ' << ToString(metadata.SourcePolicy)
                    << ' ' << std::quoted(metadata.SourcePath)
                    << ' ' << std::quoted(metadata.Name)
                    << ' ' << std::quoted(metadata.CookedRoot);
                if (canonicalLine.str() != line)
                    return false;
            }
            loadedAssets.push_back(std::move(metadata));
        }

        if (!input.eof())
            return false;

        m_Assets = std::move(loadedAssets);
        m_CookedArtifactBasePath = loadedArtifactBase;
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

    bool AssetRegistry::IsValidCookedRoot(std::string_view cookedRoot)
    {
        if (cookedRoot.empty())
            return true;
        if (cookedRoot.size() > 1024 || cookedRoot.front() == '/' || cookedRoot.back() == '/'
            || cookedRoot.find('\\') != std::string_view::npos)
            return false;

        const std::filesystem::path path { std::string(cookedRoot) };
        if (path.is_absolute() || path.has_root_path() || path.has_root_name()
            || path.has_root_directory()
            || path.lexically_normal().generic_string() != std::string(cookedRoot))
            return false;

        size_t segmentStart = 0;
        size_t depth = 0;
        while (segmentStart < cookedRoot.size())
        {
            const size_t separator = cookedRoot.find('/', segmentStart);
            const size_t segmentEnd = separator == std::string_view::npos
                ? cookedRoot.size() : separator;
            const std::string_view segment = cookedRoot.substr(segmentStart, segmentEnd - segmentStart);
            if (segment.empty() || segment == "." || segment == ".." || segment.size() > 255
                || ++depth > 16 || IsWindowsReservedName(segment)
                || std::isspace(static_cast<unsigned char>(segment.front()))
                || std::isspace(static_cast<unsigned char>(segment.back()))
                || segment.back() == '.')
                return false;

            for (const unsigned char character : segment)
            {
                if (character < 0x20 || character > 0x7e
                    || character == '<' || character == '>' || character == ':'
                    || character == '"' || character == '|' || character == '?'
                    || character == '*')
                    return false;
            }

            if (separator == std::string_view::npos)
                break;
            segmentStart = separator + 1;
        }

        return true;
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
