#include "BrowserPanelCore.h"

#include "Engine/Events/ApplicationEvent.h"
#include "Engine/Events/KeyEvent.h"
#include "Engine/Events/MouseEvent.h"
#include "NullBrowserSurface.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <thread>
#include <utility>

namespace Fab
{
    namespace
    {
        using Engine::u32;
        using Engine::u64;

        void DeleteSurface(IBrowserSurface* surface)
        {
            delete surface;
        }

        // Stands in when the environment has no provider, so the uploader never
        // holds a null reference; the panel then fails its start with a clear text.
        class UnavailableTextures final : public IBrowserUiTextures
        {
        public:
            Engine::UiTextureHandle Create(u32, u32, std::string_view) override { return Engine::kInvalidUiTextureHandle; }
            bool Update(Engine::UiTextureHandle, const Engine::UiTextureUpdate&) override { return false; }
            bool Destroy(Engine::UiTextureHandle) override { return false; }
            u64 GetImGuiId(Engine::UiTextureHandle) override { return 0; }
            Engine::UiTextureError LastError() override { return Engine::UiTextureError::ServiceUnavailable; }
        };

        UnavailableTextures g_UnavailableTextures;

        std::string LowerAscii(std::string_view text)
        {
            std::string lower(text);
            for (char& character : lower)
            {
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            }
            return lower;
        }

        bool Contains(const std::string& lowerText, const char* needle)
        {
            return lowerText.find(needle) != std::string::npos;
        }

        std::string TrimTrailingPeriod(std::string_view text)
        {
            while (!text.empty() && (text.back() == '.' || text.back() == ' '))
                text.remove_suffix(1);
            return std::string(text);
        }

        std::string NameOrFile(const BrowserDownloadEvent& event)
        {
            return event.DisplayName.empty() ? std::string("file") : event.DisplayName;
        }

        u64 SteadyMilliseconds()
        {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        void SleepForMilliseconds(u32 milliseconds)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        }
    }

    // ---- BrowserTextureUploader ------------------------------------------------------

    BrowserTextureUploader::BrowserTextureUploader(IBrowserUiTextures& textures)
        : m_Textures(textures)
        , m_Mirror(Engine::kMaximumUiTextureDimension)
    {
    }

    BrowserTextureUploader::~BrowserTextureUploader()
    {
        Release();
    }

    bool BrowserTextureUploader::HasPendingWork() const
    {
        return !m_Pending.empty() || m_Next.Handle != Engine::kInvalidUiTextureHandle;
    }

    void BrowserTextureUploader::OnFrame(const BrowserFrameView& frame)
    {
        const BrowserFrameMirror::ApplyResult result = m_Mirror.ApplyFrame(frame);
        if (result == BrowserFrameMirror::ApplyResult::Rejected)
        {
            ++m_Counters.FramesRejected;
            return;
        }
        ++m_Counters.FramesApplied;

        m_Scratch.clear();
        m_Mirror.TakeDirty(m_Scratch);
        const Engine::u32 width = m_Mirror.Width();
        const Engine::u32 height = m_Mirror.Height();
        if (result == BrowserFrameMirror::ApplyResult::Resized)
        {
            // The storage was replaced: older regions refer to another size. The
            // whole frame is the new dirty region, and a different size earns a
            // fresh attempt after a latched failure.
            m_Pending.assign(1, BrowserDirtyRect { 0, 0, static_cast<int>(width), static_cast<int>(height) });
            m_Failed = false;
            m_FailureText.clear();
            return;
        }

        m_Pending.insert(m_Pending.end(), m_Scratch.begin(), m_Scratch.end());
        m_Pending = CoalesceDirtyRects(m_Pending, width, height, BrowserPanelLimits::kMaximumPendingRects);
    }

    void BrowserTextureUploader::Service()
    {
        const Engine::u32 width = m_Mirror.Width();
        const Engine::u32 height = m_Mirror.Height();
        if (m_Failed || width == 0 || height == 0)
            return;

        if (m_Next.Handle != Engine::kInvalidUiTextureHandle && (m_Next.Width != width || m_Next.Height != height))
            DestroySlot(m_Next);

        const bool activeFits = m_Active.Handle != Engine::kInvalidUiTextureHandle && m_Active.Width == width
            && m_Active.Height == height;
        if (activeFits && m_Next.Handle == Engine::kInvalidUiTextureHandle)
        {
            UploadPending();
            return;
        }

        if (m_Next.Handle == Engine::kInvalidUiTextureHandle && !CreateSlot(m_Next, width, height))
            return;

        switch (UploadRect(m_Next, { 0, 0, static_cast<int>(width), static_cast<int>(height) }))
        {
        case UploadResult::Done:
            DestroySlot(m_Active);
            m_Active = m_Next;
            m_Next = Slot {};
            m_Pending.clear();
            ++m_Counters.TextureSwaps;
            break;
        case UploadResult::Backpressure:
            break;
        case UploadResult::Failed:
            DestroySlot(m_Next);
            break;
        }
    }

