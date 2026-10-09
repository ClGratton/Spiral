-- The Chromium Embedded Framework adapter lives in a shared library that the
-- Editor loads with dlopen on first use of the Fab panel, and a CEF-free smoke
-- driver that loads it the same way. Included from Vendor/CEF.premake.lua because
-- it consumes the toolchain flags defined there; built only when has_cef is set.

project "SpiralBrowserHost"
    kind "SharedLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    pic "On"
    optimize "On"
    symbols "Off"

    -- Beside libcef.so, the helper and the staged runtime (Scripts/StageBrowserRuntime.sh)
    -- so one $ORIGIN resolves libcef and locates the resource files.
    targetdir ("%{wks.location}/bin/" .. outputdir .. "/Editor/cef")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    -- The pure Fab core and the one Engine source it composes are compiled in
    -- directly: the library must not link libEngine.a or any Engine global state.
    files
    {
        "BrowserHostExports.cpp",
        "../src/Fab/CefBrowserSurface.cpp",
        "../src/Fab/BrowserDownloadPolicy.cpp",
        "../src/Fab/BrowserNavigationPolicy.cpp",
        "../src/Fab/BrowserSurface.cpp",
        "%{wks.location}/Engine/src/Engine/Platform/ExternalUrl.cpp"
    }

    includedirs
    {
        "%{wks.location}/" .. cef_root,
        "%{wks.location}/Engine/src",
        "%{wks.location}/Editor/src/Fab"
    }

    defines(cef_toolchain_defines)
    buildoptions(cef_toolchain_buildoptions)
    buildoptions { "-Wextra", "-Wpedantic" }

    -- libcef must follow the wrapper archive that references it. -z defs turns an
    -- unresolved symbol into a link error instead of a dlopen failure, and
    -- --exclude-libs keeps the wrapper archive's symbols out of the export table.
    links { "CEFWrapper", "cef" }
    libdirs { "%{wks.location}/" .. cef_root .. "/Release" }
    linkoptions { "-Wl,-rpath,'$$ORIGIN'", "-Wl,-z,defs", "-Wl,--exclude-libs,ALL" }

project "SpiralBrowserSmoke"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"

    -- Two projects may not share one generated-makefile directory.
    location (path.join(workspace_root, "Editor/browserhost/smoke"))
    targetdir ("%{wks.location}/bin/" .. outputdir .. "/Editor")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "BrowserSmokeMain.cpp",
        "../src/Fab/BrowserDownloadPolicy.cpp",
        "../src/Fab/BrowserNavigationPolicy.cpp",
        "%{wks.location}/Engine/src/Engine/Platform/ExternalUrl.cpp"
    }

    includedirs
    {
        "%{wks.location}/Engine/src",
        "%{wks.location}/Editor/src/Fab",
        "%{wks.location}/Editor/browserhost"
    }

    -- No libcef and no CEF header: the library is reached only through dlopen.
    links { "dl", "pthread" }
    dependson { "SpiralBrowserHost" }

    filter "toolset:gcc or toolset:clang"
        buildoptions { "-Wall", "-Wextra", "-Wpedantic" }

    filter "configurations:Debug"
        symbols "on"

    filter "configurations:Release or Dist"
        optimize "on"
