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
        "%{wks.location}/Vendor/miniz/miniz.c",
        "%{wks.location}/Vendor/miniz/miniz_tdef.c",
        "%{wks.location}/Vendor/miniz/miniz_tinfl.c",
        "%{wks.location}/Vendor/miniz/miniz_zip.c",
        "%{wks.location}/Vendor/zlib/adler32.c",
        "%{wks.location}/Vendor/zlib/compress.c",
        "%{wks.location}/Vendor/zlib/crc32.c",
        "%{wks.location}/Vendor/zlib/deflate.c",
        "%{wks.location}/Vendor/zlib/infback.c",
        "%{wks.location}/Vendor/zlib/inffast.c",
        "%{wks.location}/Vendor/zlib/inflate.c",
        "%{wks.location}/Vendor/zlib/inftrees.c",
        "%{wks.location}/Vendor/zlib/trees.c",
        "%{wks.location}/Vendor/zlib/uncompr.c",
        "%{wks.location}/Vendor/zlib/zutil.c",
        "%{wks.location}/Vendor/libspng/spng.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jcomapi.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jdapimin.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jdatasrc.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jdhuff.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jdinput.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jdmarker.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jdmaster.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jdphuff.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jerror.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jmemmgr.c",
        "%{wks.location}/Vendor/libjpeg-turbo/jmemnobs.c",
        "%{wks.location}/Vendor/libjpeg-turbo/spiral_unsupported.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jdapistd-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jdcoefct-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jdcolor-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jddctmgr-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jdmainct-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jdmerge-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jdpostct-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jdsample-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jidctflt-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jidctfst-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jidctint-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jidctred-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jquant1-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jquant2-8.c",
        "%{wks.location}/Vendor/libjpeg-turbo/wrapper/jutils-8.c",
        "%{wks.location}/Vendor/KTX-Software/lib/basis_transcode.cpp",
        "%{wks.location}/Vendor/KTX-Software/lib/checkheader.c",
        "%{wks.location}/Vendor/KTX-Software/lib/filestream.c",
        "%{wks.location}/Vendor/KTX-Software/lib/glformat_str.c",
        "%{wks.location}/Vendor/KTX-Software/lib/hashlist.c",
        "%{wks.location}/Vendor/KTX-Software/lib/info.c",
        "%{wks.location}/Vendor/KTX-Software/lib/memstream.c",
        "%{wks.location}/Vendor/KTX-Software/lib/miniz_wrapper.cpp",
        "%{wks.location}/Vendor/KTX-Software/lib/strings.c",
        "%{wks.location}/Vendor/KTX-Software/lib/swap.c",
        "%{wks.location}/Vendor/KTX-Software/lib/texture.c",
        "%{wks.location}/Vendor/KTX-Software/lib/texture2.c",
        "%{wks.location}/Vendor/KTX-Software/lib/vkformat_check.c",
        "%{wks.location}/Vendor/KTX-Software/lib/vkformat_check_variant.c",
        "%{wks.location}/Vendor/KTX-Software/lib/vkformat_str.c",
        "%{wks.location}/Vendor/KTX-Software/lib/vkformat_typesize.c",
        "%{wks.location}/Vendor/KTX-Software/external/basisu/transcoder/basisu_transcoder.cpp",
        "%{wks.location}/Vendor/KTX-Software/external/basisu/zstd/zstd.c",
        "%{wks.location}/Vendor/KTX-Software/external/dfdutils/createdfd.c",
        "%{wks.location}/Vendor/KTX-Software/external/dfdutils/colourspaces.c",
        "%{wks.location}/Vendor/KTX-Software/external/dfdutils/interpretdfd.c",
        "%{wks.location}/Vendor/KTX-Software/external/dfdutils/printdfd.c",
        "%{wks.location}/Vendor/KTX-Software/external/dfdutils/queries.c",
        "%{wks.location}/Vendor/KTX-Software/external/dfdutils/vk2dfd.c",
        "Shaders/**.hlsl"
    }

    includedirs
    {
        "src",
        "%{wks.location}/Vendor/GLFW/include",
        "%{wks.location}/Vendor/ImGui",
        "%{wks.location}/Vendor/ImGui/backends",
        "%{wks.location}/Vendor/cgltf",
        "%{wks.location}/Vendor/KTX-Software/include",
        "%{wks.location}/Vendor/KTX-Software/lib",
        "%{wks.location}/Vendor/KTX-Software/external",
        "%{wks.location}/Vendor/KTX-Software/external/dfdutils",
        "%{wks.location}/Vendor/KTX-Software/external/basisu/transcoder",
        "%{wks.location}/Vendor/KTX-Software/external/basisu/zstd",
        "%{wks.location}/Vendor/KTX-Software/other_include",
        "%{wks.location}/Vendor/KTX-Software/utils"
        , "%{wks.location}/Vendor/miniz"
        , "%{wks.location}/Vendor/zlib"
        , "%{wks.location}/Vendor/libspng"
        , "%{wks.location}/Vendor/libjpeg-turbo"
    }

    links
    {
        "GLFW",
        "ImGui"
    }

    -- These values come from the same admitted pin ledger consumed by the
    -- fetch scripts. Runtime cache identity must use the archive digests, not
    -- a manually duplicated version-only label.
    defines(shader_toolchain_defines)

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

    filter "system:windows"
        includedirs { "%{wks.location}/" .. slang_root .. "/windows-x86_64/include" }
        libdirs { "%{wks.location}/" .. slang_root .. "/windows-x86_64/lib" }
        links { "slang", "Shell32", "Advapi32" }

    filter "system:linux"
        includedirs { "%{wks.location}/" .. slang_root .. "/linux-x86_64/include" }
        libdirs { "%{wks.location}/" .. slang_root .. "/linux-x86_64/lib" }
        links { "slang" }

    filter "system:macosx"
        includedirs { "%{wks.location}/" .. slang_root .. "/macos-x86_64/include" }
        libdirs { "%{wks.location}/" .. slang_root .. "/macos-x86_64/lib" }
        links { "slang" }

    filter {}

    defines
    {
        "_CRT_SECURE_NO_WARNINGS",
        "LIBKTX",
        "KHRONOS_STATIC",
        "KTX_FEATURE_KTX2",
        "KTX_OMIT_VULKAN=1",
        "SUPPORT_SOFTWARE_ETC_UNPACK=0",
        "BASISD_SUPPORT_KTX2=1",
        "BASISD_SUPPORT_KTX2_ZSTD=1",
        "BASISD_SUPPORT_DXT1=0",
        "BASISD_SUPPORT_DXT5A=1",
        "BASISD_SUPPORT_BC7=1",
        "BASISD_SUPPORT_BC7_MODE5=1",
        "BASISD_SUPPORT_UASTC=1",
        "BASISD_SUPPORT_ASTC=1",
        "BASISD_SUPPORT_ATC=0",
        "BASISD_SUPPORT_PVRTC1=0",
        "BASISD_SUPPORT_PVRTC2=0",
        "BASISD_SUPPORT_ETC2_EAC_A8=0",
        "BASISD_SUPPORT_ETC2_EAC_RG11=0",
        "BASISD_SUPPORT_FXT1=0"
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

    filter "files:../Vendor/KTX-Software/** or ../Vendor/miniz/** or ../Vendor/zlib/** or ../Vendor/libspng/** or ../Vendor/libjpeg-turbo/**"
        warnings "Off"

    filter "files:../Vendor/zlib/**"
        defines { "Z_SOLO" }

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

    apply_spiral_sanitizers(false)

    -- The admitted sanitizer oracle covers project-owned Engine code. KTX is
    -- pinned third-party source compiled into this static library, so keep it
    -- outside instrumentation just like the separate GLFW/ImGui/NVRHI vendor
    -- projects while retaining sanitizer linkage for Engine consumers.
    if _OPTIONS["sanitize"] == "address-undefined" then
        filter { "toolset:clang", "files:../Vendor/KTX-Software/**" }
            buildoptions { "-fno-sanitize=address,undefined" }
        filter {}
    end
