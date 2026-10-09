#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Events/Event.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// ImGui panel that hosts the embedded Fab browser. The panel owns the on-demand
// load of the CEF host library, the frame mirror, the UI texture, and the input
// routing. It is Editor-private and never includes CEF headers; everything
// engine-facing goes through IBrowserSurface. All calls are main-thread only.
namespace Fab
{
    struct BrowserPanelConfig
    {
        // Directory containing the Editor executable; the host library is
        // <EditorDirectory>/cef/libSpiralBrowserHost.so.
        std::filesystem::path EditorDirectory;
        // Canonical absolute owner-only profile, outside every project.
        std::filesystem::path ProfileDirectory;
        // Canonical absolute owner-only download staging, distinct from the profile.
        std::filesystem::path DownloadStagingDirectory;
        std::string HomeUrl = "https://www.fab.com/";
        // Extra exact identity-provider hosts allowed for top-level sign-in
        // navigation (e.g. accounts.google.com), supplied by the user.
        std::vector<std::string> ProviderHosts;
    };

    struct BrowserPanelDownload
    {
        std::filesystem::path StagedPath;
        std::string DisplayName;
    };

    // Read-only snapshot for the typed control and the headed acceptance.
    // Never contains URLs beyond the display host, cookies, or credentials.
    struct BrowserPanelDiagnostics
    {
        std::string State; // NotStarted, Starting, Running, Failed, Closing, Closed
        bool Initialized = false;
        bool Failed = false;
        bool Visible = false;
        bool KeyboardOwnedByPage = false;
        bool TextureValid = false;
        bool Loading = false;
        Engine::u64 FramesReceived = 0;
        Engine::u32 FrameWidth = 0;
        Engine::u32 FrameHeight = 0;
        Engine::u32 NavigationDenials = 0;
        Engine::u32 DownloadsCompleted = 0;
        std::string DisplayHost;
        std::string Error;
    };

    class BrowserPanel
    {
    public:
        BrowserPanel();
        ~BrowserPanel();
        BrowserPanel(const BrowserPanel&) = delete;
        BrowserPanel& operator=(const BrowserPanel&) = delete;

        // Cheap and idempotent. Never loads the host library or touches CEF.
        void Configure(BrowserPanelConfig config);

        // The first transition to visible loads and initializes the browser.
        // A failed initialization is final for the process and is shown in the
        // panel; the Editor must be restarted.
        void SetVisible(bool visible);
        bool IsVisible() const;

        // Call once per frame at the top of the UI render, before any ImGui
        // input guard. A no-op until the browser has been initialized.
        void Pump();

        // Draws the ImGui window when visible. Safe to call every frame.
        void Draw();

        // Offers a window event to the panel. Returns true when the browser
        // consumed it (keyboard focus in the page, or pointer capture).
        bool OnEvent(Engine::Event& event);

        // True while a web page owns keyboard focus. The Editor must suppress
        // its own shortcuts (undo/redo, focus, delete, ...) while this holds.
        bool WantsKeyboard() const;

        // Signs the user out by deleting the browser profile after shutdown.
        // A restart is required to sign in again.
        void ClearBrowsingData();

        // Pops one completed, fully staged user download, if any.
        bool TryTakeCompletedDownload(BrowserPanelDownload& outDownload);

        // Orderly close for Editor shutdown: request close, pump until closed,
        // shut the surface down, and release the UI texture. Safe if unused.
        void Shutdown();

        const std::string& StatusLine() const;
        BrowserPanelDiagnostics GetDiagnostics() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
