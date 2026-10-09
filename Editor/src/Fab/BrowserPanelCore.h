#pragma once

#include "BrowserFrameMirror.h"
#include "BrowserInputRouter.h"
#include "BrowserPanel.h"
#include "BrowserSignInHosts.h"
#include "BrowserSurface.h"
#include "Engine/Renderer/UiTexture.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The testable half of the Fab browser panel: lifecycle state machine, frame
// upload with dirty-region retry, view-size debounce, input routing against
// IBrowserSurface, and the completed-download queue. It includes no ImGui,
// GLFW, dlfcn, CEF, or Renderer header and holds no global state, so
// EngineTests drive it with FakeBrowserSurface and a fake texture provider.
// BrowserPanel.cpp is the thin ImGui/Renderer/dlopen adapter around it.
// Every call is main-thread only.
namespace Fab
{
    namespace BrowserPanelLimits
    {
        // The view size sent to the browser follows the content region only after
        // it has stayed unchanged this long (the first size is applied at once).
        constexpr Engine::u64 kResizeSettleMilliseconds = 150;
        // RequestClose to IsClosed, in Shutdown() and while signing out.
        constexpr Engine::u64 kCloseDeadlineMilliseconds = 3000;
        // Handed to IBrowserSurface::Shutdown after the close completed (or timed out).
        constexpr Engine::u32 kSurfaceShutdownMilliseconds = 2000;
        // The Fab doc caps the panel at 30 fps; a hidden page is throttled by WasHidden.
        constexpr Engine::u32 kMaximumBrowserFps = 30;
        constexpr size_t kMaximumQueuedDownloads = 16;
        // Dirty regions kept for upload and submitted per frame. Each update
        // takes one of the service's eight in-flight command lists.
        constexpr size_t kMaximumPendingRects = 4;
        constexpr size_t kMaximumUpdatesPerService = 4;
        // Content regions smaller than this on either axis count as hidden.
        constexpr float kMinimumSurfaceExtent = 16.0f;
        constexpr float kMinimumDeviceScale = 0.25f;
        constexpr float kMaximumDeviceScale = 8.0f;
        // Denials are always counted; only this many are logged.
        constexpr Engine::u32 kMaximumLoggedDenials = 64;
        constexpr size_t kMaximumRememberedDeniedHosts = 16;
    }

    // A surface created by the dynamically loaded host library must be destroyed
    // by that library, so the owner carries its destroy function.
    using BrowserSurfacePtr = std::unique_ptr<IBrowserSurface, void (*)(IBrowserSurface*)>;

    inline BrowserSurfacePtr MakeBrowserSurfacePtr(IBrowserSurface* surface, void (*destroy)(IBrowserSurface*))
    {
        return BrowserSurfacePtr(surface, destroy);
    }

    inline BrowserSurfacePtr EmptyBrowserSurfacePtr()
    {
        return BrowserSurfacePtr(nullptr, nullptr);
    }

    struct BrowserSurfaceLoadResult
    {
        BrowserSurfacePtr Surface = EmptyBrowserSurfacePtr();
        // Set when Surface is null: plain reason, no recovery advice.
        std::string Error;
    };

    // Seam over the Renderer UI-texture service (Engine/Renderer/UiTexture.h).
    // The production implementation forwards to the Renderer statics.
    class IBrowserUiTextures
    {
    public:
        virtual ~IBrowserUiTextures() = default;
        virtual Engine::UiTextureHandle Create(Engine::u32 width, Engine::u32 height, std::string_view debugName) = 0;
        virtual bool Update(Engine::UiTextureHandle handle, const Engine::UiTextureUpdate& update) = 0;
        virtual bool Destroy(Engine::UiTextureHandle handle) = 0;
        virtual Engine::u64 GetImGuiId(Engine::UiTextureHandle handle) = 0;
        // Reason the most recent Create/Update/Destroy failed.
        virtual Engine::UiTextureError LastError() = 0;
    };

    // ---- frame upload ---------------------------------------------------------------