    void BrowserTextureUploader::Release()
    {
        DestroySlot(m_Next);
        DestroySlot(m_Active);
        m_Pending.clear();
    }

    bool BrowserTextureUploader::CreateSlot(Slot& slot, Engine::u32 width, Engine::u32 height)
    {
        const Engine::UiTextureHandle handle = m_Textures.Create(width, height, "Fab browser");
        if (handle == Engine::kInvalidUiTextureHandle)
        {
            const Engine::UiTextureError error = m_Textures.LastError();
            if (error == Engine::UiTextureError::UpdateBackpressure)
            {
                ++m_Counters.BackpressureRetries;
                return false;
            }
            Fail(error == Engine::UiTextureError::None ? Engine::UiTextureError::DeviceCreateFailed : error);
            return false;
        }
        const Engine::u64 imGuiId = m_Textures.GetImGuiId(handle);
        if (imGuiId == 0)
        {
            m_Textures.Destroy(handle);
            Fail(Engine::UiTextureError::NativeRegistrationFailed);
            return false;
        }
        slot = Slot { handle, imGuiId, width, height };
        ++m_Counters.TexturesCreated;
        return true;
    }

    void BrowserTextureUploader::DestroySlot(Slot& slot)
    {
        if (slot.Handle != Engine::kInvalidUiTextureHandle)
            m_Textures.Destroy(slot.Handle);
        slot = Slot {};
    }

    BrowserTextureUploader::UploadResult BrowserTextureUploader::UploadRect(const Slot& slot, const BrowserDirtyRect& rect)
    {
        const std::span<const Engine::u8> pixels = m_Mirror.Pixels();
        if (slot.Width != m_Mirror.Width() || slot.Height != m_Mirror.Height() || pixels.empty())
            return UploadResult::Done;

        const int x0 = std::max(rect.X, 0);
        const int y0 = std::max(rect.Y, 0);
        const int x1 = std::min(rect.X + rect.Width, static_cast<int>(slot.Width));
        const int y1 = std::min(rect.Y + rect.Height, static_cast<int>(slot.Height));
        if (x1 <= x0 || y1 <= y0)
            return UploadResult::Done;

        const size_t offset = (static_cast<size_t>(y0) * slot.Width + static_cast<size_t>(x0)) * 4u;
        Engine::UiTextureUpdate update;
        update.Rect = { static_cast<Engine::u32>(x0), static_cast<Engine::u32>(y0), static_cast<Engine::u32>(x1 - x0),
            static_cast<Engine::u32>(y1 - y0) };
        update.Pixels = pixels.data() + offset;
        update.RowPitchBytes = static_cast<Engine::u64>(slot.Width) * 4u;
        update.PixelBytes = pixels.size() - offset;
        if (m_Textures.Update(slot.Handle, update))
        {
            ++m_Counters.UpdatesSubmitted;
            m_Counters.UpdatedTexels += static_cast<Engine::u64>(update.Rect.Width) * update.Rect.Height;
            return UploadResult::Done;
        }
        const Engine::UiTextureError error = m_Textures.LastError();
        if (error == Engine::UiTextureError::UpdateBackpressure)
        {
            ++m_Counters.BackpressureRetries;
            return UploadResult::Backpressure;
        }
        Fail(error);
        return UploadResult::Failed;
    }

    void BrowserTextureUploader::UploadPending()
    {
        size_t submitted = 0;
        while (!m_Pending.empty() && submitted < BrowserPanelLimits::kMaximumUpdatesPerService)
        {
            if (UploadRect(m_Active, m_Pending.front()) != UploadResult::Done)
                return;
            m_Pending.erase(m_Pending.begin());
            ++submitted;
        }
    }

    void BrowserTextureUploader::Fail(Engine::UiTextureError error)
    {
        m_Failed = true;
        ++m_Counters.Failures;
        m_FailureText = std::string("browser texture unavailable (") + Engine::ToString(error) + ")";
    }

    // ---- BrowserViewDebouncer --------------------------------------------------------

