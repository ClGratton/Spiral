#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/FramePacingPolicy.h"
#include "Engine/RHI/RHICommon.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct ImDrawData;

namespace Engine::RHI
{
    struct DeviceCapabilities;
}

namespace Engine
{
    struct ClearColor
    {
        float R = 0.08f;
        float G = 0.09f;
        float B = 0.10f;
        float A = 1.0f;
    };

    struct RendererCapabilities
    {
        bool HasNativeRayTracing = false;
        bool HasWorkGraphs = false;
        bool HasNeuralAccelerators = false;
    };

    enum class RendererTimingStatus
    {
        Unavailable,
        Pending,
        Ready
    };

    struct RendererPassTiming
    {
        std::string Name;
        double CpuMilliseconds = 0.0;
        double GpuMilliseconds = 0.0;
        RendererTimingStatus GpuStatus = RendererTimingStatus::Unavailable;
    };

    struct RendererPresentationTiming
    {
        bool UsesWaitableSwapchain = false;
        bool WaitedForFrameLatency = false;
        bool PresentSucceeded = false;
        u32 MaximumFrameLatency = 0;
        u64 SwapchainGeneration = 0;
        u64 LastSuccessfulPresentGeneration = 0;
        double FrameLatencyWaitMilliseconds = 0.0;
        double PresentMilliseconds = 0.0;
        bool DisplayCadenceAvailable = false;
        bool ReplacementDropStatusAvailable = false;
        bool InputLatencyAvailable = false;
        const char* DisplayCadenceDetail = "unavailable: no display-feedback API path is implemented";
        const char* ReplacementDropDetail = "unavailable: no frame replacement/drop API path is implemented";
        const char* InputLatencyDetail = "unavailable: no input-to-photon measurement path is implemented";
    };

    enum class RendererFrameLifecyclePhase
    {
        FrameStart,
        InputSimulation,
        RenderSubmission,
        PresentBegin,
        PresentEnd,
        IntentionalPacingWait,
        GpuCompletionObservation
    };

    enum class RendererFrameWaitKind
    {
        IntentionalPacing,
        MandatoryDxgiFrameLatency,
        MandatoryVulkanAcquire,
        MandatoryVulkanFence
    };

    struct RendererFrameLifecycleEvent
    {
        RendererFrameLifecyclePhase Phase = RendererFrameLifecyclePhase::FrameStart;
        double MillisecondsFromFrameStart = 0.0;
    };

    struct RendererFrameWaitTiming
    {
        RendererFrameWaitKind Kind = RendererFrameWaitKind::IntentionalPacing;
        bool Applied = false;
        double Milliseconds = 0.0;
        SmoothFrametimeCandidate Candidate = SmoothFrametimeCandidate::InterFrame;
        bool DeadlineMissed = false;
        double RequestedDeadlineMilliseconds = 0.0;
        double ActualReleaseMilliseconds = 0.0;
    };

    struct RendererFrameTiming
    {
        u64 FrameIndex = 0;
        double CpuMilliseconds = 0.0;
        double CpuActiveMilliseconds = 0.0;
        double IntentionalPacingMilliseconds = 0.0;
        double StartToStartMilliseconds = 0.0;
        double GpuMilliseconds = 0.0;
        RendererTimingStatus GpuStatus = RendererTimingStatus::Unavailable;
        RendererPresentationTiming Presentation;
        ResolvedFramePacingPolicy FramePacingPolicy;
        std::vector<RendererFrameLifecycleEvent> Lifecycle;
        std::vector<RendererFrameWaitTiming> Waits;
        bool HasGpuCompletionObservation = false;
        u64 LastGpuCompletionObservedFrameIndex = 0;
        std::vector<RendererPassTiming> Passes;
    };

    inline bool HasValidFrameLifecycleTelemetry(const RendererFrameTiming& timing, bool requireDxgiWait = false, bool requireVulkanWaits = false)
    {
        constexpr RendererFrameLifecyclePhase required[] = {
            RendererFrameLifecyclePhase::FrameStart,
            RendererFrameLifecyclePhase::InputSimulation,
            RendererFrameLifecyclePhase::RenderSubmission,
            RendererFrameLifecyclePhase::PresentBegin,
            RendererFrameLifecyclePhase::PresentEnd
        };
        size_t requiredIndex = 0;
        for (const RendererFrameLifecycleEvent& event : timing.Lifecycle)
        {
            if (requiredIndex < std::size(required) && event.Phase == required[requiredIndex])
                ++requiredIndex;
        }
        bool dxgiWait = false;
        bool vulkanAcquire = false;
        bool vulkanFence = false;
        for (const RendererFrameWaitTiming& wait : timing.Waits)
        {
            dxgiWait |= wait.Kind == RendererFrameWaitKind::MandatoryDxgiFrameLatency && wait.Applied;
            vulkanAcquire |= wait.Kind == RendererFrameWaitKind::MandatoryVulkanAcquire && wait.Applied;
            vulkanFence |= wait.Kind == RendererFrameWaitKind::MandatoryVulkanFence && wait.Applied;
        }
        return requiredIndex == std::size(required)
            && !timing.Waits.empty()
            && timing.Waits.front().Kind == RendererFrameWaitKind::IntentionalPacing
            && !timing.Waits.front().Applied
            && timing.Waits.front().Milliseconds == 0.0
            && timing.HasGpuCompletionObservation
            && (!requireDxgiWait || dxgiWait)
            && (!requireVulkanWaits || (vulkanAcquire && vulkanFence))
            && !timing.Presentation.DisplayCadenceAvailable
            && !timing.Presentation.ReplacementDropStatusAvailable
            && !timing.Presentation.InputLatencyAvailable;
    }