    struct BrowserTextureUploaderCounters
    {
        Engine::u64 FramesApplied = 0;
        Engine::u64 FramesRejected = 0;
        // UpdateUiTexture calls that succeeded, and the texels they carried.
        Engine::u64 UpdatesSubmitted = 0;
        Engine::u64 UpdatedTexels = 0;
        // Create/Update rejected with UpdateBackpressure (transient; retried).
        Engine::u64 BackpressureRetries = 0;
        Engine::u64 TexturesCreated = 0;
        // A new texture finished its first full upload and replaced the displayed one.
        Engine::u64 TextureSwaps = 0;
        Engine::u64 Failures = 0;
    };

    // Persistent RGBA mirror plus the UI texture it feeds.
    //
    // Resize policy: the texture always has exactly the mirror's (frame's) size,
    // so there is no capacity/UV bookkeeping. When a frame of a new size arrives
    // (which only happens after the debounced SetViewSize), a NEW texture is
    // created and filled completely; only after that upload is accepted does it
    // replace the displayed one, whose texture is destroyed (the Renderer retires
    // it without a wait). The previous frame therefore stays on screen for the
    // whole transition and the Renderer's blanking Resize is never used.
    //
    // Retry policy: dirty rectangles that the Renderer rejects with
    // UpdateBackpressure stay queued, are merged with later dirty rectangles
    // (coverage only grows, never shrinks), and are submitted again by the next
    // Service(). Any other failure latches "texture unavailable" until a frame
    // of a different size arrives; the browser itself keeps running.
    class BrowserTextureUploader
    {
    public:
        explicit BrowserTextureUploader(IBrowserUiTextures& textures);
        ~BrowserTextureUploader();

        BrowserTextureUploader(const BrowserTextureUploader&) = delete;
        BrowserTextureUploader& operator=(const BrowserTextureUploader&) = delete;

        // Copies the frame into the mirror (dirty rectangles only) and queues its
        // dirty region. Never touches the Renderer.
        void OnFrame(const BrowserFrameView& frame);
        // Creates, uploads, and swaps as far as the Renderer accepts this call.
        // Never blocks and never waits for the GPU.
        void Service();
        // Destroys every texture this object created. Safe to repeat.
        void Release();

        bool HasDisplayTexture() const { return m_Active.Handle != Engine::kInvalidUiTextureHandle && !m_Failed; }
        Engine::u64 DisplayId() const { return HasDisplayTexture() ? m_Active.ImGuiId : 0; }
        Engine::u32 DisplayWidth() const { return m_Active.Width; }
        Engine::u32 DisplayHeight() const { return m_Active.Height; }
        // Size of the newest frame received.
        Engine::u32 FrameWidth() const { return m_Mirror.Width(); }
        Engine::u32 FrameHeight() const { return m_Mirror.Height(); }

        bool Failed() const { return m_Failed; }
        const std::string& FailureText() const { return m_FailureText; }
        // Dirty regions or a replacement texture still waiting for the Renderer.
        bool HasPendingWork() const;
        size_t PendingRectCount() const { return m_Pending.size(); }
        const BrowserTextureUploaderCounters& Counters() const { return m_Counters; }
        const BrowserFrameMirror& Mirror() const { return m_Mirror; }

    private:
        struct Slot
        {
            Engine::UiTextureHandle Handle = Engine::kInvalidUiTextureHandle;
            Engine::u64 ImGuiId = 0;
            Engine::u32 Width = 0;
            Engine::u32 Height = 0;
        };

        enum class UploadResult
        {
            Done,
            Backpressure,
            Failed
        };

        bool CreateSlot(Slot& slot, Engine::u32 width, Engine::u32 height);
        void DestroySlot(Slot& slot);
        UploadResult UploadRect(const Slot& slot, const BrowserDirtyRect& rect);
        void UploadPending();
        void Fail(Engine::UiTextureError error);

        IBrowserUiTextures& m_Textures;
        BrowserFrameMirror m_Mirror;
        Slot m_Active;
        Slot m_Next;
        std::vector<BrowserDirtyRect> m_Pending;
        std::vector<BrowserDirtyRect> m_Scratch;
        bool m_Failed = false;
        std::string m_FailureText;
        BrowserTextureUploaderCounters m_Counters;
    };

