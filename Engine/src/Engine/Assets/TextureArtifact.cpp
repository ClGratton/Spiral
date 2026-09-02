#include "Engine/Assets/TextureArtifact.h"

#include "Engine/Assets/AssetRegistry.h"

#include <ktx.h>
#include <ktxint.h>

#include <algorithm>
#include <array>
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
        constexpr u32 kTextureArtifactVersion = 2;
        constexpr u64 kBytesPerRgba8Pixel = 4;
        constexpr u32 kMaxTextureDimension = 16384;
        constexpr u64 kMaxTexturePayloadBytes = 1024ull * 1024ull * 1024ull;
        // IEC 61966-2-1 sRGB decode sampled at every 8-bit code and rounded to
        // UNORM16. Generated mip encoding selects the nearest table entry, so
        // artifact bytes do not depend on a platform libm implementation.
        constexpr std::array<u16, 256> kSrgbToLinearUnorm16 {
            0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219, 241, 264, 288, 313,
            340, 367, 396, 427, 458, 491, 526, 562, 599, 637, 677, 718, 761, 805, 851, 898,
            947, 997, 1048, 1101, 1156, 1212, 1270, 1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863,
            1937, 2013, 2090, 2170, 2250, 2333, 2418, 2504, 2592, 2681, 2773, 2866, 2961, 3058, 3157, 3258,
            3360, 3464, 3570, 3678, 3788, 3900, 4014, 4129, 4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124,
            5257, 5392, 5530, 5669, 5810, 5953, 6099, 6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500,
            7666, 7834, 8004, 8177, 8352, 8528, 8708, 8889, 9072, 9258, 9445, 9635, 9828, 10022, 10219, 10417,
            10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909,
            14146, 14387, 14629, 14874, 15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
            18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
            23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325, 25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094,
            28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
            34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
            41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359,
            48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
            57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535
        };

        bool IsMipPolicyValid(TextureMipPolicy policy)
        {
            return policy == TextureMipPolicy::PreserveAuthored || policy == TextureMipPolicy::CompleteMissing;
        }

        u32 GetLegalMipCount(u32 width, u32 height)
        {
            u32 count = 1;
            while (width > 1 || height > 1)
            {
                width = width > 1 ? width / 2 : 1;
                height = height > 1 ? height / 2 : 1;
                ++count;
            }
            return count;
        }

        u64 IntegerSquareRoot(u64 value)
        {
            u64 result = 0;
            u64 bit = 1ull << 62;
            while (bit > value) bit >>= 2;
            while (bit != 0)
            {
                if (value >= result + bit)
                {
                    value -= result + bit;
                    result = (result >> 1) + bit;
                }
                else result >>= 1;
                bit >>= 2;
            }
            return result;
        }

        u8 EncodeSrgbFromLinearQ16(u32 linear)
        {
            const auto upper = std::lower_bound(kSrgbToLinearUnorm16.begin(), kSrgbToLinearUnorm16.end(), linear);
            if (upper == kSrgbToLinearUnorm16.begin()) return 0;
            if (upper == kSrgbToLinearUnorm16.end()) return 255;
            const auto lower = upper - 1;
            return static_cast<u8>((linear - *lower) <= (*upper - linear)
                ? std::distance(kSrgbToLinearUnorm16.begin(), lower)
                : std::distance(kSrgbToLinearUnorm16.begin(), upper));
        }

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

        bool ValidateNormalizedMipSource(const NormalizedTextureSource& source, u32& outLegalMipCount,
            u64& outPayloadBytes, std::string& outError)
        {
            if (source.SourcePath.empty() || source.Width == 0 || source.Height == 0
                || source.Width > kMaxTextureDimension || source.Height > kMaxTextureDimension
                || source.Mips.empty() || !IsRoleColorSpaceValid(source.Role, source.ColorSpace)
                || !IsMipPolicyValid(source.MipPolicy))
            {
                outError = "normalized texture source or mip policy is invalid";
                return false;
            }
            outLegalMipCount = GetLegalMipCount(source.Width, source.Height);
            if (source.Mips.size() > outLegalMipCount)
            {
                outError = "normalized texture source has levels beyond the legal 1x1 mip";
                return false;
            }
            u32 width = source.Width;
            u32 height = source.Height;
            outPayloadBytes = 0;
            for (const std::vector<u8>& mip : source.Mips)
            {
                const u64 byteCount = static_cast<u64>(width) * height * kBytesPerRgba8Pixel;
                if (mip.size() != byteCount || byteCount > kMaxTexturePayloadBytes
                    || outPayloadBytes > kMaxTexturePayloadBytes - byteCount)
                {
                    outError = "normalized texture source mip payload size is invalid";
                    return false;
                }
                outPayloadBytes += byteCount;
                width = width > 1 ? width / 2 : 1;
                height = height > 1 ? height / 2 : 1;
            }
            return true;
        }

        u8 AverageLinearByte(const std::vector<u8>& source, u32 sourceWidth, u32 sourceHeight,
            u32 destinationX, u32 destinationY, u32 destinationWidth, u32 destinationHeight, u32 channel)
        {
            const u32 beginX = destinationX * sourceWidth / destinationWidth;
            const u32 endX = (destinationX + 1) * sourceWidth / destinationWidth;
            const u32 beginY = destinationY * sourceHeight / destinationHeight;
            const u32 endY = (destinationY + 1) * sourceHeight / destinationHeight;
            u32 sum = 0;
            u32 count = 0;
            for (u32 y = beginY; y < endY; ++y)
                for (u32 x = beginX; x < endX; ++x)
                {
                    sum += source[(static_cast<size_t>(y) * sourceWidth + x) * 4 + channel];
                    ++count;
                }
            return static_cast<u8>((sum + count / 2) / count);
        }

        std::vector<u8> GenerateNextNormalizedMip(const std::vector<u8>& source, u32 sourceWidth, u32 sourceHeight,
            TextureRole role, TextureColorSpace colorSpace)
        {
            const u32 destinationWidth = sourceWidth > 1 ? sourceWidth / 2 : 1;
            const u32 destinationHeight = sourceHeight > 1 ? sourceHeight / 2 : 1;
            std::vector<u8> destination(static_cast<size_t>(destinationWidth) * destinationHeight * 4);
            for (u32 y = 0; y < destinationHeight; ++y)
                for (u32 x = 0; x < destinationWidth; ++x)
                {
                    const size_t destinationOffset = (static_cast<size_t>(y) * destinationWidth + x) * 4;
                    if (role == TextureRole::Normal)
                    {
                        const u32 beginX = x * sourceWidth / destinationWidth;
                        const u32 endX = (x + 1) * sourceWidth / destinationWidth;
                        const u32 beginY = y * sourceHeight / destinationHeight;
                        const u32 endY = (y + 1) * sourceHeight / destinationHeight;
                        std::array<i64, 3> sum {};
                        for (u32 sourceY = beginY; sourceY < endY; ++sourceY)
                            for (u32 sourceX = beginX; sourceX < endX; ++sourceX)
                            {
                                const size_t offset = (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4;
                                for (u32 channel = 0; channel < 3; ++channel)
                                    sum[channel] += static_cast<i64>(source[offset + channel]) * 2 - 255;
                            }
                        const u64 squaredLength = static_cast<u64>(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                        if (squaredLength == 0)
                        {
                            destination[destinationOffset] = 128;
                            destination[destinationOffset + 1] = 128;
                            destination[destinationOffset + 2] = 255;
                        }
                        else
                        {
                            const u64 lengthQ16 = IntegerSquareRoot(squaredLength << 32);
                            for (u32 channel = 0; channel < 3; ++channel)
                            {
                                const i64 numerator = sum[channel] * 255 * 65536;
                                const i64 normalized = numerator >= 0
                                    ? (numerator + static_cast<i64>(lengthQ16 / 2)) / static_cast<i64>(lengthQ16)
                                    : -((-numerator + static_cast<i64>(lengthQ16 / 2)) / static_cast<i64>(lengthQ16));
                                destination[destinationOffset + channel] = static_cast<u8>(std::clamp<i64>((normalized + 256) / 2, 0, 255));
                            }
                        }
                    }
                    else if (colorSpace == TextureColorSpace::Srgb)
                    {
                        const u32 beginX = x * sourceWidth / destinationWidth;
                        const u32 endX = (x + 1) * sourceWidth / destinationWidth;
                        const u32 beginY = y * sourceHeight / destinationHeight;
                        const u32 endY = (y + 1) * sourceHeight / destinationHeight;
                        for (u32 channel = 0; channel < 3; ++channel)
                        {
                            u32 sum = 0;
                            u32 count = 0;
                            for (u32 sourceY = beginY; sourceY < endY; ++sourceY)
                                for (u32 sourceX = beginX; sourceX < endX; ++sourceX)
                                {
                                    sum += kSrgbToLinearUnorm16[source[(static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4 + channel]];
                                    ++count;
                                }
                            destination[destinationOffset + channel] = EncodeSrgbFromLinearQ16((sum + count / 2) / count);
                        }
                    }
                    else
                    {
                        for (u32 channel = 0; channel < 3; ++channel)
                            destination[destinationOffset + channel] = AverageLinearByte(source, sourceWidth, sourceHeight,
                                x, y, destinationWidth, destinationHeight, channel);
                    }
                    destination[destinationOffset + 3] = AverageLinearByte(source, sourceWidth, sourceHeight,
                        x, y, destinationWidth, destinationHeight, 3);
                }
            return destination;
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
            if (value == "DesktopBC") { out = TextureTargetProfile::DesktopBC; return true; }
            if (value == "Astc") { out = TextureTargetProfile::Astc; return true; }
            return false;
        }

        bool ParseFormat(const std::string& value, TextureCookedFormat& out)
        {
            if (value == "R8G8B8A8Unorm") { out = TextureCookedFormat::R8G8B8A8Unorm; return true; }
            if (value == "R8G8B8A8Srgb") { out = TextureCookedFormat::R8G8B8A8Srgb; return true; }
            if (value == "Bc5Unorm") { out = TextureCookedFormat::Bc5Unorm; return true; }
            if (value == "Bc7Unorm") { out = TextureCookedFormat::Bc7Unorm; return true; }
            if (value == "Bc7Srgb") { out = TextureCookedFormat::Bc7Srgb; return true; }
            if (value == "Astc4x4Unorm") { out = TextureCookedFormat::Astc4x4Unorm; return true; }
            if (value == "Astc4x4Srgb") { out = TextureCookedFormat::Astc4x4Srgb; return true; }
            return false;
        }

        bool IsTargetValid(TextureTargetProfile target)
        {
            return target == TextureTargetProfile::DesktopBC || target == TextureTargetProfile::Astc || target == TextureTargetProfile::RGBAFallback;
        }

        bool GetBlockLayout(TextureCookedFormat format, u32& outWidth, u32& outHeight, u32& outBytes)
        {
            switch (format)
            {
                case TextureCookedFormat::R8G8B8A8Unorm:
                case TextureCookedFormat::R8G8B8A8Srgb: outWidth = outHeight = 1; outBytes = 4; return true;
                case TextureCookedFormat::Bc5Unorm:
                case TextureCookedFormat::Bc7Unorm:
                case TextureCookedFormat::Bc7Srgb:
                case TextureCookedFormat::Astc4x4Unorm:
                case TextureCookedFormat::Astc4x4Srgb: outWidth = outHeight = 4; outBytes = 16; return true;
            }
            return false;
        }

        bool IsFormatFor(TextureTargetProfile target, TextureRole role, TextureColorSpace colorSpace, TextureCookedFormat format)
        {
            if (!IsTargetValid(target)) return false;
            if (target == TextureTargetProfile::RGBAFallback)
                return format == (colorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::R8G8B8A8Srgb : TextureCookedFormat::R8G8B8A8Unorm);
            if (target == TextureTargetProfile::Astc)
                return format == (colorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::Astc4x4Srgb : TextureCookedFormat::Astc4x4Unorm);
            if (role == TextureRole::Normal) return format == TextureCookedFormat::Bc5Unorm;
            return format == (colorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::Bc7Srgb : TextureCookedFormat::Bc7Unorm);
        }

        bool SelectFormat(TextureRole role, TextureColorSpace colorSpace, TextureTargetProfile target, TextureCookedFormat& outFormat, ktx_transcode_fmt_e& outKtxFormat)
        {
            if (!IsRoleColorSpaceValid(role, colorSpace) || !IsTargetValid(target)) return false;
            if (target == TextureTargetProfile::RGBAFallback) { outFormat = colorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::R8G8B8A8Srgb : TextureCookedFormat::R8G8B8A8Unorm; outKtxFormat = KTX_TTF_RGBA32; return true; }
            if (target == TextureTargetProfile::Astc) { outFormat = colorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::Astc4x4Srgb : TextureCookedFormat::Astc4x4Unorm; outKtxFormat = KTX_TTF_ASTC_4x4_RGBA; return true; }
            if (role == TextureRole::Normal) { outFormat = TextureCookedFormat::Bc5Unorm; outKtxFormat = KTX_TTF_BC5_RG; return true; }
            outFormat = colorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::Bc7Srgb : TextureCookedFormat::Bc7Unorm; outKtxFormat = KTX_TTF_BC7_RGBA; return true;
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
    const char* ToString(TextureCookedFormat value)
    {
        switch (value)
        {
            case TextureCookedFormat::R8G8B8A8Unorm: return "R8G8B8A8Unorm";
            case TextureCookedFormat::R8G8B8A8Srgb: return "R8G8B8A8Srgb";
            case TextureCookedFormat::Bc5Unorm: return "Bc5Unorm";
            case TextureCookedFormat::Bc7Unorm: return "Bc7Unorm";
            case TextureCookedFormat::Bc7Srgb: return "Bc7Srgb";
            case TextureCookedFormat::Astc4x4Unorm: return "Astc4x4Unorm";
            case TextureCookedFormat::Astc4x4Srgb: return "Astc4x4Srgb";
        }
        return "Invalid";
    }

    std::filesystem::path GetCookedTextureArtifactPath(AssetHandle asset)
    {
        return std::filesystem::path("output") / "imports" / "textures" / (std::to_string(asset) + ".spiraltexture");
    }

    bool ValidateTextureArtifact(const TextureArtifact& artifact, std::string& outError)
    {
        if (artifact.Asset == kInvalidAssetHandle || artifact.SourcePath.empty()) { outError = "texture artifact has invalid provenance"; return false; }
        if (!IsTargetValid(artifact.TargetProfile) || !IsRoleColorSpaceValid(artifact.Role, artifact.ColorSpace)) { outError = "texture artifact target profile, role, or color space is invalid"; return false; }
        if (!IsFormatFor(artifact.TargetProfile, artifact.Role, artifact.ColorSpace, artifact.CookedFormat) || artifact.Mips.empty() || artifact.Payload.empty() || artifact.Payload.size() > kMaxTexturePayloadBytes) { outError = "texture artifact format or payload is invalid"; return false; }
        const u32 legalMipCount = GetLegalMipCount(artifact.Mips.front().Width, artifact.Mips.front().Height);
        if (artifact.Mips.size() > legalMipCount) { outError = "texture artifact has levels beyond the legal 1x1 mip"; return false; }
        u32 blockWidth = 0, blockHeight = 0, bytesPerBlock = 0;
        if (!GetBlockLayout(artifact.CookedFormat, blockWidth, blockHeight, bytesPerBlock)) { outError = "texture artifact block layout is invalid"; return false; }
        u64 expectedOffset = 0;
        for (size_t index = 0; index < artifact.Mips.size(); ++index)
        {
            const TextureArtifactMip& mip = artifact.Mips[index];
            if (mip.Width == 0 || mip.Height == 0 || mip.Width > kMaxTextureDimension || mip.Height > kMaxTextureDimension || mip.ByteOffset != expectedOffset || mip.ByteOffset > artifact.Payload.size()
                || (index > 0 && (!IsPowerOfTwoStep(artifact.Mips[index - 1].Width, mip.Width) || !IsPowerOfTwoStep(artifact.Mips[index - 1].Height, mip.Height)))) { outError = "texture artifact mip shape is invalid"; return false; }
            const u64 blockColumns = (static_cast<u64>(mip.Width) + blockWidth - 1) / blockWidth;
            const u64 blockRows = (static_cast<u64>(mip.Height) + blockHeight - 1) / blockHeight;
            if (blockColumns > std::numeric_limits<u64>::max() / blockRows || blockColumns * blockRows > std::numeric_limits<u64>::max() / bytesPerBlock
                || mip.ByteSize != blockColumns * blockRows * bytesPerBlock || mip.ByteSize > artifact.Payload.size() - mip.ByteOffset) { outError = "texture artifact mip range is invalid"; return false; }
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
            << "CookedFormat " << ToString(artifact.CookedFormat) << '\n' << "HasAlpha " << (artifact.HasAlpha ? 1 : 0) << '\n' << "MipCount " << artifact.Mips.size() << '\n';
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
        TextureArtifact candidate; u32 version = 0; std::string role, colorSpace, target, format; u64 mipCount = 0, payloadBytes = 0; u32 hasAlpha = 0;
        if (!ReadExpected(input, "SpiralTextureArtifact") || !(input >> version) || (version != 1 && version != kTextureArtifactVersion) || !ReadExpected(input, "Source") || !(input >> std::quoted(candidate.SourcePath))
            || !ReadExpected(input, "TextureAsset") || !(input >> candidate.Asset) || !ReadExpected(input, "Role") || !(input >> role) || !ParseRole(role, candidate.Role)
            || !ReadExpected(input, "ColorSpace") || !(input >> colorSpace) || !ParseColorSpace(colorSpace, candidate.ColorSpace) || !ReadExpected(input, "TargetProfile") || !(input >> target) || !ParseTarget(target, candidate.TargetProfile)
            || !ReadExpected(input, "CookedFormat") || !(input >> format) || !ParseFormat(format, candidate.CookedFormat)) { outError = "cooked texture artifact header is malformed or unsupported"; return false; }
        if (version == 1)
        {
            // Schema 1 did not record alpha. Treat it as present so old pages
            // are never optimized as opaque by a later consumer.
            candidate.HasAlpha = true;
        }
        else if (!ReadExpected(input, "HasAlpha") || !(input >> hasAlpha) || hasAlpha > 1)
        {
            outError = "cooked texture artifact schema-2 alpha metadata is malformed"; return false;
        }
        else candidate.HasAlpha = hasAlpha != 0;
        if (!ReadExpected(input, "MipCount") || !(input >> mipCount) || mipCount == 0 || mipCount > 16) { outError = "cooked texture artifact mip count is malformed"; return false; }
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

    bool TextureImporter::ApplyNormalizedRgba8MipPolicy(const NormalizedTextureSource& source,
        NormalizedTextureSource& outSource, u32& outGeneratedMipCount, std::string& outError)
    {
        u32 legalMipCount = 0;
        u64 payloadBytes = 0;
        if (!ValidateNormalizedMipSource(source, legalMipCount, payloadBytes, outError)) return false;
        NormalizedTextureSource candidate = source;
        u32 generatedMipCount = 0;
        if (source.MipPolicy == TextureMipPolicy::CompleteMissing)
        {
            u32 width = source.Width;
            u32 height = source.Height;
            for (size_t index = 1; index < source.Mips.size(); ++index)
            {
                width = width > 1 ? width / 2 : 1;
                height = height > 1 ? height / 2 : 1;
            }
            while (candidate.Mips.size() < legalMipCount)
            {
                std::vector<u8> mip = GenerateNextNormalizedMip(candidate.Mips.back(), width, height, source.Role, source.ColorSpace);
                if (mip.size() > kMaxTexturePayloadBytes - payloadBytes)
                {
                    outError = "generated normalized texture mip chain exceeds the payload bound";
                    return false;
                }
                payloadBytes += mip.size();
                candidate.Mips.push_back(std::move(mip));
                width = width > 1 ? width / 2 : 1;
                height = height > 1 ? height / 2 : 1;
                ++generatedMipCount;
            }
        }
        outSource = std::move(candidate);
        outGeneratedMipCount = generatedMipCount;
        outError.clear();
        return true;
    }

    bool TextureImporter::CookNormalizedRgba8(const NormalizedTextureSource& source, AssetRegistry& registry, TextureTargetProfile target, TextureArtifact& outArtifact, std::string& outError)
    {
        if (target != TextureTargetProfile::RGBAFallback) { outError = "selected target profile requires the deferred KTX2/Basis transcode boundary"; return false; }
        NormalizedTextureSource prepared;
        u32 generatedMipCount = 0;
        if (!ApplyNormalizedRgba8MipPolicy(source, prepared, generatedMipCount, outError)) return false;
        TextureArtifact candidate; candidate.SourcePath = AssetRegistry::NormalizeSourcePath(prepared.SourcePath); candidate.Role = prepared.Role; candidate.ColorSpace = prepared.ColorSpace; candidate.TargetProfile = target; candidate.HasAlpha = true;
        candidate.CookedFormat = prepared.ColorSpace == TextureColorSpace::Srgb ? TextureCookedFormat::R8G8B8A8Srgb : TextureCookedFormat::R8G8B8A8Unorm;
        if (candidate.SourcePath.empty()) { outError = "normalized texture source path is invalid"; return false; }
        u32 width = prepared.Width, height = prepared.Height;
        for (const std::vector<u8>& pixels : prepared.Mips)
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

    bool TextureImporter::CookKtx2Basis(const Ktx2BasisTextureSource& source, AssetRegistry& registry, TextureTargetProfile target, TextureArtifact& outArtifact, std::string& outError)
    {
        TextureArtifact candidate;
        candidate.SourcePath = AssetRegistry::NormalizeSourcePath(source.SourcePath);
        candidate.Role = source.Role;
        candidate.ColorSpace = source.ColorSpace;
        candidate.TargetProfile = target;
        ktx_transcode_fmt_e ktxFormat = KTX_TTF_NOSELECTION;
        // Pinned libktx v4.4.2 transfers its memory-stream allocation to the
        // texture only after the complete KTX2 header has been read. Reject a
        // shorter buffer before entry so the library's early-read failure path
        // cannot strand that private stream allocation.
        if (candidate.SourcePath.empty() || source.Bytes.size() < KTX2_HEADER_SIZE || !IsMipPolicyValid(source.MipPolicy)
            || !SelectFormat(source.Role, source.ColorSpace, target, candidate.CookedFormat, ktxFormat))
        {
            outError = "KTX2 import source, role, color space, or target profile is invalid";
            return false;
        }

        ktxTexture2* texture = nullptr;
        const KTX_error_code createResult = ktxTexture2_CreateFromMemory(source.Bytes.data(), source.Bytes.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture);
        if (createResult != KTX_SUCCESS || !texture)
        {
            outError = "KTX2 container validation failed for " + candidate.SourcePath;
            return false;
        }
        const auto destroy = [&texture]() { if (texture) { ktxTexture2_Destroy(texture); texture = nullptr; } };
        const khr_df_transfer_e transfer = ktxTexture2_GetTransferFunction_e(texture);
        const khr_df_primaries_e primaries = ktxTexture2_GetPrimaries_e(texture);
        const bool expectedTransfer = candidate.ColorSpace == TextureColorSpace::Srgb ? transfer == KHR_DF_TRANSFER_SRGB : transfer == KHR_DF_TRANSFER_LINEAR;
        const u32 legalMipCount = texture->baseWidth > 0 && texture->baseHeight > 0 ? GetLegalMipCount(texture->baseWidth, texture->baseHeight) : 0;
        if (texture->numDimensions != 2 || texture->baseDepth != 1 || texture->isArray || texture->isCubemap || texture->isVideo || texture->numFaces != 1
            || texture->baseWidth == 0 || texture->baseHeight == 0 || texture->baseWidth > kMaxTextureDimension || texture->baseHeight > kMaxTextureDimension
            || texture->numLevels == 0 || texture->numLevels > legalMipCount || !expectedTransfer || primaries != KHR_DF_PRIMARIES_SRGB || !ktxTexture2_NeedsTranscoding(texture))
        {
            destroy(); outError = "KTX2 import rejected unsupported shape, metadata, or non-Basis payload for " + candidate.SourcePath; return false;
        }
        if (source.MipPolicy == TextureMipPolicy::CompleteMissing && texture->numLevels < legalMipCount)
        {
            destroy();
            outError = "KTX2 compressed mip completion is unsupported for " + candidate.SourcePath
                + "; provide a complete authored chain or normalized RGBA8 source";
            return false;
        }
        candidate.HasAlpha = ktxTexture2_GetNumComponents(texture) >= 4;
        if (ktxTexture2_TranscodeBasis(texture, ktxFormat, 0) != KTX_SUCCESS)
        {
            destroy(); outError = "KTX2 Basis transcode failed for " + candidate.SourcePath + " target " + ToString(target); return false;
        }
        u32 blockWidth = 0, blockHeight = 0, bytesPerBlock = 0;
        GetBlockLayout(candidate.CookedFormat, blockWidth, blockHeight, bytesPerBlock);
        u32 width = texture->baseWidth, height = texture->baseHeight;
        for (u32 level = 0; level < texture->numLevels; ++level)
        {
            const u64 byteCount = ((static_cast<u64>(width) + blockWidth - 1) / blockWidth) * ((static_cast<u64>(height) + blockHeight - 1) / blockHeight) * bytesPerBlock;
            ktx_size_t offset = 0;
            if (byteCount > kMaxTexturePayloadBytes || candidate.Payload.size() > kMaxTexturePayloadBytes - byteCount
                || ktxTexture2_GetImageOffset(texture, level, 0, 0, &offset) != KTX_SUCCESS || offset > texture->dataSize || byteCount > texture->dataSize - offset)
            {
                destroy(); outError = "KTX2 transcode produced an invalid mip range for " + candidate.SourcePath; return false;
            }
            candidate.Mips.push_back({ width, height, static_cast<u64>(candidate.Payload.size()), byteCount });
            candidate.Payload.insert(candidate.Payload.end(), texture->pData + offset, texture->pData + offset + byteCount);
            width = width > 1 ? width / 2 : 1; height = height > 1 ? height / 2 : 1;
        }
        destroy();
        const bool existed = registry.FindAssetByPath(AssetType::Texture, candidate.SourcePath) != kInvalidAssetHandle;
        candidate.Asset = registry.RegisterAsset(AssetType::Texture, candidate.SourcePath);
        if (candidate.Asset == kInvalidAssetHandle || !StoreTextureArtifact(GetCookedTextureArtifactPath(candidate.Asset), candidate, outError))
        {
            if (!existed && candidate.Asset != kInvalidAssetHandle) registry.RemoveAsset(candidate.Asset);
            return false;
        }
        outArtifact = std::move(candidate); outError.clear(); return true;
    }
}