    BrowserViewDebouncer::BrowserViewDebouncer(Engine::u64 settleMilliseconds, Engine::u32 maximumPhysical)
        : m_Settle(settleMilliseconds)
        , m_MaximumPhysical(std::max<Engine::u32>(maximumPhysical, 1))
    {
    }

    bool BrowserViewDebouncer::Same(const BrowserViewSize& a, const BrowserViewSize& b)
    {
        return a.Width == b.Width && a.Height == b.Height && a.DeviceScale == b.DeviceScale;
    }

    bool BrowserViewDebouncer::Update(
        Engine::u64 nowMilliseconds, float widthPixels, float heightPixels, float deviceScale, BrowserViewSize& out)
    {
        float scale = std::isfinite(deviceScale) ? deviceScale : 1.0f;
        scale = std::clamp(scale, BrowserPanelLimits::kMinimumDeviceScale, BrowserPanelLimits::kMaximumDeviceScale);
        const float maximumDip = std::max(1.0f, std::floor(static_cast<float>(m_MaximumPhysical) / scale));
        const auto toDip = [&](float pixels)
        {
            const float dip = std::isfinite(pixels) ? std::floor(pixels / scale) : 1.0f;
            return static_cast<Engine::u32>(std::clamp(dip, 1.0f, maximumDip));
        };
        const BrowserViewSize desired { toDip(widthPixels), toDip(heightPixels), scale };

        if (!m_HasApplied)
        {
            m_HasApplied = true;
            m_HasPending = false;
            m_Applied = desired;
            out = desired;
            return true;
        }
        if (Same(desired, m_Applied))
        {
            m_HasPending = false;
            return false;
        }
        if (!m_HasPending || !Same(desired, m_Pending))
        {
            m_HasPending = true;
            m_Pending = desired;
            m_PendingSince = nowMilliseconds;
        }
        if (nowMilliseconds < m_PendingSince || nowMilliseconds - m_PendingSince < m_Settle)
            return false;
        m_Applied = desired;
        m_HasPending = false;
        out = desired;
        return true;
    }

    // ---- BrowserDownloadQueue --------------------------------------------------------

    BrowserDownloadQueue::BrowserDownloadQueue(size_t capacity)
        : m_Capacity(std::max<size_t>(capacity, 1))
    {
    }

    void BrowserDownloadQueue::Push(BrowserPanelDownload download)
    {
        if (m_Items.size() >= m_Capacity)
        {
            m_Items.pop_front();
            ++m_Dropped;
        }
        m_Items.push_back(std::move(download));
    }

    bool BrowserDownloadQueue::TryTake(BrowserPanelDownload& out)
    {
        if (m_Items.empty())
            return false;
        out = std::move(m_Items.front());
        m_Items.pop_front();
        return true;
    }

    // ---- startup failure text ----------------------------------------------------------

    std::string DescribeBrowserStartupFailure(std::string_view reason)
    {
        const std::string plain = TrimTrailingPeriod(reason);
        const std::string lower = LowerAscii(plain);
        if (Contains(lower, "in use"))
        {
            return "The Fab browser profile is in use by another Spiral instance. Close that instance, then restart the "
                   "Editor.";
        }
        if (Contains(lower, "sandbox"))
        {
            return "The Chromium sandbox is unavailable because this system denies unprivileged user namespaces. Allow "
                   "them, then restart the Editor. ("
                + plain + ")";
        }
        if (Contains(lower, "not installed"))
            return "The Fab browser runtime is not installed (" + plain + "). Install it, then restart the Editor.";
        return "The Fab browser could not start: " + plain + ". Restart the Editor to try again.";
    }

    // ---- BrowserPanelCore ---------------------------------------------------------------

    BrowserPanelCore::BrowserPanelCore(BrowserPanelEnvironment environment)
        : m_Env(std::move(environment))
        , m_Uploader(m_Env.Textures ? *m_Env.Textures : static_cast<IBrowserUiTextures&>(g_UnavailableTextures))
    {
        if (!m_Env.NowMs)
            m_Env.NowMs = SteadyMilliseconds;
        if (!m_Env.SleepMs)
            m_Env.SleepMs = SleepForMilliseconds;
    }

    BrowserPanelCore::~BrowserPanelCore()
    {
        Shutdown();
    }

    const char* BrowserPanelCore::StateName(State state)
    {
        switch (state)
        {
        case State::NotStarted: return "NotStarted";
        case State::Starting: return "Starting";
        case State::Running: return "Running";
        case State::Failed: return "Failed";
        case State::Closing: return "Closing";
        case State::Closed: return "Closed";
        }
        return "Unknown";
    }

