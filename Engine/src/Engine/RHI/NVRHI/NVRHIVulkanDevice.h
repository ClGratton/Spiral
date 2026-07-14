#pragma once

#include "Engine/RHI/Device.h"

namespace nvrhi { class IDevice; }

namespace Engine::RHI
{
    // Wraps the NVRHI device created by NVRHIVulkanContext. It never creates a
    // Vulkan instance, logical device, queue, or native command buffer itself.
    Scope<Device> CreateNVRHIVulkanDevice(
        DeviceDescription description,
        const DeviceCapabilities& capabilities,
        nvrhi::IDevice* nativeDevice);
}
