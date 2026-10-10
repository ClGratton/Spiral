#pragma once

#include "BrowserDownloadPolicy.h"
#include "BrowserNavigationPolicy.h"
#include "Engine/Core/Base.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Editor-private browser abstraction. This header and everything it includes
// use plain C++ types only: no CEF, ImGui, GLFW, or RHI. Every Listener
// callback is delivered on the main thread from inside IBrowserSurface::Pump()
// and never from any other call, so implementations and listeners need no
// locks. Engine and Sandbox never include this directory.
namespace Fab
{
    // Logical (DIP) size of the browser view plus the device scale used to
    // derive the physical pixel size of its frames.
    struct BrowserViewSize
    {
        Engine::u32 Width = 0;
        Engine::u32 Height = 0;
        float DeviceScale = 1.0f;
    };

    struct BrowserPixelSize
    {
        Engine::u32 Width = 0;
        Engine::u32 Height = 0;

        bool operator==(const BrowserPixelSize&) const = default;
    };

    struct BrowserDirtyRect
    {
        int X = 0;
        int Y = 0;
        int Width = 0;
        int Height = 0;

        bool operator==(const BrowserDirtyRect&) const = default;
    };

    // A full-view BGRA frame, top-left origin, valid only for the duration of
    // Listener::OnFrame. Dirty lists the rectangles that changed since the
    // previous frame; pixels outside them are unchanged by contract.
    struct BrowserFrameView
    {
        const Engine::u8* Bgra = nullptr;
        Engine::u32 Width = 0;
        Engine::u32 Height = 0;
        Engine::u32 StrideBytes = 0;
        std::span<const BrowserDirtyRect> Dirty;
        Engine::u64 Sequence = 0;
    };

    // Bits 0-5 equal GLFW_MOD_* so a GLFW modifier mask converts by masking.
    namespace BrowserModifier
    {
        constexpr Engine::u32 Shift = 1;
        constexpr Engine::u32 Control = 2;
        constexpr Engine::u32 Alt = 4;
        constexpr Engine::u32 Super = 8;
        constexpr Engine::u32 CapsLock = 16;
        constexpr Engine::u32 NumLock = 32;
        constexpr Engine::u32 KeyMask = 63;
        constexpr Engine::u32 LeftButton = 64;
        constexpr Engine::u32 MiddleButton = 128;
        constexpr Engine::u32 RightButton = 256;
    }

    enum class BrowserMouseButton
    {
        Left,
        Middle,
        Right
    };

    // View-local logical coordinates.
    struct BrowserMouse
    {
        float X = 0.0f;
        float Y = 0.0f;
        Engine::u32 Modifiers = 0;
    };

    struct BrowserKey
    {
        enum class Phase
        {
            Down,
            Char,
            Up
        };

        Phase Kind = Phase::Down;
        int GlfwKey = 0;
        int WindowsVirtualKey = 0;
        int Scancode = 0;
        Engine::u32 Modifiers = 0;
        char32_t Codepoint = 0;
        bool Repeat = false;
    };

    enum class BrowserCursor
    {
        Arrow,
        IBeam,
        Hand,
        ResizeEW,
        ResizeNS,
        ResizeAll,
        NotAllowed,
        Wait,
        Hidden
    };

    struct BrowserDownloadEvent
    {
        enum class State
        {
            Started,
            Progress,
            Completed,
            Failed,
            Cancelled,
            // Refused by BrowserDownloadPolicy (type, size, or name).
            Blocked
        };

        State Kind = State::Started;
        Engine::u64 Id = 0;
        // Sanitized name only. Never a URL: download URLs are credentials.
        std::string DisplayName;
        std::filesystem::path StagedPath;
        Engine::u64 Bytes = 0;
        Engine::u64 TotalBytes = 0;
    };

    // Software: the engine composites and rasterizes on the CPU (SwiftShader for
    // WebGL); known to work with the windowless panel on every host. Hardware:
    // the engine is allowed to use the GPU; an experiment, off by default, that
    // is only trusted once a headed run shows it works beside the Editor's own
    // graphics device.
    enum class BrowserRenderMode
    {
        Software,
        Hardware
    };

    // The monitor the Editor window sits on and its work area, in window
    // (screen) pixels, so the page sees truthful screen.width/height and
    // availWidth/availHeight that do not change when the panel is resized.
    // Valid is false when the platform reported nothing usable; the adapter then
    // keeps the view rectangle as its screen, as before this record existed.
    // (window.outerWidth/outerHeight and screenX/screenY stay 0 under CEF 154
    // windowless rendering whatever is reported, so the window position is not
    // part of the record.)
    struct BrowserScreenInfo
    {
        bool Valid = false;
        int MonitorX = 0;
        int MonitorY = 0;
        int MonitorWidth = 0;
        int MonitorHeight = 0;
        int WorkX = 0;
        int WorkY = 0;
        int WorkWidth = 0;
        int WorkHeight = 0;

        bool operator==(const BrowserScreenInfo&) const = default;
    };

    struct BrowserSurfaceConfig
    {
        std::filesystem::path ProfileDir;
        std::filesystem::path DownloadStagingDir;
        std::filesystem::path HelperPath;
        std::filesystem::path ResourceDir;
        BrowserNavigationPolicy Navigation;
        BrowserDownloadPolicy Downloads;
        Engine::u32 MaxFps = 60;
        BrowserRenderMode RenderMode = BrowserRenderMode::Software;
    };