    void BrowserPanelCore::Info(std::string_view message) const
    {
        if (m_Env.LogInfo)
            m_Env.LogInfo(message);
    }

    void BrowserPanelCore::Warn(std::string_view message) const
    {
        if (m_Env.LogWarn)
            m_Env.LogWarn(message);
    }

    template <typename Function>
    bool BrowserPanelCore::Guarded(const char* where, Function&& function)
    {
        std::string problem;
        try
        {
            function();
            return true;
        }
        catch (const std::exception& exception)
        {
            problem = exception.what();
        }
        catch (...)
        {
            problem = "unknown exception";
        }
        Fail(DescribeBrowserStartupFailure(std::string("internal error in ") + where + ": " + problem));
        return false;
    }

    void BrowserPanelCore::Configure(BrowserPanelConfig config)
    {
        if (m_State == State::NotStarted)
            m_Config = std::move(config);
    }

    void BrowserPanelCore::SetVisible(bool visible)
    {
        if (!visible)
        {
            m_Visible = false;
            m_ContentShown = false;
            if (Live())
            {
                ApplySurfaceVisibility();
                FeedRouterHidden();
            }
            return;
        }
        if (m_ShutDown)
            return;
        m_Visible = true;
        if (m_State == State::NotStarted)
            Start();
    }

    void BrowserPanelCore::Start()
    {
        m_State = State::Starting;
        if (!m_Env.Textures || !m_Env.LoadSurface)
        {
            Fail(DescribeBrowserStartupFailure("the panel has no texture provider or surface loader"));
            return;
        }

        BrowserSurfaceConfig surfaceConfig;
        surfaceConfig.ProfileDir = m_Config.ProfileDirectory;
        surfaceConfig.DownloadStagingDir = m_Config.DownloadStagingDirectory;
        surfaceConfig.MaxFps = BrowserPanelLimits::kMaximumBrowserFps;
        surfaceConfig.SoftwareRendering = true;
        std::string error;
        for (const std::string& host : m_Config.ProviderHosts)
        {
            if (!surfaceConfig.Navigation.AddProviderHost(host, error))
            {
                Fail(DescribeBrowserStartupFailure("a sign-in provider host is invalid: " + error));
                return;
            }
        }
        if (!ValidateBrowserSurfaceConfig(surfaceConfig, error))
        {
            Fail(DescribeBrowserStartupFailure("the Fab browser configuration is invalid: " + error));
            return;
        }
        if (!surfaceConfig.Navigation.IsTopLevelAllowed(m_Config.HomeUrl))
        {
            Fail(DescribeBrowserStartupFailure("the home page is not allowed by the navigation policy"));
            return;
        }

        Guarded("browser start", [&]
        {
            BrowserSurfaceLoadResult loaded = m_Env.LoadSurface(m_Config);
            if (!loaded.Surface)
            {
                Fail(DescribeBrowserStartupFailure(loaded.Error.empty() ? "the browser host could not be loaded" : loaded.Error));
                return;
            }
            std::string initializeError;
            if (!loaded.Surface->Initialize(surfaceConfig, *this, initializeError))
            {
                // The library destroys the half-initialised surface; it is never retried.
                loaded.Surface.reset();
                Fail(DescribeBrowserStartupFailure(
                    initializeError.empty() ? "the browser engine failed to initialize" : initializeError));
                return;
            }
            m_Surface = std::move(loaded.Surface);
            m_Initialized = true;
            // The engine starts visible; keep the hidden/visible bookkeeping exact so the
            // first drawn frame is a visible transition that also forces a full repaint.
            m_Surface->SetVisible(false);
            m_SurfaceVisible = false;
            m_Surface->SetFocus(false);
            Info(std::string("Fab browser initialized (backend ") + std::string(m_Surface->BackendName()) + ")");
        });
    }

    void BrowserPanelCore::Fail(const std::string& finalText)
    {
        // Never reset a surface from inside one of its own callbacks: the caller
        // that is inside IBrowserSurface::Pump() uses m_DeferredFailure instead.
        m_Error = finalText;
        m_State = State::Failed;
        m_Surface.reset();
        m_Uploader.Release();
        m_ContentShown = false;
        m_SurfaceVisible = false;
        m_Loading = false;
        m_CanGoBack = false;
        m_CanGoForward = false;
        m_Surface = MakeBrowserSurfacePtr(new NullBrowserSurface(finalText), &DeleteSurface);
        Warn(finalText);
    }

