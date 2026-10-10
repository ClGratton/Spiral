#include "Engine/UI/UiViewportSmoke.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/UiViewportPolicy.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace Engine
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // Frame budgets (counted in application frames) and the stall limit. None of
        // them is a performance claim; they only bound how long a broken run can last.
        constexpr u32 kWarmupFrames = 10;
        constexpr u32 kBaselineMeasuredFrames = 60;
        constexpr u32 kSecondaryCreateBudgetFrames = 240;
        constexpr u64 kSecondaryRenderedFrames = 70;
        constexpr u32 kDestroyBudgetFrames = 240;
        constexpr u32 kTeardownBudgetFrames = 400;
        constexpr long long kStallLimitSeconds = 20;
        constexpr u64 kPixelCaptureAfterFrames = 25;
        constexpr u64 kTextureBDestroyAfterFrames = 40;

        constexpr u32 kTextureAWidth = 96;
        constexpr u32 kTextureAHeight = 64;
        constexpr u32 kTextureBWidth = 48;
        constexpr u32 kTextureBHeight = 32;

        const char* const kMainWindowName = "##UiViewportSmokeMain";
        const char* const kSecondaryWindowName = "##UiViewportSmokeDetached";

        // One analytic opaque texel per (texture, x, y), independent of the buffers
        // handed to the UI-texture service, so the expected image is its own oracle.
        std::array<u8, 4> Texel(u32 texture, u32 x, u32 y)
        {
            return { static_cast<u8>(37u * texture + 5u * x + 3u * y + 1u), static_cast<u8>(61u * texture + 3u * x + 7u * y + 2u),
                static_cast<u8>(11u * texture + 9u * x + 13u * y + 3u), u8 { 255 } };
        }

        bool UploadTexture(UiTextureHandle handle, u32 textureIndex, u32 width, u32 height)
        {
            std::vector<u8> bytes(static_cast<size_t>(width) * height * 4u);
            for (u32 y = 0; y < height; ++y)
                for (u32 x = 0; x < width; ++x)
                {
                    const std::array<u8, 4> texel = Texel(textureIndex, x, y);
                    std::copy(texel.begin(), texel.end(), bytes.begin() + (static_cast<size_t>(y) * width + x) * 4u);
                }
            UiTextureUpdate update;
            update.Rect = { 0, 0, width, height };
            update.Pixels = bytes.data();
            update.PixelBytes = bytes.size();
            return Renderer::UpdateUiTexture(handle, update);
        }

        const ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

        void DrawImage(UiTextureHandle handle, u32 width, u32 height)
        {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(Renderer::GetUiTextureImGuiId(handle))),
                ImVec2(static_cast<float>(width), static_cast<float>(height)));
        }
    }

    UiViewportSmoke::UiViewportSmoke() = default;

    UiViewportSmoke::~UiViewportSmoke()
    {
        StopWatchdog();
    }

    UiViewportSmoke::FrameStatistics UiViewportSmoke::Summarize(std::vector<double> samples)
    {
        FrameStatistics statistics;
        statistics.Samples = samples.size();
        if (samples.empty())
            return statistics;
        double sum = 0.0;
        for (double sample : samples)
            sum += sample;
        std::sort(samples.begin(), samples.end());
        statistics.AverageMilliseconds = sum / static_cast<double>(samples.size());
        // Nearest-rank 95th percentile.
        statistics.P95Milliseconds = samples[static_cast<size_t>(std::ceil(0.95 * static_cast<double>(samples.size()))) - 1];
        statistics.MaximumMilliseconds = samples.back();
        return statistics;
    }

    const char* UiViewportSmoke::PhaseName() const
    {
        switch (m_Phase)
        {
            case Phase::Preflight: return "preflight";
            case Phase::Baseline: return "baseline";
            case Phase::WaitForSecondary: return "wait-for-detached-window";
            case Phase::Measure: return "measure-with-detached-window";
            case Phase::WaitForDestroy: return "wait-for-detached-destroy";
            case Phase::Teardown: return "teardown";
            case Phase::Done: return "done";
        }
        return "unknown";
    }

    void UiViewportSmoke::EnterPhase(Phase phase)
    {
        m_Phase = phase;
        m_FramesInPhase = 0;
        m_WatchdogPhase.store(static_cast<int>(phase), std::memory_order_relaxed);
        Heartbeat();
    }

    void UiViewportSmoke::Heartbeat()
    {
        m_HeartbeatNanoseconds.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
    }

    void UiViewportSmoke::StartWatchdog()
    {
        Heartbeat();
        m_Watchdog = std::thread([this]
        {
            while (!m_WatchdogStop.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
                const long long stalledSeconds = (now - m_HeartbeatNanoseconds.load(std::memory_order_relaxed)) / 1000000000ll;
                if (stalledSeconds < kStallLimitSeconds || m_WatchdogStop.load(std::memory_order_relaxed))
                    continue;
                // The frame loop is blocked (an unbounded acquire in a detached
                // swapchain is the case being guarded), so no cooperative shutdown is
                // possible: report and terminate the process.
                std::fprintf(stderr, "UiViewportV1 backend=Vulkan result=fail failedStage=frame-stall stallSeconds=%lld phase=%d\n",
                    stalledSeconds, m_WatchdogPhase.load(std::memory_order_relaxed));
                std::fflush(stderr);
                std::_Exit(124);
            }
        });
    }

    void UiViewportSmoke::StopWatchdog()
    {
        m_WatchdogStop.store(true, std::memory_order_relaxed);
        if (m_Watchdog.joinable())
            m_Watchdog.join();
    }

    // --vulkan-render-smoke closes the application itself once its resize and
    // presentation checks are done and this smoke is no longer pending; closing
    // earlier would cut that smoke short.
    void UiViewportSmoke::CloseApplicationUnlessVulkanSmokeOwnsIt()
    {
        if (!Application::Get().GetSpecification().CommandLineArgs.HasFlag("--vulkan-render-smoke"))
            Application::Get().Close();
    }

    void UiViewportSmoke::Skip(const std::string& reason)
    {
        Log::Info("UiViewportV1 backend=", m_RendererName, " platform=", m_Platform, " result=skip reason=", reason);
        m_Passed = false;
        EnterPhase(Phase::Done);
        StopWatchdog();
        CloseApplicationUnlessVulkanSmokeOwnsIt();
    }

    void UiViewportSmoke::Fail(const std::string& stage)
    {
        if (m_Failed)
            return;
        m_Failed = true;
        const UiViewportDiagnostics diagnostics = Renderer::GetUiViewportDiagnostics();
        const UiTextureCounters counters = Renderer::GetUiTextureCounters();
        Log::Error("UiViewportV1 backend=", m_RendererName, " platform=", m_Platform, " result=fail failedStage=", stage,
            " phase=", PhaseName(), " frame=", m_TotalFrames, " secondaries=", diagnostics.SecondaryCount,
            " secondaryFramesRendered=", diagnostics.SecondaryFramesRendered, " backendErrors=", diagnostics.BackendErrors,
            " uiTexturesLive=", counters.LiveTextures, " uiTexturesPending=", counters.PendingRetirements);
        m_Passed = false;
        EnterPhase(Phase::Done);
        StopWatchdog();
        throw std::runtime_error("UI viewport smoke failed at stage " + stage);
    }

    void UiViewportSmoke::Finish(bool passed)
    {
        m_Passed = passed;
        EnterPhase(Phase::Done);
        StopWatchdog();
        CloseApplicationUnlessVulkanSmokeOwnsIt();
    }

    bool UiViewportSmoke::CreateTextures()
    {
        m_TextureA = Renderer::CreateUiTexture(kTextureAWidth, kTextureAHeight, "ui-viewport-smoke-A");
        m_TextureB = Renderer::CreateUiTexture(kTextureBWidth, kTextureBHeight, "ui-viewport-smoke-B");
        return m_TextureA != kInvalidUiTextureHandle && m_TextureB != kInvalidUiTextureHandle
            && UploadTexture(m_TextureA, 0, kTextureAWidth, kTextureAHeight) && UploadTexture(m_TextureB, 1, kTextureBWidth, kTextureBHeight);
    }

    void UiViewportSmoke::SubmitMainWindow()
    {
        const ImGuiViewport* main = ImGui::GetMainViewport();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::SetNextWindowPos(ImVec2(main->Pos.x + 16.0f, main->Pos.y + 64.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kTextureAWidth), static_cast<float>(kTextureAHeight)));
        if (ImGui::Begin(kMainWindowName, nullptr, kWindowFlags))
        {
            if (m_TextureA != kInvalidUiTextureHandle)
            {
                DrawImage(m_TextureA, kTextureAWidth, kTextureAHeight);
                ++m_ImagesDrawnMain;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    void UiViewportSmoke::SubmitSecondaryWindow()
    {
        const ImGuiViewport* main = ImGui::GetMainViewport();
        m_SecondaryDrawnThisFrame = false;

        // Partly outside the main viewport, never merged back into it: if the
        // compositor moves or clamps the OS window over the main window, the
        // window must stay a separate platform window.
        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&windowClass);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x - 40.0f, main->Pos.y + 120.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kTextureAWidth + kTextureBWidth), static_cast<float>(kTextureAHeight)));
        if (ImGui::Begin(kSecondaryWindowName, nullptr, kWindowFlags))
        {
            const ImGuiViewport* viewport = ImGui::GetWindowViewport();
            if (viewport != main && viewport->PlatformWindowCreated)
            {
                m_SecondaryViewportId = viewport->ID;
                m_SecondaryDrawnThisFrame = true;
            }
            DrawImage(m_TextureA, kTextureAWidth, kTextureAHeight);
            if (m_SecondaryDrawnThisFrame)
                ++m_ImagesDrawnSecondary;
            if (!m_TextureBDestroyed && m_TextureB != kInvalidUiTextureHandle)
            {
                ImGui::SameLine(0.0f, 0.0f);
                DrawImage(m_TextureB, kTextureBWidth, kTextureBHeight);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    void UiViewportSmoke::OnFrameBegin()
    {
        if (m_Phase == Phase::Done)
            return;
        Heartbeat();
        ++m_TotalFrames;
        ++m_FramesInPhase;

        const Clock::time_point now = Clock::now();
        const double frameMilliseconds = m_HasLastFrameStart
            ? std::chrono::duration<double, std::milli>(now - m_LastFrameStart).count() : 0.0;
        const bool timed = m_HasLastFrameStart;
        m_LastFrameStart = now;
        m_HasLastFrameStart = true;

        const UiViewportDiagnostics diagnostics = Renderer::GetUiViewportDiagnostics();

        switch (m_Phase)
        {
            case Phase::Preflight:
            {
                m_RendererName = diagnostics.Renderer;
                m_Platform = diagnostics.Platform;
                if (!diagnostics.Enabled)
                {
                    Skip(ToString(diagnostics.Reason));
                    return;
                }
                // Can this machine create a plain secondary OS window at all? A host
                // without a window system (or with a broken one) skips instead of
                // failing inside the ImGui platform backend.
                glfwDefaultWindowHints();
                glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
                GLFWwindow* probe = glfwCreateWindow(64, 64, "ui-viewport-smoke-probe", nullptr, nullptr);
                if (!probe)
                {
                    Skip("platform-window-creation-failed");
                    return;
                }
                glfwDestroyWindow(probe);

                m_CountersAtStart = Renderer::GetUiTextureCounters();
                m_BackendErrorsAtStart = diagnostics.BackendErrors;
                m_SecondaryCreatesAtStart = diagnostics.SecondaryCreates;
                m_SecondaryDestroysAtStart = diagnostics.SecondaryDestroys;
                if (!CreateTextures())
                    Fail("texture-create");
                StartWatchdog();
                EnterPhase(Phase::Baseline);
                return;
            }
            case Phase::Baseline:
            {
                SubmitMainWindow();
                if (timed && m_FramesInPhase > kWarmupFrames)
                    m_BaselineSamples.push_back(frameMilliseconds);
                if (m_BaselineSamples.size() >= kBaselineMeasuredFrames)
                    EnterPhase(Phase::WaitForSecondary);
                return;
            }
            case Phase::WaitForSecondary:
            {
                SubmitMainWindow();
                SubmitSecondaryWindow();
                const auto found = std::find_if(diagnostics.Secondaries.begin(), diagnostics.Secondaries.end(),
                    [&](const UiViewportSecondaryInfo& info) { return info.ViewportId == m_SecondaryViewportId; });
                if (m_SecondaryViewportId != 0 && found != diagnostics.Secondaries.end())
                {
                    if (found->Skipped)
                        Fail("detached-window-skipped-" + found->SkipReason);
                    if (found->FramesRendered > 0)
                    {
                        m_SecondaryPresentMode = found->PresentMode;
                        m_CreateMilliseconds = diagnostics.LastCreateMilliseconds;
                        m_SecondaryFramesAtMeasureStart = diagnostics.SecondaryFramesRendered;
                        EnterPhase(Phase::Measure);
                    }
                }
                else if (m_FramesInPhase > kSecondaryCreateBudgetFrames)
                    Fail("detached-window-not-created");
                return;
            }
            case Phase::Measure:
            {
                SubmitMainWindow();
                SubmitSecondaryWindow();
                const u64 rendered = diagnostics.SecondaryFramesRendered - m_SecondaryFramesAtMeasureStart;
                if (timed && rendered > kWarmupFrames)
                    m_SecondarySamples.push_back(frameMilliseconds);
                if (diagnostics.SecondaryCount != 1)
                    Fail("detached-window-count-changed");
                // Destroy texture B in a frame in which the detached window still
                // drew it: the registration and image must survive until the
                // detached submission of this frame has finished.
                if (!m_TextureBDestroyed && rendered >= kTextureBDestroyAfterFrames && m_SecondaryDrawnThisFrame)
                {
                    m_HeldBySecondaryAtDestroy = diagnostics.SerialPollsHeldBySecondary;
                    if (!Renderer::DestroyUiTexture(m_TextureB))
                        Fail("texture-b-destroy");
                    m_TextureBDestroyed = true;
                    m_TextureBDestroyFrame = m_TotalFrames;
                }
                if (rendered >= kSecondaryRenderedFrames && m_TextureBDestroyed)
                    EnterPhase(Phase::WaitForDestroy);
                else if (m_FramesInPhase > kSecondaryRenderedFrames * 20)
                    Fail("detached-window-render-stalled");
                return;
            }
            case Phase::WaitForDestroy:
            {
                SubmitMainWindow();
                if (diagnostics.SecondaryCount == 0 && diagnostics.SecondaryDestroys > m_SecondaryDestroysAtStart)
                {
                    m_DestroyMilliseconds = diagnostics.LastDestroyMilliseconds;
                    EnterPhase(Phase::Teardown);
                    if (m_TextureA != kInvalidUiTextureHandle && !Renderer::DestroyUiTexture(m_TextureA))
                        Fail("texture-a-destroy");
                    m_TextureA = kInvalidUiTextureHandle;
                }
                else if (m_FramesInPhase > kDestroyBudgetFrames)
                    Fail("detached-window-not-destroyed");
                return;
            }
            case Phase::Teardown:
            {
                const UiTextureCounters counters = Renderer::GetUiTextureCounters();
                if (counters.PendingRetirements <= m_CountersAtStart.PendingRetirements)
                    EvaluateTeardown();
                else if (m_FramesInPhase > kTeardownBudgetFrames)
                    Fail("ui-texture-retirement-did-not-drain");
                return;
            }
            case Phase::Done:
                return;
        }
    }

    void UiViewportSmoke::CheckSecondaryPixels()
    {
        m_PixelsChecked = true;
        ImGuiViewport* viewport = ImGui::FindViewportByID(m_SecondaryViewportId);
        ImDrawData* drawData = viewport ? viewport->DrawData : nullptr;
        if (!drawData || !drawData->Valid || drawData->CmdListsCount == 0)
        {
            m_PixelStatus = "failed(no-draw-data)";
            Fail("detached-pixels-no-draw-data");
            return;
        }
        if (drawData->FramebufferScale.x != 1.0f || drawData->FramebufferScale.y != 1.0f)
        {
            m_PixelStatus = "not-executed(framebuffer-scale)";
            return;
        }
        const u32 width = static_cast<u32>(drawData->DisplaySize.x);
        const u32 height = static_cast<u32>(drawData->DisplaySize.y);
        if (width < kTextureAWidth + kTextureBWidth || height < kTextureAHeight)
        {
            m_PixelStatus = "not-executed(window-smaller-than-content)";
            return;
        }
        std::vector<u8> pixels;
        if (!Renderer::CaptureUiDrawDataOffscreen(drawData, width, height, pixels) || pixels.size() != static_cast<size_t>(width) * height * 4u)
        {
            m_PixelStatus = "failed(capture)";
            Fail("detached-pixels-capture");
            return;
        }

        const auto compare = [&](u32 texture, u32 originX, u32 textureWidth, u32 textureHeight)
        {
            for (u32 y = 0; y < textureHeight; ++y)
                for (u32 x = 0; x < textureWidth; ++x)
                {
                    const std::array<u8, 4> expected = Texel(texture, x, y);
                    const u8* actual = pixels.data() + (static_cast<size_t>(y) * width + originX + x) * 4u;
                    u32 delta = 0;
                    for (size_t channel = 0; channel < 4; ++channel)
                        delta = std::max<u32>(delta, actual[channel] > expected[channel] ? actual[channel] - expected[channel] : expected[channel] - actual[channel]);
                    m_PixelMaxDelta = std::max(m_PixelMaxDelta, delta);
                    // Texel-centre sampling is exact on conforming GPUs; one code of
                    // slack absorbs filter-weight rounding only.
                    m_PixelMismatches += delta > 1 ? 1u : 0u;
                    ++m_PixelsCompared;
                }
        };
        compare(0, 0, kTextureAWidth, kTextureAHeight);
        compare(1, kTextureAWidth, kTextureBWidth, kTextureBHeight);
        m_PixelStatus = m_PixelMismatches == 0 ? "exact" : "mismatch";
        if (m_PixelMismatches != 0)
            Fail("detached-pixels-mismatch");
    }

    void UiViewportSmoke::OnFrameEnd()
    {
        if (m_Phase == Phase::Done)
            return;
        Heartbeat();
        if (m_Phase == Phase::Measure && !m_PixelsChecked && m_SecondaryDrawnThisFrame && !m_TextureBDestroyed)
        {
            const UiViewportDiagnostics diagnostics = Renderer::GetUiViewportDiagnostics();
            if (diagnostics.SecondaryFramesRendered - m_SecondaryFramesAtMeasureStart >= kPixelCaptureAfterFrames)
                CheckSecondaryPixels();
        }
        if (m_TextureBDestroyed && m_TextureBReleaseFrame == 0
            && Renderer::GetUiTextureCounters().RetirementsReleased > m_CountersAtStart.RetirementsReleased)
            m_TextureBReleaseFrame = m_TotalFrames;
    }

    void UiViewportSmoke::EvaluateTeardown()
    {
        const UiViewportDiagnostics diagnostics = Renderer::GetUiViewportDiagnostics();
        const UiTextureCounters counters = Renderer::GetUiTextureCounters();
        const FrameStatistics baseline = Summarize(m_BaselineSamples);
        const FrameStatistics withSecondary = Summarize(m_SecondarySamples);
        const u64 secondaryRendered = diagnostics.SecondaryFramesRendered - m_SecondaryFramesAtMeasureStart;

        const u64 createdDelta = counters.Created - m_CountersAtStart.Created;
        const u64 destroyedDelta = counters.Destroyed - m_CountersAtStart.Destroyed;
        const bool countersExact = createdDelta == 2 && destroyedDelta == 2 && counters.IsBalanced()
            && counters.LiveTextures == m_CountersAtStart.LiveTextures && counters.PendingRetirements == m_CountersAtStart.PendingRetirements
            && counters.LeakedAtShutdown == m_CountersAtStart.LeakedAtShutdown && counters.WrongThreadRejections == m_CountersAtStart.WrongThreadRejections
            && counters.GpuTexturesCreated - m_CountersAtStart.GpuTexturesCreated == counters.GpuTexturesReleased - m_CountersAtStart.GpuTexturesReleased
            && counters.NativeRegistrations - m_CountersAtStart.NativeRegistrations == counters.NativeUnregistrations - m_CountersAtStart.NativeUnregistrations;
        const bool heldForFrame = counters.RetirementHoldsPresentation > m_CountersAtStart.RetirementHoldsPresentation
            && m_TextureBReleaseFrame > m_TextureBDestroyFrame;
        const double ratio = baseline.AverageMilliseconds > 0.0 ? withSecondary.AverageMilliseconds / baseline.AverageMilliseconds : 0.0;
        // A hang or a vsync-capped secondary shows as an order-of-magnitude slowdown;
        // this is a deadlock/throttle guard, not a performance claim.
        const bool notThrottled = withSecondary.AverageMilliseconds <= std::max(10.0 * baseline.AverageMilliseconds, baseline.AverageMilliseconds + 50.0);
        const u64 errors = diagnostics.BackendErrors - m_BackendErrorsAtStart;
        const u64 createdViewports = diagnostics.SecondaryCreates - m_SecondaryCreatesAtStart;
        const u64 destroyedViewports = diagnostics.SecondaryDestroys - m_SecondaryDestroysAtStart;

        std::vector<std::string> problems;
        if (baseline.Samples < kBaselineMeasuredFrames) problems.push_back("baseline-samples");
        if (withSecondary.Samples < 60) problems.push_back("secondary-samples");
        if (secondaryRendered < 60) problems.push_back("secondary-frames");
        if (m_ImagesDrawnSecondary < 60) problems.push_back("images-in-detached-window");
        if (m_ImagesDrawnMain < baseline.Samples) problems.push_back("images-in-main-window");
        if (!m_PixelsChecked) problems.push_back("detached-pixels-not-checked");
        if (createdViewports != 1 || destroyedViewports != 1 || diagnostics.SecondaryCount != 0) problems.push_back("viewport-lifecycle");
        if (errors != 0) problems.push_back("backend-errors");
        // Generous bound on the backend's hidden vkDeviceWaitIdle (typically tens of
        // milliseconds); only a drain that never finishes in practice exceeds it.
        if (m_CreateMilliseconds > 1000.0 || m_DestroyMilliseconds > 1000.0) problems.push_back("waitidle-cost");
        if (!notThrottled) problems.push_back("main-loop-throttled");
        if (!countersExact) problems.push_back("ui-texture-counters");
        if (!heldForFrame) problems.push_back("ui-texture-retirement-not-held-across-detached-frame");

        std::ostringstream marker;
        marker.setf(std::ios::fixed);
        marker.precision(3);
        marker << "UiViewportV1 backend=" << m_RendererName << " platform=" << m_Platform
               << " secondaryPresent=" << (m_SecondaryPresentMode == UiViewportPresentModeValue::Mailbox ? "MAILBOX"
                      : m_SecondaryPresentMode == UiViewportPresentModeValue::Immediate ? "IMMEDIATE" : "other")
               << " order=main-present-then-detached"
               << " baselineFrames=" << baseline.Samples << " baselineMs=" << baseline.AverageMilliseconds << "/" << baseline.P95Milliseconds
               << "/" << baseline.MaximumMilliseconds
               << " withDetachedFrames=" << withSecondary.Samples << " withDetachedMs=" << withSecondary.AverageMilliseconds << "/"
               << withSecondary.P95Milliseconds << "/" << withSecondary.MaximumMilliseconds << " avgRatio=" << ratio
               << " detachedFramesRendered=" << secondaryRendered << " imagesMain=" << m_ImagesDrawnMain << " imagesDetached=" << m_ImagesDrawnSecondary
               << " detachedPixels=" << m_PixelStatus << "(compared=" << m_PixelsCompared << ",mismatches=" << m_PixelMismatches
               << ",maxDelta=" << m_PixelMaxDelta << ",source=offscreen-draw-data)"
               << " swapchainReadback=none createMs=" << m_CreateMilliseconds << " destroyMs=" << m_DestroyMilliseconds
               << " slowOperations=" << diagnostics.SlowOperations << " viewportsCreated=" << createdViewports << " viewportsDestroyed=" << destroyedViewports << " backendErrors=" << errors
               << " textureDestroyedInDrawFrame=" << (m_TextureBDestroyed ? "yes" : "no")
               << " retirementHeldUntilFrame=" << (m_TextureBReleaseFrame - m_TextureBDestroyFrame)
               << " heldBySecondaryPolls=" << (diagnostics.SerialPollsHeldBySecondary - m_HeldBySecondaryAtDestroy)
               << " uiTexturesCreated=" << createdDelta << " destroyed=" << destroyedDelta
               << " balanced=" << (counters.IsBalanced() ? "yes" : "no") << " leaked=" << counters.LeakedAtShutdown
               << " deadlock=none(watchdog=" << kStallLimitSeconds << "s)";
        if (problems.empty())
        {
            marker << " result=pass";
            Log::Info(marker.str());
            Finish(true);
            return;
        }
        std::string joined;
        for (const std::string& problem : problems)
            joined += (joined.empty() ? "" : ",") + problem;
        marker << " result=fail failedStage=" << joined;
        Log::Error(marker.str());
        m_Failed = true;
        m_Passed = false;
        EnterPhase(Phase::Done);
        StopWatchdog();
        throw std::runtime_error("UI viewport smoke failed: " + joined);
    }
}
