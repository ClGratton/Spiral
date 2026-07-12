#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Device.h"
#include "Engine/RHI/NVRHI/NVRHIAdapter.h"

namespace Engine::RHI
{
    struct NVRHIVulkanNativeHandles
    {
        void* Instance = nullptr;
        void* PhysicalDevice = nullptr;
        void* Device = nullptr;
        void* Surface = nullptr;
        void* GraphicsQueue = nullptr;
        void* NVRHIDevice = nullptr;
        u32 GraphicsQueueFamily = 0;
    };

    class NVRHIVulkanContext final
    {
    public:
        NVRHIVulkanContext();
        ~NVRHIVulkanContext();

        bool Initialize(void* nativeWindow, const DeviceDescription& description, NVRHIAdapterInfo& adapterInfo);
        void Shutdown();
        void WaitIdle();

        bool IsInitialized() const;
        const NVRHIVulkanNativeHandles& GetNativeHandles() const;
        const DeviceCapabilities& GetCapabilities() const;
        void* GetInstanceProcAddress(const char* name) const;

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
}
