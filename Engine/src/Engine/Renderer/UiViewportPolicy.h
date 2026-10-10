#pragma once

#include "Engine/Core/Base.h"

#include <string>
#include <vector>

namespace Engine
{
    // Pure decision tables for detachable OS-window UI panels (Dear ImGui
    // multi-viewport). Nothing here touches ImGui, GLFW or Vulkan, so the
    // tables are exercised by headless tests; the native half lives in
    // NVRHIVulkanPresentation and Engine/UI/ImGuiLayer.
    //
    // The feature is OFF unless explicitly requested (--ui-viewports or
    // ImGuiLayer::SetViewportsRequested) and every denial carries a reason that
    // the Editor can show next to a disabled control.

    // Vulkan VkPresentModeKHR values from the Vulkan specification, kept as
    // integers so this header stays Vulkan-free. NVRHIVulkanPresentation.cpp
    // static_asserts that they equal the real enumerators.
    namespace UiViewportPresentModeValue
    {
        constexpr int Immediate = 0;
        constexpr int Mailbox = 1;
        constexpr int Fifo = 2;
        constexpr int FifoRelaxed = 3;
    }

    // Present mode a secondary (detached-window) swapchain may use. FIFO and
    // FIFO_RELAXED are never allowed: measured on the X11/XWayland target, a FIFO
    // secondary swapchain whose window is not visible blocks the whole process
    // in vkAcquireNextImageKHR(UINT64_MAX), and a visible one caps the entire
    // application at that window's refresh rate.
    enum class UiViewportSecondaryMode : u8
    {
        None,
        Immediate,
        Mailbox
    };

    inline const char* ToString(UiViewportSecondaryMode mode)
    {
        switch (mode)
        {
            case UiViewportSecondaryMode::Immediate: return "IMMEDIATE";
            case UiViewportSecondaryMode::Mailbox: return "MAILBOX";
            default: return "none";
        }
    }

    // True only for MAILBOX and IMMEDIATE.
    bool IsUiViewportSecondaryPresentModeAllowed(int presentMode);

    // Best allowed mode in `supportedModes` (MAILBOX, then IMMEDIATE; the same
    // order as the Dear ImGui Vulkan backend, minus its FIFO fallback), or None.
    UiViewportSecondaryMode SelectUiViewportSecondaryMode(const std::vector<int>& supportedModes);

    enum class UiViewportSecondaryAction : u8
    {
        // The backend's own choice is allowed and supported: leave it alone.
        Keep,
        // The backend chose a blocking mode although an allowed one exists:
        // recreate the swapchain with `Mode`.
        Override,
        // No allowed mode: never acquire from or present to this window.
        Skip
    };

    struct UiViewportSecondaryResolution
    {
        UiViewportSecondaryAction Action = UiViewportSecondaryAction::Skip;
        int Mode = UiViewportPresentModeValue::Fifo;
    };

    // `backendChosen` is the mode the Dear ImGui Vulkan backend created the
    // secondary swapchain with; `supportedModes` are the modes that window's own
    // surface reports (secondary surfaces are not guaranteed to match the main one).
    UiViewportSecondaryResolution ResolveUiViewportSecondaryMode(int backendChosen, const std::vector<int>& supportedModes);

    enum class UiViewportRendererKind : u8
    {
        // OpenGL2 fallback or no native renderer.
        None,
        Vulkan,
        D3D12
    };

    enum class UiViewportPlatformKind : u8
    {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
        Null
    };

    inline const char* ToString(UiViewportRendererKind kind)
    {
        switch (kind)
        {
            case UiViewportRendererKind::Vulkan: return "Vulkan";
            case UiViewportRendererKind::D3D12: return "D3D12";
            default: return "none";
        }
    }

    inline const char* ToString(UiViewportPlatformKind kind)
    {
        switch (kind)
        {
            case UiViewportPlatformKind::Win32: return "Win32";
            case UiViewportPlatformKind::X11: return "X11";
            case UiViewportPlatformKind::Wayland: return "Wayland";
            case UiViewportPlatformKind::Cocoa: return "Cocoa";
            case UiViewportPlatformKind::Null: return "Null";
            default: return "Unknown";
        }
    }

    struct UiViewportCapabilityInput
    {
        bool Requested = false;
        UiViewportRendererKind Renderer = UiViewportRendererKind::None;
        UiViewportPlatformKind Platform = UiViewportPlatformKind::Unknown;
        // ImGuiBackendFlags_PlatformHasViewports / RendererHasViewports after
        // both backends were initialised.
        bool PlatformBackendHasViewports = false;
        bool RendererBackendHasViewports = false;
        // Present modes of the main surface; false when the query failed.
        bool SurfaceModesKnown = false;
        std::vector<int> SurfacePresentModes;
    };

