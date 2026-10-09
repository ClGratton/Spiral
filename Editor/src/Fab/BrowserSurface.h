#pragma once

#include "BrowserDownloadPolicy.h"
#include "BrowserNavigationPolicy.h"
#include "Engine/Core/Base.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

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

    struct BrowserSurfaceConfig
    {
        std::filesystem::path ProfileDir;
        std::filesystem::path DownloadStagingDir;
        std::filesystem::path HelperPath;
        std::filesystem::path ResourceDir;
        BrowserNavigationPolicy Navigation;
        BrowserDownloadPolicy Downloads;
        Engine::u32 MaxFps = 60;
        bool SoftwareRendering = true;
    };

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
            virtual void OnLoadState(bool loading, bool canGoBack, bool canGoForward) = 0;
            virtual void OnDownload(const BrowserDownloadEvent& event) = 0;
            // Host text from BrowserNavigationPolicy::HostForLog.
            virtual void OnNavigationDenied(std::string_view host) = 0;
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
