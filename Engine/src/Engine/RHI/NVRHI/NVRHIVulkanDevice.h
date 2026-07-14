#pragma once

#include "Engine/RHI/Device.h"

namespace nvrhi
{
    class IDevice;
    namespace vulkan { class IDevice; }
}

namespace Engine::RHI
{
    // Borrowed native image/view for the documented Vulkan presentation bridge.
    // They remain valid only while the renderer-owned texture remains alive.
    struct NVRHIVulkanTextureNativeHandles
    {
        void* Image = nullptr;
        void* ImageView = nullptr;
    };

    // Wraps the NVRHI device created by NVRHIVulkanContext. It never creates a
    // Vulkan instance, logical device, queue, or native command buffer itself.
    Scope<Device> CreateNVRHIVulkanDevice(
        DeviceDescription description,
        const DeviceCapabilities& capabilities,
        nvrhi::IDevice* nativeDevice,
        nvrhi::vulkan::IDevice* completionDevice);
    NVRHIVulkanTextureNativeHandles GetNVRHIVulkanTextureNativeHandles(Texture& texture);
}
