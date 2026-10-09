#include "CefBrowserSurface.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_cookie.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <thread>
#include <utility>

#include <dlfcn.h>
#include <csignal>
#include <fcntl.h>
#include <sched.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__linux__)
#error "CefBrowserSurface is Linux-only until its Windows and macOS CEF slices exist."
#endif

namespace Fab
{
    namespace
    {
        constexpr int kPumpCapMilliseconds = 33;
        constexpr size_t kMaximumPendingDirtyRects = 32;
        constexpr Engine::u32 kMaximumViewDimension = 4096;
        constexpr Engine::u64 kProgressIntervalMilliseconds = 200;
        constexpr char kBlankUrl[] = "about:blank";
        constexpr char kLogFileName[] = "SpiralBrowser.log";
        constexpr int kVkReturn = 0x0D;

        // Hardware mode: no CPU rasterizer switches, and ANGLE is pointed at Vulkan,
        // which the Chromium headless-GPU notes describe as the route that needs
        // no window-system connection. Unverified here: whether this host's GPU
        // process comes up under the headless platform. Off unless requested.
        struct EngineSwitch
        {
            const char* Name;
            const char* Value;
        };
        constexpr EngineSwitch kHardwareSwitches[] = {
            { "use-angle", "vulkan" },
            { "enable-features", "Vulkan" },
            { "disable-vulkan-surface", nullptr },
        };

        std::string EnvironmentValue(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr ? std::string(value) : std::string();
        }

        // CEF supports one initialize/shutdown cycle per process, and a failed
        // CefInitialize leaves the process unusable for a second attempt, so the
        // lifecycle is claimed once and a failure is remembered for later callers.
        bool g_EngineLifecycleClaimed = false;
        std::string g_EngineLifecycleFailure;

        int64_t NowMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // OnScheduleMessagePumpWork may be called from any thread; Pump() reads
        // the earliest requested time on the main thread.
        struct PumpSignal
        {
            std::atomic<int64_t> DueMilliseconds { std::numeric_limits<int64_t>::max() };

            void Schedule(int64_t delayMilliseconds)
            {
                const int64_t due = NowMilliseconds() + std::max<int64_t>(delayMilliseconds, 0);
                int64_t current = DueMilliseconds.load();
                while (due < current && !DueMilliseconds.compare_exchange_weak(current, due))
                {
                }
            }
        };

        Engine::u64 RandomNonce()
        {
            Engine::u64 nonce = 0;
            if (::getrandom(&nonce, sizeof(nonce), 0) != static_cast<ssize_t>(sizeof(nonce)))
                nonce = static_cast<Engine::u64>(NowMilliseconds()) * 0x9E3779B97F4A7C15ull;
            return nonce;
        }

        std::string EncodeUtf8(char32_t codepoint)
        {
            std::string text;
            if (codepoint < 0x80)
            {
                text.push_back(static_cast<char>(codepoint));
            }
            else if (codepoint < 0x800)
            {
                text.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint < 0x10000)
            {
                text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else
            {
                text.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            return text;
        }

        uint32_t ToCefModifiers(Engine::u32 modifiers)
        {
            uint32_t flags = 0;
            if (modifiers & BrowserModifier::Shift)
                flags |= EVENTFLAG_SHIFT_DOWN;
            if (modifiers & BrowserModifier::Control)
                flags |= EVENTFLAG_CONTROL_DOWN;
            if (modifiers & BrowserModifier::Alt)
                flags |= EVENTFLAG_ALT_DOWN;
            if (modifiers & BrowserModifier::Super)
                flags |= EVENTFLAG_COMMAND_DOWN;
            if (modifiers & BrowserModifier::CapsLock)
                flags |= EVENTFLAG_CAPS_LOCK_ON;
            if (modifiers & BrowserModifier::NumLock)
                flags |= EVENTFLAG_NUM_LOCK_ON;
            if (modifiers & BrowserModifier::LeftButton)
                flags |= EVENTFLAG_LEFT_MOUSE_BUTTON;
            if (modifiers & BrowserModifier::MiddleButton)
                flags |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
            if (modifiers & BrowserModifier::RightButton)
                flags |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
            return flags;
        }

        CefMouseEvent ToCefMouse(const BrowserMouse& mouse)
        {
            CefMouseEvent event;
            event.x = static_cast<int>(std::lround(mouse.X));
            event.y = static_cast<int>(std::lround(mouse.Y));
            event.modifiers = ToCefModifiers(mouse.Modifiers);
            return event;
        }

        BrowserCursor MapCursor(cef_cursor_type_t type)
        {
            switch (type)
            {
            case CT_HAND:
            case CT_GRAB:
                return BrowserCursor::Hand;
            case CT_IBEAM:
            case CT_VERTICALTEXT:
                return BrowserCursor::IBeam;
            case CT_WAIT:
            case CT_PROGRESS:
                return BrowserCursor::Wait;
            case CT_EASTRESIZE:
            case CT_WESTRESIZE:
            case CT_EASTWESTRESIZE:
            case CT_COLUMNRESIZE:
                return BrowserCursor::ResizeEW;
            case CT_NORTHRESIZE:
            case CT_SOUTHRESIZE:
            case CT_NORTHSOUTHRESIZE:
            case CT_ROWRESIZE:
                return BrowserCursor::ResizeNS;
            case CT_MOVE:
            case CT_GRABBING:
            case CT_MIDDLEPANNING:
            case CT_NORTHEASTRESIZE:
            case CT_NORTHWESTRESIZE:
            case CT_SOUTHEASTRESIZE:
            case CT_SOUTHWESTRESIZE:
            case CT_NORTHEASTSOUTHWESTRESIZE:
            case CT_NORTHWESTSOUTHEASTRESIZE:
                return BrowserCursor::ResizeAll;
            case CT_NOTALLOWED:
            case CT_NODROP:
                return BrowserCursor::NotAllowed;
            case CT_NONE:
                return BrowserCursor::Hidden;
            default:
                return BrowserCursor::Arrow;
            }
        }

        // Creates missing ancestors owner-only, refuses a final symlink or a
        // directory owned by someone else, tightens its mode to 0700, and returns
        // the symlink-free path CEF requires (a relative or non-canonical
        // cache path makes it fall back to in-memory storage without an error).
        bool PreparePrivateDirectory(
            const std::filesystem::path& path, const char* what, std::filesystem::path& canonical, std::string& error)
        {
            std::error_code code;
            std::vector<std::filesystem::path> missing;
            for (std::filesystem::path current = path; !std::filesystem::exists(current, code); current = current.parent_path())
            {
                missing.push_back(current);
                if (current == current.root_path())
                    break;
            }
            for (auto it = missing.rbegin(); it != missing.rend(); ++it)
            {
                if (::mkdir(it->c_str(), 0700) != 0 && errno != EEXIST)
                {
                    error = std::string(what) + " could not be created";
                    return false;
                }
            }

            const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (descriptor < 0)
            {
                error = std::string(what) + " is not a real directory (symbolic links are not allowed)";
                return false;
            }
            struct stat info {};
            bool ok = ::fstat(descriptor, &info) == 0;
            if (!ok || info.st_uid != ::geteuid())
            {
                ::close(descriptor);
                error = std::string(what) + " is not owned by the current user";
                return false;
            }
            if ((info.st_mode & 077) != 0 && ::fchmod(descriptor, 0700) != 0)
            {
                ::close(descriptor);
                error = std::string(what) + " could not be made owner-only";
                return false;
            }
            ::close(descriptor);

            canonical = std::filesystem::canonical(path, code);
            if (code)
            {
                error = std::string(what) + " could not be resolved";
                return false;
            }
            return true;
        }

        // A profile directory is deleted wholesale on sign-out, so the adapter
        // only ever adopts a directory that is empty or already holds a browser
        // profile (Chromium's Default / Local State, or this adapter's own log).
        bool IsEmptyOrBrowserProfile(const std::filesystem::path& directory)
        {
            std::error_code code;
            std::filesystem::directory_iterator it(directory, code);
            if (code)
                return false;
            bool any = false;
            for (; it != std::filesystem::directory_iterator(); it.increment(code))
            {
                if (code)
                    return false;
                any = true;
                const std::string name = it->path().filename().string();
                if (name == "Default" || name == "Local State" || name == kLogFileName)
                    return true;
            }
            return !any;
        }

        bool IsSameOrNested(const std::filesystem::path& child, const std::filesystem::path& parent)
        {
            const std::filesystem::path relative = child.lexically_relative(parent);
            return !relative.empty() && *relative.begin() != "..";
        }

        bool IsInsideProject(const std::filesystem::path& path)
        {
            std::error_code code;
            for (std::filesystem::path directory = path.parent_path();; directory = directory.parent_path())
            {
                std::filesystem::directory_iterator it(directory, code);
                if (!code)
                {
                    for (; it != std::filesystem::directory_iterator(); it.increment(code))
                    {
                        if (code)
                            break;
                        if (it->path().extension() == ".spiralproject")
                            return true;
                    }
                }
                if (directory == directory.root_path() || directory.empty())
                    return false;
            }
        }

        // Chromium aborts the whole process (a FATAL, not an error return) when it
        // finds no usable sandbox, so the exact kernel facilities its zygote
        // needs are probed first in a throwaway child. CLONE_NEWUSER alone is not
        // enough: an AppArmor-restricted kernel lets it succeed but strips the
        // capabilities, which makes the PID and network namespaces fail.
        bool ProbeUserNamespaceSandbox(std::string& error)
        {
            int channel[2];
            if (::pipe2(channel, O_CLOEXEC) != 0)
            {
                error = "the sandbox probe could not create a pipe";
                return false;
            }
            const pid_t child = ::fork();
            if (child < 0)
            {
                ::close(channel[0]);
                ::close(channel[1]);
                error = "the sandbox probe could not start a child process";
                return false;
            }
            if (child == 0)
            {
                // Async-signal-safe calls only: the parent may be multithreaded.
                unsigned char result = 0;
                if (::unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET) != 0)
                    result = static_cast<unsigned char>(errno);
                const ssize_t written = ::write(channel[1], &result, 1);
                (void)written;
                ::_exit(0);
            }
            ::close(channel[1]);
            unsigned char result = 0;
            ssize_t count = 0;
            do
            {
                count = ::read(channel[0], &result, 1);
            } while (count < 0 && errno == EINTR);
            ::close(channel[0]);
            int status = 0;
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
            {
            }
            if (count != 1)
            {
                error = "the sandbox probe child ended without a result";
                return false;
            }
            if (result != 0)
            {
                error = "the Chromium sandbox is unavailable: this system denies unprivileged user namespaces (error "
                    + std::to_string(static_cast<int>(result)) + ")";
                return false;
            }
            return true;
        }

        std::filesystem::path ModuleDirectory()
        {
            Dl_info info {};
            if (::dladdr(reinterpret_cast<void*>(&ModuleDirectory), &info) == 0 || info.dli_fname == nullptr)
                return {};
            std::error_code code;
            std::filesystem::path module = std::filesystem::absolute(info.dli_fname, code);
            return code ? std::filesystem::path() : module.parent_path();
        }

        using Event = std::function<void(IBrowserSurface::Listener&)>;
    }

