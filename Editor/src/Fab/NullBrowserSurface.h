#pragma once

#include "BrowserSurface.h"

namespace Fab
{
    // The surface used when no browser engine is available: headless runs, a
    // build without the engine, or an initialisation failure. Initialize
    // always fails with the stored reason; every other call is a no-op and
    // IsClosed() is true, so a panel can hold one unconditionally.
    class NullBrowserSurface final : public IBrowserSurface
    {
    public:
        explicit NullBrowserSurface(std::string reason);

        std::string_view BackendName() const override { return "null"; }
        bool Initialize(const BrowserSurfaceConfig& config, Listener& listener, std::string& error) override;
        void Pump() override {}

        void SetViewSize(const BrowserViewSize&) override {}
        void SetVisible(bool) override {}
        void SetFocus(bool) override {}
        void SendMouseMove(const BrowserMouse&, bool) override {}
        void SendMouseButton(const BrowserMouse&, BrowserMouseButton, bool, int) override {}
        void SendMouseCaptureLost() override {}
        void SendMouseWheel(const BrowserMouse&, float, float) override {}
        void SendKey(const BrowserKey&) override {}

        void Navigate(std::string_view) override {}
        void GoBack() override {}
        void GoForward() override {}
        void Reload() override {}
        void Stop() override {}
        void CancelDownload(Engine::u64) override {}
        void ClearBrowsingData() override {}

        void RequestClose() override {}
        bool IsClosed() const override { return true; }
        void Shutdown(Engine::u32) override {}

        const std::string& Reason() const { return m_Reason; }

    private:
        std::string m_Reason;
    };
}
