project "SpiralBrowserHelper"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"
    optimize "On"
    symbols "Off"

    -- Beside the staged browser runtime (Scripts/StageBrowserRuntime.sh) so that
    -- libcef.so, its resources and this executable share one $ORIGIN, while the
    -- bundled libvulkan.so.1 cannot shadow the Editor's own loader lookup.
    targetdir ("%{wks.location}/bin/" .. outputdir .. "/Editor/cef")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files { "BrowserHelperMain.cpp" }

    includedirs { "%{wks.location}/" .. cef_root }

    defines(cef_toolchain_defines)
    buildoptions(cef_toolchain_buildoptions)

    -- libcef must follow the wrapper archive that references it, or a linker
    -- defaulting to --as-needed drops it.
    links { "CEFWrapper", "cef" }
    libdirs { "%{wks.location}/" .. cef_root .. "/Release" }
    linkoptions { "-Wl,-rpath,'$$ORIGIN'" }
