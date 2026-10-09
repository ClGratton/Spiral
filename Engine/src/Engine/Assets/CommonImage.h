#pragma once

#include "Engine/Core/Base.h"

#include <span>
#include <string>
#include <vector>

namespace Engine
{
    enum class CommonImageSource { Png, Jpeg };

    struct CommonImageLimits
    {
        u64 MaximumSourceBytes = 256ull * 1024ull * 1024ull;
        u32 MaximumWidth = 16384;
        u32 MaximumHeight = 16384;
        u64 MaximumPixels = 64ull * 1024ull * 1024ull;
        u64 MaximumOutputBytes = 256ull * 1024ull * 1024ull;
    };

    struct DecodedCommonImage
    {
        CommonImageSource Source = CommonImageSource::Png;
        u32 Width = 0;
        u32 Height = 0;
        std::vector<u8> Rgba8;
    };

    // Decodes one PNG or JPEG selected by content magic only, never by file name.
    // Output is straight RGBA8 with the encoded sample values preserved; the
    // caller assigns sRGB or linear interpretation by glTF role. JPEG alpha is
    // 255 and EXIF orientation is not applied. The output is untouched and
    // `error` names the stable reason whenever the result is false. JPEG
    // corruption warnings (truncation, bad entropy data, extraneous bytes) are
    // failures, and bytes after the final EOI/IEND marker are rejected.
    bool DecodeCommonImage(std::span<const u8> source, const CommonImageLimits& limits,
        DecodedCommonImage& output, std::string& error);
}
