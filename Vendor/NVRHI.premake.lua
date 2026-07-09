project "NVRHI"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"

    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "%{wks.location}/Vendor/NVRHI/include/nvrhi/nvrhi.h",
        "%{wks.location}/Vendor/NVRHI/include/nvrhi/nvrhiHLSL.h",
        "%{wks.location}/Vendor/NVRHI/include/nvrhi/utils.h",
        "%{wks.location}/Vendor/NVRHI/include/nvrhi/validation.h",
        "%{wks.location}/Vendor/NVRHI/include/nvrhi/common/containers.h",
        "%{wks.location}/Vendor/NVRHI/include/nvrhi/common/misc.h",
        "%{wks.location}/Vendor/NVRHI/include/nvrhi/common/resource.h",
        "%{wks.location}/Vendor/NVRHI/include/nvrhi/common/aftermath.h",
        "%{wks.location}/Vendor/NVRHI/src/common/format-info.cpp",
        "%{wks.location}/Vendor/NVRHI/src/common/misc.cpp",
        "%{wks.location}/Vendor/NVRHI/src/common/state-tracking.cpp",
        "%{wks.location}/Vendor/NVRHI/src/common/state-tracking.h",
        "%{wks.location}/Vendor/NVRHI/src/common/utils.cpp",
        "%{wks.location}/Vendor/NVRHI/src/common/aftermath.cpp",
        "%{wks.location}/Vendor/NVRHI/src/validation/validation-commandlist.cpp",
        "%{wks.location}/Vendor/NVRHI/src/validation/validation-device.cpp",
        "%{wks.location}/Vendor/NVRHI/src/validation/validation-backend.h"
    }

    includedirs
    {
        "%{wks.location}/Vendor/NVRHI/include"
    }

    defines
    {
        "_CRT_SECURE_NO_WARNINGS",
        "NVRHI_WITH_AFTERMATH=0"
    }

    if has_vulkan_headers then
        includedirs
        {
            "%{wks.location}/Vendor/Vulkan-Headers/include"
        }

        defines
        {
            "GE_HAS_NVRHI_VULKAN=1"
        }

        files
        {
            "%{wks.location}/Vendor/NVRHI/include/nvrhi/vulkan.h",
            "%{wks.location}/Vendor/NVRHI/src/common/versioning.h",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-allocator.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-buffer.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-commandlist.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-compute.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-constants.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-device.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-graphics.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-meshlets.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-queries.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-queue.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-raytracing.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-resource-bindings.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-shader.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-staging-texture.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-state-tracking.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-texture.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-upload.cpp",
            "%{wks.location}/Vendor/NVRHI/src/vulkan/vulkan-backend.h"
        }
    end

    filter "system:windows"
        systemversion "latest"
        defines { "GE_PLATFORM_WINDOWS" }
        if has_vulkan_headers then
            defines { "VK_USE_PLATFORM_WIN32_KHR", "NOMINMAX" }
        end

    filter { "system:windows", "action:vs*" }
        buildoptions { "/Zc:preprocessor" }
        editandcontinue "Off"

        if has_directx_headers then
            includedirs
            {
                "%{wks.location}/Vendor/DirectX-Headers/include",
                "%{wks.location}/Vendor/DirectX-Headers/include/directx",
                "%{wks.location}/Vendor/DirectX-Headers/include/dxguids"
            }

            defines
            {
                "GE_HAS_NVRHI_D3D12=1",
                "NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=0",
                "NVRHI_D3D12_WITH_NVAPI=0"
            }

            files
            {
                "%{wks.location}/Vendor/NVRHI/include/nvrhi/d3d12.h",
                "%{wks.location}/Vendor/NVRHI/src/common/dxgi-format.h",
                "%{wks.location}/Vendor/NVRHI/src/common/dxgi-format.cpp",
                "%{wks.location}/Vendor/NVRHI/src/common/versioning.h",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-backend.h",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-buffer.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-commandlist.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-compute.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-constants.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-descriptor-heap.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-device.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-graphics.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-meshlets.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-queries.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-raytracing.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-resource-bindings.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-shader.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-state-tracking.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-texture.cpp",
                "%{wks.location}/Vendor/NVRHI/src/d3d12/d3d12-upload.cpp"
            }
        end

    filter "system:linux"
        defines { "GE_PLATFORM_LINUX" }

    filter "system:macosx"
        defines { "GE_PLATFORM_MACOS" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
