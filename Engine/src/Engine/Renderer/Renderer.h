#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RenderGraph/RenderGraph.h"
#include "Engine/Renderer/FramePacingPolicy.h"
#include "Engine/Renderer/PresentationPolicy.h"
#include "Engine/Renderer/OpticalInputCorrelation.h"
#include "Engine/RHI/RHICommon.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
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
    class AssetRegistry;
    class FramePacingBenchmarkCapture;
    struct MeshArtifact;
    struct FramePacingBenchmarkSnapshot;
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
        Ready,
        Disjoint
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
        // Native presentation evidence is valid only for this exact submitted
        // application frame.  A default/old snapshot is deliberately not
        // evidence for a later timing record.
        std::optional<u64> ApplicationFrameIndex;
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

    enum class RendererEffectiveLimitingSource
    {
        Unresolved,
        RequestedTargetCadence,
        CpuActiveWork,
        GpuWork,
        D3D12SynchronizedPresent,
        VulkanFifoPresent
    };

    enum class RendererFrameLifecyclePhase
    {
        FrameStart,
        InputSample,
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
        // Windows external collectors use this explicit QPC tick domain. Zero
        // remains truthful on platforms where the benchmark has no QPC source.
        u64 QpcTick = 0;
    };

    struct RendererInputSample
    {
        u64 FrameIndex = 0;
        double MillisecondsFromFrameStart = 0.0;
        u64 QpcTick = 0;
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
        Platform::DeadlineWaitTelemetry DeadlineWaitTelemetry;
    };

    struct RendererFrameTiming
    {
        u64 FrameIndex = 0;
        double CpuMilliseconds = 0.0;
        double CpuActiveMilliseconds = 0.0;
        double IntentionalPacingMilliseconds = 0.0;
        double StartToStartMilliseconds = 0.0;
        // Cadence belongs to this terminal frame N and spans FrameStart[N-1]
        // to FrameStart[N].  The source work frame is retained separately.
        std::optional<u64> CadencePreviousFrameIndex;
        std::optional<u64> EffectiveLimitingSourceFrameIndex;
        double GpuMilliseconds = 0.0;
        RendererTimingStatus GpuStatus = RendererTimingStatus::Unavailable;
        std::optional<double> GpuHeadroomMilliseconds;
        RendererPresentationTiming Presentation;
        RendererEffectiveLimitingSource EffectiveLimitingSource = RendererEffectiveLimitingSource::Unresolved;
        ResolvedFramePacingPolicy FramePacingPolicy;
        std::optional<RendererInputSample> InputSample;
        std::optional<u64> InputLatencySourceFrameIndex;
        std::optional<double> InputToSimulationMilliseconds;
        std::optional<double> InputToRenderSubmissionMilliseconds;
        std::optional<double> InputToPresentMilliseconds;
        std::vector<RendererFrameLifecycleEvent> Lifecycle;
        std::vector<RendererFrameWaitTiming> Waits;
        bool HasGpuCompletionObservation = false;
        u64 LastGpuCompletionObservedFrameIndex = 0;
        std::vector<RendererPassTiming> Passes;
    };

    struct RendererGpuTimingPublication
    {
        u64 FrameIndex = 0;
        double GpuMilliseconds = 0.0;
        RendererTimingStatus Status = RendererTimingStatus::Unavailable;
        std::vector<RendererPassTiming> Passes;
    };

    bool BuildRendererGpuTimingPublication(
        const std::vector<RenderGraph::RawTimestampScope>& scopes,
        RendererGpuTimingPublication& publication, std::string* error = nullptr);
    bool ApplyRendererGpuTimingPublication(RendererFrameTiming& timing,
        const RendererGpuTimingPublication& publication, std::string* error = nullptr);
    bool ApplyRendererInputSample(RendererFrameTiming& timing,
        const RendererInputSample& sample, std::string* error = nullptr);
    bool RefreshRendererInputLatencyIntervals(RendererFrameTiming& timing,
        std::string* error = nullptr);

    struct FramePacingBenchmarkIdentity
    {
        std::string RunId;
        u32 ProcessId = 0;
        std::string ExecutablePath;
        u64 QpcFrequency = 0;
    };

    inline bool HasValidFrameLifecycleTelemetry(const RendererFrameTiming& timing, bool requireDxgiWait = false, bool requireVulkanWaits = false)
    {
        constexpr RendererFrameLifecyclePhase required[] = {
            RendererFrameLifecyclePhase::FrameStart,
            RendererFrameLifecyclePhase::InputSample,
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
            && timing.InputSample
            && timing.InputSample->FrameIndex == timing.FrameIndex
            && std::isfinite(timing.InputSample->MillisecondsFromFrameStart)
            && timing.InputSample->MillisecondsFromFrameStart >= 0.0
            && timing.InputLatencySourceFrameIndex == timing.FrameIndex
            && timing.InputToSimulationMilliseconds && timing.InputToRenderSubmissionMilliseconds
            && timing.InputToPresentMilliseconds
            && *timing.InputToSimulationMilliseconds >= 0.0
            && *timing.InputToRenderSubmissionMilliseconds >= *timing.InputToSimulationMilliseconds
            && *timing.InputToPresentMilliseconds >= *timing.InputToRenderSubmissionMilliseconds
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

    struct RendererEffectiveLimitingClassification
    {
        RendererEffectiveLimitingSource Source = RendererEffectiveLimitingSource::Unresolved;
        std::optional<u64> SourceFrameIndex;
    };

    inline RendererEffectiveLimitingClassification ClassifyEffectiveLimitingSource(const RendererFrameTiming& cadence,
        const RendererFrameTiming* sourceWork, RendererBackend backend)
    {
        if (cadence.StartToStartMilliseconds <= 0.0 || !cadence.CadencePreviousFrameIndex || !sourceWork
            || sourceWork->FrameIndex != *cadence.CadencePreviousFrameIndex)
            return {};

        const double observed = cadence.StartToStartMilliseconds;
        const auto requestedCadence = [](const RendererFrameTiming& timing) -> std::optional<double>
        {
            if (timing.FramePacingPolicy.EffectiveMode != FramePacingMode::SmoothFrametime
                || !timing.FramePacingPolicy.SmoothTargetFramesPerSecond
                || !IsValidSmoothTargetFramesPerSecond(*timing.FramePacingPolicy.SmoothTargetFramesPerSecond))
                return std::nullopt;
            return 1000.0 / *timing.FramePacingPolicy.SmoothTargetFramesPerSecond;
        };
        const auto nearRequested = [&](const RendererFrameTiming& timing)
        {
            const std::optional<double> requested = requestedCadence(timing);
            // Native low-rate smoke devices can release late by more than a
            // millisecond while the exact intentional wait remains the only
            // qualifying source. Keep the band relative and bounded without
            // borrowing work or presentation evidence from another frame.
            return requested && std::abs(observed - *requested) <= std::max(1.0, *requested * .15);
        };
        const auto hasAppliedCandidate = [](const RendererFrameTiming& timing, SmoothFrametimeCandidate candidate)
        {
            return std::any_of(timing.Waits.begin(), timing.Waits.end(), [candidate](const RendererFrameWaitTiming& wait)
                { return wait.Kind == RendererFrameWaitKind::IntentionalPacing && wait.Applied && wait.Candidate == candidate; });
        };

        std::vector<RendererEffectiveLimitingClassification> candidates;
        // Inter-frame release is performed on N and is inside cadence N.
        if (cadence.FramePacingPolicy.Candidate == SmoothFrametimeCandidate::InterFrame
            && hasAppliedCandidate(cadence, SmoothFrametimeCandidate::InterFrame) && nearRequested(cadence))
            candidates.push_back({ RendererEffectiveLimitingSource::RequestedTargetCadence, cadence.FrameIndex });
        // Submission-gate release and all rendered work occupy N-1's interval.
        if (sourceWork->FramePacingPolicy.Candidate == SmoothFrametimeCandidate::SubmissionGate
            && hasAppliedCandidate(*sourceWork, SmoothFrametimeCandidate::SubmissionGate) && nearRequested(*sourceWork))
            candidates.push_back({ RendererEffectiveLimitingSource::RequestedTargetCadence, sourceWork->FrameIndex });
        const double sourceRequested = requestedCadence(*sourceWork).value_or(0.0);
        const double tolerance = sourceRequested > 0.0 ? std::max(0.5, sourceRequested * .10) : .5;
        if (sourceRequested > 0.0 && sourceWork->GpuStatus == RendererTimingStatus::Ready && sourceWork->GpuMilliseconds >= observed - tolerance)
            candidates.push_back({ RendererEffectiveLimitingSource::GpuWork, sourceWork->FrameIndex });
        if (sourceRequested > 0.0 && sourceWork->CpuActiveMilliseconds >= observed - tolerance)
            candidates.push_back({ RendererEffectiveLimitingSource::CpuActiveWork, sourceWork->FrameIndex });
        if (sourceWork->Presentation.ApplicationFrameIndex && *sourceWork->Presentation.ApplicationFrameIndex == sourceWork->FrameIndex
            && sourceWork->Presentation.PresentSucceeded && sourceRequested > 0.0 && observed > sourceRequested + tolerance)
        {
            if (backend == RendererBackend::NVRHID3D12) candidates.push_back({ RendererEffectiveLimitingSource::D3D12SynchronizedPresent, sourceWork->FrameIndex });
            if (backend == RendererBackend::NVRHIVulkan) candidates.push_back({ RendererEffectiveLimitingSource::VulkanFifoPresent, sourceWork->FrameIndex });
        }
        return candidates.size() == 1 ? candidates.front() : RendererEffectiveLimitingClassification {};
    }

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
        static const RendererFrameTiming& GetLastCompletedFrameTiming();
        static bool PublishRenderGraphTimestampScopes(
            const std::vector<RenderGraph::RawTimestampScope>& scopes);
        static void SetFramePacingPolicy(const ResolvedFramePacingPolicy& policy);
        static ResolvedFramePacingPolicy GetFramePacingPolicy();
        static void SetPresentationPolicy(PresentationPolicy policy);
        static PresentationPolicy GetPresentationPolicy();
        static RendererPresentationPolicyDiagnostics GetPresentationPolicyDiagnostics();
        static std::optional<RendererInputSample> RecordInputSample(u64 applicationFrameIndex);
        static bool ScheduleOpticalResponseMarker(const OpticalResponseMarker& marker);
        static std::optional<OpticalResponseMarker> GetOpticalResponseMarker(u64 applicationFrameIndex);
        static void ClearOpticalResponseMarker(u64 applicationFrameIndex);
        static void RecordFrameLifecyclePhase(u64 applicationFrameIndex, RendererFrameLifecyclePhase phase);
        static void RecordFrameWait(u64 applicationFrameIndex, RendererFrameWaitKind kind, bool applied, double milliseconds);
        static void RecordCpuPassTiming(std::string name, double milliseconds);
        static FramePacingWaitResult ApplySmoothFrametimeCandidate(SmoothFrametimeCandidate candidate);
        static void BeginFramePacingBenchmark(size_t capacity, double targetFramesPerSecond, u32 warmupFrames,
            FramePacingBenchmarkIdentity identity = {});
        static std::shared_ptr<const FramePacingBenchmarkSnapshot> GetFramePacingBenchmarkSnapshot();
        static void RecordGpuCompletionObservation(u64 completedApplicationFrameIndex);
        static void PublishSceneRenderSnapshot(SceneRenderSnapshot snapshot);
        static std::shared_ptr<const SceneRenderSnapshot> GetSceneRenderSnapshot();
        static void PublishMeshArtifactResolver(const AssetRegistry& registry);
        static bool ResolvePublishedMeshArtifact(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError);
        // CPU-only preparation is safe on a FrameTaskGraph worker: it consumes only
        // the immutable scene snapshot and publishes one immutable raster frame.
        static bool PrepareCurrentSceneRasterFrame();
        static std::shared_ptr<const SceneRasterFrame> GetPreparedSceneRasterFrame();
        static void PublishSceneRasterFrame(SceneRasterFrame frame);
        static std::shared_ptr<const SceneRasterFrame> GetLastSceneRasterFrame();

    private:
        static void ClearMeshArtifactResolver();
    };

    inline const char* ToString(RendererTimingStatus status)
    {
        switch (status)
        {
            case RendererTimingStatus::Unavailable: return "Unavailable";
            case RendererTimingStatus::Pending: return "Pending";
            case RendererTimingStatus::Ready: return "Ready";
            case RendererTimingStatus::Disjoint: return "Disjoint";
        }

        return "Unknown";
    }

    inline const char* ToString(RendererEffectiveLimitingSource source)
    {
        switch (source)
        {
            case RendererEffectiveLimitingSource::Unresolved: return "Unresolved";
            case RendererEffectiveLimitingSource::RequestedTargetCadence: return "RequestedTargetCadence";
            case RendererEffectiveLimitingSource::CpuActiveWork: return "CpuActiveWork";
            case RendererEffectiveLimitingSource::GpuWork: return "GpuWork";
            case RendererEffectiveLimitingSource::D3D12SynchronizedPresent: return "D3D12SynchronizedPresent";
            case RendererEffectiveLimitingSource::VulkanFifoPresent: return "VulkanFifoPresent";
        }
        return "Unknown";
    }
}
