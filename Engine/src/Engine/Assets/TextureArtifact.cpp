#include "Engine/Assets/TextureArtifact.h"

#include "Engine/Assets/AssetRegistry.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <Windows.h>
#endif

namespace Engine
{
    namespace
    {
        constexpr u32 kTextureArtifactVersion = 1;
        constexpr u64 kBytesPerRgba8Pixel = 4;
        constexpr u32 kMaxTextureDimension = 16384;
        constexpr u64 kMaxTexturePayloadBytes = 1024ull * 1024ull * 1024ull;

        bool ReadExpected(std::istream& input, const char* expected)
        {
            std::string value;
            return static_cast<bool>(input >> value) && value == expected;
        }

        bool IsRoleColorSpaceValid(TextureRole role, TextureColorSpace colorSpace)
        {
            switch (role)
            {
                case TextureRole::BaseColor:
                case TextureRole::Emissive:
                    return colorSpace == TextureColorSpace::Srgb;
                case TextureRole::Normal:
                case TextureRole::Orm:
                case TextureRole::Mask:
                    return colorSpace == TextureColorSpace::Linear;
            }
            return false;
        }

        bool IsPowerOfTwoStep(u32 previous, u32 current)
        {
            return current == (previous > 1 ? previous / 2 : 1);
        }

        bool ParseRole(const std::string& value, TextureRole& out)
        {
            if (value == "BaseColor") { out = TextureRole::BaseColor; return true; }
            if (value == "Emissive") { out = TextureRole::Emissive; return true; }
            if (value == "Normal") { out = TextureRole::Normal; return true; }
            if (value == "Orm") { out = TextureRole::Orm; return true; }
            if (value == "Mask") { out = TextureRole::Mask; return true; }
            return false;
        }

        bool ParseColorSpace(const std::string& value, TextureColorSpace& out)
        {
            if (value == "Srgb") { out = TextureColorSpace::Srgb; return true; }
            if (value == "Linear") { out = TextureColorSpace::Linear; return true; }
            return false;
        }

        bool ParseTarget(const std::string& value, TextureTargetProfile& out)
        {
            if (value == "RGBAFallback") { out = TextureTargetProfile::RGBAFallback; return true; }
            return false;
        }

        bool ParseFormat(const std::string& value, TextureCookedFormat& out)
        {
            if (value == "R8G8B8A8Unorm") { out = TextureCookedFormat::R8G8B8A8Unorm; return true; }
            if (value == "R8G8B8A8Srgb") { out = TextureCookedFormat::R8G8B8A8Srgb; return true; }
            return false;
        }

