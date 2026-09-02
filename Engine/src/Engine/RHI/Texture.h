#pragma once

#include "Engine/RHI/RHICommon.h"

#include <limits>
#include <string>
#include <vector>

namespace Engine::RHI
{
    struct TextureFormatBlockLayout
    {
        u32 Width = 0;
        u32 Height = 0;
        u32 Bytes = 0;
    };

    inline bool GetTextureFormatBlockLayout(Format format, TextureFormatBlockLayout& outLayout)
    {
        TextureFormatBlockLayout layout;
        switch (format)
        {
            case Format::R8Unorm: layout = { 1, 1, 1 }; break;
            case Format::R8G8B8A8Unorm:
            case Format::R8G8B8A8UnormSrgb:
            case Format::R11G11B10Float:
            case Format::R32Uint:
            case Format::D24UnormS8Uint:
            case Format::D32Float: layout = { 1, 1, 4 }; break;
            case Format::R32G32Float:
            case Format::R16G16B16A16Float: layout = { 1, 1, 8 }; break;
            case Format::R32G32B32Float: layout = { 1, 1, 12 }; break;
            case Format::R32G32B32A32Float: layout = { 1, 1, 16 }; break;
            case Format::BC5Unorm:
            case Format::BC7Unorm:
            case Format::BC7UnormSrgb:
            case Format::ASTC4x4Unorm:
            case Format::ASTC4x4UnormSrgb: layout = { 4, 4, 16 }; break;
            case Format::Unknown: return false;
        }
        outLayout = layout;
        return true;
    }

    inline bool CalculateTextureSubresourceStorage(Format format, Extent2D extent,
        u64& outRowPitchBytes, u64& outByteCount)
    {
        TextureFormatBlockLayout layout;
        if (extent.Width == 0 || extent.Height == 0 || !GetTextureFormatBlockLayout(format, layout))
            return false;
        const u64 blockColumns = (static_cast<u64>(extent.Width) + layout.Width - 1) / layout.Width;
        const u64 blockRows = (static_cast<u64>(extent.Height) + layout.Height - 1) / layout.Height;
        if (blockColumns > std::numeric_limits<u64>::max() / layout.Bytes)
            return false;
        const u64 rowPitch = blockColumns * layout.Bytes;
        if (blockRows > std::numeric_limits<u64>::max() / rowPitch)
            return false;
        outRowPitchBytes = rowPitch;
        outByteCount = rowPitch * blockRows;
        return true;
    }

    inline bool IsTextureReadbackFormatSupported(Format format)
    {
        return format == Format::R8G8B8A8Unorm || format == Format::R8G8B8A8UnormSrgb
            || format == Format::BC5Unorm || format == Format::BC7Unorm
            || format == Format::BC7UnormSrgb;
    }

    inline u32 CalculateMaximumTextureMipLevels(Extent2D extent)
    {
        if (extent.Width == 0 || extent.Height == 0)
            return 0;
        u32 levels = 1;
        while (extent.Width > 1 || extent.Height > 1)
        {
            extent.Width = extent.Width > 1 ? extent.Width / 2 : 1;
            extent.Height = extent.Height > 1 ? extent.Height / 2 : 1;
            ++levels;
        }
        return levels;
    }

    enum class TextureUsage : u32
    {
        None = 0,
        ShaderResource = 1u << 0u,
        RenderTarget = 1u << 1u,
        DepthStencil = 1u << 2u,
        UnorderedAccess = 1u << 3u,
        CopySource = 1u << 4u,
        CopyDest = 1u << 5u,
        Present = 1u << 6u
    };

    struct TextureDescription
    {
        std::string DebugName;
        Extent2D Extent;
        Format TextureFormat = Format::Unknown;
        TextureUsage Usage = TextureUsage::None;
        ResourceState InitialState = ResourceState::Common;
        u32 MipLevels = 1;
        u32 ArrayLayers = 1;
        u32 SampleCount = 1;
    };

    // A CPU-owned copy of one tightly-addressable texture subresource. Backends may
    // expose a larger native row pitch; consumers must use RowPitchBytes.
    struct TextureReadback
    {
        Extent2D Extent;
        Format TextureFormat = Format::Unknown;
        u32 RowPitchBytes = 0;
        std::vector<u8> Data;
    };

    // The first texture-content path is deliberately restricted to one complete
    // tightly-addressable 2D RGBA8 subresource. `Bytes` is engine-owned and is
    // retained by the caller through native submission acceptance.
    struct TextureUpload
    {
        Extent2D Extent;
        Format TextureFormat = Format::Unknown;
        u32 RowPitchBytes = 0;
        Ref<const std::vector<u8>> Bytes;
    };

    struct TextureSubresourceUpload
    {
        u32 MipLevel = 0;
        u32 ArrayLayer = 0;
        Extent2D Extent;
        u64 ByteOffset = 0;
        u64 RowPitchBytes = 0;
        u64 ByteSize = 0;
    };