    // The value for the engine's accept-language list from the process locale
    // variables, in the precedence the platform uses (LANGUAGE, LC_ALL,
    // LC_MESSAGES, LANG; the first non-empty one wins). "it_IT.UTF-8:en_US"
    // becomes "it-IT,it,en-US,en". The C/POSIX locales and anything that is not
    // a plain language[_REGION][.charset][@modifier] tag are ignored; at most 8
    // tags are kept and an empty result is "en-US,en". Never reads the
    // environment itself and never derives anything from a site.
    std::string BuildAcceptLanguageList(
        std::string_view language, std::string_view lcAll, std::string_view lcMessages, std::string_view lang);

    // Lexical checks that an engine adapter can rely on before touching the
    // disk. The 2026-10-09 CEF spike showed that a relative profile path, a path
    // with "." or ".." components, or an empty one makes the engine silently fall
    // back to in-memory storage, which would defeat persistent sign-in without
    // any error. So ProfileDir and DownloadStagingDir must be non-empty, absolute,
    // lexically normal, free of a trailing separator, distinct, and not nested in
    // one another; HelperPath and ResourceDir follow the same rule when set;
    // MaxFps must be 1..60. Symlink resolution, owner-only (0700) permissions,
    // and keeping the profile outside every project are the adapter's job and are
    // not checked here. On failure error names the offending field, never a path.
    bool ValidateBrowserSurfaceConfig(const BrowserSurfaceConfig& config, std::string& error);

    class IBrowserSurface
    {
    public:
        class Listener
        {
        public:
            virtual ~Listener() = default;
            virtual void OnFrame(const BrowserFrameView& frame) = 0;
            virtual void OnCursor(BrowserCursor cursor) = 0;
            // Display text from BrowserNavigationPolicy::DisplayAddress; UI only, never logged.
            virtual void OnAddress(std::string_view displayAddress) = 0;
            // The scheme of the same main-frame address in lower case ("https"; at
            // most 16 characters of a-z, 0-9, '+', '-', '.'; empty when there is
            // none), delivered right before OnAddress. The toolbar shows it next to
            // the host so the user can see where they are before typing credentials.
            virtual void OnAddressScheme(std::string_view /*scheme*/) {}
            // The main frame finished loading and the server answered with this HTTP
            // status (0 when the engine reports none). Only the number: no URL, body,
            // or header. A 403 or 503 is how a managed security check usually answers.
            virtual void OnMainFrameLoaded(int /*httpStatus*/) {}
            virtual void OnLoadState(bool loading, bool canGoBack, bool canGoForward) = 0;
            virtual void OnDownload(const BrowserDownloadEvent& event) = 0;
            // Host text from BrowserNavigationPolicy::HostForLog. Delivered once per
            // refused top-level navigation or popup, host only.
            virtual void OnNavigationDenied(std::string_view host) = 0;
            // Follows OnNavigationDenied for the same navigation when the surface
            // kept the target so RetryDeniedNavigation can repeat it after the
            // user allows `host` (BrowserNavigationPolicy::ConsentHost).
            virtual void OnNavigationConsentOffered(std::string_view /*host*/) {}
            // A popup whose target host is allowed was opened as a top-level
            // navigation in the panel itself (no window was created).
            virtual void OnPopupRedirected(std::string_view /*host*/) {}
            virtual void OnFailed(std::string_view reason) = 0;
            virtual void OnClosed() = 0;
        };

        virtual ~IBrowserSurface() = default;

        // Names the implementation that is actually running ("cef", "null", ...).
        virtual std::string_view BackendName() const = 0;
        virtual bool Initialize(const BrowserSurfaceConfig& config, Listener& listener, std::string& error) = 0;
        virtual void Pump() = 0;

        virtual void SetViewSize(const BrowserViewSize& size) = 0;
        virtual void SetVisible(bool visible) = 0;
        virtual void SetFocus(bool focused) = 0;
        virtual void SendMouseMove(const BrowserMouse& mouse, bool leave) = 0;
        virtual void SendMouseButton(const BrowserMouse& mouse, BrowserMouseButton button, bool down, int clickCount) = 0;
        virtual void SendMouseCaptureLost() = 0;
        virtual void SendMouseWheel(const BrowserMouse& mouse, float deltaX, float deltaY) = 0;
        virtual void SendKey(const BrowserKey& key) = 0;

        // The monitor the Editor window is on; cheap to call every frame.
        virtual void SetScreenInfo(const BrowserScreenInfo& info) = 0;

        // Replaces the user-granted exact hosts of the navigation policy
        // (BrowserNavigationPolicy::SetGrantedHosts). The fixed list is untouched.
        virtual void SetGrantedHosts(std::span<const std::string> hosts) = 0;
        // Repeats the most recent denied navigation as a main-frame load when
        // its host is allowed now; the target never leaves the surface. False
        // when there is nothing to repeat (none kept, a non-GET request, or
        // still denied). The kept target is consumed either way.
        virtual bool RetryDeniedNavigation() = 0;

        // Validated against BrowserNavigationPolicy exactly like a page-initiated top-level load.
        virtual void Navigate(std::string_view httpsUrl) = 0;
        virtual void GoBack() = 0;
        virtual void GoForward() = 0;
        virtual void Reload() = 0;
        virtual void Stop() = 0;
        virtual void CancelDownload(Engine::u64 id) = 0;
        // Visible sign-out action; removes the persistent profile data.
        virtual void ClearBrowsingData() = 0;

        virtual void RequestClose() = 0;
        virtual bool IsClosed() const = 0;
        // Valid after IsClosed(); bounded by timeoutMs.
        virtual void Shutdown(Engine::u32 timeoutMs) = 0;
    };
}
