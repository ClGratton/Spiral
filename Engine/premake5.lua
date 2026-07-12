project "Engine"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"

    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "src/**.h",
        "src/**.cpp",
        "Shaders/**.hlsl"
    }

    includedirs
    {
        "src",
        "%{wks.location}/Vendor/GLFW/include",
        "%{wks.location}/Vendor/ImGui",
        "%{wks.location}/Vendor/ImGui/backends",
        "%{wks.location}/Vendor/cgltf"
    }

    links
    {
        "GLFW",
        "ImGui"
    }

    if has_nvrhi then
        includedirs { "%{wks.location}/Vendor/NVRHI/include" }
        links { "NVRHI" }
        defines { "GE_HAS_NVRHI=1" }
    end

    if has_vulkan_headers then
        includedirs { "%{wks.location}/Vendor/Vulkan-Headers/include" }
        defines { "GE_HAS_VULKAN_HEADERS=1", "VK_NO_PROTOTYPES" }
    end

    if has_nvrhi and has_vulkan_headers then
        defines { "GE_HAS_NVRHI_VULKAN=1" }
    end

    if has_directx_headers then
        includedirs
        {
            "%{wks.location}/Vendor/DirectX-Headers/include",
            "%{wks.location}/Vendor/DirectX-Headers/include/directx",
            "%{wks.location}/Vendor/DirectX-Headers/include/dxguids"
        }
        defines { "GE_HAS_DIRECTX_HEADERS=1" }
    end

    defines
    {
        "_CRT_SECURE_NO_WARNINGS"
    }

    filter "system:windows"
        systemversion "latest"
        defines { "GE_PLATFORM_WINDOWS" }

    filter { "system:windows", "action:vs*" }
        if has_nvrhi and has_directx_headers then
            defines { "GE_HAS_NVRHI_D3D12=1" }
        end

    filter "action:vs*"
        buildoptions { "/Zc:preprocessor" }
        editandcontinue "Off"

    filter "system:linux"
        defines { "GE_PLATFORM_LINUX" }
        links { "X11", "pthread", "dl", "m", "GL" }

    filter "system:macosx"
        defines { "GE_PLATFORM_MACOS" }
        links { "Cocoa.framework", "IOKit.framework", "CoreFoundation.framework", "OpenGL.framework" }

    filter "toolset:gcc or toolset:clang"
        buildoptions { "-Wall", "-Wextra", "-Wpedantic" }

    filter "files:Shaders/**.hlsl"
        buildaction "None"

    filter "configurations:Debug"
        defines { "GE_DEBUG", "GE_ENABLE_ASSERTS", "GE_ENABLE_PROFILE" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "GE_RELEASE", "GE_ENABLE_ASSERTS" }
        runtime "Release"
        optimize "on"

    filter "configurations:Dist"
        defines { "GE_DIST" }
        runtime "Release"
        optimize "on"
