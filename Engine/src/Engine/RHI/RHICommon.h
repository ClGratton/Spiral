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
        R32G32Float,
        R32G32B32Float,
        R32G32B32A32Float,
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

    inline const char* ToString(Format format)
    {
        switch (format)
        {
            case Format::Unknown: return "Unknown";
            case Format::R8Unorm: return "R8_UNORM";
            case Format::R8G8B8A8Unorm: return "RGBA8_UNORM";
            case Format::R8G8B8A8UnormSrgb: return "RGBA8_UNORM_SRGB";
            case Format::R11G11B10Float: return "R11G11B10_FLOAT";
            case Format::R16G16B16A16Float: return "RGBA16_FLOAT";
            case Format::R32G32Float: return "RG32_FLOAT";
            case Format::R32G32B32Float: return "RGB32_FLOAT";
            case Format::R32G32B32A32Float: return "RGBA32_FLOAT";
            case Format::R32Uint: return "R32_UINT";
            case Format::D24UnormS8Uint: return "D24_UNORM_S8_UINT";
            case Format::D32Float: return "D32_FLOAT";
        }
        return "Unknown";
    }

    struct DeviceDescription
    {
        Backend RequestedBackend = Backend::None;
        bool EnableValidation = true;
        bool EnableRayTracing = false;
        bool EnableWorkGraphs = false;
        std::string PreferredAdapterName;
        bool RequirePreferredAdapter = false;
    };

    struct Extent2D
    {
        u32 Width = 0;
        u32 Height = 0;
    };
}
