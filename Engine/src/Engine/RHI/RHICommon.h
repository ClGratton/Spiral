#pragma once

#include "Engine/Core/Base.h"

#include <string>

namespace Engine::RHI
{
    enum class Backend
    {
        None,
        NVRHICommon,
        NVRHID3D12,
        NVRHIVulkan,
        NRI,
        Metal
    };

    enum class Format
    {
        Unknown,
        R8Unorm,
        R8G8B8A8Unorm,
        R8G8B8A8UnormSrgb,
        R11G11B10Float,
        R16G16B16A16Float,
        R32Uint,
        D24UnormS8Uint,
        D32Float
    };

    enum class QueueType
    {
        Graphics,
        Compute,
        Copy
    };

    enum class ResourceState
    {
        Unknown,
        Common,
        RenderTarget,
        DepthWrite,
        ShaderResource,
        UnorderedAccess,
        CopySource,
        CopyDest,
        Present
    };

    enum class ShaderStage : u32
    {
        None = 0,
        Vertex = 1u << 0u,
        Pixel = 1u << 1u,
        Compute = 1u << 2u,
        RayGeneration = 1u << 3u,
        Miss = 1u << 4u,
        ClosestHit = 1u << 5u,
        AnyHit = 1u << 6u,
        AllGraphics = Vertex | Pixel,
        All = 0xFFFFFFFFu
    };

    inline const char* ToString(Backend backend)
    {
        switch (backend)
        {
            case Backend::None: return "None";
            case Backend::NVRHICommon: return "NVRHI Common";
            case Backend::NVRHID3D12: return "NVRHI D3D12";
            case Backend::NVRHIVulkan: return "NVRHI Vulkan";
            case Backend::NRI: return "NRI";
            case Backend::Metal: return "Metal";
        }

        return "Unknown";
    }

    struct DeviceDescription
    {
        Backend RequestedBackend = Backend::None;
        bool EnableValidation = true;
        bool EnableRayTracing = false;
        bool EnableWorkGraphs = false;
    };

    struct Extent2D
    {
        u32 Width = 0;
        u32 Height = 0;
    };
}