    class BrowserClient;

    struct CefBrowserSurface::State
    {
        explicit State(CefBrowserSurfaceTestOptions testOptions)
            : Options(std::move(testOptions))
        {
        }

        struct DownloadRecord
        {
            std::string Name;
            std::filesystem::path Staged;
            Engine::u64 TotalBytes = 0;
            int64_t LastProgressMilliseconds = 0;
            bool PolicyCancelled = false;
            CefRefPtr<CefDownloadItemCallback> Callback;
        };

        CefBrowserSurfaceTestOptions Options;
        Listener* Sink = nullptr;
        BrowserSurfaceConfig Config;
        std::filesystem::path Profile;
        std::filesystem::path Staging;

        bool Started = false;
        bool ShutDown = false;
        bool Closed = true;
        bool CloseRequested = false;
        bool ClearOnShutdown = false;
        bool InPump = false;
        bool FlushRequested = false;
        std::shared_ptr<bool> FlushDone = std::make_shared<bool>(false);

        std::shared_ptr<PumpSignal> Signal = std::make_shared<PumpSignal>();
        int64_t LastPumpMilliseconds = 0;
        CefRefPtr<CefApp> App;
        CefRefPtr<BrowserClient> Client;
        CefRefPtr<CefBrowser> Browser;

        BrowserViewSize View { 800, 600, 1.0f };
        BrowserScreenInfo Screen;

        // The most recent refused navigation the user could be asked about; empty
        // when there is none or it cannot be repeated (not a GET). It never leaves
        // this object and is never logged.
        std::string LastDeniedUrl;
        std::string PendingLoad;

        std::vector<Engine::u8> Frame;
        Engine::u32 FrameWidth = 0;
        Engine::u32 FrameHeight = 0;
        std::vector<BrowserDirtyRect> PendingDirty;
        bool FramePending = false;
        Engine::u64 FrameSequence = 0;

        bool PopupVisible = false;
        CefRect PopupRect;
        std::vector<Engine::u8> PopupPixels;
        int PopupWidth = 0;
        int PopupHeight = 0;

        std::vector<Event> Events;
        BrowserCursor LastCursor = BrowserCursor::Arrow;
        std::map<uint32_t, DownloadRecord> Downloads;
        std::vector<int> SwallowedKeys;

        bool StartEngine(const BrowserSurfaceConfig& config, Listener& listener, std::string& error);
        void Pump();
        void Queue(Event event) { Events.push_back(std::move(event)); }
        void DeliverQueued();

        CefRect ViewRect() const;
        void OnViewPaint(const std::vector<CefRect>& dirty, const void* buffer, int width, int height);
        void OnPopupPaint(const void* buffer, int width, int height);
        void BlitPopup();
        void AddDirty(const BrowserDirtyRect& rect);

