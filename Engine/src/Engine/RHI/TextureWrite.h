#pragma once

#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/Texture.h"

#include <algorithm>
#include <limits>

namespace Engine::RHI
{
    // Upper bound on the CPU bytes one write may stage. It keeps a malformed or
    // hostile extent from requesting an unbounded private upload allocation and
    // is large enough for the biggest 4096x4096 RGBA8 dynamic UI texture.
    constexpr u64 kMaximumTextureWriteStagingBytes = 256ull * 1024ull * 1024ull;

    // One rectangular write of tightly or row-pitched CPU RGBA8 data into one
    // mip/layer of an exact-device texture. The backend copies `Data` while the
    // command is recorded, so the caller may overwrite or free it as soon as
    // `CommandList::WriteTexture` returns.
    struct TextureWrite
    {
        u32 MipLevel = 0;
        u32 ArrayLayer = 0;
        // Top-left destination texel inside the selected mip.
        u32 X = 0;
        u32 Y = 0;
        // Region size in texels. A write covering the whole mip uses the
        // backend's direct upload path; anything smaller is a region write.
        Extent2D Extent;
        Format TextureFormat = Format::Unknown;
        // Distance between source rows; zero means tightly packed.
        u64 RowPitchBytes = 0;
        const void* Data = nullptr;
        // Readable bytes at `Data`. The last row needs only its tight width.
        u64 DataSizeBytes = 0;
    };

    enum class TextureWriteStatus
    {
        Valid,
        NullData,
        UnsupportedFormat,
        FormatMismatch,
        UnsupportedTexture,
        MissingCopyDestUsage,
        MipOutOfRange,
        ArrayLayerOutOfRange,
        EmptyRegion,
        RegionOutOfBounds,
        RowPitchTooSmall,
        SizeOverflow,
        DataTooSmall,
        StagingTooLarge,
        WrongTextureState
    };

    // Values a backend needs after validation; meaningful only for `Valid`.
    struct TextureWritePlan
    {
        Extent2D MipExtent;
        u64 RowPitchBytes = 0;
        u64 RequiredSourceBytes = 0;
        bool WholeSubresource = false;
    };

    inline bool IsTextureWriteFormatSupported(Format format)
    {
        return format == Format::R8G8B8A8Unorm || format == Format::R8G8B8A8UnormSrgb;
    }

    // Pure, backend-independent validation shared by every command-list
    // implementation. `currentState` is the state the command list would see for
    // the texture at the point of the write (list-staged state first, then the
    // last accepted device state); the caller owns the bracketing transitions.
    // The description is trusted only as far as it is re-checked here, so test
    // devices and malformed descriptions cannot reach a backend copy.
    inline TextureWriteStatus ValidateTextureWrite(const TextureDescription& description, ResourceState currentState,
        const TextureWrite& write, TextureWritePlan* outPlan = nullptr)
    {
        const auto hasUsage = [usage = description.Usage](TextureUsage flag)
        {
            return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
        };
        if (!write.Data)
            return TextureWriteStatus::NullData;
        if (!IsTextureWriteFormatSupported(write.TextureFormat))
            return TextureWriteStatus::UnsupportedFormat;
        if (description.TextureFormat != write.TextureFormat)
            return TextureWriteStatus::FormatMismatch;
        if (description.Extent.Width == 0 || description.Extent.Height == 0 || description.MipLevels == 0
            || description.MipLevels > CalculateMaximumTextureMipLevels(description.Extent)
            || description.ArrayLayers == 0 || description.SampleCount != 1 || hasUsage(TextureUsage::DepthStencil))
            return TextureWriteStatus::UnsupportedTexture;
        if (!hasUsage(TextureUsage::CopyDest))
            return TextureWriteStatus::MissingCopyDestUsage;
        if (write.MipLevel >= description.MipLevels)
            return TextureWriteStatus::MipOutOfRange;
        if (write.ArrayLayer >= description.ArrayLayers)
            return TextureWriteStatus::ArrayLayerOutOfRange;

        const Extent2D mipExtent {
            std::max(description.Extent.Width >> write.MipLevel, 1u),
            std::max(description.Extent.Height >> write.MipLevel, 1u)
        };
        if (write.Extent.Width == 0 || write.Extent.Height == 0)
            return TextureWriteStatus::EmptyRegion;
        if (static_cast<u64>(write.X) + write.Extent.Width > mipExtent.Width
            || static_cast<u64>(write.Y) + write.Extent.Height > mipExtent.Height)
            return TextureWriteStatus::RegionOutOfBounds;
        if (currentState != ResourceState::CopyDest)
            return TextureWriteStatus::WrongTextureState;

        constexpr u64 bytesPerTexel = 4;
        const u64 tightRowBytes = static_cast<u64>(write.Extent.Width) * bytesPerTexel;
        if (write.RowPitchBytes != 0 && write.RowPitchBytes < tightRowBytes)
            return TextureWriteStatus::RowPitchTooSmall;
        const u64 rowPitch = write.RowPitchBytes != 0 ? write.RowPitchBytes : tightRowBytes;
        const u64 leadingRows = static_cast<u64>(write.Extent.Height) - 1u;
        if (leadingRows > std::numeric_limits<u64>::max() / rowPitch
            || leadingRows * rowPitch > std::numeric_limits<u64>::max() - tightRowBytes)
            return TextureWriteStatus::SizeOverflow;
        const u64 requiredBytes = leadingRows * rowPitch + tightRowBytes;
        if (requiredBytes > std::numeric_limits<size_t>::max() || rowPitch > std::numeric_limits<size_t>::max())
            return TextureWriteStatus::SizeOverflow;
        if (requiredBytes > write.DataSizeBytes)
            return TextureWriteStatus::DataTooSmall;
        if (tightRowBytes > kMaximumTextureWriteStagingBytes / write.Extent.Height)
            return TextureWriteStatus::StagingTooLarge;

        if (outPlan)
        {
            outPlan->MipExtent = mipExtent;
            outPlan->RowPitchBytes = rowPitch;
            outPlan->RequiredSourceBytes = requiredBytes;
            outPlan->WholeSubresource = write.X == 0 && write.Y == 0
                && write.Extent.Width == mipExtent.Width && write.Extent.Height == mipExtent.Height;
        }
        return TextureWriteStatus::Valid;
    }

    inline const char* ToString(TextureWriteStatus status)
    {
        switch (status)
        {
            case TextureWriteStatus::Valid: return "Valid";
            case TextureWriteStatus::NullData: return "NullData";
            case TextureWriteStatus::UnsupportedFormat: return "UnsupportedFormat";
            case TextureWriteStatus::FormatMismatch: return "FormatMismatch";
            case TextureWriteStatus::UnsupportedTexture: return "UnsupportedTexture";
            case TextureWriteStatus::MissingCopyDestUsage: return "MissingCopyDestUsage";
            case TextureWriteStatus::MipOutOfRange: return "MipOutOfRange";
            case TextureWriteStatus::ArrayLayerOutOfRange: return "ArrayLayerOutOfRange";
            case TextureWriteStatus::EmptyRegion: return "EmptyRegion";
            case TextureWriteStatus::RegionOutOfBounds: return "RegionOutOfBounds";
            case TextureWriteStatus::RowPitchTooSmall: return "RowPitchTooSmall";
            case TextureWriteStatus::SizeOverflow: return "SizeOverflow";
            case TextureWriteStatus::DataTooSmall: return "DataTooSmall";
            case TextureWriteStatus::StagingTooLarge: return "StagingTooLarge";
            case TextureWriteStatus::WrongTextureState: return "WrongTextureState";
        }
        return "Unknown";
    }
}
