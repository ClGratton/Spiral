#pragma once

#include "BrowserSurface.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace SpiralTests
{
    // Scriptable IBrowserSurface for tests. Scripted page behaviour is queued
    // and every listener callback is made from inside Pump(), the contract of
    // the real interface; OutOfPumpCallbacks() counts any violation. Page
    // navigation and downloads run through the real BrowserNavigationPolicy and
    // BrowserDownloadPolicy from the configuration passed to Initialize.
    class FakeBrowserSurface final : public Fab::IBrowserSurface
    {
    public:
        struct MouseMoveCall
        {
            Fab::BrowserMouse Mouse;
            bool Leave = false;
        };

        struct MouseButtonCall
        {
            Fab::BrowserMouse Mouse;
            Fab::BrowserMouseButton Button = Fab::BrowserMouseButton::Left;
            bool Down = false;
            int ClickCount = 0;
        };

        struct WheelCall
        {
            Fab::BrowserMouse Mouse;
            float DeltaX = 0.0f;
            float DeltaY = 0.0f;
        };

        enum class DownloadEnd
        {
            Complete,
            Fail
        };

        bool FailInitialize = false;

        std::string_view BackendName() const override { return "fake"; }

        bool Initialize(const Fab::BrowserSurfaceConfig& config, Listener& listener, std::string& error) override
        {
            if (FailInitialize)
            {
                error = "fake initialisation failure";
                return false;
            }
            m_Config = config;
            m_Listener = &listener;
            m_Initialized = true;
            error.clear();
            return true;
        }

        void Pump() override
        {
            ++m_PumpCount;
            m_InPump = true;
            const size_t scripted = m_Script.size();
            for (size_t index = 0; index < scripted; ++index)
                m_Script[index]();
            m_Script.erase(m_Script.begin(), m_Script.begin() + static_cast<std::ptrdiff_t>(scripted));
            if (m_CloseRequested && !m_Closed)
            {
                m_Closed = true;
                Emit([&] { m_Listener->OnClosed(); });
            }
            m_InPump = false;
        }

        // ---- scripting (queued until the next Pump) -------------------------------

        void ScriptFrame(Engine::u32 width, Engine::u32 height, std::vector<Engine::u8> bgra,
            std::vector<Fab::BrowserDirtyRect> dirty)
        {
            m_Script.push_back([this, width, height, bgra = std::move(bgra), dirty = std::move(dirty)]
            {
                Fab::BrowserFrameView frame;
                frame.Bgra = bgra.data();
                frame.Width = width;
                frame.Height = height;
                frame.StrideBytes = width * 4;
                frame.Dirty = dirty;
                frame.Sequence = ++m_FrameSequence;
                Emit([&] { m_Listener->OnFrame(frame); });
            });
        }

        void ScriptNavigationRequest(std::string url, Fab::BrowserNavigationKind kind)
        {
            m_Script.push_back([this, url = std::move(url), kind] { ProcessNavigation(url, kind); });
        }

        void ScriptDownload(std::string suggestedName, Engine::u64 totalBytes, std::vector<Engine::u64> progress,
            DownloadEnd end)
        {
            m_Script.push_back([this, name = std::move(suggestedName), totalBytes, progress = std::move(progress), end]
            {
                ProcessDownload(name, totalBytes, progress, end);
            });
        }

        void ScriptCursor(Fab::BrowserCursor cursor)
        {
            m_Script.push_back([this, cursor] { Emit([&] { m_Listener->OnCursor(cursor); }); });
        }

        void ScriptFailure(std::string reason)
        {
            m_Script.push_back([this, reason = std::move(reason)] { Emit([&] { m_Listener->OnFailed(reason); }); });
        }

        // ---- IBrowserSurface inputs (recorded) ----------------------------------

        void SetViewSize(const Fab::BrowserViewSize& size) override { ViewSizes.push_back(size); }
        void SetVisible(bool visible) override { VisibleStates.push_back(visible); }
        void SetFocus(bool focused) override { FocusStates.push_back(focused); }
        void SendMouseMove(const Fab::BrowserMouse& mouse, bool leave) override { MouseMoves.push_back({ mouse, leave }); }

        void SendMouseButton(const Fab::BrowserMouse& mouse, Fab::BrowserMouseButton button, bool down, int clickCount) override
        {
            MouseButtons.push_back({ mouse, button, down, clickCount });
        }

        void SendMouseCaptureLost() override { ++CaptureLostCount; }
        void SendMouseWheel(const Fab::BrowserMouse& mouse, float deltaX, float deltaY) override { Wheels.push_back({ mouse, deltaX, deltaY }); }
        void SendKey(const Fab::BrowserKey& key) override { Keys.push_back(key); }

        void Navigate(std::string_view httpsUrl) override
        {
            ScriptNavigationRequest(std::string(httpsUrl), Fab::BrowserNavigationKind::TopLevel);
        }

        void GoBack() override { ++GoBackCount; }
        void GoForward() override { ++GoForwardCount; }
        void Reload() override { ++ReloadCount; }
        void Stop() override { ++StopCount; }
        void CancelDownload(Engine::u64 id) override { CancelledDownloads.push_back(id); }
        void ClearBrowsingData() override { ++ClearBrowsingDataCount; }

        void RequestClose() override { m_CloseRequested = true; }
        bool IsClosed() const override { return m_Closed; }

        void Shutdown(Engine::u32 timeoutMs) override
        {
            ShutdownTimeouts.push_back(timeoutMs);
            if (!m_Closed)
                ShutdownBeforeClose = true;
        }

        // ---- observation -------------------------------------------------------

        bool IsInitialized() const { return m_Initialized; }
        size_t OutOfPumpCallbacks() const { return m_OutOfPumpCallbacks; }
        size_t PumpCount() const { return m_PumpCount; }
        const Fab::BrowserSurfaceConfig& Config() const { return m_Config; }

        std::vector<Fab::BrowserViewSize> ViewSizes;
        std::vector<bool> VisibleStates;
        std::vector<bool> FocusStates;
        std::vector<MouseMoveCall> MouseMoves;
        std::vector<MouseButtonCall> MouseButtons;
        std::vector<WheelCall> Wheels;
        std::vector<Fab::BrowserKey> Keys;
        std::vector<Engine::u64> CancelledDownloads;
        std::vector<Engine::u32> ShutdownTimeouts;
        size_t CaptureLostCount = 0;
        size_t GoBackCount = 0;
        size_t GoForwardCount = 0;
        size_t ReloadCount = 0;
        size_t StopCount = 0;
        size_t ClearBrowsingDataCount = 0;
        bool ShutdownBeforeClose = false;

    private:
        template <typename Callback>
        void Emit(Callback&& callback)
        {
            if (!m_InPump)
                ++m_OutOfPumpCallbacks;
            callback();
        }

        void ProcessNavigation(const std::string& url, Fab::BrowserNavigationKind kind)
        {
            const Fab::BrowserNavigationVerdict verdict = m_Config.Navigation.Evaluate(url, kind);
            if (!Fab::IsNavigationAllowed(verdict))
            {
                Emit([&] { m_Listener->OnNavigationDenied(Fab::BrowserNavigationPolicy::HostForLog(url)); });
                return;
            }
            if (kind != Fab::BrowserNavigationKind::TopLevel)
                return;
            Emit([&] { m_Listener->OnAddress(Fab::BrowserNavigationPolicy::DisplayAddress(url)); });
            Emit([&] { m_Listener->OnLoadState(true, m_HasHistory, false); });
            m_HasHistory = true;
            Emit([&] { m_Listener->OnLoadState(false, m_HasHistory, false); });
        }

        void ProcessDownload(const std::string& suggestedName, Engine::u64 totalBytes,
            const std::vector<Engine::u64>& progress, DownloadEnd end)
        {
            const Engine::u64 id = ++m_NextDownloadId;
            Fab::BrowserDownloadEvent event;
            event.Id = id;
            event.TotalBytes = totalBytes;
            const Fab::BrowserDownloadDecision decision =
                m_Config.Downloads.Evaluate({ suggestedName, totalBytes }, 0x5eed0000ull + id);
            event.DisplayName = decision.SanitizedName;
            if (decision.Verdict != Fab::BrowserDownloadVerdict::Accept)
            {
                event.Kind = Fab::BrowserDownloadEvent::State::Blocked;
                Emit([&] { m_Listener->OnDownload(event); });
                return;
            }
            std::string error;
            event.StagedPath = Fab::BrowserDownloadPolicy::ResolveStagedPath(
                m_Config.DownloadStagingDir, decision, error);
            if (event.StagedPath.empty())
            {
                event.Kind = Fab::BrowserDownloadEvent::State::Failed;
                Emit([&] { m_Listener->OnDownload(event); });
                return;
            }
            event.Kind = Fab::BrowserDownloadEvent::State::Started;
            Emit([&] { m_Listener->OnDownload(event); });
            for (const Engine::u64 bytes : progress)
            {
                event.Bytes = bytes;
                if (!m_Config.Downloads.WithinSizeLimit(bytes)
                    || std::find(CancelledDownloads.begin(), CancelledDownloads.end(), id) != CancelledDownloads.end())
                {
                    event.Kind = Fab::BrowserDownloadEvent::State::Cancelled;
                    Emit([&] { m_Listener->OnDownload(event); });
                    return;
                }
                event.Kind = Fab::BrowserDownloadEvent::State::Progress;
                Emit([&] { m_Listener->OnDownload(event); });
            }
            event.Kind = end == DownloadEnd::Complete ? Fab::BrowserDownloadEvent::State::Completed
                                                       : Fab::BrowserDownloadEvent::State::Failed;
            Emit([&] { m_Listener->OnDownload(event); });
        }

        Fab::BrowserSurfaceConfig m_Config;
        Listener* m_Listener = nullptr;
        std::vector<std::function<void()>> m_Script;
        size_t m_OutOfPumpCallbacks = 0;
        size_t m_PumpCount = 0;
        Engine::u64 m_FrameSequence = 0;
        Engine::u64 m_NextDownloadId = 0;
        bool m_Initialized = false;
        bool m_InPump = false;
        bool m_HasHistory = false;
        bool m_CloseRequested = false;
        bool m_Closed = false;
    };
}