    void BrowserPanelCore::ApplySurfaceVisibility()
    {
        const bool wanted = Live() && m_Visible && m_ContentShown;
        if (wanted == m_SurfaceVisible)
            return;
        m_SurfaceVisible = wanted;
        if (Live())
            m_Surface->SetVisible(wanted);
    }

    void BrowserPanelCore::SetContentShown(bool shown)
    {
        m_ContentShown = shown;
        ApplySurfaceVisibility();
    }

    void BrowserPanelCore::FeedRouterHidden()
    {
        BrowserPanelState state = m_LastPanelState;
        state.PanelVisible = false;
        state.PanelFocused = false;
        state.SurfaceHovered = false;
        m_LastPanelState = state;
        ApplyRoute(m_Router.UpdatePanel(state));
    }

    void BrowserPanelCore::Pump()
    {
        const bool drawn = m_DrawnSincePump;
        m_DrawnSincePump = false;
        if (m_State == State::Closing)
        {
            Guarded("browser close", [&] { AdvanceClosing(); });
            return;
        }
        if (!Live())
            return;

        Guarded("browser pump", [&]
        {
            // A panel that was not drawn since the last pump is hidden for the
            // browser even if the window flag is still set (tab switched, docked away).
            if (m_ContentShown && !drawn)
            {
                m_ContentShown = false;
                ApplySurfaceVisibility();
                FeedRouterHidden();
            }
            m_Surface->Pump();
            if (!m_DeferredFailure.empty())
            {
                const std::string text = std::move(m_DeferredFailure);
                m_DeferredFailure.clear();
                Fail(text);
                return;
            }
            if (m_SurfaceVisible)
            {
                m_Uploader.Service();
                if (m_Uploader.Counters().Failures > m_LoggedTextureFailures)
                {
                    m_LoggedTextureFailures = m_Uploader.Counters().Failures;
                    Warn(m_Uploader.FailureText());
                }
            }
        });
    }

    void BrowserPanelCore::SetDeviceScale(float scale)
    {
        if (std::isfinite(scale))
        {
            m_DeviceScale = std::clamp(
                scale, BrowserPanelLimits::kMinimumDeviceScale, BrowserPanelLimits::kMaximumDeviceScale);
        }
    }

    void BrowserPanelCore::UpdateLayout(const BrowserPanelLayout& layout)
    {
        m_DrawnSincePump = true;
        m_Modifiers = layout.Modifiers & BrowserModifier::KeyMask;
        if (!Live() || !m_Visible)
            return;
        Guarded("panel layout", [&]
        {
            if (layout.Surface.Width < BrowserPanelLimits::kMinimumSurfaceExtent
                || layout.Surface.Height < BrowserPanelLimits::kMinimumSurfaceExtent)
            {
                SetContentShown(false);
                FeedRouterHidden();
                return;
            }

            m_SurfaceRect = layout.Surface;
            BrowserViewSize view;
            if (m_View.Update(m_Env.NowMs(), layout.Surface.Width, layout.Surface.Height, m_DeviceScale, view))
            {
                m_AppliedScale = view.DeviceScale;
                m_Surface->SetViewSize(view);
            }
            SetContentShown(true);
            if (!m_HomeRequested)
            {
                m_HomeRequested = true;
                m_Surface->Navigate(m_Config.HomeUrl);
            }

            BrowserPanelState state;
            state.Surface = layout.Surface;
            state.PanelVisible = true;
            state.PanelFocused = layout.PanelFocused;
            state.SurfaceHovered = layout.SurfaceHovered;
            state.OtherTextInputActive = layout.OtherTextInputActive;
            state.DragDropPayloadActive = layout.DragDropPayloadActive;
            state.ModalOrPopupOpen = layout.ModalOrPopupOpen;
            m_LastPanelState = state;
            ApplyRoute(m_Router.UpdatePanel(state));
        });
    }

    void BrowserPanelCore::NotifyContentHidden()
    {
        m_DrawnSincePump = true;
        if (!Live())
            return;
        SetContentShown(false);
        FeedRouterHidden();
    }

    BrowserMouse BrowserPanelCore::ToDip(const BrowserMouse& mouse) const
    {
        const float scale = std::max(m_AppliedScale, BrowserPanelLimits::kMinimumDeviceScale);
        return { mouse.X / scale, mouse.Y / scale, mouse.Modifiers };
    }

