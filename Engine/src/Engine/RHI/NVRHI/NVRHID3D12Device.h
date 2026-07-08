#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Device.h"
#include "Engine/RHI/NVRHI/NVRHIAdapter.h"

namespace Engine::RHI
{
    struct NVRHID3D12NativeHandles
    {
        void* Factory = nullptr;
        void* Device = nullptr;
        void* GraphicsQueue = nullptr;
        void* ComputeQueue = nullptr;
        void* CopyQueue = nullptr;
        void* NVRHIDevice = nullptr;
    };

    struct NVRHID3D12BufferNativeHandles
    {
        void* Resource = nullptr;
    };

    struct NVRHID3D12TextureNativeHandles
    {
        void* Resource = nullptr;
    };

    Scope<Device> CreateNVRHID3D12Device(DeviceDescription description, NVRHIAdapterInfo& adapterInfo, NVRHID3D12NativeHandles* nativeHandles = nullptr);
    NVRHID3D12BufferNativeHandles GetNVRHID3D12BufferNativeHandles(Buffer& buffer);
    NVRHID3D12TextureNativeHandles GetNVRHID3D12TextureNativeHandles(Texture& texture);
}