    struct RendererBuildInfo
    {
        bool HasNVRHI = false;
        bool HasDirectXHeaders = false;
        bool HasNVRHID3D12 = false;
        bool HasNVRHIVulkan = false;
        bool HasVulkanHeaders = false;
        const char* NativeViewportHint = "";
    };

    struct RenderViewportRect
    {
        int X = 0;
        int Y = 0;
        int Width = 0;
        int Height = 0;

        bool IsValid() const { return Width > 0 && Height > 0; }
    };

    enum class RendererBackend
    {
        Auto,
        Headless,
        NVRHICommon,
        NVRHID3D12,
        NVRHIVulkan,
        Metal,
        NRI
    };

    struct RendererBackendOption
    {
        RendererBackend Backend = RendererBackend::Auto;
        RHI::Backend RHIBackend = RHI::Backend::None;
        const char* Name = "Unknown";
        const char* Detail = "";
        bool Selectable = false;
        bool Active = false;
    };

    class Renderer
    {
    public:
        static void Initialize();
        static void Shutdown();
        static FramePacingWaitResult WaitForInterFrameBeforeFrame();
        static void BeginFrame(u64 applicationFrameIndex, const FramePacingWaitResult& preFramePacing = {});
        static void EndFrame();
        static bool InitializeImGui(void* nativeWindow);
        static void ShutdownImGui();
        static bool IsNativeImGuiEnabled();
        static void BeginImGuiFrame();
        static void RenderImGuiDrawData(ImDrawData* drawData);

        static void SetClearColor(const ClearColor& color);
        static const ClearColor& GetClearColor();
        static void SetViewportRect(const RenderViewportRect& rect);
        static const RenderViewportRect& GetViewportRect();
        static bool PrepareViewportTexture(u32 width, u32 height);
        static u64 GetViewportTextureId();
        static void MarkViewportTextureQueued(u64 textureId);
        static bool CaptureViewportToFile(std::string_view path);
        static RendererBackend GetActiveBackend();
        static const char* GetActiveBackendName();
        static const std::vector<RendererBackendOption>& GetBackendOptions();
        static bool RequestBackend(RendererBackend backend);
        static const RendererCapabilities& GetCapabilities();
        static const RHI::DeviceCapabilities* GetDeviceCapabilities();
        static const RendererBuildInfo& GetBuildInfo();
        static const RendererFrameTiming& GetLastFrameTiming();
        static void SetFramePacingPolicy(const ResolvedFramePacingPolicy& policy);
        static ResolvedFramePacingPolicy GetFramePacingPolicy();
        static void RecordFrameLifecyclePhase(u64 applicationFrameIndex, RendererFrameLifecyclePhase phase);
        static void RecordFrameWait(u64 applicationFrameIndex, RendererFrameWaitKind kind, bool applied, double milliseconds);
        static FramePacingWaitResult ApplySmoothFrametimeCandidate(SmoothFrametimeCandidate candidate);
        static void RecordGpuCompletionObservation(u64 completedApplicationFrameIndex);
        static void PublishSceneRenderSnapshot(SceneRenderSnapshot snapshot);
        static std::shared_ptr<const SceneRenderSnapshot> GetSceneRenderSnapshot();
        // CPU-only preparation is safe on a FrameTaskGraph worker: it consumes only
        // the immutable scene snapshot and publishes one immutable raster frame.
        static bool PrepareCurrentSceneRasterFrame();
        static std::shared_ptr<const SceneRasterFrame> GetPreparedSceneRasterFrame();
        static void PublishSceneRasterFrame(SceneRasterFrame frame);
        static std::shared_ptr<const SceneRasterFrame> GetLastSceneRasterFrame();
    };

    inline const char* ToString(RendererTimingStatus status)
    {
        switch (status)
        {
            case RendererTimingStatus::Unavailable: return "Unavailable";
            case RendererTimingStatus::Pending: return "Pending";
            case RendererTimingStatus::Ready: return "Ready";
        }

        return "Unknown";
    }
}