        bool AllowTopLevel(const std::string& url, bool repeatable = true);
        void DenyNavigation(const std::string& url, bool repeatable = true);
        void OpenPopupTarget(const std::string& url);
        CefRect ScaledRect(int x, int y, int width, int height) const;
        bool BeforeDownload(CefRefPtr<CefDownloadItem> item, const std::string& suggestedName,
            CefRefPtr<CefBeforeDownloadCallback> callback);
        void DownloadUpdated(CefRefPtr<CefDownloadItem> item, CefRefPtr<CefDownloadItemCallback> callback);
        void QueueDownload(BrowserDownloadEvent::State kind, uint32_t id, const DownloadRecord& record, Engine::u64 bytes);

        void SendKey(const BrowserKey& key);
        bool TryEditingCommand(const BrowserKey& key);
        void SendChar(const BrowserKey& key);

        void RequestClose();
        void Shutdown(Engine::u32 timeoutMilliseconds);
        void DeleteProfile();
    };

    namespace
    {
        class BrowserApp final : public CefApp, public CefBrowserProcessHandler
        {
        public:
            BrowserApp(std::shared_ptr<PumpSignal> signal, std::vector<std::string> switches, BrowserRenderMode renderMode)
                : m_Signal(std::move(signal))
                , m_Switches(std::move(switches))
                , m_RenderMode(renderMode)
            {
            }

            CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

            void OnBeforeCommandLineProcessing(const CefString& processType, CefRefPtr<CefCommandLine> commandLine) override
            {
                if (!processType.empty())
                    return;
                // No display-server connection at all: the panel renders off screen
                // and the Editor bridges cursor, IME and clipboard itself.
                commandLine->AppendSwitchWithValue("ozone-platform", "headless");
                if (m_RenderMode == BrowserRenderMode::Software)
                {
                    commandLine->AppendSwitch("disable-gpu");
                    commandLine->AppendSwitch("enable-unsafe-swiftshader");
                }
                else
                {
                    for (const EngineSwitch& entry : kHardwareSwitches)
                    {
                        if (entry.Value != nullptr)
                            commandLine->AppendSwitchWithValue(entry.Name, entry.Value);
                        else
                            commandLine->AppendSwitch(entry.Name);
                    }
                }
                for (const std::string& entry : m_Switches)
                {
                    const size_t equals = entry.find('=');
                    if (equals == std::string::npos)
                        commandLine->AppendSwitch(entry);
                    else
                        commandLine->AppendSwitchWithValue(entry.substr(0, equals), entry.substr(equals + 1));
                }
            }

            void OnScheduleMessagePumpWork(int64_t delayMilliseconds) override { m_Signal->Schedule(delayMilliseconds); }

            // A second instance forwards its command line here; it must not open a window.
            bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine>, const CefString&) override { return true; }

        private:
            std::shared_ptr<PumpSignal> m_Signal;
            std::vector<std::string> m_Switches;
            BrowserRenderMode m_RenderMode;

            IMPLEMENT_REFCOUNTING(BrowserApp);
        };

        class FlushCallback final : public CefCompletionCallback
        {
        public:
            explicit FlushCallback(std::shared_ptr<bool> done)
                : m_Done(std::move(done))
            {
            }

            void OnComplete() override { *m_Done = true; }

        private:
            std::shared_ptr<bool> m_Done;

            IMPLEMENT_REFCOUNTING(FlushCallback);
        };
    }

    class BrowserClient final : public CefClient,
                                public CefRenderHandler,
                                public CefLifeSpanHandler,
                                public CefRequestHandler,
                                public CefDownloadHandler,
                                public CefLoadHandler,
                                public CefDisplayHandler,
                                public CefContextMenuHandler,
                                public CefJSDialogHandler,
                                public CefPermissionHandler,
                                public CefDialogHandler,
                                public CefDragHandler
    {
    public:
        explicit BrowserClient(CefBrowserSurface::State* state)
            : m_State(state)
        {
        }

        // Called before the state is destroyed; late callbacks become no-ops.
        void Detach() { m_State = nullptr; }

        CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
        CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
        CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
        CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
        CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
        CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
        CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
        CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
        CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
        CefRefPtr<CefDragHandler> GetDragHandler() override { return this; }

        // Render handler
        void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
        {
            rect = m_State != nullptr ? m_State->ViewRect() : CefRect(0, 0, 1, 1);
        }

        // With platform geometry the page sees the real monitor and work area, which
        // do not change when the panel is resized; without it the view rectangle
        // stands in for both, as before. GetRootScreenRect and GetScreenPoint are
        // deliberately not implemented: with CEF 154 windowless rendering neither
        // changes window.outerWidth/outerHeight or window.screenX/screenY (measured:
        // they stay 0 whatever is returned), and a root rectangle larger than the
        // view would only move popup widgets such as select lists out of the view.
        bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override
        {
            if (m_State == nullptr)
                return false;
            const BrowserScreenInfo& screen = m_State->Screen;
            if (screen.Valid)
            {
                info.Set(m_State->View.DeviceScale, 24, 8, false,
                    m_State->ScaledRect(screen.MonitorX, screen.MonitorY, screen.MonitorWidth, screen.MonitorHeight),
                    m_State->ScaledRect(screen.WorkX, screen.WorkY, screen.WorkWidth, screen.WorkHeight));
                return true;
            }
            const CefRect rect = m_State->ViewRect();
            info.Set(m_State->View.DeviceScale, 24, 8, false, rect, rect);
            return true;
        }

        void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override
        {
            if (m_State == nullptr)
                return;
            m_State->PopupVisible = show;
            // Hiding a popup leaves its pixels in the composited frame; a full
            // view repaint overwrites them.
            if (!show)
                browser->GetHost()->Invalidate(PET_VIEW);
        }

        void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override
        {
            if (m_State != nullptr)
                m_State->PopupRect = rect;
        }

        void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirtyRects, const void* buffer,
            int width, int height) override
        {
            if (m_State == nullptr)
                return;
            if (type == PET_VIEW)
                m_State->OnViewPaint(dirtyRects, buffer, width, height);
            else
                m_State->OnPopupPaint(buffer, width, height);
        }

        // Page-initiated drags are not supported; abort them.
        bool StartDragging(CefRefPtr<CefBrowser>, CefRefPtr<CefDragData>, cef_drag_operations_mask_t, int, int) override
        {
            return false;
        }

        bool OnDragEnter(CefRefPtr<CefBrowser>, CefRefPtr<CefDragData>, cef_drag_operations_mask_t) override { return true; }

        // Life span: a popup window is never created. A target on the allow list is
        // opened in this panel instead (window.opener does not survive that); any
        // other target is denied and reported.
        bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int, const CefString& targetUrl, const CefString&,
            cef_window_open_disposition_t, bool, const CefPopupFeatures&, CefWindowInfo&, CefRefPtr<CefClient>&,
            CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override
        {
            if (m_State != nullptr)
                m_State->OpenPopupTarget(targetUrl.ToString());
            return true;
        }

        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
        {
            if (m_State != nullptr)
                m_State->Browser = browser;
        }

        void OnBeforeClose(CefRefPtr<CefBrowser>) override
        {
            if (m_State == nullptr)
                return;
            m_State->Browser = nullptr;
            m_State->Closed = true;
            m_State->Queue([](IBrowserSurface::Listener& listener) { listener.OnClosed(); });
        }

