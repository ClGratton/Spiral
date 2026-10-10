#include "Engine/Renderer/UiViewportPolicy.h"

#include <algorithm>

namespace Engine
{
    namespace
    {
        bool Contains(const std::vector<int>& values, int value)
        {
            return std::find(values.begin(), values.end(), value) != values.end();
        }
    }

    bool IsUiViewportSecondaryPresentModeAllowed(int presentMode)
    {
        return presentMode == UiViewportPresentModeValue::Mailbox || presentMode == UiViewportPresentModeValue::Immediate;
    }

    UiViewportSecondaryMode SelectUiViewportSecondaryMode(const std::vector<int>& supportedModes)
    {
        if (Contains(supportedModes, UiViewportPresentModeValue::Mailbox))
            return UiViewportSecondaryMode::Mailbox;
        if (Contains(supportedModes, UiViewportPresentModeValue::Immediate))
            return UiViewportSecondaryMode::Immediate;
        return UiViewportSecondaryMode::None;
    }

    UiViewportSecondaryResolution ResolveUiViewportSecondaryMode(int backendChosen, const std::vector<int>& supportedModes)
    {
        if (IsUiViewportSecondaryPresentModeAllowed(backendChosen) && Contains(supportedModes, backendChosen))
            return { UiViewportSecondaryAction::Keep, backendChosen };
        switch (SelectUiViewportSecondaryMode(supportedModes))
        {
            case UiViewportSecondaryMode::Mailbox: return { UiViewportSecondaryAction::Override, UiViewportPresentModeValue::Mailbox };
            case UiViewportSecondaryMode::Immediate: return { UiViewportSecondaryAction::Override, UiViewportPresentModeValue::Immediate };
            default: return { UiViewportSecondaryAction::Skip, backendChosen };
        }
    }

    const char* ToString(UiViewportReason reason)
    {
        switch (reason)
        {
            case UiViewportReason::Enabled: return "enabled";
            case UiViewportReason::NotRequested: return "not-requested";
            case UiViewportReason::NativeRendererUnavailable: return "native-renderer-unavailable";
            case UiViewportReason::RendererNotImplemented: return "renderer-not-implemented";
            case UiViewportReason::PlatformWayland: return "platform-wayland";
            case UiViewportReason::PlatformUnsupported: return "platform-unsupported";
            case UiViewportReason::PlatformBackendLacksViewports: return "platform-backend-lacks-viewports";
            case UiViewportReason::RendererBackendLacksViewports: return "renderer-backend-lacks-viewports";
            case UiViewportReason::PresentModesUnknown: return "present-modes-unknown";
            case UiViewportReason::NoNonBlockingPresentMode: return "no-non-blocking-present-mode";
            case UiViewportReason::RendererHandlersUnavailable: return "renderer-handlers-unavailable";
        }
        return "unknown";
    }

    const char* Describe(UiViewportReason reason)
    {
        switch (reason)
        {
            case UiViewportReason::Enabled: return "Detachable panel windows are enabled.";
            case UiViewportReason::NotRequested: return "Detachable panel windows are off. Start with --ui-viewports to enable them.";
            case UiViewportReason::NativeRendererUnavailable:
                return "Detachable panel windows need the native Vulkan renderer; the OpenGL2 fallback does not support them.";
            case UiViewportReason::RendererNotImplemented:
                return "Detachable panel windows are not available on D3D12 yet; the backend needs verification on a Windows host.";
            case UiViewportReason::PlatformWayland:
                return "Detachable panel windows are unavailable on native Wayland windows; run the X11/XWayland build.";
            case UiViewportReason::PlatformUnsupported:
                return "Detachable panel windows are unavailable on this windowing platform.";
            case UiViewportReason::PlatformBackendLacksViewports:
                return "The Dear ImGui platform backend did not report multi-window support.";
            case UiViewportReason::RendererBackendLacksViewports:
                return "The Dear ImGui renderer backend did not report multi-window support.";
            case UiViewportReason::PresentModesUnknown:
                return "The Vulkan surface present modes could not be queried, so detached windows cannot be made safe.";
            case UiViewportReason::NoNonBlockingPresentMode:
                return "The Vulkan surface offers neither MAILBOX nor IMMEDIATE presentation. A FIFO-only detached window can stall the whole application when it is hidden.";
            case UiViewportReason::RendererHandlersUnavailable:
                return "The renderer could not install its safe detached-window handlers.";
        }
        return "Detachable panel windows are unavailable.";
    }

    UiViewportDecision DecideUiViewports(const UiViewportCapabilityInput& input)
    {
        const auto deny = [](UiViewportReason reason) { return UiViewportDecision { false, reason }; };
        if (!input.Requested)
            return deny(UiViewportReason::NotRequested);
        if (input.Renderer == UiViewportRendererKind::None)
            return deny(UiViewportReason::NativeRendererUnavailable);
        if (input.Renderer == UiViewportRendererKind::D3D12)
            return deny(UiViewportReason::RendererNotImplemented);
        if (input.Platform == UiViewportPlatformKind::Wayland)
            return deny(UiViewportReason::PlatformWayland);
        if (input.Platform != UiViewportPlatformKind::X11 && input.Platform != UiViewportPlatformKind::Win32)
            return deny(UiViewportReason::PlatformUnsupported);
        if (!input.PlatformBackendHasViewports)
            return deny(UiViewportReason::PlatformBackendLacksViewports);
        if (!input.RendererBackendHasViewports)
            return deny(UiViewportReason::RendererBackendLacksViewports);
        if (!input.SurfaceModesKnown)
            return deny(UiViewportReason::PresentModesUnknown);
        if (SelectUiViewportSecondaryMode(input.SurfacePresentModes) == UiViewportSecondaryMode::None)
            return deny(UiViewportReason::NoNonBlockingPresentMode);
        return { true, UiViewportReason::Enabled };
    }
}
