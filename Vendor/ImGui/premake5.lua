project "ImGui"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"

    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "imconfig.h",
        "imgui.h",
        "imgui.cpp",
        "imgui_draw.cpp",
        "imgui_tables.cpp",
        "imgui_widgets.cpp",
        "imgui_demo.cpp",
        "backends/imgui_impl_glfw.h",
        "backends/imgui_impl_glfw.cpp",
        "backends/imgui_impl_opengl2.h",
        "backends/imgui_impl_opengl2.cpp"
    }

    includedirs
    {
        ".",
        "backends",
        "%{wks.location}/Vendor/GLFW/include"
    }

    filter "system:windows"
        systemversion "latest"
        defines { "GE_PLATFORM_WINDOWS" }

    filter { "system:windows", "action:vs*" }
        if has_directx_headers then
            includedirs
            {
                "%{wks.location}/Vendor/DirectX-Headers/include",
                "%{wks.location}/Vendor/DirectX-Headers/include/directx"
            }

            files
            {
                "backends/imgui_impl_dx12.h",
                "backends/imgui_impl_dx12.cpp"
            }
        end

    filter "action:vs*"
        buildoptions { "/Zc:preprocessor" }
        editandcontinue "Off"

    filter "system:linux"
        defines { "GE_PLATFORM_LINUX" }

    filter "system:macosx"
        defines { "GE_PLATFORM_MACOS" }

    filter "toolset:gcc or toolset:clang"
        buildoptions { "-Wall", "-Wextra", "-Wpedantic" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
