project "Sandbox"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"

    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "src/**.h",
        "src/**.cpp"
    }

    includedirs
    {
        "%{wks.location}/Engine/src",
        "%{wks.location}/Vendor/ImGui",
        "%{wks.location}/Vendor/ImGui/backends"
    }

    links
    {
        "Engine",
        "GLFW",
        "ImGui"
    }

    if has_nvrhi then
        links { "NVRHI" }
    end

    filter "system:windows"
        libdirs { "%{wks.location}/" .. slang_root .. "/windows-x86_64/lib" }
        links { "slang" }
        postbuildcommands { 'powershell -ExecutionPolicy Bypass -NoProfile -File "' .. workspace_root .. '/Scripts/StageSlangRuntime.ps1" -Source "' .. workspace_root .. '/' .. slang_root .. '/windows-x86_64" -DxcSource "' .. workspace_root .. '/' .. dxc_root .. '/windows-x86_64" -Destination "%{cfg.targetdir}"' }

    filter "system:linux"
        libdirs { "%{wks.location}/" .. slang_root .. "/linux-x86_64/lib" }
        links { "slang" }
        postbuildcommands { 'bash "' .. workspace_root .. '/Scripts/StageSlangRuntime.sh" "' .. workspace_root .. '/' .. slang_root .. '/linux-x86_64" "%{cfg.targetdir}"' }

    filter "system:macosx"
        libdirs { "%{wks.location}/" .. slang_root .. "/macos-x86_64/lib" }
        links { "slang" }
        postbuildcommands { 'bash "' .. workspace_root .. '/Scripts/StageSlangRuntime.sh" "' .. workspace_root .. '/' .. slang_root .. '/macos-x86_64" "%{cfg.targetdir}"' }

    filter {}

    filter "system:windows"
        systemversion "latest"
        defines { "GE_PLATFORM_WINDOWS" }
        links { "gdi32", "user32", "shell32", "opengl32", "d3d12", "dxgi", "dxguid" }

    filter "action:vs*"
        buildoptions { "/Zc:preprocessor" }
        editandcontinue "Off"
        incrementallink "Off"

    filter "system:linux"
        defines { "GE_PLATFORM_LINUX" }
        links { "X11", "pthread", "dl", "m", "GL" }

    filter "system:macosx"
        defines { "GE_PLATFORM_MACOS" }
        links { "Cocoa.framework", "IOKit.framework", "CoreFoundation.framework", "OpenGL.framework" }

    filter "toolset:gcc or toolset:clang"
        buildoptions { "-Wall", "-Wextra", "-Wpedantic" }

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
