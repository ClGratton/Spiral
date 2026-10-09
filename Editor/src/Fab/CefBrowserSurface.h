#pragma once

#include "BrowserSurface.h"

#include <memory>
#include <string>
#include <vector>

// The Chromium Embedded Framework adapter for IBrowserSurface. This header is
// CEF-free on purpose, but CefBrowserSurface.cpp is not: it compiles only into
// libSpiralBrowserHost.so (Editor/browserhost), never into the Editor
// executable, so the Editor never links libcef and headless runs never load it.
// The Editor reaches this class only through the C factory in
// Editor/browserhost/BrowserHostExports.h.
//
// Threading: Initialize, Pump, every input call, RequestClose and Shutdown
// must run on one thread (the Editor main thread). CEF allows one browser
// engine lifecycle per process: after Shutdown() a new surface in the same
// process fails Initialize.
namespace Fab
{
    // Reachable only through the test factory export, never the production one.
    struct CefBrowserSurfaceTestOptions
    {
        // "name=value" or bare "name" Chromium switches appended in the browser
        // process, for example a host-resolver rule that maps a fixture host
        // name to a loopback port.
        std::vector<std::string> CommandLineSwitches;
    };

    class CefBrowserSurface final : public IBrowserSurface
    {
    public:
        // Opaque; defined in CefBrowserSurface.cpp.
        struct State;

        explicit CefBrowserSurface(CefBrowserSurfaceTestOptions options = {});
        ~CefBrowserSurface() override;

        CefBrowserSurface(const CefBrowserSurface&) = delete;
        CefBrowserSurface& operator=(const CefBrowserSurface&) = delete;

        std::string_view BackendName() const override { return "cef"; }

        // Fails with a human-readable error, and leaves the process free of
        // browser state, when: the config is invalid; the profile lies inside a
        // project, belongs to another user, or is a non-empty directory that is
        // not a browser profile; the runtime files are missing; the Chromium
        // user-namespace sandbox is unavailable ("sandbox"); or another running
        // instance owns the profile ("profile in use").
        bool Initialize(const BrowserSurfaceConfig& config, Listener& listener, std::string& error) override;

        // Runs the CEF message loop when it is due (at least every 33 ms) and
        // then delivers every queued Listener callback. At most one OnFrame per
        // call, carrying every dirty rectangle since the previous one.
        void Pump() override;

        void SetViewSize(const BrowserViewSize& size) override;
        void SetVisible(bool visible) override;
        void SetFocus(bool focused) override;
        void SendMouseMove(const BrowserMouse& mouse, bool leave) override;
        void SendMouseButton(const BrowserMouse& mouse, BrowserMouseButton button, bool down, int clickCount) override;
        void SendMouseCaptureLost() override;
        // Pixel deltas, positive = content moves down/right (the GLFW scroll
        // sign), forwarded unchanged.
        void SendMouseWheel(const BrowserMouse& mouse, float deltaX, float deltaY) override;
        // Down/Up carry the Windows virtual key; Char carries text (ASCII as a
        // key character, anything else as an IME commit). Ctrl+A/C/X/V/Z/Y are
        // executed as focused-frame editing commands (the windowless browser
        // has no native shortcut handling) and not forwarded to the page.
        void SendKey(const BrowserKey& key) override;

        void Navigate(std::string_view httpsUrl) override;
        void GoBack() override;
        void GoForward() override;
        void Reload() override;
        void Stop() override;
        void CancelDownload(Engine::u64 id) override;
        // Closes the browser and deletes the whole profile directory inside
        // Shutdown(), after the engine has released it. The engine cannot be
        // restarted in this process afterwards.
        void ClearBrowsingData() override;

        void RequestClose() override;
        bool IsClosed() const override;
        void Shutdown(Engine::u32 timeoutMs) override;

    private:
        std::unique_ptr<State> m_State;
    };
}
