#pragma once

#include "BrowserSurface.h"
#include "FabDisclosure.h"
#include "Engine/Core/Base.h"
#include "Engine/Events/Event.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
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
        // File that keeps the "Allow always" sign-in hosts (see BrowserSignInHosts.h).
        // Empty selects "SignInHosts.json" beside ProfileDirectory, inside the same
        // owner-only parent, never in a project and never in the profile itself, so
        // signing out (which deletes the profile) does not discard host decisions.
        std::filesystem::path SignInHostsFile;
        // Software until a headed run shows that hardware rendering works beside
        // the Editor's own graphics device.
        BrowserRenderMode RenderMode = BrowserRenderMode::Software;
        // The window's content scale at startup (the window reports changes
        // through events only). Clamped like any device scale.
        float InitialDeviceScale = 1.0f;
        // File that keeps the notice dismissal (see FabDisclosure.h). Empty selects
        // kFabNoticeDismissalFileName beside ProfileDirectory, inside the same
        // owner-only parent, never in a project and never in the profile itself, so
        // signing out (which deletes the profile) does not discard it.
        std::filesystem::path NoticeDismissalFile;
        // The kill switch. Anything but None keeps the panel closed: it cannot be
        // made visible, the browser is never started, and nothing else changes.
        FabBrowserDisabledReason Disabled = FabBrowserDisabledReason::None;
    };

    // Opens `url` in the system browser; `host` is the exact host it must be on.
    // The production implementation is Engine::OpenExternalHttpsUrl.
    using BrowserExternalOpener = std::function<bool(std::string_view url, std::string_view host, std::string& error)>;

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
        // Hosts only (BrowserNavigationPolicy::HostForLog text), never a URL.
        // DeniedHosts lists the most recent distinct denied hosts, newest last,
        // at most 16.
        std::string LastDeniedHost;
        std::vector<std::string> DeniedHosts;
        // The host the consent banner currently asks about, empty when none.
        std::string ConsentHost;
        Engine::u32 PopupRedirects = 0;
        // User-granted sign-in hosts ("Allow once" plus "Allow always").
        Engine::u32 GrantedHostCount = 0;
        std::string RenderMode; // "software" or "hardware"
        // The kill switch: empty when the panel is enabled, else the stable reason
        // token (fab_browser_disabled_by_command_line, fab_browser_disabled_by_setting).
        std::string DisabledReason;
        // The one-line notice is drawn above the page.
        bool NoticeShown = false;
        // The notice was read from the store or dismissed and is hidden.
        bool NoticeDismissed = false;
        // none, load-error, navigation-denied, security-check-likely
        std::string PageHint;
        // The scheme of the page address as the toolbar shows it ("https").
        std::string DisplayScheme;
    };

    // What the most recent Draw() put on screen, so the headless smoke can check the
    // toolbar and the notice card without reading widget state.
    struct BrowserPanelDrawRecord
    {
        bool OpenInBrowserEnabled = false;
        bool NoticeLine = false;
        bool PageHint = false;
        bool LockGlyph = false;
        // The scheme and host text as drawn ("https://www.fab.com"), or "-".
        std::string AddressText;
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

        // ---- Epic terms safeguards (see FabDisclosure.h) ----
        // The kill switch (BrowserPanelConfig::Disabled). A disabled panel ignores
        // SetVisible(true); DisabledText says why for the menu and the log.
        bool IsDisabled() const;
        std::string DisabledText() const;
        // Applies the kill switch at runtime; see BrowserPanelCore::SetDisabled.
        void SetDisabled(FabBrowserDisabledReason reason);
        // The one-line notice above the page; Dismiss hides it and saves the dismissal.
        bool NoticeVisible() const;
        void DismissNotice();
        // The toolbar's permanent "Open in browser" button: the current https page
        // when the navigation policy allows it, else the home page. Works in every
        // state, including a browser that failed to start. The outcome is in the
        // status line. False when nothing was opened.
        bool OpenInBrowser();
        // Replaces the external-navigation function (default: Engine::OpenExternalHttpsUrl)
        // so a headless check can observe the request without launching a browser.
        void SetExternalOpener(BrowserExternalOpener opener);
        // Describes the last frame in which the window was drawn open.
        BrowserPanelDrawRecord LastDrawRecord() const;

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
