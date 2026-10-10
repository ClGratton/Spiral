project "EngineTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"

    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "src/**.cpp",
        -- Pure Editor-private Fab browser core (plain types, no CEF/ImGui/GLFW/RHI),
        -- compiled here so it is tested without CEF ever entering Engine.
        "../Editor/src/Fab/BrowserDownloadPolicy.cpp",
        "../Editor/src/Fab/BrowserFrameMirror.cpp",
        "../Editor/src/Fab/BrowserInputRouter.cpp",
        "../Editor/src/Fab/BrowserPanelCore.cpp",
        "../Editor/src/Fab/BrowserNavigationPolicy.cpp",
        "../Editor/src/Fab/BrowserSignInHosts.cpp",
        "../Editor/src/Fab/BrowserSurface.cpp",
        "../Editor/src/Fab/FabDisclosure.cpp",
        "../Editor/src/Fab/FabImportController.cpp",
        "../Editor/src/Fab/FabIntake.cpp",
        "../Editor/src/Fab/FabLicenseGate.cpp",
        "../Editor/src/Fab/NullBrowserSurface.cpp",
        -- Pure Editor-private History core (plain types, no ImGui/GLFW/Scene/RHI).
        "../Editor/src/History/EditGesture.cpp",
        "../Editor/src/History/HistoryLabel.cpp",
        "../Editor/src/History/ShortcutDispatch.cpp",
        "../Editor/src/History/HistoryNaming.cpp",
        -- Project-state snapshot for undo: equality, estimate, camera-edit test (Engine types only).
        "../Editor/src/EditorHistoryState.cpp",
        -- Pure Editor-private viewport picking and framing math (Engine::Math only).
        "../Editor/src/Viewport/PickingMath.cpp",
        -- Pure Editor-private gizmo session (tool/space/snap state, commands, pointer
        -- ownership, status text) over the Gizmo state machine; no ImGui or GLFW.
        "../Editor/src/Viewport/GizmoSession.cpp",
        -- Pure Editor-private panel visibility codec (standard C++ only).
        "../Editor/src/Layout/PanelVisibility.cpp",
        "../Editor/src/Commands/CommandRegistry.cpp",
        "../Editor/src/Commands/FuzzyMatch.cpp",
        "../Editor/src/Commands/ShortcutMap.cpp",
        "../Editor/src/Core/LogBuffer.cpp",
        "../Editor/src/Core/Notifications.cpp",
        "../Editor/src/Core/EntityClipboard.cpp",
        "../Editor/src/Core/EntityNaming.cpp",
        "../Editor/src/Core/HierarchyModel.cpp",
        "../Editor/src/Core/SelectionModel.cpp",
        "../Editor/src/Layout/LayoutFile.cpp",
        "../Editor/src/Layout/LayoutMigration.cpp",
        "../Editor/src/Gizmo/EulerRotation.cpp",
        "../Editor/src/Gizmo/GizmoApply.cpp",
        "../Editor/src/Gizmo/GizmoHandles.cpp",
        "../Editor/src/Gizmo/GizmoProjection.cpp",
        "../Editor/src/Gizmo/GizmoSolvers.cpp",
        "../Editor/src/Gizmo/SnapMath.cpp",
        "../Editor/src/Gizmo/SnapSettings.cpp",
        "../Editor/src/Gizmo/TransformGizmo.cpp"
    }

    removefiles { "src/EngineFuzzTests.cpp" }

    includedirs
    {
        "%{wks.location}/Engine/src",
        "%{wks.location}/Editor/src",
        "%{wks.location}/Editor/src/Fab",
        "%{wks.location}/Editor/src/History",
        "%{wks.location}/Editor/src/Core",
        "%{wks.location}/Editor/src/Gizmo",
        "%{wks.location}/Editor/src/Viewport",
        "%{wks.location}/Editor/src/Layout",
        "%{wks.location}/Vendor/miniz"
    }

    links
    {
        "Engine"
    }

    -- Engine is a static library, so the executable that runs the Slang adapter
    -- tests links and stages the pinned compiler explicitly.
    filter "system:windows"
        libdirs { "%{wks.location}/" .. slang_root .. "/windows-x86_64/lib" }
        links { "slang" }
        postbuildcommands { 'powershell -ExecutionPolicy Bypass -NoProfile -File "' .. workspace_root .. '/Scripts/StageSlangRuntime.ps1" -Source "' .. workspace_root .. '/' .. slang_root .. '/windows-x86_64" -DxcSource "' .. workspace_root .. '/' .. dxc_root .. '/windows-x86_64" -Destination "%{cfg.targetdir}"' }

    filter "system:linux"
        libdirs { "%{wks.location}/" .. slang_root .. "/linux-x86_64/lib" }
        links { "slang" }
        linkoptions { "-Wl,-rpath,'$$ORIGIN'" }
        postbuildcommands { 'bash "' .. workspace_root .. '/Scripts/StageSlangRuntime.sh" "' .. workspace_root .. '/' .. slang_root .. '/linux-x86_64" "%{cfg.targetdir}"' }

    filter "system:macosx"
        libdirs { "%{wks.location}/" .. slang_root .. "/macos-x86_64/lib" }
        links { "slang" }
        linkoptions { "-Wl,-rpath,@loader_path" }
        postbuildcommands { 'bash "' .. workspace_root .. '/Scripts/StageSlangRuntime.sh" "' .. workspace_root .. '/' .. slang_root .. '/macos-x86_64" "%{cfg.targetdir}"' }

    filter {}

    filter "system:windows"
        systemversion "latest"
        defines { "GE_PLATFORM_WINDOWS" }

    filter { "system:windows", "action:vs*" }
        buildoptions { "/bigobj" }

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

    apply_spiral_sanitizers(false)

project "EngineFuzzTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "off"

    targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
    objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")
    files { "src/EngineFuzzTests.cpp" }
    includedirs { "%{wks.location}/Engine/src" }
    links { "Engine" }

    filter "system:windows"
        systemversion "latest"
        defines { "GE_PLATFORM_WINDOWS" }
        libdirs { "%{wks.location}/" .. slang_root .. "/windows-x86_64/lib" }
        links { "slang" }
        postbuildcommands { 'powershell -ExecutionPolicy Bypass -NoProfile -File "' .. workspace_root .. '/Scripts/StageSlangRuntime.ps1" -Source "' .. workspace_root .. '/' .. slang_root .. '/windows-x86_64" -DxcSource "' .. workspace_root .. '/' .. dxc_root .. '/windows-x86_64" -Destination "%{cfg.targetdir}"' }

    filter "system:linux"
        defines { "GE_PLATFORM_LINUX" }
        links { "pthread", "dl" }
        libdirs { "%{wks.location}/" .. slang_root .. "/linux-x86_64/lib" }
        links { "slang" }
        linkoptions { "-Wl,-rpath,'$$ORIGIN'" }
        postbuildcommands { 'bash "' .. workspace_root .. '/Scripts/StageSlangRuntime.sh" "' .. workspace_root .. '/' .. slang_root .. '/linux-x86_64" "%{cfg.targetdir}"' }

    filter "system:macosx"
        defines { "GE_PLATFORM_MACOS" }
        libdirs { "%{wks.location}/" .. slang_root .. "/macos-x86_64/lib" }
        links { "slang" }
        linkoptions { "-Wl,-rpath,@loader_path" }

    filter "toolset:gcc or toolset:clang"
        buildoptions { "-Wall", "-Wextra", "-Wpedantic" }

    filter "configurations:Debug"
        defines { "GE_DEBUG", "GE_ENABLE_ASSERTS" }
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

    apply_spiral_sanitizers(true)
