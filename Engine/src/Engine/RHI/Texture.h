#pragma once

#include "Engine/RHI/RHICommon.h"

#include <limits>
#include <string>
#include <vector>

namespace Engine::RHI
{
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

    class Texture
    {
    public:
        virtual ~Texture() = default;

        virtual const TextureDescription& GetDescription() const = 0;
    };
}