        bool PublishAtomically(const std::filesystem::path& temporary, const std::filesystem::path& final, std::string& outError)
        {
#if defined(_WIN32)
            if (::ReplaceFileW(final.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
                return true;
            const DWORD replaceError = ::GetLastError();
            if (replaceError == ERROR_FILE_NOT_FOUND && ::MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            outError = "could not atomically publish cooked texture artifact";
#else
            if (std::rename(temporary.c_str(), final.c_str()) == 0)
                return true;
            outError = "could not atomically publish cooked texture artifact";
#endif
            return false;
        }
    }

    const char* ToString(TextureRole value)
    {
        switch (value) { case TextureRole::BaseColor: return "BaseColor"; case TextureRole::Emissive: return "Emissive"; case TextureRole::Normal: return "Normal"; case TextureRole::Orm: return "Orm"; case TextureRole::Mask: return "Mask"; }
        return "Invalid";
    }

    const char* ToString(TextureColorSpace value) { return value == TextureColorSpace::Srgb ? "Srgb" : "Linear"; }
    const char* ToString(TextureTargetProfile value) { return value == TextureTargetProfile::RGBAFallback ? "RGBAFallback" : value == TextureTargetProfile::DesktopBC ? "DesktopBC" : "Astc"; }
    const char* ToString(TextureCookedFormat value) { return value == TextureCookedFormat::R8G8B8A8Srgb ? "R8G8B8A8Srgb" : "R8G8B8A8Unorm"; }

    std::filesystem::path GetCookedTextureArtifactPath(AssetHandle asset)
    {
        return std::filesystem::path("output") / "imports" / "textures" / (std::to_string(asset) + ".spiraltexture");
    }

    bool ValidateTextureArtifact(const TextureArtifact& artifact, std::string& outError)
    {
        if (artifact.Asset == kInvalidAssetHandle || artifact.SourcePath.empty()) { outError = "texture artifact has invalid provenance"; return false; }
        if (artifact.TargetProfile != TextureTargetProfile::RGBAFallback) { outError = "texture artifact target profile is not cooked by this prerequisite"; return false; }
        if (!IsRoleColorSpaceValid(artifact.Role, artifact.ColorSpace)) { outError = "texture artifact role and color space conflict"; return false; }
        const TextureCookedFormat expected = artifact.ColorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::R8G8B8A8Srgb : TextureCookedFormat::R8G8B8A8Unorm;
        if (artifact.CookedFormat != expected || artifact.Mips.empty() || artifact.Payload.empty() || artifact.Payload.size() > kMaxTexturePayloadBytes) { outError = "texture artifact format or payload is invalid"; return false; }
        u64 expectedOffset = 0;
        for (size_t index = 0; index < artifact.Mips.size(); ++index)
        {
            const TextureArtifactMip& mip = artifact.Mips[index];
            if (mip.Width == 0 || mip.Height == 0 || mip.Width > kMaxTextureDimension || mip.Height > kMaxTextureDimension || mip.ByteOffset != expectedOffset || mip.ByteOffset > artifact.Payload.size()
                || (index > 0 && (!IsPowerOfTwoStep(artifact.Mips[index - 1].Width, mip.Width) || !IsPowerOfTwoStep(artifact.Mips[index - 1].Height, mip.Height)))) { outError = "texture artifact mip shape is invalid"; return false; }
            if (mip.Width > std::numeric_limits<u64>::max() / mip.Height || mip.Width * static_cast<u64>(mip.Height) > std::numeric_limits<u64>::max() / kBytesPerRgba8Pixel
                || mip.ByteSize != mip.Width * static_cast<u64>(mip.Height) * kBytesPerRgba8Pixel || mip.ByteSize > artifact.Payload.size() - mip.ByteOffset) { outError = "texture artifact mip range is invalid"; return false; }
            expectedOffset += mip.ByteSize;
        }
        if (expectedOffset != artifact.Payload.size()) { outError = "texture artifact payload has trailing or missing bytes"; return false; }
        outError.clear(); return true;
    }

    bool StoreTextureArtifact(const std::filesystem::path& path, const TextureArtifact& artifact, std::string& outError)
    {
        if (!ValidateTextureArtifact(artifact, outError)) return false;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) { outError = "could not create cooked texture artifact directory"; return false; }
        static std::atomic<u64> sequence { 0 };
        const std::filesystem::path temporary = path.string() + ".tmp." + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) { outError = "could not open temporary cooked texture artifact"; return false; }
        output << "SpiralTextureArtifact " << kTextureArtifactVersion << '\n' << "Source " << std::quoted(artifact.SourcePath) << '\n' << "TextureAsset " << artifact.Asset << '\n'
            << "Role " << ToString(artifact.Role) << '\n' << "ColorSpace " << ToString(artifact.ColorSpace) << '\n' << "TargetProfile " << ToString(artifact.TargetProfile) << '\n'
            << "CookedFormat " << ToString(artifact.CookedFormat) << '\n' << "MipCount " << artifact.Mips.size() << '\n';
        for (const TextureArtifactMip& mip : artifact.Mips) output << "Mip " << mip.Width << ' ' << mip.Height << ' ' << mip.ByteOffset << ' ' << mip.ByteSize << '\n';
        output << "PayloadBytes " << artifact.Payload.size() << '\n' << "Payload\n";
        output.write(reinterpret_cast<const char*>(artifact.Payload.data()), static_cast<std::streamsize>(artifact.Payload.size()));
        output.close();
        if (!output) { std::filesystem::remove(temporary, error); outError = "could not write cooked texture artifact"; return false; }
        if (!PublishAtomically(temporary, path, outError)) { std::filesystem::remove(temporary, error); return false; }
        outError.clear(); return true;
    }