    void BrowserPanelCore::ApplyRoute(const BrowserInputRoute& route)
    {
        if (!Live())
            return;
        // Releases first, then the focus change, so the page never sees a key
        // stay down after it lost the keyboard.
        for (const int glfwKey : route.SyntheticKeyUps)
        {
            BrowserKey key;
            key.Kind = BrowserKey::Phase::Up;
            key.GlfwKey = glfwKey;
            key.WindowsVirtualKey = TranslateGlfwKeyToWindowsVirtualKey(glfwKey);
            m_Surface->SendKey(key);
        }
        if (route.SendCaptureLost)
            m_Surface->SendMouseCaptureLost();
        if (route.SendMouseLeave)
        {
            const BrowserMouse last { m_CursorX - m_SurfaceRect.X, m_CursorY - m_SurfaceRect.Y, m_Modifiers };
            m_Surface->SendMouseMove(ToDip(last), true);
        }
        if (route.KeyboardOwnerChanged)
            m_Surface->SetFocus(m_Router.OwnsKeyboard());
    }

    void BrowserPanelCore::ReleaseKeyboard()
    {
        ApplyRoute(m_Router.ReleaseKeyboard());
    }

    bool BrowserPanelCore::OnEvent(Engine::Event& event)
    {
        if (!Live())
            return false;
        bool consumed = false;
        Guarded("input", [&]
        {
            switch (event.GetEventType())
            {
            case Engine::EventType::WindowFocus:
            {
                ApplyRoute(m_Router.OnWindowFocus(static_cast<Engine::WindowFocusEvent&>(event).IsFocused()));
                break;
            }
            case Engine::EventType::WindowContentScale:
            {
                SetDeviceScale(static_cast<Engine::WindowContentScaleEvent&>(event).GetXScale());
                break;
            }
            case Engine::EventType::CursorEnter:
            {
                if (!static_cast<Engine::CursorEnterEvent&>(event).Entered())
                    ApplyRoute(m_Router.OnCursorLeftWindow());
                break;
            }
            case Engine::EventType::MouseMoved:
            {
                const auto& moved = static_cast<Engine::MouseMovedEvent&>(event);
                m_CursorX = moved.GetX();
                m_CursorY = moved.GetY();
                const BrowserInputRoute route = m_Router.OnMouseMove(m_CursorX, m_CursorY, m_Modifiers);
                ApplyRoute(route);
                if (route.Browser)
                {
                    m_Surface->SendMouseMove(ToDip(route.Mouse), false);
                    consumed = true;
                }
                break;
            }
            case Engine::EventType::MouseButtonPressed:
            case Engine::EventType::MouseButtonReleased:
            {
                const auto& button = static_cast<Engine::MouseButtonEvent&>(event);
                BrowserMouseButton translated;
                if (!TranslateGlfwMouseButton(button.GetMouseButton(), translated))
                    break;
                const bool down = event.GetEventType() == Engine::EventType::MouseButtonPressed;
                m_Modifiers = button.GetModifiers() & BrowserModifier::KeyMask;
                const BrowserInputRoute route = m_Router.OnMouseButton(
                    translated, down, m_CursorX, m_CursorY, m_Modifiers, m_Env.NowMs());
                ApplyRoute(route);
                if (route.Browser)
                {
                    if (down)
                        m_ClickCount = std::max(route.ClickCount, 1);
                    m_Surface->SendMouseButton(ToDip(route.Mouse), translated, down, m_ClickCount);
                    consumed = true;
                }
                break;
            }
            case Engine::EventType::MouseScrolled:
            {
                const auto& scrolled = static_cast<Engine::MouseScrolledEvent&>(event);
                const BrowserInputRoute route = m_Router.OnScroll(
                    scrolled.GetXOffset(), scrolled.GetYOffset(), m_CursorX, m_CursorY, m_Modifiers);
                ApplyRoute(route);
                if (route.Browser)
                {
                    // Pixel deltas; the GLFW sign (positive = content moves down/right) is
                    // forwarded unchanged and is not divided by the device scale.
                    m_Surface->SendMouseWheel(ToDip(route.Mouse), route.WheelDeltaX, route.WheelDeltaY);
                    consumed = true;
                }
                break;
            }
            case Engine::EventType::KeyPressed:
            {
                const auto& key = static_cast<Engine::KeyPressedEvent&>(event);
                m_Modifiers = key.GetModifiers() & BrowserModifier::KeyMask;
                const BrowserInputRoute route = m_Router.OnKey(
                    key.GetKeyCode(), key.GetScancode(), key.IsRepeat() ? 2 : 1, m_Modifiers);
                ApplyRoute(route);
                if (route.Browser)
                {
                    m_Surface->SendKey(route.Key);
                    consumed = true;
                }
                break;
            }
            case Engine::EventType::KeyReleased:
            {
                const auto& key = static_cast<Engine::KeyReleasedEvent&>(event);
                m_Modifiers = key.GetModifiers() & BrowserModifier::KeyMask;
                const BrowserInputRoute route = m_Router.OnKey(key.GetKeyCode(), key.GetScancode(), 0, m_Modifiers);
                ApplyRoute(route);
                if (route.Browser)
                {
                    m_Surface->SendKey(route.Key);
                    consumed = true;
                }
                break;
            }
            case Engine::EventType::CharTyped:
            {
                const auto& typed = static_cast<Engine::CharTypedEvent&>(event);
                const BrowserInputRoute route = m_Router.OnChar(static_cast<char32_t>(typed.GetCodePoint()), m_Modifiers);
                ApplyRoute(route);
                if (route.Browser)
                {
                    m_Surface->SendKey(route.Key);
                    consumed = true;
                }
                break;
            }
            default:
                break;
            }
        });
        if (consumed)
            event.Handled = true;
        return consumed;
    }