    // One immutable payload containing every declared subresource in ascending
    // mip order. Native row alignment remains backend-private; these ranges use
    // the cooked artifact's tightly packed block layout.
    struct TextureUploadBatch
    {
        Format TextureFormat = Format::Unknown;
        std::vector<TextureSubresourceUpload> Subresources;
        Ref<const std::vector<u8>> Bytes;
    };

    inline bool IsReadOnlyTextureUploadCompatible(const TextureDescription& description, const TextureUpload& upload)
    {
        const auto hasUsage = [usage = description.Usage](TextureUsage flag)
        {
            return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
        };
        if (description.Extent.Width == 0 || description.Extent.Height == 0
            || description.MipLevels != 1 || description.ArrayLayers != 1 || description.SampleCount != 1
            || description.Extent.Width != upload.Extent.Width || description.Extent.Height != upload.Extent.Height
            || description.TextureFormat != upload.TextureFormat || !upload.Bytes
            || !hasUsage(TextureUsage::CopyDest)
            || !hasUsage(TextureUsage::ShaderResource)
            || hasUsage(TextureUsage::RenderTarget) || hasUsage(TextureUsage::DepthStencil) || hasUsage(TextureUsage::UnorderedAccess)
            || description.InitialState != ResourceState::CopyDest)
            return false;

        if (description.TextureFormat != Format::R8G8B8A8Unorm && description.TextureFormat != Format::R8G8B8A8UnormSrgb)
            return false;

        const u64 minimumRowPitch = static_cast<u64>(description.Extent.Width) * 4u;
        const u64 requiredBytes = static_cast<u64>(upload.RowPitchBytes) * description.Extent.Height;
        return upload.RowPitchBytes >= minimumRowPitch
            && requiredBytes <= std::numeric_limits<size_t>::max()
            && upload.Bytes->size() == static_cast<size_t>(requiredBytes);
    }

    inline bool IsReadOnlyTextureUploadCompatible(const TextureDescription& description, const TextureUploadBatch& upload)
    {
        const auto hasUsage = [usage = description.Usage](TextureUsage flag)
        {
            return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
        };
        if (description.Extent.Width == 0 || description.Extent.Height == 0
            || description.MipLevels == 0 || description.MipLevels > CalculateMaximumTextureMipLevels(description.Extent)
            || description.ArrayLayers != 1 || description.SampleCount != 1
            || description.TextureFormat != upload.TextureFormat || !upload.Bytes
            || upload.Subresources.size() != description.MipLevels
            || !hasUsage(TextureUsage::CopyDest) || !hasUsage(TextureUsage::ShaderResource)
            || hasUsage(TextureUsage::RenderTarget) || hasUsage(TextureUsage::DepthStencil)
            || hasUsage(TextureUsage::UnorderedAccess) || description.InitialState != ResourceState::CopyDest)
            return false;

        switch (description.TextureFormat)
        {
            case Format::R8G8B8A8Unorm:
            case Format::R8G8B8A8UnormSrgb:
            case Format::BC5Unorm:
            case Format::BC7Unorm:
            case Format::BC7UnormSrgb:
            case Format::ASTC4x4Unorm:
            case Format::ASTC4x4UnormSrgb: break;
            default: return false;
        }

        Extent2D expectedExtent = description.Extent;
        u64 expectedOffset = 0;
        for (u32 mipLevel = 0; mipLevel < description.MipLevels; ++mipLevel)
        {
            const TextureSubresourceUpload& subresource = upload.Subresources[mipLevel];
            u64 tightRowPitch = 0;
            u64 tightByteSize = 0;
            TextureFormatBlockLayout layout;
            if (subresource.MipLevel != mipLevel || subresource.ArrayLayer != 0
                || subresource.Extent.Width != expectedExtent.Width || subresource.Extent.Height != expectedExtent.Height
                || subresource.ByteOffset != expectedOffset
                || !GetTextureFormatBlockLayout(description.TextureFormat, layout)
                || !CalculateTextureSubresourceStorage(description.TextureFormat, expectedExtent, tightRowPitch, tightByteSize)
                || subresource.RowPitchBytes < tightRowPitch)
                return false;

            const u64 blockRows = (static_cast<u64>(expectedExtent.Height) + layout.Height - 1) / layout.Height;
            if (blockRows > std::numeric_limits<u64>::max() / subresource.RowPitchBytes
                || subresource.ByteSize != subresource.RowPitchBytes * blockRows
                || subresource.ByteSize < tightByteSize
                || expectedOffset > std::numeric_limits<u64>::max() - subresource.ByteSize)
                return false;
            expectedOffset += subresource.ByteSize;
            expectedExtent.Width = expectedExtent.Width > 1 ? expectedExtent.Width / 2 : 1;
            expectedExtent.Height = expectedExtent.Height > 1 ? expectedExtent.Height / 2 : 1;
        }
        return expectedOffset <= std::numeric_limits<size_t>::max()
            && upload.Bytes->size() == static_cast<size_t>(expectedOffset);
    }

    class Texture
    {
    public:
        virtual ~Texture() = default;

        virtual const TextureDescription& GetDescription() const = 0;
    };
}