    // ---- view size ---------------------------------------------------------------------

    // Decides when the content region's size is sent to the browser. A change is
    // applied only after the same desired size has been observed for
    // `settleMilliseconds`; the first size is applied immediately. The result is
    // in DIPs (pixels / scale), capped so the physical size stays within
    // `maximumPhysical` (the UI-texture limit).
    class BrowserViewDebouncer
    {
    public:
        explicit BrowserViewDebouncer(Engine::u64 settleMilliseconds = BrowserPanelLimits::kResizeSettleMilliseconds,
            Engine::u32 maximumPhysical = Engine::kMaximumUiTextureDimension);

        // widthPixels/heightPixels: the content region in window pixels. Returns
        // true, and fills `out`, when the view must be applied now.
        bool Update(Engine::u64 nowMilliseconds, float widthPixels, float heightPixels, float deviceScale, BrowserViewSize& out);

        bool HasApplied() const { return m_HasApplied; }
        const BrowserViewSize& Applied() const { return m_Applied; }
        bool HasPending() const { return m_HasPending; }

    private:
        static bool Same(const BrowserViewSize& a, const BrowserViewSize& b);

        Engine::u64 m_Settle;
        Engine::u32 m_MaximumPhysical;
        bool m_HasApplied = false;
        BrowserViewSize m_Applied;
        bool m_HasPending = false;
        BrowserViewSize m_Pending;
        Engine::u64 m_PendingSince = 0;
    };

    // ---- downloads -----------------------------------------------------------------------

    // Bounded FIFO of completed, fully staged downloads. When full, the oldest
    // entry is dropped (its file stays in the staging directory, which owns it).
    class BrowserDownloadQueue
    {
    public:
        explicit BrowserDownloadQueue(size_t capacity = BrowserPanelLimits::kMaximumQueuedDownloads);

        void Push(BrowserPanelDownload download);
        bool TryTake(BrowserPanelDownload& out);
        size_t Size() const { return m_Items.size(); }
        Engine::u64 Dropped() const { return m_Dropped; }

    private:
        size_t m_Capacity;
        std::deque<BrowserPanelDownload> m_Items;
        Engine::u64 m_Dropped = 0;
    };

    // Turns the raw reason from a failed library load or surface Initialize into
    // the final panel text: a specific explanation for a profile in use or a
    // denied sandbox, and always the "restart the Editor" guidance, because the
    // browser engine cannot be re-initialised in this process.
    std::string DescribeBrowserStartupFailure(std::string_view reason);

    // ---- the panel core ------------------------------------------------------------------

    struct BrowserPanelEnvironment
    {
        // Required before the first SetVisible(true); not owned.
        IBrowserUiTextures* Textures = nullptr;
        // Loads the host library and creates an uninitialised surface. Called at
        // most once, on the first transition to visible.
        std::function<BrowserSurfaceLoadResult(const BrowserPanelConfig&)> LoadSurface;
        // Monotonic milliseconds. Defaults to std::chrono::steady_clock.
        std::function<Engine::u64()> NowMs;
        // Defaults to std::this_thread::sleep_for. Only used while closing.
        std::function<void(Engine::u32)> SleepMs;
        // Optional log sinks. Messages never contain a URL.
        std::function<void(std::string_view)> LogInfo;
        std::function<void(std::string_view)> LogWarn;
    };

    // Everything the ImGui adapter latches once per frame while the window is
    // drawn. Coordinates are window (screen) pixels.
    struct BrowserPanelLayout
    {
        BrowserSurfaceRect Surface;
        bool PanelFocused = false;
        bool SurfaceHovered = false;
        // An ImGui text widget outside the page wants typed text.
        bool OtherTextInputActive = false;
        bool DragDropPayloadActive = false;
        bool ModalOrPopupOpen = false;
        // Current Shift/Ctrl/Alt/Super state (BrowserModifier::KeyMask bits).
        Engine::u32 Modifiers = 0;
        // Monitor and work area in window pixels, as the platform reports them.
        // Valid=false leaves the page's screen equal to its view rectangle.
        BrowserScreenInfo Screen;
    };