    void BrowserPanelCore::GoBack()
    {
        if (CanGoBack())
            m_Surface->GoBack();
    }

    void BrowserPanelCore::GoForward()
    {
        if (CanGoForward())
            m_Surface->GoForward();
    }

    void BrowserPanelCore::Reload()
    {
        if (Live())
            m_Surface->Reload();
    }

    void BrowserPanelCore::GoHome()
    {
        if (Live())
            m_Surface->Navigate(m_Config.HomeUrl);
    }

    void BrowserPanelCore::ClearBrowsingData()
    {
        if (!Live())
            return;
        Guarded("sign out", [&]
        {
            Info("Fab sign-out requested; the browser profile will be deleted after the browser closes");
            m_SignedOut = true;
            SetContentShown(false);
            FeedRouterHidden();
            m_Surface->ClearBrowsingData();
            BeginClosing();
        });
    }

    void BrowserPanelCore::BeginClosing()
    {
        m_State = State::Closing;
        m_CloseRequested = true;
        m_CloseDeadline = m_Env.NowMs() + BrowserPanelLimits::kCloseDeadlineMilliseconds;
        m_Surface->RequestClose();
    }

    void BrowserPanelCore::AdvanceClosing()
    {
        m_Surface->Pump();
        if (m_Surface->IsClosed() || m_Env.NowMs() >= m_CloseDeadline)
            FinishClosing();
    }

    void BrowserPanelCore::FinishClosing()
    {
        if (!m_Surface->IsClosed())
            Warn("The Fab browser did not close in time; forcing shutdown");
        m_Surface->Shutdown(BrowserPanelLimits::kSurfaceShutdownMilliseconds);
        m_Surface.reset();
        m_Uploader.Release();
        m_ContentShown = false;
        m_SurfaceVisible = false;
        m_Loading = false;
        m_CanGoBack = false;
        m_CanGoForward = false;
        m_State = State::Closed;
    }

    void BrowserPanelCore::Shutdown()
    {
        if (m_ShutDown)
            return;
        m_ShutDown = true;
        m_Visible = false;
        Guarded("shutdown", [&]
        {
            if (Live())
            {
                FeedRouterHidden();
                BeginClosing();
            }
            while (m_State == State::Closing)
            {
                AdvanceClosing();
                if (m_State == State::Closing)
                    m_Env.SleepMs(2);
            }
        });
        m_Uploader.Release();
        m_Surface.reset();
        if (m_State == State::NotStarted)
            m_State = State::Closed;
    }

    // ---- Listener -------------------------------------------------------------------------

    void BrowserPanelCore::OnFrame(const BrowserFrameView& frame)
    {
        if (!Live())
            return;
        m_Uploader.OnFrame(frame);
        if (m_State == State::Starting && m_Uploader.Counters().FramesApplied > 0)
        {
            m_State = State::Running;
            Info("Fab browser received its first frame");
        }
    }

    void BrowserPanelCore::OnCursor(BrowserCursor cursor)
    {
        m_Cursor = cursor;
    }

    void BrowserPanelCore::OnAddress(std::string_view displayAddress)
    {
        const size_t slash = displayAddress.find('/');
        m_DisplayHost = std::string(displayAddress.substr(0, std::min<size_t>(slash, 253)));
    }

    void BrowserPanelCore::OnLoadState(bool loading, bool canGoBack, bool canGoForward)
    {
        m_Loading = loading;
        m_CanGoBack = canGoBack;
        m_CanGoForward = canGoForward;
        if (loading)
            m_PageError.clear();
    }