        // Request handler: only top-level documents are gated; frames and
        // sub-resources are outside the navigation policy by design.
        bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool,
            bool) override
        {
            if (m_State == nullptr || !frame->IsMain())
                return false;
            // The decision looks at the destination only; the method is used just to
            // know whether a denied request could be repeated as a plain load.
            return !m_State->AllowTopLevel(request->GetURL().ToString(), request->GetMethod().ToString() == "GET");
        }

        bool OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString& targetUrl,
            cef_window_open_disposition_t, bool) override
        {
            if (m_State != nullptr)
                m_State->OpenPopupTarget(targetUrl.ToString());
            return true;
        }

        void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus status, int, const CefString&) override
        {
            if (m_State == nullptr)
                return;
            const std::string reason = "the browser content process terminated (status " + std::to_string(static_cast<int>(status)) + ")";
            m_State->Queue([reason](IBrowserSurface::Listener& listener) { listener.OnFailed(reason); });
        }

        // Downloads
        bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item, const CefString& suggestedName,
            CefRefPtr<CefBeforeDownloadCallback> callback) override
        {
            if (m_State == nullptr)
                return false;
            return m_State->BeforeDownload(item, suggestedName.ToString(), callback);
        }

        void OnDownloadUpdated(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> item,
            CefRefPtr<CefDownloadItemCallback> callback) override
        {
            if (m_State != nullptr)
                m_State->DownloadUpdated(item, callback);
        }

        // Load and display
        void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool isLoading, bool canGoBack, bool canGoForward) override
        {
            if (m_State == nullptr)
                return;
            m_State->Queue([isLoading, canGoBack, canGoForward](IBrowserSurface::Listener& listener)
            {
                listener.OnLoadState(isLoading, canGoBack, canGoForward);
            });
        }

        void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText,
            const CefString&) override
        {
            if (m_State == nullptr || !frame->IsMain() || errorCode == ERR_ABORTED)
                return;
            const std::string reason = "the page failed to load (" + errorText.ToString() + ", net error "
                + std::to_string(static_cast<int>(errorCode)) + ")";
            m_State->Queue([reason](IBrowserSurface::Listener& listener) { listener.OnFailed(reason); });
        }

        void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& url) override
        {
            if (m_State == nullptr || !frame->IsMain())
                return;
            const std::string address = BrowserNavigationPolicy::DisplayAddress(url.ToString());
            m_State->Queue([address](IBrowserSurface::Listener& listener) { listener.OnAddress(address); });
        }

        bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle, cef_cursor_type_t type, const CefCursorInfo&) override
        {
            if (m_State == nullptr)
                return true;
            const BrowserCursor cursor = MapCursor(type);
            if (cursor != m_State->LastCursor)
            {
                m_State->LastCursor = cursor;
                m_State->Queue([cursor](IBrowserSurface::Listener& listener) { listener.OnCursor(cursor); });
            }
            return true;
        }

        // Page console output can carry URLs and tokens; it is dropped.
        bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString&, const CefString&, int) override
        {
            return true;
        }

        void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>,
            CefRefPtr<CefMenuModel> model) override
        {
            model->Clear();
        }

        // The panel has no dialog UI: script dialogs, file choosers, and
        // permission prompts are refused rather than left pending.
        bool OnJSDialog(CefRefPtr<CefBrowser>, const CefString&, JSDialogType, const CefString&, const CefString&,
            CefRefPtr<CefJSDialogCallback> callback, bool&) override
        {
            callback->Continue(false, CefString());
            return true;
        }

        bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString&, bool, CefRefPtr<CefJSDialogCallback> callback) override
        {
            callback->Continue(true, CefString());
            return true;
        }

        bool OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode, const CefString&, const CefString&,
            const std::vector<CefString>&, const std::vector<CefString>&, const std::vector<CefString>&,
            CefRefPtr<CefFileDialogCallback> callback) override
        {
            callback->Cancel();
            return true;
        }

        bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString&, uint32_t,
            CefRefPtr<CefMediaAccessCallback> callback) override
        {
            callback->Cancel();
            return true;
        }

        bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t, const CefString&, uint32_t,
            CefRefPtr<CefPermissionPromptCallback> callback) override
        {
            callback->Continue(CEF_PERMISSION_RESULT_DENY);
            return true;
        }

    private:
        CefBrowserSurface::State* m_State;

        IMPLEMENT_REFCOUNTING(BrowserClient);
    };

    // State: engine start

    bool CefBrowserSurface::State::StartEngine(const BrowserSurfaceConfig& config, Listener& listener, std::string& error)
    {
        if (!ValidateBrowserSurfaceConfig(config, error))
            return false;
        if (Started || ShutDown)
        {
            error = "the browser surface was already initialized";
            return false;
        }
        if (g_EngineLifecycleClaimed)
        {
            error = g_EngineLifecycleFailure.empty()
                ? "the browser engine can only be started once per process"
                : g_EngineLifecycleFailure;
            return false;
        }

        const std::filesystem::path moduleDirectory = ModuleDirectory();
        const std::filesystem::path resourceDirectory = config.ResourceDir.empty() ? moduleDirectory : config.ResourceDir;
        const std::filesystem::path helperPath =
            config.HelperPath.empty() ? resourceDirectory / "SpiralBrowserHelper" : config.HelperPath;
        for (const char* required : { "icudtl.dat", "v8_context_snapshot.bin", "resources.pak", "chrome_100_percent.pak",
                 "locales/en-US.pak" })
        {
            std::error_code code;
            if (resourceDirectory.empty() || !std::filesystem::exists(resourceDirectory / required, code))
            {
                error = std::string("the browser runtime is incomplete: ") + required + " is missing";
                return false;
            }
        }
        if (helperPath.empty() || ::access(helperPath.c_str(), X_OK) != 0)
        {
            error = "the browser helper executable is missing or not executable";
            return false;
        }

        // Cheapest and side-effect-free checks first: nothing is created on disk
        // for a system that cannot run the sandbox or for a profile inside a project.
        if (!ProbeUserNamespaceSandbox(error))
            return false;
        constexpr char kInsideProject[] = "the browser profile directory must not be inside a project";
        if (IsInsideProject(config.ProfileDir))
        {
            error = kInsideProject;
            return false;
        }
        if (!PreparePrivateDirectory(config.ProfileDir, "the browser profile directory", Profile, error)
            || !PreparePrivateDirectory(config.DownloadStagingDir, "the download staging directory", Staging, error))
        {
            return false;
        }
        if (IsSameOrNested(Staging, Profile) || IsSameOrNested(Profile, Staging))
        {
            error = "the profile and download staging directories must be distinct and not nested";
            return false;
        }
        if (IsInsideProject(Profile))
        {
            error = kInsideProject;
            return false;
        }
        if (!IsEmptyOrBrowserProfile(Profile))
        {
            error = "the browser profile directory is not empty and is not a browser profile";
            return false;
        }

        Config = config;
        Sink = &listener;

        App = new BrowserApp(Signal, Options.CommandLineSwitches, config.RenderMode);
        static char programName[] = "SpiralBrowserHost";
        static char* arguments[] = { programName, nullptr };
        CefMainArgs mainArgs(1, arguments);

        CefSettings settings;
        settings.no_sandbox = 0;
        settings.windowless_rendering_enabled = 1;
        settings.multi_threaded_message_loop = 0;
        settings.external_message_pump = 1;
        settings.persist_session_cookies = 1;
        settings.disable_signal_handlers = 1;
        settings.log_severity = LOGSEVERITY_ERROR;
        CefString(&settings.log_file) = (Profile / kLogFileName).string();
        CefString(&settings.root_cache_path) = Profile.string();
        CefString(&settings.cache_path) = (Profile / "Default").string();
        CefString(&settings.browser_subprocess_path) = helperPath.string();
        CefString(&settings.resources_dir_path) = resourceDirectory.string();
        CefString(&settings.locales_dir_path) = (resourceDirectory / "locales").string();
        CefString(&settings.locale) = "en-US";
        // Accept-Language and navigator.languages follow the user's own locale
        // variables (settings.locale above is ignored on Linux). Nothing else about
        // the browser identity is set: the user agent, client hints, and cookie
        // policy stay at the engine defaults.
        CefString(&settings.accept_language_list) = BuildAcceptLanguageList(
            EnvironmentValue("LANGUAGE"), EnvironmentValue("LC_ALL"), EnvironmentValue("LC_MESSAGES"), EnvironmentValue("LANG"));

        // CefInitialize installs Chromium's shutdown detector over SIGHUP, SIGINT and
        // SIGTERM even with disable_signal_handlers, which would make a terminal
        // Ctrl+C skip the Editor's own exit path. The host process owns those signals.
        constexpr int kHostSignals[] = { SIGHUP, SIGINT, SIGTERM };
        struct sigaction savedActions[std::size(kHostSignals)] = {};
        for (size_t i = 0; i < std::size(kHostSignals); ++i)
            ::sigaction(kHostSignals[i], nullptr, &savedActions[i]);

        g_EngineLifecycleClaimed = true;
        const bool initialized = CefInitialize(mainArgs, settings, App, nullptr);
        for (size_t i = 0; i < std::size(kHostSignals); ++i)
            ::sigaction(kHostSignals[i], &savedActions[i], nullptr);
        if (!initialized)
        {
            const int exitCode = CefGetExitCode();
            if (exitCode == CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED || exitCode == CEF_RESULT_CODE_PROFILE_IN_USE)
                error = "the browser profile is in use by another running Spiral instance";
            else
                error = "the browser engine failed to initialize (exit code " + std::to_string(exitCode) + ")";
            g_EngineLifecycleFailure = error + "; restart the Editor to try again";
            App = nullptr;
            Sink = nullptr;
            return false;
        }
        Started = true;

        Client = new BrowserClient(this);
        CefWindowInfo windowInfo;
        windowInfo.SetAsWindowless(0);
        CefBrowserSettings browserSettings;
        browserSettings.windowless_frame_rate = static_cast<int>(config.MaxFps);
        browserSettings.background_color = 0xFFFFFFFF;
        Browser = CefBrowserHost::CreateBrowserSync(windowInfo, Client, kBlankUrl, browserSettings, nullptr, nullptr);
        if (!Browser)
        {
            error = "the browser could not be created";
            g_EngineLifecycleFailure = error + "; restart the Editor to try again";
            Client->Detach();
            Client = nullptr;
            CefShutdown();
            Started = false;
            ShutDown = true;
            App = nullptr;
            Sink = nullptr;
            return false;
        }
        Closed = false;
        CefRefPtr<CefBrowserHost> host = Browser->GetHost();
        host->SetWindowlessFrameRate(static_cast<int>(config.MaxFps));
        host->SetFocus(false);
        host->WasResized();
        LastPumpMilliseconds = NowMilliseconds();
        return true;
    }

    void CefBrowserSurface::State::Pump()
    {
        if (!Started || ShutDown || InPump)
            return;
        InPump = true;
        const int64_t now = NowMilliseconds();
        if (now >= Signal->DueMilliseconds.load() || now - LastPumpMilliseconds >= kPumpCapMilliseconds)
        {
            Signal->DueMilliseconds.store(std::numeric_limits<int64_t>::max());
            CefDoMessageLoopWork();
            LastPumpMilliseconds = NowMilliseconds();
        }
        if (!PendingLoad.empty() && Browser)
        {
            // Started outside the engine callback that asked for it.
            const std::string url = std::move(PendingLoad);
            PendingLoad.clear();
            Browser->GetMainFrame()->LoadURL(url);
        }
        DeliverQueued();
        InPump = false;
    }

    void CefBrowserSurface::State::DeliverQueued()
    {
        if (Sink == nullptr)
            return;
        // Swapped out first so a listener that calls back into the surface only
        // queues work for the next Pump().
        std::vector<Event> events;
        events.swap(Events);
        for (const Event& event : events)
            event(*Sink);

        if (FramePending && !Frame.empty())
        {
            FramePending = false;
            std::vector<BrowserDirtyRect> dirty;
            dirty.swap(PendingDirty);
            BrowserFrameView view;
            view.Bgra = Frame.data();
            view.Width = FrameWidth;
            view.Height = FrameHeight;
            view.StrideBytes = FrameWidth * 4u;
            view.Dirty = dirty;
            view.Sequence = ++FrameSequence;
            Sink->OnFrame(view);
        }
    }

    // State: frames

    CefRect CefBrowserSurface::State::ViewRect() const
    {
        return CefRect(0, 0, static_cast<int>(View.Width), static_cast<int>(View.Height));
    }

    void CefBrowserSurface::State::AddDirty(const BrowserDirtyRect& rect)
    {
        PendingDirty.push_back(rect);
        if (PendingDirty.size() <= kMaximumPendingDirtyRects)
            return;
        int left = PendingDirty.front().X;
        int top = PendingDirty.front().Y;
        int right = left + PendingDirty.front().Width;
        int bottom = top + PendingDirty.front().Height;
        for (const BrowserDirtyRect& other : PendingDirty)
        {
            left = std::min(left, other.X);
            top = std::min(top, other.Y);
            right = std::max(right, other.X + other.Width);
            bottom = std::max(bottom, other.Y + other.Height);
        }
        PendingDirty.assign(1, BrowserDirtyRect { left, top, right - left, bottom - top });
    }

    void CefBrowserSurface::State::OnViewPaint(const std::vector<CefRect>& dirty, const void* buffer, int width, int height)
    {
        if (width <= 0 || height <= 0 || buffer == nullptr)
            return;
        const auto* source = static_cast<const Engine::u8*>(buffer);
        const size_t stride = static_cast<size_t>(width) * 4u;
        if (static_cast<Engine::u32>(width) != FrameWidth || static_cast<Engine::u32>(height) != FrameHeight)
        {
            FrameWidth = static_cast<Engine::u32>(width);
            FrameHeight = static_cast<Engine::u32>(height);
            Frame.assign(source, source + stride * static_cast<size_t>(height));
            PendingDirty.assign(1, BrowserDirtyRect { 0, 0, width, height });
        }
        else
        {
            bool copied = false;
            for (const CefRect& rect : dirty)
            {
                const int left = std::max(rect.x, 0);
                const int top = std::max(rect.y, 0);
                const int right = std::min(rect.x + rect.width, width);
                const int bottom = std::min(rect.y + rect.height, height);
                if (right <= left || bottom <= top)
                    continue;
                for (int row = top; row < bottom; ++row)
                {
                    const size_t offset = static_cast<size_t>(row) * stride + static_cast<size_t>(left) * 4u;
                    std::memcpy(Frame.data() + offset, source + offset, static_cast<size_t>(right - left) * 4u);
                }
                AddDirty(BrowserDirtyRect { left, top, right - left, bottom - top });
                copied = true;
            }
            // No usable change set: trust nothing and take the whole frame.
            if (!copied)
            {
                std::memcpy(Frame.data(), source, stride * static_cast<size_t>(height));
                PendingDirty.assign(1, BrowserDirtyRect { 0, 0, width, height });
            }
        }
        if (PopupVisible)
            BlitPopup();
        FramePending = true;
    }

    void CefBrowserSurface::State::OnPopupPaint(const void* buffer, int width, int height)
    {
        if (width <= 0 || height <= 0 || buffer == nullptr)
            return;
        const auto* source = static_cast<const Engine::u8*>(buffer);
        PopupWidth = width;
        PopupHeight = height;
        PopupPixels.assign(source, source + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        if (PopupVisible && !Frame.empty())
        {
            BlitPopup();
            FramePending = true;
        }
    }

    void CefBrowserSurface::State::BlitPopup()
    {
        if (PopupPixels.empty() || Frame.empty())
            return;
        const float scale = View.DeviceScale;
        const int originX = static_cast<int>(std::lround(PopupRect.x * scale));
        const int originY = static_cast<int>(std::lround(PopupRect.y * scale));
        const int left = std::max(originX, 0);
        const int top = std::max(originY, 0);
        const int right = std::min(originX + PopupWidth, static_cast<int>(FrameWidth));
        const int bottom = std::min(originY + PopupHeight, static_cast<int>(FrameHeight));
        if (right <= left || bottom <= top)
            return;
        for (int row = top; row < bottom; ++row)
        {
            const Engine::u8* from = PopupPixels.data()
                + (static_cast<size_t>(row - originY) * static_cast<size_t>(PopupWidth) + static_cast<size_t>(left - originX)) * 4u;
            Engine::u8* to = Frame.data() + (static_cast<size_t>(row) * FrameWidth + static_cast<size_t>(left)) * 4u;
            std::memcpy(to, from, static_cast<size_t>(right - left) * 4u);
        }
        AddDirty(BrowserDirtyRect { left, top, right - left, bottom - top });
    }

    // State: navigation

    bool CefBrowserSurface::State::AllowTopLevel(const std::string& url, bool repeatable)
    {
        if (url == kBlankUrl)
            return true;
        if (IsNavigationAllowed(Config.Navigation.Evaluate(url, BrowserNavigationKind::TopLevel)))
            return true;
        DenyNavigation(url, repeatable);
        return false;
    }

    void CefBrowserSurface::State::DenyNavigation(const std::string& url, bool repeatable)
    {
        const std::string host = BrowserNavigationPolicy::HostForLog(url);
        Queue([host](IBrowserSurface::Listener& listener) { listener.OnNavigationDenied(host); });

        // Only a target the user could sensibly be asked about is kept; any other
        // denial forgets the previous one so a stale address is never repeated.
        const std::string consent = Config.Navigation.ConsentHost(url);
        LastDeniedUrl.clear();
        if (consent.empty())
            return;
        if (repeatable)
            LastDeniedUrl = url;
        Queue([consent](IBrowserSurface::Listener& listener) { listener.OnNavigationConsentOffered(consent); });
    }

    void CefBrowserSurface::State::OpenPopupTarget(const std::string& url)
    {
        if (Config.Navigation.EvaluatePopupTarget(url) == BrowserNavigationVerdict::Allow)
        {
            PendingLoad = url;
            const std::string host = BrowserNavigationPolicy::HostForLog(url);
            Queue([host](IBrowserSurface::Listener& listener) { listener.OnPopupRedirected(host); });
            return;
        }
        DenyNavigation(url);
    }

    CefRect CefBrowserSurface::State::ScaledRect(int x, int y, int width, int height) const
    {
        const float scale = std::max(View.DeviceScale, 0.25f);
        const auto toDip = [scale](int value) { return static_cast<int>(std::lround(static_cast<float>(value) / scale)); };
        return CefRect(toDip(x), toDip(y), std::max(toDip(width), 1), std::max(toDip(height), 1));
    }

    // State: downloads

    void CefBrowserSurface::State::QueueDownload(
        BrowserDownloadEvent::State kind, uint32_t id, const DownloadRecord& record, Engine::u64 bytes)
    {
        BrowserDownloadEvent event;
        event.Kind = kind;
        event.Id = id;
        event.DisplayName = record.Name;
        event.StagedPath = record.Staged;
        event.Bytes = bytes;
        event.TotalBytes = record.TotalBytes;
        Queue([event](IBrowserSurface::Listener& listener) { listener.OnDownload(event); });
    }

    bool CefBrowserSurface::State::BeforeDownload(
        CefRefPtr<CefDownloadItem> item, const std::string& suggestedName, CefRefPtr<CefBeforeDownloadCallback> callback)
    {
        const uint32_t id = item->GetId();
        const int64_t declared = item->GetTotalBytes();
        BrowserDownloadOffer offer;
        offer.SuggestedName = suggestedName;
        offer.TotalBytes = declared > 0 ? static_cast<Engine::u64>(declared) : 0;
        const BrowserDownloadDecision decision = Config.Downloads.Evaluate(offer, RandomNonce());

        DownloadRecord record;
        record.Name = decision.SanitizedName;
        record.TotalBytes = offer.TotalBytes;
        if (decision.Verdict != BrowserDownloadVerdict::Accept)
        {
            // Returning true without running the callback cancels the download.
            QueueDownload(BrowserDownloadEvent::State::Blocked, id, record, 0);
            return true;
        }

        std::string pathError;
        const std::filesystem::path staged = BrowserDownloadPolicy::ResolveStagedPath(Staging, decision, pathError);
        if (staged.empty() || (::mkdir(staged.parent_path().c_str(), 0700) != 0 && errno != EEXIST))
        {
            QueueDownload(BrowserDownloadEvent::State::Failed, id, record, 0);
            return true;
        }
        record.Staged = staged;
        record.LastProgressMilliseconds = NowMilliseconds();
        QueueDownload(BrowserDownloadEvent::State::Started, id, record, 0);
        Downloads[id] = std::move(record);
        callback->Continue(staged.string(), false);
        return true;
    }

    void CefBrowserSurface::State::DownloadUpdated(
        CefRefPtr<CefDownloadItem> item, CefRefPtr<CefDownloadItemCallback> callback)
    {
        const uint32_t id = item->GetId();
        const auto found = Downloads.find(id);
        // Updates also arrive before OnBeforeDownload and for downloads the policy
        // refused (cancelled there by not continuing); neither is tracked.
        if (found == Downloads.end())
            return;
        DownloadRecord& record = found->second;
        const int64_t received = item->GetReceivedBytes();
        const Engine::u64 bytes = received > 0 ? static_cast<Engine::u64>(received) : 0;
        std::error_code code;

        if (item->IsComplete())
        {
            ::chmod(record.Staged.c_str(), 0600);
            QueueDownload(BrowserDownloadEvent::State::Completed, id, record, bytes);
            Downloads.erase(found);
        }
        else if (item->IsCanceled())
        {
            std::filesystem::remove(record.Staged, code);
            if (!record.PolicyCancelled)
                QueueDownload(BrowserDownloadEvent::State::Cancelled, id, record, bytes);
            Downloads.erase(found);
        }
        else if (!item->IsInProgress())
        {
            std::filesystem::remove(record.Staged, code);
            QueueDownload(BrowserDownloadEvent::State::Failed, id, record, bytes);
            Downloads.erase(found);
        }
        else if (!Config.Downloads.WithinSizeLimit(bytes))
        {
            record.PolicyCancelled = true;
            QueueDownload(BrowserDownloadEvent::State::Blocked, id, record, bytes);
            if (callback)
                callback->Cancel();
        }
        else
        {
            record.Callback = callback;
            const int64_t now = NowMilliseconds();
            if (static_cast<Engine::u64>(now - record.LastProgressMilliseconds) >= kProgressIntervalMilliseconds)
            {
                record.LastProgressMilliseconds = now;
                QueueDownload(BrowserDownloadEvent::State::Progress, id, record, bytes);
            }
        }
    }

    // State: keyboard

    bool CefBrowserSurface::State::TryEditingCommand(const BrowserKey& key)
    {
        if ((key.Modifiers & BrowserModifier::Control) == 0 || (key.Modifiers & (BrowserModifier::Alt | BrowserModifier::Super)) != 0)
            return false;
        const bool shift = (key.Modifiers & BrowserModifier::Shift) != 0;
        CefRefPtr<CefFrame> frame = Browser->GetFocusedFrame();
        if (!frame)
            frame = Browser->GetMainFrame();
        if (!frame)
            return false;
        switch (key.WindowsVirtualKey)
        {
        case 'A':
            if (shift)
                return false;
            frame->SelectAll();
            return true;
        case 'C':
            if (shift)
                return false;
            frame->Copy();
            return true;
        case 'X':
            if (shift)
                return false;
            frame->Cut();
            return true;
        case 'V':
            if (shift)
                return false;
            frame->Paste();
            return true;
        case 'Z':
            if (shift)
                frame->Redo();
            else
                frame->Undo();
            return true;
        case 'Y':
            if (shift)
                return false;
            frame->Redo();
            return true;
        default:
            return false;
        }
    }

    void CefBrowserSurface::State::SendKey(const BrowserKey& key)
    {
        if (!Browser)
            return;
        if (key.Kind == BrowserKey::Phase::Char)
        {
            SendChar(key);
            return;
        }
        if (key.WindowsVirtualKey == 0)
            return;

        const auto swallowed = std::find(SwallowedKeys.begin(), SwallowedKeys.end(), key.GlfwKey);
        if (key.Kind == BrowserKey::Phase::Up && swallowed != SwallowedKeys.end())
        {
            SwallowedKeys.erase(swallowed);
            return;
        }
        if (key.Kind == BrowserKey::Phase::Down && TryEditingCommand(key))
        {
            if (swallowed == SwallowedKeys.end())
                SwallowedKeys.push_back(key.GlfwKey);
            return;
        }

        CefKeyEvent event;
        event.type = key.Kind == BrowserKey::Phase::Down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
        event.modifiers = ToCefModifiers(key.Modifiers);
        if (key.Repeat)
            event.modifiers |= EVENTFLAG_IS_REPEAT;
        // GLFW_KEY_KP_0..GLFW_KEY_KP_EQUAL
        if (key.GlfwKey >= 320 && key.GlfwKey <= 336)
            event.modifiers |= EVENTFLAG_IS_KEY_PAD;
        event.windows_key_code = key.WindowsVirtualKey;
        event.native_key_code = key.Scancode;
        event.is_system_key = false;
        event.character = 0;
        event.unmodified_character = 0;
        event.focus_on_editable_field = false;
        CefRefPtr<CefBrowserHost> host = Browser->GetHost();
        host->SendKeyEvent(event);

        // Enter's default action (submit, newline) runs on the character event.
        if (key.Kind == BrowserKey::Phase::Down && key.WindowsVirtualKey == kVkReturn)
        {
            event.type = KEYEVENT_CHAR;
            event.character = u'\r';
            event.unmodified_character = u'\r';
            host->SendKeyEvent(event);
        }
    }

    void CefBrowserSurface::State::SendChar(const BrowserKey& key)
    {
        const char32_t codepoint = key.Codepoint;
        if (codepoint < 0x20 || codepoint == 0x7F || (codepoint >= 0x80 && codepoint < 0xA0) || codepoint > 0x10FFFF)
            return;
        CefRefPtr<CefBrowserHost> host = Browser->GetHost();
        if (codepoint < 0x7F)
        {
            CefKeyEvent event;
            event.type = KEYEVENT_CHAR;
            event.modifiers = ToCefModifiers(key.Modifiers);
            event.windows_key_code = static_cast<int>(codepoint);
            event.native_key_code = key.Scancode;
            event.is_system_key = false;
            event.character = static_cast<char16_t>(codepoint);
            event.unmodified_character = static_cast<char16_t>(codepoint);
            event.focus_on_editable_field = false;
            host->SendKeyEvent(event);
            return;
        }
        // Composed or non-ASCII text arrives as an IME commit at the caret.
        host->ImeCommitText(CefString(EncodeUtf8(codepoint)), CefRange(UINT32_MAX, UINT32_MAX), 0);
    }

    // State: shutdown

    void CefBrowserSurface::State::RequestClose()
    {
        if (!Started || ShutDown || CloseRequested)
            return;
        CloseRequested = true;
        if (!ClearOnShutdown)
        {
            // Persist cookies now; a SIGKILL right after sign-in must not lose the session.
            CefRefPtr<CefCookieManager> cookies = CefCookieManager::GetGlobalManager(nullptr);
            if (cookies && cookies->FlushStore(new FlushCallback(FlushDone)))
                FlushRequested = true;
        }
        if (Browser)
            Browser->GetHost()->CloseBrowser(true);
        else
            Closed = true;
    }

    void CefBrowserSurface::State::Shutdown(Engine::u32 timeoutMilliseconds)
    {
        if (!Started || ShutDown)
            return;
        RequestClose();
        const int64_t deadline = NowMilliseconds() + timeoutMilliseconds;
        while (NowMilliseconds() < deadline && !(Closed && (!FlushRequested || *FlushDone)))
        {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // Let the close finish before the engine is torn down.
        for (int i = 0; i < 10; ++i)
        {
            CefDoMessageLoopWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Events.clear();
        FramePending = false;
        Browser = nullptr;
        if (Client)
        {
            Client->Detach();
            Client = nullptr;
        }
        CefShutdown();
        App = nullptr;
        Started = false;
        ShutDown = true;
        Closed = true;
        if (ClearOnShutdown)
            DeleteProfile();
    }

    void CefBrowserSurface::State::DeleteProfile()
    {
        if (Profile.empty() || !IsEmptyOrBrowserProfile(Profile))
            return;
        std::error_code code;
        std::filesystem::remove_all(Profile, code);
    }

    // CefBrowserSurface

    CefBrowserSurface::CefBrowserSurface(CefBrowserSurfaceTestOptions options)
        : m_State(std::make_unique<State>(std::move(options)))
    {
    }

    CefBrowserSurface::~CefBrowserSurface()
    {
        m_State->Shutdown(2000);
    }

    bool CefBrowserSurface::Initialize(const BrowserSurfaceConfig& config, Listener& listener, std::string& error)
    {
        return m_State->StartEngine(config, listener, error);
    }

    void CefBrowserSurface::Pump()
    {
        m_State->Pump();
    }

    void CefBrowserSurface::SetViewSize(const BrowserViewSize& size)
    {
        State& state = *m_State;
        float scale = std::isfinite(size.DeviceScale) ? size.DeviceScale : 1.0f;
        scale = std::clamp(scale, 0.25f, 8.0f);
        const Engine::u32 maximumDip = std::max<Engine::u32>(1, static_cast<Engine::u32>(kMaximumViewDimension / scale));
        const BrowserViewSize next { std::clamp<Engine::u32>(size.Width, 1, maximumDip),
            std::clamp<Engine::u32>(size.Height, 1, maximumDip), scale };
        const bool scaleChanged = next.DeviceScale != state.View.DeviceScale;
        const bool changed = scaleChanged || next.Width != state.View.Width || next.Height != state.View.Height;
        state.View = next;
        if (!changed || !state.Browser)
            return;
        CefRefPtr<CefBrowserHost> host = state.Browser->GetHost();
        if (scaleChanged)
            host->NotifyScreenInfoChanged();
        host->WasResized();
    }

    void CefBrowserSurface::SetVisible(bool visible)
    {
        if (!m_State->Browser)
            return;
        CefRefPtr<CefBrowserHost> host = m_State->Browser->GetHost();
        host->WasHidden(!visible);
        if (visible)
            host->Invalidate(PET_VIEW);
    }

    void CefBrowserSurface::SetFocus(bool focused)
    {
        if (m_State->Browser)
            m_State->Browser->GetHost()->SetFocus(focused);
    }

    void CefBrowserSurface::SendMouseMove(const BrowserMouse& mouse, bool leave)
    {
        if (m_State->Browser)
            m_State->Browser->GetHost()->SendMouseMoveEvent(ToCefMouse(mouse), leave);
    }

    void CefBrowserSurface::SendMouseButton(const BrowserMouse& mouse, BrowserMouseButton button, bool down, int clickCount)
    {
        if (!m_State->Browser)
            return;
        CefBrowserHost::MouseButtonType type = MBT_LEFT;
        if (button == BrowserMouseButton::Middle)
            type = MBT_MIDDLE;
        else if (button == BrowserMouseButton::Right)
            type = MBT_RIGHT;
        m_State->Browser->GetHost()->SendMouseClickEvent(ToCefMouse(mouse), type, !down, std::max(clickCount, 1));
    }

    void CefBrowserSurface::SendMouseCaptureLost()
    {
        if (m_State->Browser)
            m_State->Browser->GetHost()->SendCaptureLostEvent();
    }

    void CefBrowserSurface::SendMouseWheel(const BrowserMouse& mouse, float deltaX, float deltaY)
    {
        if (m_State->Browser)
        {
            m_State->Browser->GetHost()->SendMouseWheelEvent(
                ToCefMouse(mouse), static_cast<int>(std::lround(deltaX)), static_cast<int>(std::lround(deltaY)));
        }
    }

    void CefBrowserSurface::SendKey(const BrowserKey& key)
    {
        m_State->SendKey(key);
    }

    void CefBrowserSurface::SetScreenInfo(const BrowserScreenInfo& info)
    {
        State& state = *m_State;
        BrowserScreenInfo next = info;
        if (next.MonitorWidth < 1 || next.MonitorHeight < 1 || next.WorkWidth < 1 || next.WorkHeight < 1)
            next = BrowserScreenInfo {};
        const BrowserScreenInfo& previous = state.Screen;
        const bool monitorChanged = next.Valid != previous.Valid || next.MonitorX != previous.MonitorX
            || next.MonitorY != previous.MonitorY || next.MonitorWidth != previous.MonitorWidth
            || next.MonitorHeight != previous.MonitorHeight || next.WorkX != previous.WorkX || next.WorkY != previous.WorkY
            || next.WorkWidth != previous.WorkWidth || next.WorkHeight != previous.WorkHeight;
        state.Screen = next;
        if (monitorChanged && state.Browser)
            state.Browser->GetHost()->NotifyScreenInfoChanged();
    }

    void CefBrowserSurface::SetGrantedHosts(std::span<const std::string> hosts)
    {
        m_State->Config.Navigation.SetGrantedHosts(hosts);
    }

    bool CefBrowserSurface::RetryDeniedNavigation()
    {
        State& state = *m_State;
        const std::string url = std::move(state.LastDeniedUrl);
        state.LastDeniedUrl.clear();
        if (!state.Browser || url.empty()
            || state.Config.Navigation.Evaluate(url, BrowserNavigationKind::TopLevel) != BrowserNavigationVerdict::Allow)
        {
            return false;
        }
        state.Browser->GetMainFrame()->LoadURL(url);
        return true;
    }

    void CefBrowserSurface::Navigate(std::string_view httpsUrl)
    {
        State& state = *m_State;
        if (!state.Browser)
            return;
        const std::string url(httpsUrl);
        if (state.AllowTopLevel(url))
            state.Browser->GetMainFrame()->LoadURL(url);
    }

    void CefBrowserSurface::GoBack()
    {
        if (m_State->Browser)
            m_State->Browser->GoBack();
    }

    void CefBrowserSurface::GoForward()
    {
        if (m_State->Browser)
            m_State->Browser->GoForward();
    }

    void CefBrowserSurface::Reload()
    {
        if (m_State->Browser)
            m_State->Browser->Reload();
    }

    void CefBrowserSurface::Stop()
    {
        if (m_State->Browser)
            m_State->Browser->StopLoad();
    }

    void CefBrowserSurface::CancelDownload(Engine::u64 id)
    {
        State& state = *m_State;
        const auto found = state.Downloads.find(static_cast<uint32_t>(id));
        if (found != state.Downloads.end() && found->second.Callback)
            found->second.Callback->Cancel();
    }

    void CefBrowserSurface::ClearBrowsingData()
    {
        State& state = *m_State;
        if (!state.Started || state.ShutDown)
            return;
        state.ClearOnShutdown = true;
        state.RequestClose();
    }

    void CefBrowserSurface::RequestClose()
    {
        m_State->RequestClose();
    }

    bool CefBrowserSurface::IsClosed() const
    {
        return !m_State->Started || m_State->Closed;
    }

    void CefBrowserSurface::Shutdown(Engine::u32 timeoutMs)
    {
        m_State->Shutdown(timeoutMs);
    }
}
