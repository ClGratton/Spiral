#pragma once

#include "Engine/RHI/RHICommon.h"

#include <string>

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

    class Texture
    {
    public:
        virtual ~Texture() = default;

        virtual const TextureDescription& GetDescription() const = 0;
    };
}