    enum class UiViewportReason : u8
    {
        Enabled,
        NotRequested,
        NativeRendererUnavailable,
        RendererNotImplemented,
        PlatformWayland,
        PlatformUnsupported,
        PlatformBackendLacksViewports,
        RendererBackendLacksViewports,
        PresentModesUnknown,
        NoNonBlockingPresentMode,
        // Decided "enabled" but the renderer could not install its safe handlers.
        RendererHandlersUnavailable
    };

    // Stable kebab-case identifier used in log markers and tests.
    const char* ToString(UiViewportReason reason);
    // One sentence the Editor shows beside the disabled control.
    const char* Describe(UiViewportReason reason);

    struct UiViewportDecision
    {
        bool Enabled = false;
        UiViewportReason Reason = UiViewportReason::NotRequested;
    };

    // First failing row wins, in this order: not requested; no native renderer
    // (OpenGL2 fallback); renderer not implemented (D3D12); platform (Wayland,
    // then anything other than X11/Win32); platform backend flag; renderer
    // backend flag; unknown present modes; no MAILBOX/IMMEDIATE.
    UiViewportDecision DecideUiViewports(const UiViewportCapabilityInput& input);

    // Dear ImGui's Vulkan backend runs vkDeviceWaitIdle inside every secondary
    // swapchain create, resize and destroy, on the main thread. On an idle GPU
    // that measured 30-46 ms for a create and about 3 ms for a per-frame resize;
    // with GPU work in flight the wait lasts until the queue drains, so the cost is
    // bounded only by the GPU frame time. An operation above this threshold is
    // counted and logged so a stall is attributable.
    constexpr double kUiViewportSlowOperationMilliseconds = 250.0;

    // Renderer-published state, read by the Editor. Every counter is cumulative
    // since renderer start unless its name says Last or Peak.
    struct UiViewportSecondaryInfo
    {
        u32 ViewportId = 0;
        int PresentMode = UiViewportPresentModeValue::Fifo;
        u32 Width = 0;
        u32 Height = 0;
        u32 ImageCount = 0;
        u64 FramesRendered = 0;
        bool Skipped = false;
        std::string SkipReason;
    };

    struct UiViewportDiagnostics
    {
        bool Requested = false;
        bool Enabled = false;
        UiViewportReason Reason = UiViewportReason::NotRequested;
        std::string Renderer = "none";
        std::string Platform = "Unknown";

        u32 SecondaryCount = 0;
        u32 PeakSecondaryCount = 0;
        std::vector<UiViewportSecondaryInfo> Secondaries;

        u64 SecondaryCreates = 0;
        u64 SecondaryCreateFailures = 0;
        u64 SecondaryResizes = 0;
        u64 SecondaryDestroys = 0;
        // Swapchains recreated because the backend chose a blocking mode.
        u64 PresentModeOverrides = 0;
        // Windows that were never rendered because no allowed mode, a different
        // surface format or an unsafe ring existed.
        u64 SkippedWindows = 0;
        u64 SkippedRenders = 0;
        u64 SecondaryFramesRendered = 0;
        // Each create/resize/destroy runs vkDeviceWaitIdle inside the backend on
        // the main thread, so these are GPU-drain costs, not CPU-only costs.
        double LastCreateMilliseconds = 0.0;
        double MaxCreateMilliseconds = 0.0;
        double LastResizeMilliseconds = 0.0;
        double MaxResizeMilliseconds = 0.0;
        double LastDestroyMilliseconds = 0.0;
        double MaxDestroyMilliseconds = 0.0;
        // Create/resize/destroy calls that took longer than kUiViewportSlowOperationMilliseconds.
        u64 SlowOperations = 0;
        // CPU time of the most recent frame's secondary pass.
        double LastUpdateMilliseconds = 0.0;
        double LastRenderMilliseconds = 0.0;
        double LastSwapMilliseconds = 0.0;
        // Secondary submissions entered into the presentation serial ledger, and
        // polls where a secondary fence (not the main one) held the completed
        // serial back. Retirement of UI textures depends on both.
        u64 SecondarySubmissionsTracked = 0;
        u64 SerialPollsHeldBySecondary = 0;
        // Negative VkResult values reported by the Dear ImGui Vulkan backend.
        u64 BackendErrors = 0;
    };
}
