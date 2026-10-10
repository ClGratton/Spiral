#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/UiTexture.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

struct ImGuiViewport;

namespace Engine
{
    // --ui-viewport-smoke (needs --ui-viewports): creates one detached OS window
    // programmatically through the Dear ImGui API (no input of any kind), draws a
    // UI-service texture into the main window and into that window, measures the
    // main-loop frame time with and without it, destroys it again and prints the
    // UiViewportV1 marker. Driven by ImGuiLayer::Begin/End.
    //
    // Failure hypotheses it targets: a detached swapchain that blocks the main loop
    // (watchdog, frame-time ratio), a texture drawn in a second window that fails to
    // sample (offscreen pixel oracle of that window's draw data), UI-texture
    // counters that do not balance after destroying a texture that the detached
    // window drew in the same frame, and swapchain/viewport teardown that leaves a
    // viewport behind.
    //
    // Non-claims: the detached swapchain image itself is not read back, validation
    // layers are not used, and a headless or Wayland host prints a skip marker.
    class UiViewportSmoke
    {
    public:
        // Application raises MaxFrames to at least this when the smoke is on.
        static constexpr u32 kFrameBudget = 2000;

        UiViewportSmoke();
        ~UiViewportSmoke();

        UiViewportSmoke(const UiViewportSmoke&) = delete;
        UiViewportSmoke& operator=(const UiViewportSmoke&) = delete;

        // After ImGui::NewFrame: submits the smoke windows and advances the state.
        void OnFrameBegin();
        // After the main window and every detached window were rendered.
        void OnFrameEnd();

        bool IsPending() const { return m_Phase != Phase::Done; }
        bool IsFinished() const { return m_Phase == Phase::Done; }
        bool Passed() const { return m_Passed; }

    private:
        enum class Phase : u8
        {
            Preflight,
            Baseline,
            WaitForSecondary,
            Measure,
            WaitForDestroy,
            Teardown,
            Done
        };

        struct FrameStatistics
        {
            size_t Samples = 0;
            double AverageMilliseconds = 0.0;
            double P95Milliseconds = 0.0;
            double MaximumMilliseconds = 0.0;
        };

        static FrameStatistics Summarize(std::vector<double> samples);

        void CloseApplicationUnlessVulkanSmokeOwnsIt();
        void Skip(const std::string& reason);
        void Fail(const std::string& stage);
        void Finish(bool passed);
        void Heartbeat();
        void StartWatchdog();
        void StopWatchdog();
        void EnterPhase(Phase phase);
        const char* PhaseName() const;
        bool CreateTextures();
        void SubmitMainWindow();
        void SubmitSecondaryWindow();
        void CheckSecondaryPixels();
        void EvaluateTeardown();

        Phase m_Phase = Phase::Preflight;
        bool m_Passed = false;
        bool m_Failed = false;
        u32 m_FramesInPhase = 0;
        u64 m_TotalFrames = 0;

        std::chrono::steady_clock::time_point m_LastFrameStart;
        bool m_HasLastFrameStart = false;
        std::vector<double> m_BaselineSamples;
        std::vector<double> m_SecondarySamples;

        // Textures drawn by the smoke. A: both windows; B: the detached window
        // only, destroyed in a frame in which that window still draws it.
        UiTextureHandle m_TextureA = kInvalidUiTextureHandle;
        UiTextureHandle m_TextureB = kInvalidUiTextureHandle;
        bool m_TextureBDestroyed = false;
        u64 m_TextureBDestroyFrame = 0;
        u64 m_TextureBReleaseFrame = 0;
        u64 m_HeldBySecondaryAtDestroy = 0;
        UiTextureCounters m_CountersAtStart;

        // Detached window under test.
        u32 m_SecondaryViewportId = 0;
        bool m_SecondaryDrawnThisFrame = false;
        u64 m_SecondaryFramesAtMeasureStart = 0;
        u64 m_ImagesDrawnMain = 0;
        u64 m_ImagesDrawnSecondary = 0;
        bool m_PixelsChecked = false;
        std::string m_PixelStatus = "not-executed(not-reached)";
        u64 m_PixelsCompared = 0;
        u64 m_PixelMismatches = 0;
        u32 m_PixelMaxDelta = 0;
        int m_SecondaryPresentMode = -1;
        double m_CreateMilliseconds = 0.0;
        double m_DestroyMilliseconds = 0.0;
        u64 m_BackendErrorsAtStart = 0;
        u64 m_SecondaryCreatesAtStart = 0;
        u64 m_SecondaryDestroysAtStart = 0;
        std::string m_Platform;
        std::string m_RendererName;

        // Frame-stall watchdog: a detached swapchain that blocks the process in
        // an unbounded acquire cannot be noticed from the frame loop it blocks.
        std::thread m_Watchdog;
        std::atomic<bool> m_WatchdogStop { false };
        std::atomic<long long> m_HeartbeatNanoseconds { 0 };
        std::atomic<int> m_WatchdogPhase { 0 };
    };
}
