#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Base.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Engine
{
    class AssetRegistry;

    enum class TextureRole { BaseColor, Emissive, Normal, Orm, Mask };
    enum class TextureColorSpace { Srgb, Linear };
    enum class TextureTargetProfile { DesktopBC, Astc, RGBAFallback };
    enum class TextureCookedFormat { R8G8B8A8Unorm, R8G8B8A8Srgb };

    const char* ToString(TextureRole value);
    const char* ToString(TextureColorSpace value);
    const char* ToString(TextureTargetProfile value);
    const char* ToString(TextureCookedFormat value);

    struct TextureArtifactMip
    {
        u32 Width = 0;
        u32 Height = 0;
        u64 ByteOffset = 0;
        u64 ByteSize = 0;
    };

    struct TextureArtifact
    {
        AssetHandle Asset = kInvalidAssetHandle;
        std::string SourcePath;
        TextureRole Role = TextureRole::BaseColor;
        TextureColorSpace ColorSpace = TextureColorSpace::Srgb;
        TextureTargetProfile TargetProfile = TextureTargetProfile::RGBAFallback;
        TextureCookedFormat CookedFormat = TextureCookedFormat::R8G8B8A8Srgb;
        std::vector<TextureArtifactMip> Mips;
        std::vector<u8> Payload;
    };

    // This is the decoder-to-importer boundary, not a source-image decoder. The
    // first implementation deliberately accepts only already-normalized RGBA8
    // pixels and preserves every supplied mip; KTX2/libktx ingestion is later.
    struct NormalizedTextureSource
    {
        std::string SourcePath;
        TextureRole Role = TextureRole::BaseColor;
        TextureColorSpace ColorSpace = TextureColorSpace::Srgb;
        u32 Width = 0;
        u32 Height = 0;
        std::vector<std::vector<u8>> Mips;
    };

    std::filesystem::path GetCookedTextureArtifactPath(AssetHandle asset);
    bool ValidateTextureArtifact(const TextureArtifact& artifact, std::string& outError);
    bool StoreTextureArtifact(const std::filesystem::path& path, const TextureArtifact& artifact, std::string& outError);
    bool LoadTextureArtifact(const std::filesystem::path& path, TextureArtifact& outArtifact, std::string& outError);
    bool ResolveTextureArtifact(const AssetRegistry& registry, AssetHandle asset, TextureArtifact& outArtifact, std::string& outError);

    class TextureImporter
    {
    public:
        // The only selected profile is the uncompressed fallback. DesktopBC and
        // Astc require the still-unadmitted KTX2/Basis transcode boundary.
        static bool CookNormalizedRgba8(const NormalizedTextureSource& source, AssetRegistry& registry,
            TextureTargetProfile target, TextureArtifact& outArtifact, std::string& outError);
    };
}
