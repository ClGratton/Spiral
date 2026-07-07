#pragma once

#include "Engine/RHI/Texture.h"

namespace Engine::RHI
{
    struct SwapchainDescription
    {
        Extent2D Extent;
        Format ColorFormat = Format::R8G8B8A8Unorm;
        bool VSync = true;
    };

    class Swapchain
    {
    public:
        virtual ~Swapchain() = default;

        virtual const SwapchainDescription& GetDescription() const = 0;
        virtual void Resize(Extent2D extent) = 0;
        virtual Texture* GetCurrentBackBuffer() = 0;
        virtual void Present() = 0;
    };
}
