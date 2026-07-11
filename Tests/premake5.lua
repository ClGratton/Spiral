project "EngineTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"

    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "src/**.cpp"
    }

    includedirs
    {
        "%{wks.location}/Engine/src"
    }

    links
    {
        "Engine"
    }

    filter "system:windows"
        systemversion "latest"
        defines { "GE_PLATFORM_WINDOWS" }

    filter "system:linux"
        defines { "GE_PLATFORM_LINUX" }
        links { "pthread", "dl" }

    filter "system:macosx"
        defines { "GE_PLATFORM_MACOS" }

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