    class BrowserPanelCore final : public IBrowserSurface::Listener
    {
    public:
        enum class State
        {
            NotStarted,
            // The surface is initialised; no frame has arrived yet.
            Starting,
            Running,
            Failed,
            Closing,
            Closed
        };

        explicit BrowserPanelCore(BrowserPanelEnvironment environment);
        ~BrowserPanelCore() override;

        BrowserPanelCore(const BrowserPanelCore&) = delete;
        BrowserPanelCore& operator=(const BrowserPanelCore&) = delete;

        // Ignored once the browser has been started.
        void Configure(BrowserPanelConfig config);

        // The first transition to true loads and initialises the browser, on the
        // calling thread. Every failure is final for the process.
        void SetVisible(bool visible);
        bool IsVisible() const { return m_Visible; }

        // Once per frame, before Draw: pumps the surface (all Listener callbacks
        // happen inside), hides a panel that was not drawn since the last call,
        // and advances the texture upload. Advances an in-progress close.
        void Pump();

        // ---- called by the adapter while the window is drawn ----
        // Latches layout, applies the debounced view size, shows the surface,
        // issues the first navigation, and feeds the input router.
        void UpdateLayout(const BrowserPanelLayout& layout);
        // The window exists but its content is not drawn (collapsed, hidden dock tab).
        void NotifyContentHidden();
        void SetDeviceScale(float scale);

        // Offers an engine event. Consumed events have Handled set and return true.
        bool OnEvent(Engine::Event& event);
        bool WantsKeyboard() const { return Live() && m_Router.OwnsKeyboard(); }
        // The toolbar's "release keyboard" affordance.
        void ReleaseKeyboard();

        // ---- sign-in hosts ----
        // The host the banner asks about ("The page tried to open <host>. Allow for
        // sign-in?"), empty when no banner is shown.
        const std::string& PendingConsentHost() const { return m_Consent.PendingHost(); }
        // Allow once: this session only. Allow always: also saved to the sign-in
        // host file. Both repeat the denied navigation when the surface kept it.
        // Dismiss hides the banner and suppresses this host for a short time.
        void ResolveConsent(SignInConsentChoice choice);
        std::span<const BrowserGrantedHost> GrantedHosts() const { return m_Granted.Entries(); }
        static std::span<const BrowserSignInProvider> DefaultSignInProviders() { return BrowserNavigationPolicy::DefaultSignInProviders(); }
        bool RevokeGrantedHost(std::string_view host);
        // Removes every user-granted host, saved ones included.
        void ClearGrantedHosts();
        // Why the last grant, revoke, or load of the saved list failed; empty when fine.
        const std::string& SignInHostsNotice() const { return m_SignInNotice; }
        const std::filesystem::path& SignInHostsFile() const { return m_SignInHostsFile; }

        // ---- commands ----
        void GoBack();
        void GoForward();
        void Reload();
        void GoHome();
        void ClearBrowsingData();
        bool TryTakeCompletedDownload(BrowserPanelDownload& out) { return m_Downloads.TryTake(out); }
        void Shutdown();

        // ---- observation ----
        State GetState() const { return m_State; }
        static const char* StateName(State state);
        bool Live() const { return m_State == State::Starting || m_State == State::Running; }
        bool CanGoBack() const { return Live() && m_CanGoBack; }
        bool CanGoForward() const { return Live() && m_CanGoForward; }
        bool IsLoading() const { return Live() && m_Loading; }
        const std::string& DisplayHost() const { return m_DisplayHost; }
        BrowserCursor Cursor() const { return m_Cursor; }
        // Rebuilt on every call from the current state; the reference stays valid
        // for the life of the object.
        const std::string& StatusLine() const;
        const BrowserTextureUploader& Uploader() const { return m_Uploader; }
        const BrowserInputRouter& Router() const { return m_Router; }
        const BrowserDownloadQueue& Downloads() const { return m_Downloads; }
        // Debounced DIP size currently applied to the browser.
        const BrowserViewDebouncer& ViewDebouncer() const { return m_View; }
        float AppliedDeviceScale() const { return m_AppliedScale; }
        bool SignedOut() const { return m_SignedOut; }
        BrowserPanelDiagnostics GetDiagnostics() const;
        const IBrowserSurface* Surface() const { return m_Surface.get(); }