    bool LoadTextureArtifact(const std::filesystem::path& path, TextureArtifact& outArtifact, std::string& outError)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) { outError = "could not open cooked texture artifact"; return false; }
        TextureArtifact candidate; u32 version = 0; std::string role, colorSpace, target, format; u64 mipCount = 0, payloadBytes = 0;
        if (!ReadExpected(input, "SpiralTextureArtifact") || !(input >> version) || version != kTextureArtifactVersion || !ReadExpected(input, "Source") || !(input >> std::quoted(candidate.SourcePath))
            || !ReadExpected(input, "TextureAsset") || !(input >> candidate.Asset) || !ReadExpected(input, "Role") || !(input >> role) || !ParseRole(role, candidate.Role)
            || !ReadExpected(input, "ColorSpace") || !(input >> colorSpace) || !ParseColorSpace(colorSpace, candidate.ColorSpace) || !ReadExpected(input, "TargetProfile") || !(input >> target) || !ParseTarget(target, candidate.TargetProfile)
            || !ReadExpected(input, "CookedFormat") || !(input >> format) || !ParseFormat(format, candidate.CookedFormat) || !ReadExpected(input, "MipCount") || !(input >> mipCount) || mipCount == 0 || mipCount > 16) { outError = "cooked texture artifact header is malformed or unsupported"; return false; }
        candidate.Mips.resize(static_cast<size_t>(mipCount));
        for (TextureArtifactMip& mip : candidate.Mips) if (!ReadExpected(input, "Mip") || !(input >> mip.Width >> mip.Height >> mip.ByteOffset >> mip.ByteSize)) { outError = "cooked texture artifact mip range is malformed"; return false; }
        if (!ReadExpected(input, "PayloadBytes") || !(input >> payloadBytes) || payloadBytes == 0 || payloadBytes > kMaxTexturePayloadBytes || !ReadExpected(input, "Payload")) { outError = "cooked texture artifact payload header is malformed"; return false; }
        if (input.get() != '\n') { outError = "cooked texture artifact payload separator is malformed"; return false; }
        candidate.Payload.resize(static_cast<size_t>(payloadBytes));
        input.read(reinterpret_cast<char*>(candidate.Payload.data()), static_cast<std::streamsize>(payloadBytes));
        if (input.gcount() != static_cast<std::streamsize>(payloadBytes) || input.peek() != std::char_traits<char>::eof() || !ValidateTextureArtifact(candidate, outError)) return false;
        outArtifact = std::move(candidate); outError.clear(); return true;
    }

    bool ResolveTextureArtifact(const AssetRegistry& registry, AssetHandle asset, TextureArtifact& outArtifact, std::string& outError)
    {
        const AssetMetadata* metadata = registry.GetAsset(asset);
        if (!metadata || metadata->Type != AssetType::Texture) { outError = "texture asset handle is missing or has the wrong type"; return false; }
        TextureArtifact candidate;
        if (!LoadTextureArtifact(GetCookedTextureArtifactPath(asset), candidate, outError)) return false;
        if (candidate.Asset != asset || candidate.SourcePath != metadata->SourcePath) { outError = "cooked texture artifact provenance does not match the registry"; return false; }
        outArtifact = std::move(candidate); outError.clear(); return true;
    }

    bool TextureImporter::CookNormalizedRgba8(const NormalizedTextureSource& source, AssetRegistry& registry, TextureTargetProfile target, TextureArtifact& outArtifact, std::string& outError)
    {
        if (target != TextureTargetProfile::RGBAFallback) { outError = "selected target profile requires the deferred KTX2/Basis transcode boundary"; return false; }
        TextureArtifact candidate; candidate.SourcePath = AssetRegistry::NormalizeSourcePath(source.SourcePath); candidate.Role = source.Role; candidate.ColorSpace = source.ColorSpace; candidate.TargetProfile = target;
        candidate.CookedFormat = source.ColorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::R8G8B8A8Srgb : TextureCookedFormat::R8G8B8A8Unorm;
        if (candidate.SourcePath.empty() || source.Width == 0 || source.Height == 0 || source.Width > kMaxTextureDimension || source.Height > kMaxTextureDimension
            || source.Mips.empty() || !IsRoleColorSpaceValid(source.Role, source.ColorSpace)) { outError = "normalized texture source is invalid"; return false; }
        u32 width = source.Width, height = source.Height;
        for (const std::vector<u8>& pixels : source.Mips)
        {
            const u64 byteCount = static_cast<u64>(width) * height * kBytesPerRgba8Pixel;
            if (pixels.size() != byteCount || byteCount > kMaxTexturePayloadBytes || candidate.Payload.size() > kMaxTexturePayloadBytes - byteCount) { outError = "normalized texture source mip payload size is invalid"; return false; }
            candidate.Mips.push_back({ width, height, static_cast<u64>(candidate.Payload.size()), byteCount }); candidate.Payload.insert(candidate.Payload.end(), pixels.begin(), pixels.end());
            width = width > 1 ? width / 2 : 1; height = height > 1 ? height / 2 : 1;
        }
        const bool existed = registry.FindAssetByPath(AssetType::Texture, candidate.SourcePath) != kInvalidAssetHandle;
        candidate.Asset = registry.RegisterAsset(AssetType::Texture, candidate.SourcePath);
        if (candidate.Asset == kInvalidAssetHandle || !StoreTextureArtifact(GetCookedTextureArtifactPath(candidate.Asset), candidate, outError)) { if (!existed && candidate.Asset != kInvalidAssetHandle) registry.RemoveAsset(candidate.Asset); return false; }
        outArtifact = std::move(candidate); outError.clear(); return true;
    }
}