    void BrowserPanelCore::OnDownload(const BrowserDownloadEvent& event)
    {
        using DownloadState = BrowserDownloadEvent::State;
        const std::string name = NameOrFile(event);
        switch (event.Kind)
        {
        case DownloadState::Started:
            m_Notice = "Downloading " + name + "...";
            break;
        case DownloadState::Progress:
            if (event.TotalBytes > 0)
            {
                m_Notice = "Downloading " + name + " (" + std::to_string(std::min<u64>(event.Bytes * 100 / event.TotalBytes, 100))
                    + "%)";
            }
            else
            {
                m_Notice = "Downloading " + name + " (" + std::to_string(event.Bytes / (1024 * 1024)) + " MB)";
            }
            break;
        case DownloadState::Completed:
            if (event.StagedPath.empty())
            {
                m_Notice = "Download finished without a staged file: " + name;
                break;
            }
            m_Downloads.Push({ event.StagedPath, name });
            ++m_DownloadsCompleted;
            m_Notice = "Downloaded " + name + "; ready to import.";
            Info("Fab download completed: " + name);
            break;
        case DownloadState::Failed:
            m_Notice = "Download failed: " + name;
            break;
        case DownloadState::Cancelled:
            m_Notice = "Download cancelled: " + name;
            break;
        case DownloadState::Blocked:
            m_Notice = "Download blocked (file type or size is not allowed): " + name;
            break;
        }
    }

    void BrowserPanelCore::OnNavigationDenied(std::string_view host)
    {
        ++m_NavigationDenials;
        m_Notice = "Blocked navigation to " + std::string(host) + ".";
    }

    void BrowserPanelCore::OnFailed(std::string_view reason)
    {
        m_PageError = std::string(reason);
        Warn("Fab browser reported a problem: " + m_PageError);
    }

    void BrowserPanelCore::OnClosed()
    {
        if (Live() && !m_CloseRequested)
            m_DeferredFailure = DescribeBrowserStartupFailure("the browser closed unexpectedly");
    }

    // ---- observation --------------------------------------------------------------------

    const std::string& BrowserPanelCore::StatusLine() const
    {
        switch (m_State)
        {
        case State::Failed:
            m_Status = m_Error;
            break;
        case State::Closed:
            m_Status = m_SignedOut ? "Signed out. Restart the Editor to sign in again."
                                   : "The Fab browser is closed. Restart the Editor to open it again.";
            break;
        case State::Closing:
            m_Status = m_SignedOut ? "Signing out..." : "Closing the Fab browser...";
            break;
        case State::NotStarted:
            m_Status = m_Notice.empty() ? std::string("The Fab browser starts when this panel is opened.") : m_Notice;
            break;
        case State::Starting:
            m_Status = "Starting the Fab browser...";
            break;
        case State::Running:
            if (m_Uploader.Failed())
                m_Status = m_Uploader.FailureText();
            else if (!m_PageError.empty())
                m_Status = "Page problem: " + m_PageError;
            else if (!m_Notice.empty())
                m_Status = m_Notice;
            else if (m_Loading)
                m_Status = "Loading...";
            else
                m_Status = "Ready";
            break;
        }
        return m_Status;
    }

    BrowserPanelDiagnostics BrowserPanelCore::GetDiagnostics() const
    {
        BrowserPanelDiagnostics diagnostics;
        diagnostics.State = StateName(m_State);
        diagnostics.Initialized = m_Initialized;
        diagnostics.Failed = m_State == State::Failed;
        diagnostics.Visible = m_Visible;
        diagnostics.KeyboardOwnedByPage = WantsKeyboard();
        diagnostics.TextureValid = m_Uploader.HasDisplayTexture();
        diagnostics.Loading = IsLoading();
        diagnostics.FramesReceived = m_Uploader.Counters().FramesApplied;
        diagnostics.FrameWidth = m_Uploader.FrameWidth();
        diagnostics.FrameHeight = m_Uploader.FrameHeight();
        diagnostics.NavigationDenials = m_NavigationDenials;
        diagnostics.DownloadsCompleted = m_DownloadsCompleted;
        diagnostics.DisplayHost = m_DisplayHost;
        if (m_State == State::Failed)
            diagnostics.Error = m_Error;
        else if (m_Uploader.Failed())
            diagnostics.Error = m_Uploader.FailureText();
        else
            diagnostics.Error = m_PageError;
        return diagnostics;
    }
}