        // ---- IBrowserSurface::Listener (called from inside the surface's Pump) ----
        void OnFrame(const BrowserFrameView& frame) override;
        void OnCursor(BrowserCursor cursor) override;
        void OnAddress(std::string_view displayAddress) override;
        void OnLoadState(bool loading, bool canGoBack, bool canGoForward) override;
        void OnDownload(const BrowserDownloadEvent& event) override;
        void OnNavigationDenied(std::string_view host) override;
        void OnNavigationConsentOffered(std::string_view host) override;
        void OnPopupRedirected(std::string_view host) override;
        void OnFailed(std::string_view reason) override;
        void OnClosed() override;

    private:
        void Start();
        void Fail(const std::string& finalText);
        void BeginClosing();
        void AdvanceClosing();
        void FinishClosing();
        void ApplySurfaceVisibility();
        void SetContentShown(bool shown);
        void FeedRouterHidden();
        void ApplyRoute(const BrowserInputRoute& route);
        BrowserMouse ToDip(const BrowserMouse& mouse) const;
        void LoadGrantedHosts();
        void ApplyGrantedHosts();
        bool SaveGrantedHosts();
        template <typename Function>
        bool Guarded(const char* where, Function&& function);
        void Info(std::string_view message) const;
        void Warn(std::string_view message) const;

        BrowserPanelEnvironment m_Env;
        BrowserPanelConfig m_Config;
        State m_State = State::NotStarted;
        BrowserSurfacePtr m_Surface = EmptyBrowserSurfacePtr();
        BrowserTextureUploader m_Uploader;
        BrowserViewDebouncer m_View;
        BrowserInputRouter m_Router;
        BrowserDownloadQueue m_Downloads;

        bool m_Visible = false;
        bool m_ContentShown = false;
        bool m_SurfaceVisible = false;
        bool m_DrawnSincePump = false;
        bool m_ShutDown = false;
        bool m_Initialized = false;
        bool m_HomeRequested = false;
        bool m_CloseRequested = false;
        bool m_SignedOut = false;
        Engine::u64 m_CloseDeadline = 0;

        float m_DeviceScale = 1.0f;
        float m_AppliedScale = 1.0f;
        BrowserSurfaceRect m_SurfaceRect;
        BrowserPanelState m_LastPanelState;
        float m_CursorX = 0.0f;
        float m_CursorY = 0.0f;
        Engine::u32 m_Modifiers = 0;
        int m_ClickCount = 1;

        BrowserCursor m_Cursor = BrowserCursor::Arrow;
        bool m_Loading = false;
        bool m_CanGoBack = false;
        bool m_CanGoForward = false;
        std::string m_DisplayHost;
        std::string m_Error;
        std::string m_PageError;
        std::string m_Notice;
        std::string m_DeferredFailure;
        mutable std::string m_Status;
        Engine::u32 m_NavigationDenials = 0;
        Engine::u32 m_PopupRedirects = 0;
        std::string m_LastDeniedHost;
        std::vector<std::string> m_DeniedHosts;
        // m_Baseline is the fixed list the surface started with; m_Effective adds
        // the user's grants and is what the banner decides against.
        BrowserNavigationPolicy m_Baseline;
        BrowserNavigationPolicy m_Effective;
        BrowserSignInHostBook m_Granted;
        BrowserSignInConsent m_Consent;
        std::filesystem::path m_SignInHostsFile;
        std::string m_SignInNotice;
        BrowserScreenInfo m_SentScreen;
        Engine::u32 m_DownloadsCompleted = 0;
        Engine::u64 m_LoggedTextureFailures = 0;
    };
}
