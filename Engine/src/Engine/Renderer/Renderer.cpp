#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/FramePacingBenchmark.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/RenderGraph/RenderGraph.h"

#if defined(GE_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOGDI
        #define NOGDI
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif
#include "Engine/RHI/NVRHI/NVRHIAdapter.h"
#include "Engine/Renderer/CapabilityDiagnostics.h"
#include "Engine/Renderer/NVRHI/NVRHIRenderBackend.h"
#include "Engine/Renderer/RenderBackend.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <stdexcept>

namespace Engine
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        template <typename T>
        class AtomicSharedPointer
        {
        public:
            void Store(std::shared_ptr<const T> value)
            {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
                m_Value.store(std::move(value), std::memory_order_release);
#else
                std::atomic_store_explicit(&m_Value, std::move(value), std::memory_order_release);
#endif
            }

            std::shared_ptr<const T> Load() const
            {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
                return m_Value.load(std::memory_order_acquire);
#else
                return std::atomic_load_explicit(&m_Value, std::memory_order_acquire);
#endif
            }

        private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            std::atomic<std::shared_ptr<const T>> m_Value;
#else
            std::shared_ptr<const T> m_Value;
#endif
        };

        RendererCapabilities s_Capabilities;
        RHI::DeviceCapabilities s_DeviceCapabilities;
        ClearColor s_ClearColor;
        RenderViewportRect s_ViewportRect;
        RendererBuildInfo s_BuildInfo;
        RendererFrameTiming s_FrameTiming;
        std::deque<RendererFrameTiming> s_CompletedFrameTimings;
        constexpr size_t kCompletedFrameTimingCapacity = 8;
        ResolvedFramePacingPolicy s_FramePacingPolicy;
        PresentationPolicy s_PresentationPolicy = PresentationPolicy::Synchronized;
        RendererPresentationPolicyDiagnostics s_PresentationPolicyDiagnostics;
        SystemFramePacingClock s_FramePacingClock;
        SmoothFrametimePacer s_SmoothFrametimePacer;
        AtomicSharedPointer<SceneRenderSnapshot> s_SceneRenderSnapshot;
        AtomicSharedPointer<SceneRasterFrame> s_PreparedSceneRasterFrame;
        AtomicSharedPointer<SceneRasterFrame> s_SceneRasterFrame;
        Scope<RenderBackend> s_Backend;
        std::vector<RendererBackendOption> s_BackendOptions;
        Clock::time_point s_RendererFrameStart;
        std::optional<Clock::time_point> s_PreviousRendererFrameStart;
        double s_InFrameIntentionalPacingMilliseconds = 0.0;
        RendererBackend s_ActiveBackend = RendererBackend::Headless;
        bool s_Initialized = false;
        bool s_HasDeviceCapabilities = false;
        bool s_RendererFrameTimingActive = false;
        bool s_FramePacingBenchmarkQpcActive = false;
        Scope<FramePacingBenchmarkCapture> s_FramePacingBenchmark;
        std::optional<OpticalResponseMarker> s_OpticalResponseMarker;

        RendererFrameTiming* FindRetainedTiming(u64 frameIndex)
        {
            if (s_RendererFrameTimingActive && s_FrameTiming.FrameIndex == frameIndex)
                return &s_FrameTiming;
            for (RendererFrameTiming& timing : s_CompletedFrameTimings)
                if (timing.FrameIndex == frameIndex)
                    return &timing;
            return nullptr;
        }

        void RefreshEffectiveLimitingSource(RendererFrameTiming& cadence)
        {
            cadence.EffectiveLimitingSource = RendererEffectiveLimitingSource::Unresolved;
            cadence.EffectiveLimitingSourceFrameIndex.reset();
            if (!cadence.CadencePreviousFrameIndex)
                return;
            RendererFrameTiming* source = FindRetainedTiming(*cadence.CadencePreviousFrameIndex);
            const RendererEffectiveLimitingClassification classification = ClassifyEffectiveLimitingSource(cadence, source, s_ActiveBackend);
            cadence.EffectiveLimitingSource = classification.Source;
            cadence.EffectiveLimitingSourceFrameIndex = classification.SourceFrameIndex;
        }

        u64 CurrentQpcTick()
        {
            if (!s_FramePacingBenchmarkQpcActive)
                return 0;
#if defined(GE_PLATFORM_WINDOWS)
            LARGE_INTEGER tick {};
            return QueryPerformanceCounter(&tick) ? static_cast<u64>(tick.QuadPart) : 0;
#else
            return 0;
#endif
        }

        bool HasNativeWindow()
        {
            return Application::Get().GetWindow().GetNativeWindow() != nullptr;
        }

        NVRHIRenderBackend* GetNVRHIBackend()
        {
            return dynamic_cast<NVRHIRenderBackend*>(s_Backend.get());
        }

        constexpr bool HasNVRHI()
        {
#if defined(GE_HAS_NVRHI)
            return true;
#else
            return false;
#endif
        }

        constexpr bool HasDirectXHeaders()
        {
#if defined(GE_HAS_DIRECTX_HEADERS)
            return true;
#else
            return false;
#endif
        }

        constexpr bool HasNVRHID3D12()
        {
#if defined(GE_HAS_NVRHI_D3D12)
            return true;
#else
            return false;
#endif
        }

        constexpr bool HasVulkanHeaders()
        {
#if defined(GE_HAS_VULKAN_HEADERS)
            return true;
#else
            return false;
#endif
        }

        constexpr bool HasNVRHIVulkan()
        {
#if defined(GE_HAS_NVRHI_VULKAN)
            return true;
#else
            return false;
#endif
        }

        constexpr bool IsWindows()
        {
#if defined(GE_PLATFORM_WINDOWS)
            return true;
#else
            return false;
#endif
        }

        constexpr bool IsMacOS()
        {
#if defined(GE_PLATFORM_MACOS)
            return true;
#else
            return false;
#endif
        }

        const char* BackendName(RendererBackend backend)
        {
            switch (backend)
            {
                case RendererBackend::Auto: return "Auto";
                case RendererBackend::Headless: return "Headless";
                case RendererBackend::NVRHICommon: return "NVRHI Common";
                case RendererBackend::NVRHID3D12: return "NVRHI D3D12";
                case RendererBackend::NVRHIVulkan: return "NVRHI Vulkan";
                case RendererBackend::Metal: return "Metal";
                case RendererBackend::NRI: return "NRI";
            }

            return "Unknown";
        }

        double ToMilliseconds(Clock::duration duration)
        {
            return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count()) / 1000.0;
        }

        RendererTimingStatus GetBackendGpuTimingStatus()
        {
            return s_HasDeviceCapabilities
                && s_DeviceCapabilities.GetFeature(RHI::DeviceFeature::Timestamps).IsUsable()
                ? RendererTimingStatus::Pending
                : RendererTimingStatus::Unavailable;
        }

        void RefreshFrameTimingCapabilityGroup()
        {
            RHI::CapabilityGroupState* group = s_DeviceCapabilities.GetCapabilityGroup(
                RHI::CapabilityGroupId::Phase3FrameTimingV1);
            if (!group || !group->Implemented || s_FrameTiming.Passes.empty())
                return;

            const bool selectedPathProducedTiming =
                (group->SelectedPath == RHI::CapabilityPath::CpuSteadyClock && s_FrameTiming.CpuMilliseconds >= 0.0)
                || (group->SelectedPath == RHI::CapabilityPath::GpuTimestamps
                    && s_FrameTiming.GpuStatus == RendererTimingStatus::Ready);
            if (!selectedPathProducedTiming)
                return;

            group->Exercised = true;
            if (s_FrameTiming.Presentation.PresentSucceeded)
                group->Qualification = RHI::QualificationLevel::Presentation;
            else if (group->Qualification < RHI::QualificationLevel::Bootstrap)
                group->Qualification = RHI::QualificationLevel::Bootstrap;
        }

        void RefreshTransientResourceCapabilityGroup()
        {
            RHI::CapabilityGroupState* group = s_DeviceCapabilities.GetCapabilityGroup(
                RHI::CapabilityGroupId::Phase3TransientResourcesV1);
            if (!group || !group->Implemented || !s_FrameTiming.Presentation.PresentSucceeded)
                return;

            // This exercises the selected policy/diagnostics route only. Physical
            // transient allocation and completion-token-gated reuse remain separate.
            group->Exercised = true;
            group->Qualification = RHI::QualificationLevel::Presentation;
        }

        void BeginTimingFrame(u64 applicationFrameIndex, const FramePacingWaitResult& preFramePacing)
        {
            if (s_RendererFrameTimingActive)
            {
                s_CompletedFrameTimings.push_back(std::move(s_FrameTiming));
                while (s_CompletedFrameTimings.size() > kCompletedFrameTimingCapacity)
                    s_CompletedFrameTimings.pop_front();
            }
            s_FrameTiming = {};
            s_FrameTiming.FrameIndex = applicationFrameIndex;
            s_FrameTiming.FramePacingPolicy = s_FramePacingPolicy;
            s_FrameTiming.GpuStatus = GetBackendGpuTimingStatus();
            s_RendererFrameStart = Clock::now();
            if (s_PreviousRendererFrameStart)
                s_FrameTiming.StartToStartMilliseconds = ToMilliseconds(s_RendererFrameStart - *s_PreviousRendererFrameStart);
            if (applicationFrameIndex > 0 && FindRetainedTiming(applicationFrameIndex - 1))
                s_FrameTiming.CadencePreviousFrameIndex = applicationFrameIndex - 1;
            s_PreviousRendererFrameStart = s_RendererFrameStart;
            s_InFrameIntentionalPacingMilliseconds = 0.0;
            s_RendererFrameTimingActive = true;
            s_FrameTiming.Lifecycle.push_back({ RendererFrameLifecyclePhase::FrameStart, 0.0, CurrentQpcTick() });
            // Baseline no-intentional-wait record used by Responsive validation;
            // an opted-in candidate adds a separately classified record.
            s_FrameTiming.Waits.push_back({ RendererFrameWaitKind::IntentionalPacing, false, 0.0 });
            if (s_FramePacingPolicy.EffectiveMode == FramePacingMode::SmoothFrametime && s_FramePacingPolicy.Candidate == SmoothFrametimeCandidate::InterFrame)
            {
                s_FrameTiming.Waits.push_back({ RendererFrameWaitKind::IntentionalPacing, preFramePacing.Applied, preFramePacing.WaitMilliseconds,
                    SmoothFrametimeCandidate::InterFrame, preFramePacing.DeadlineMissed, preFramePacing.RequestedDeadlineMilliseconds,
                    preFramePacing.ActualReleaseMilliseconds, preFramePacing.Telemetry });
                s_FrameTiming.IntentionalPacingMilliseconds += preFramePacing.WaitMilliseconds;
            }
            RefreshEffectiveLimitingSource(s_FrameTiming);
        }

        void AddLifecyclePhase(RendererFrameLifecyclePhase phase)
        {
            if (!s_RendererFrameTimingActive)
                return;
            s_FrameTiming.Lifecycle.push_back({ phase, ToMilliseconds(Clock::now() - s_RendererFrameStart), CurrentQpcTick() });
        }

        void RefreshTimingFrameTotal()
        {
            if (!s_RendererFrameTimingActive)
                return;

            s_FrameTiming.CpuMilliseconds = ToMilliseconds(Clock::now() - s_RendererFrameStart);
            s_FrameTiming.CpuActiveMilliseconds = std::max(0.0, s_FrameTiming.CpuMilliseconds - s_InFrameIntentionalPacingMilliseconds);
        }

        void AddPassTiming(std::string name, Clock::duration cpuDuration)
        {
            RendererPassTiming pass;
            pass.Name = std::move(name);
            pass.CpuMilliseconds = ToMilliseconds(cpuDuration);
            pass.GpuStatus = GetBackendGpuTimingStatus();
            s_FrameTiming.Passes.push_back(std::move(pass));
            RefreshTimingFrameTotal();
        }

        void RefreshPresentationTiming()
        {
            const NVRHIRenderBackend* backend = dynamic_cast<const NVRHIRenderBackend*>(s_Backend.get());
            if (const RendererPresentationPolicyDiagnostics* diagnostics = backend ? backend->GetPresentationPolicyDiagnostics() : nullptr)
                s_PresentationPolicyDiagnostics = *diagnostics;
            if (const RendererPresentationTiming* timing = backend ? backend->GetPresentationTiming() : nullptr;
                timing && timing->ApplicationFrameIndex && *timing->ApplicationFrameIndex == s_FrameTiming.FrameIndex)
                s_FrameTiming.Presentation = *timing;
            RefreshEffectiveLimitingSource(s_FrameTiming);
            RefreshFrameTimingCapabilityGroup();
            RefreshTransientResourceCapabilityGroup();
        }

        void RebuildBackendOptions()
        {
            s_BuildInfo.HasNVRHI = HasNVRHI();
            s_BuildInfo.HasDirectXHeaders = HasDirectXHeaders();
            s_BuildInfo.HasNVRHID3D12 = HasNVRHID3D12();
            s_BuildInfo.HasNVRHIVulkan = HasNVRHIVulkan();
            s_BuildInfo.HasVulkanHeaders = HasVulkanHeaders();
            s_BuildInfo.NativeViewportHint = HasNVRHID3D12()
                ? "D3D12 viewport code is compiled into this executable."
                : "D3D12 viewport code is not compiled into this executable. On Windows, use the VS2022 build/run path for the native viewport.";

            s_BackendOptions.clear();
            s_BackendOptions.push_back({
                RendererBackend::Auto,
                RHI::Backend::None,
                "Auto",
                HasNVRHI() ? "Resolves to the NVRHI renderer boundary." : "No concrete renderer dependency is available.",
                false,
                false
            });
            s_BackendOptions.push_back({
                RendererBackend::NVRHICommon,
                RHI::Backend::NVRHICommon,
                "NVRHI Common",
                HasNVRHI()
                    ? (s_ActiveBackend == RendererBackend::NVRHICommon ? "Active fallback probe backend; native device backend was unavailable." : "NVRHI is linked, but this build is using a native backend.")
                    : "NVRHI is not vendored in this checkout.",
                s_ActiveBackend == RendererBackend::NVRHICommon,
                s_ActiveBackend == RendererBackend::NVRHICommon
            });
            s_BackendOptions.push_back({
                RendererBackend::NVRHID3D12,
                RHI::Backend::NVRHID3D12,
                "NVRHI D3D12",
                IsWindows()
                    ? (HasNVRHID3D12()
                        ? (s_ActiveBackend == RendererBackend::NVRHID3D12 ? "Active native D3D12 device; DX12 presentation is used by the editor UI when initialized." : "Compiled, but live backend switching is not implemented yet.")
                        : (HasDirectXHeaders() ? "Unavailable in this executable: the current build action did not compile the NVRHI D3D12 backend. Use the VS2022 build/run path on Windows." : "DirectX-Headers are not vendored."))
                    : "D3D12 is Windows-only.",
                s_ActiveBackend == RendererBackend::NVRHID3D12,
                s_ActiveBackend == RendererBackend::NVRHID3D12
            });
            s_BackendOptions.push_back({
                RendererBackend::NVRHIVulkan,
                RHI::Backend::NVRHIVulkan,
                "NVRHI Vulkan",
                HasNVRHIVulkan()
                    ? (s_ActiveBackend == RendererBackend::NVRHIVulkan
                        ? "Active engine-owned Vulkan device, swapchain, and ImGui presentation path."
                        : "Compiled and available through --renderer-vulkan; live backend switching is not implemented yet.")
                    : (HasVulkanHeaders() ? "Vulkan headers are pinned, but the NVRHI Vulkan backend is not compiled into this executable." : "Vulkan-Headers are not vendored."),
                s_ActiveBackend == RendererBackend::NVRHIVulkan,
                s_ActiveBackend == RendererBackend::NVRHIVulkan
            });
            s_BackendOptions.push_back({
                RendererBackend::Metal,
                RHI::Backend::Metal,
                "Metal",
                IsMacOS() ? "Metal backend is not implemented yet." : "Metal is only available on Apple platforms.",
                false,
                s_ActiveBackend == RendererBackend::Metal
            });
            s_BackendOptions.push_back({
                RendererBackend::NRI,
                RHI::Backend::NRI,
                "NRI",
                "Not integrated; keep as a future low-level backend comparison point.",
                false,
                s_ActiveBackend == RendererBackend::NRI
            });
            s_BackendOptions.push_back({
                RendererBackend::Headless,
                RHI::Backend::None,
                "Headless",
                "Used for smoke tests and machines without a native renderer.",
                s_ActiveBackend == RendererBackend::Headless,
                s_ActiveBackend == RendererBackend::Headless
            });
        }
    }

    void Renderer::Initialize()
    {
        if (s_Initialized)
            return;

        s_DeviceCapabilities = {};
        s_PreviousRendererFrameStart.reset();
        s_CompletedFrameTimings.clear();
        s_RendererFrameTimingActive = false;
        s_SmoothFrametimePacer.Reset();
        s_HasDeviceCapabilities = false;
        s_PresentationPolicyDiagnostics = {};
        s_PresentationPolicyDiagnostics.Requested = s_PresentationPolicy;

        if (HasNativeWindow() && HasNVRHI())
        {
            const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
            const bool requestVulkan = args.HasFlag("--renderer-vulkan") || args.HasFlag("--vulkan-render-smoke");
            Scope<NVRHIRenderBackend> backend = CreateScope<NVRHIRenderBackend>();
            NVRHIRenderBackend* backendPtr = backend.get();
            if (requestVulkan)
                backendPtr->SetRequestedBackend(RHI::Backend::NVRHIVulkan);
            s_Backend = std::move(backend);
            if (!s_Backend->Initialize())
            {
                s_Backend.reset();
                if (requestVulkan || args.HasFlag("--renderer-adapter-strict"))
                    throw std::runtime_error("The requested renderer or strict adapter could not be initialized");
            }
            else
            {
                s_ActiveBackend = backendPtr->GetRendererBackend();
                if (const RHI::DeviceCapabilities* capabilities = backendPtr->GetDeviceCapabilities())
                {
                    s_DeviceCapabilities = *capabilities;
                    s_DeviceCapabilities.CapabilityGroups.push_back(
                        BuildFrameTimingCapabilityGroup(s_DeviceCapabilities, false));
                    s_DeviceCapabilities.CapabilityGroups.push_back(
                        RenderGraph::BuildTransientResourceCapabilityGroup(s_DeviceCapabilities));
                    s_HasDeviceCapabilities = true;
                    s_Capabilities.HasNativeRayTracing = capabilities->GetFeature(RHI::DeviceFeature::RayTracing).IsUsable();
                    s_Capabilities.HasWorkGraphs = capabilities->GetFeature(RHI::DeviceFeature::WorkGraphs).IsUsable();
                    s_Capabilities.HasNeuralAccelerators = capabilities->GetFeature(RHI::DeviceFeature::NeuralShaders).IsUsable();
                    for (const RHI::CapabilityGroupState& group : s_DeviceCapabilities.CapabilityGroups)
                    {
                        Log::Info("Renderer capability group: group=", RHI::ToString(group.Group),
                            ", profile=", group.ProfileName,
                            ", preferredPath=", RHI::ToString(group.PreferredPath),
                            ", selectedPath=", RHI::ToString(group.SelectedPath),
                            ", implemented=", group.Implemented ? "yes" : "no",
                            ", exercised=", group.Exercised ? "yes" : "no");
                    }
                }
            }
        }

        if (!s_Backend)
            s_ActiveBackend = RendererBackend::Headless;
        RebuildBackendOptions();
        s_Initialized = true;
        Log::Info("Renderer initialized with backend: ", s_Backend ? s_Backend->GetName() : "Headless");

        const RHI::NVRHIAdapterInfo nvrhiInfo = RHI::QueryNVRHIAdapter();
        if (nvrhiInfo.Available)
            Log::Info("NVRHI core linked; format probe: ", nvrhiInfo.ProbeFormatName);
    }

    void Renderer::Shutdown()
    {
        s_SceneRenderSnapshot.Store({});
        s_PreparedSceneRasterFrame.Store({});
        s_SceneRasterFrame.Store({});
        s_PreviousRendererFrameStart.reset();
        s_CompletedFrameTimings.clear();
        s_RendererFrameTimingActive = false;
        s_SmoothFrametimePacer.Reset();
        s_FramePacingBenchmark.reset();
        s_FramePacingBenchmarkQpcActive = false;
        s_PresentationPolicyDiagnostics = {};
        s_PresentationPolicyDiagnostics.Requested = s_PresentationPolicy;
        if (!s_Initialized)
            return;

        if (s_Backend)
        {
            s_Backend->Shutdown();
            s_Backend.reset();
        }

        s_Capabilities = {};
        s_DeviceCapabilities = {};
        s_HasDeviceCapabilities = false;
        s_ActiveBackend = RendererBackend::Headless;
        RebuildBackendOptions();

        Log::Info("Renderer shutdown");
        s_Initialized = false;
    }

    FramePacingWaitResult Renderer::WaitForInterFrameBeforeFrame()
    {
        return s_SmoothFrametimePacer.Apply(s_FramePacingPolicy, SmoothFrametimeCandidate::InterFrame, s_FramePacingClock);
    }

    void Renderer::BeginFrame(u64 applicationFrameIndex, const FramePacingWaitResult& preFramePacing)
    {
        if (!s_Initialized || !s_Backend)
            return;

        BeginTimingFrame(applicationFrameIndex, preFramePacing);
        const Clock::time_point passStart = Clock::now();
        s_Backend->BeginFrame(s_ClearColor);
        AddPassTiming("Renderer BeginFrame", Clock::now() - passStart);
        RefreshPresentationTiming();
    }

    void Renderer::EndFrame()
    {
        if (!s_Initialized || !s_Backend)
            return;

        const Clock::time_point passStart = Clock::now();
        s_Backend->EndFrame();
        AddPassTiming("Renderer EndFrame", Clock::now() - passStart);
    }

    bool Renderer::InitializeImGui(void* nativeWindow)
    {
        NVRHIRenderBackend* backend = GetNVRHIBackend();
        if (!backend)
            return false;

        Window& window = Application::Get().GetWindow();
        return backend->InitializeImGui(nativeWindow, window.GetWidth(), window.GetHeight());
    }

    void Renderer::ShutdownImGui()
    {
        if (NVRHIRenderBackend* backend = GetNVRHIBackend())
            backend->ShutdownImGui();
    }

    bool Renderer::IsNativeImGuiEnabled()
    {
        const NVRHIRenderBackend* backend = dynamic_cast<const NVRHIRenderBackend*>(s_Backend.get());
        return backend && backend->IsNativeImGuiEnabled();
    }

    void Renderer::BeginImGuiFrame()
    {
        if (NVRHIRenderBackend* backend = GetNVRHIBackend())
            backend->BeginImGuiFrame();
    }

    void Renderer::RenderImGuiDrawData(ImDrawData* drawData)
    {
        NVRHIRenderBackend* backend = GetNVRHIBackend();
        if (!backend)
            return;

        Window& window = Application::Get().GetWindow();
        const Clock::time_point passStart = Clock::now();
        backend->RenderImGuiDrawData(drawData, s_ClearColor, window.GetWidth(), window.GetHeight());
        AddPassTiming("Native viewport + ImGui present", Clock::now() - passStart);
        RefreshPresentationTiming();
        if (s_FramePacingBenchmark)
            s_FramePacingBenchmark->Record(s_FrameTiming);
    }

    void Renderer::SetClearColor(const ClearColor& color)
    {
        s_ClearColor = color;
    }

    const ClearColor& Renderer::GetClearColor()
    {
        return s_ClearColor;
    }

    void Renderer::SetViewportRect(const RenderViewportRect& rect)
    {
        s_ViewportRect = rect;
    }

    const RenderViewportRect& Renderer::GetViewportRect()
    {
        return s_ViewportRect;
    }

    bool Renderer::PrepareViewportTexture(u32 width, u32 height)
    {
        if (NVRHIRenderBackend* backend = GetNVRHIBackend())
            return backend->PrepareViewportTexture(width, height);

        return false;
    }

    u64 Renderer::GetViewportTextureId()
    {
        if (const NVRHIRenderBackend* backend = dynamic_cast<const NVRHIRenderBackend*>(s_Backend.get()))
            return backend->GetViewportTextureId();

        return 0;
    }

    void Renderer::MarkViewportTextureQueued(u64 textureId)
    {
        if (NVRHIRenderBackend* backend = GetNVRHIBackend())
            backend->MarkViewportTextureQueued(textureId);
    }

    bool Renderer::CaptureViewportToFile(std::string_view path)
    {
        if (NVRHIRenderBackend* backend = GetNVRHIBackend())
            return backend->CaptureViewportToFile(path);

        Log::Warn("Viewport capture requested without an active native renderer");
        return false;
    }

    RendererBackend Renderer::GetActiveBackend()
    {
        return s_ActiveBackend;
    }

    const char* Renderer::GetActiveBackendName()
    {
        return BackendName(s_ActiveBackend);
    }

    const std::vector<RendererBackendOption>& Renderer::GetBackendOptions()
    {
        if (s_BackendOptions.empty())
            RebuildBackendOptions();

        return s_BackendOptions;
    }

    bool Renderer::RequestBackend(RendererBackend backend)
    {
        for (const RendererBackendOption& option : GetBackendOptions())
        {
            if (option.Backend != backend)
                continue;

            if (option.Active)
                return true;

            if (!option.Selectable)
            {
                Log::Warn("Renderer backend is not selectable yet: ", option.Name, " (", option.Detail, ")");
                return false;
            }

            Log::Warn("Renderer backend switching is not implemented yet: ", option.Name);
            return false;
        }

        Log::Warn("Unknown renderer backend requested: ", static_cast<int>(backend));
        return false;
    }

    const RendererCapabilities& Renderer::GetCapabilities()
    {
        return s_Capabilities;
    }

    const RHI::DeviceCapabilities* Renderer::GetDeviceCapabilities()
    {
        return s_HasDeviceCapabilities ? &s_DeviceCapabilities : nullptr;
    }

    const RendererBuildInfo& Renderer::GetBuildInfo()
    {
        if (s_BuildInfo.NativeViewportHint[0] == '\0')
            RebuildBackendOptions();

        return s_BuildInfo;
    }

    const RendererFrameTiming& Renderer::GetLastFrameTiming()
    {
        return s_FrameTiming;
    }

    const RendererFrameTiming& Renderer::GetLastCompletedFrameTiming()
    {
        return s_CompletedFrameTimings.empty() ? s_FrameTiming : s_CompletedFrameTimings.back();
    }

    bool Renderer::PublishRenderGraphTimestampScopes(
        const std::vector<RenderGraph::RawTimestampScope>& scopes)
    {
        // Backend bootstrap/offscreen qualification can execute the same scene
        // renderer before Application has opened an authoritative timing frame.
        // P3A evidence remains valid there, but P3B must not invent frame state.
        if (!s_RendererFrameTimingActive)
            return true;

        RendererGpuTimingPublication publication;
        std::string error;
        if (!BuildRendererGpuTimingPublication(scopes, publication, &error))
        {
            Log::Error("Renderer GPU timing conversion failed: ", error);
            return false;
        }

        RendererFrameTiming* retained = FindRetainedTiming(publication.FrameIndex);
        if (!retained)
        {
            // Initialization smokes and a genuinely delayed GPU can retire a
            // scope after its application timing record is no longer retained.
            // Never attach that result to the current frame, but do not turn a
            // diagnostic retention miss into a renderer failure either.
            Log::Warn("RendererGpuTimingDropV1 backend=", GetActiveBackendName(),
                " frame=", publication.FrameIndex,
                " status=Unavailable reason=retained-frame-not-found result=ignored");
            return true;
        }
        if (!ApplyRendererGpuTimingPublication(*retained, publication, &error))
        {
            Log::Error("Renderer GPU timing exact-frame publication failed: ", error);
            return false;
        }
        // GPU work frame N-1 can only amend the cadence record N that owns
        // the interval containing that work. Never borrow it for an arbitrary
        // current record when N has been evicted or is nonconsecutive.
        if (publication.FrameIndex != std::numeric_limits<u64>::max())
        {
            if (RendererFrameTiming* cadence = FindRetainedTiming(publication.FrameIndex + 1);
                cadence && cadence->CadencePreviousFrameIndex && *cadence->CadencePreviousFrameIndex == publication.FrameIndex)
            {
                RefreshEffectiveLimitingSource(*cadence);
                if (s_FramePacingBenchmark)
                    s_FramePacingBenchmark->AmendEffectiveLimitingSource(cadence->FrameIndex,
                        cadence->EffectiveLimitingSource, cadence->EffectiveLimitingSourceFrameIndex);
            }
        }
        if (s_FramePacingBenchmark && !s_FramePacingBenchmark->AmendGpuTiming(publication))
            Log::Warn("BenchmarkGpuTimingDropV1 frame=", publication.FrameIndex,
                " status=Unavailable reason=frame-not-retained result=ignored");

        RHI::CapabilityPath selectedPath = RHI::CapabilityPath::CpuSteadyClock;
        if (publication.Status == RendererTimingStatus::Ready && s_HasDeviceCapabilities)
        {
            RHI::CapabilityGroupState promoted = BuildFrameTimingCapabilityGroup(s_DeviceCapabilities, true);
            promoted.Exercised = true;
            promoted.Qualification = retained->Presentation.PresentSucceeded
                ? RHI::QualificationLevel::Presentation : RHI::QualificationLevel::Bootstrap;
            if (RHI::CapabilityGroupState* group = s_DeviceCapabilities.GetCapabilityGroup(
                    RHI::CapabilityGroupId::Phase3FrameTimingV1))
                *group = std::move(promoted);
            selectedPath = RHI::CapabilityPath::GpuTimestamps;
        }
        Log::Info("RendererGpuTimingV1 backend=", GetActiveBackendName(),
            " frame=", publication.FrameIndex, " passes=", publication.Passes.size(),
            " wholeMs=", publication.GpuMilliseconds, " status=", ToString(publication.Status),
            " capability=", RHI::ToString(selectedPath), " result=pass");
        return true;
    }

    void Renderer::SetFramePacingPolicy(const ResolvedFramePacingPolicy& policy)
    {
        s_FramePacingPolicy = policy;
    }

    ResolvedFramePacingPolicy Renderer::GetFramePacingPolicy()
    {
        return s_FramePacingPolicy;
    }

    std::optional<RendererInputSample> Renderer::RecordInputSample(u64 applicationFrameIndex)
    {
        if (!s_RendererFrameTimingActive || s_FrameTiming.FrameIndex != applicationFrameIndex)
            return std::nullopt;

        const Clock::time_point sampleTime = Clock::now();
        const RendererInputSample sample {
            applicationFrameIndex,
            ToMilliseconds(sampleTime - s_RendererFrameStart),
            CurrentQpcTick()
        };
        if (!ApplyRendererInputSample(s_FrameTiming, sample))
            return std::nullopt;
        if (!RefreshRendererInputLatencyIntervals(s_FrameTiming))
            return std::nullopt;
        return sample;
    }

    bool Renderer::ScheduleOpticalResponseMarker(const OpticalResponseMarker& marker)
    {
        if (!s_RendererFrameTimingActive || !s_FrameTiming.InputSample || marker.MarkerId.empty()
            || !marker.HighContrast || marker.ApplicationFrameIndex != s_FrameTiming.FrameIndex
            || marker.InputFrameIndex != s_FrameTiming.InputSample->FrameIndex
            || marker.InputQpcTick != s_FrameTiming.InputSample->QpcTick)
            return false;
        s_OpticalResponseMarker = marker;
        return true;
    }

    std::optional<OpticalResponseMarker> Renderer::GetOpticalResponseMarker(u64 applicationFrameIndex)
    {
        if (!s_OpticalResponseMarker || s_OpticalResponseMarker->ApplicationFrameIndex != applicationFrameIndex)
            return std::nullopt;
        return s_OpticalResponseMarker;
    }

    void Renderer::ClearOpticalResponseMarker(u64 applicationFrameIndex)
    {
        if (s_OpticalResponseMarker && s_OpticalResponseMarker->ApplicationFrameIndex == applicationFrameIndex)
            s_OpticalResponseMarker.reset();
    }

    void Renderer::RecordFrameLifecyclePhase(u64 applicationFrameIndex, RendererFrameLifecyclePhase phase)
    {
        if (phase == RendererFrameLifecyclePhase::InputSample || s_FrameTiming.FrameIndex != applicationFrameIndex)
            return;
        AddLifecyclePhase(phase);
        if (phase == RendererFrameLifecyclePhase::InputSimulation
            || phase == RendererFrameLifecyclePhase::RenderSubmission
            || phase == RendererFrameLifecyclePhase::PresentEnd)
            RefreshRendererInputLatencyIntervals(s_FrameTiming);
    }

    void Renderer::RecordFrameWait(u64 applicationFrameIndex, RendererFrameWaitKind kind, bool applied, double milliseconds)
    {
        if (s_FrameTiming.FrameIndex != applicationFrameIndex)
            return;
        s_FrameTiming.Waits.push_back({ kind, applied, milliseconds });
    }

    FramePacingWaitResult Renderer::ApplySmoothFrametimeCandidate(SmoothFrametimeCandidate candidate)
    {
        FramePacingWaitResult result;
        if (!s_RendererFrameTimingActive)
            return result;

        const ResolvedFramePacingPolicy& framePolicy = s_FrameTiming.FramePacingPolicy;
        result = s_SmoothFrametimePacer.Apply(framePolicy, candidate, s_FramePacingClock);
        if (framePolicy.EffectiveMode == FramePacingMode::SmoothFrametime
            && framePolicy.Candidate == candidate)
        {
            s_FrameTiming.Waits.push_back({ RendererFrameWaitKind::IntentionalPacing, result.Applied,
                result.WaitMilliseconds, candidate, result.DeadlineMissed,
                result.RequestedDeadlineMilliseconds, result.ActualReleaseMilliseconds, result.Telemetry });
            s_FrameTiming.IntentionalPacingMilliseconds += result.WaitMilliseconds;
            s_InFrameIntentionalPacingMilliseconds += result.WaitMilliseconds;
            AddLifecyclePhase(RendererFrameLifecyclePhase::IntentionalPacingWait);
            RefreshTimingFrameTotal();
            Log::Info("SmoothFrametimeCandidateV1 candidate=", ToString(candidate),
                " control=", candidate == SmoothFrametimeCandidate::InterFrame ? "after-prior-present-before-input" : "pre-native-submit",
                " waitMs=", result.WaitMilliseconds, " missed=", result.DeadlineMissed ? "yes" : "no",
                " frame=", s_FrameTiming.FrameIndex);
        }
        return result;
    }

    void Renderer::BeginFramePacingBenchmark(size_t capacity, double targetFramesPerSecond, u32 warmupFrames,
        FramePacingBenchmarkIdentity identity)
    {
        const bool attachmentQpcActive = !identity.RunId.empty() && identity.ProcessId != 0
            && !identity.ExecutablePath.empty() && identity.QpcFrequency != 0;
        FramePacingBenchmarkCondition condition;
        condition.Backend = GetActiveBackendName();
        condition.TargetFramesPerSecond = targetFramesPerSecond;
        condition.WarmupFrames = warmupFrames;
        condition.Policy = s_FramePacingPolicy;
        // Native presentation owns these frozen conditions.
        const RendererPresentationPolicyDiagnostics presentation = GetPresentationPolicyDiagnostics();
        condition.RequestedPresentationPolicy = ToString(presentation.Requested);
        condition.PresentationCapability = presentation.Capability;
        condition.PresentationMode = ToString(presentation.Actual);
        condition.PresentationFallback = presentation.FallbackReason;
        condition.PresentationGeneration = presentation.SwapchainGeneration;
        condition.SyncMode = PresentationSyncEncoding(presentation.Actual);
        condition.VrrMode = "unavailable";
        condition.TearingMode = presentation.PresentAllowsTearing ? "allowed" : "disabled";
        condition.RunId = identity.RunId.empty() ? "unavailable" : std::move(identity.RunId);
        condition.ProcessId = identity.ProcessId;
        condition.ExecutablePath = identity.ExecutablePath.empty() ? "unavailable" : std::move(identity.ExecutablePath);
        condition.QpcFrequency = identity.QpcFrequency;
        s_FramePacingBenchmarkQpcActive = attachmentQpcActive;
        if (s_HasDeviceCapabilities)
        {
            condition.Adapter = s_DeviceCapabilities.Identity.Name;
            condition.AdapterStableId = s_DeviceCapabilities.Identity.StableId;
        }
        s_FramePacingBenchmark = CreateScope<FramePacingBenchmarkCapture>(capacity);
        s_FramePacingBenchmark->Begin(std::move(condition));
    }

    std::shared_ptr<const FramePacingBenchmarkSnapshot> Renderer::GetFramePacingBenchmarkSnapshot()
    {
        return s_FramePacingBenchmark ? s_FramePacingBenchmark->GetSnapshot() : nullptr;
    }

    void Renderer::SetPresentationPolicy(PresentationPolicy policy)
    {
        s_PresentationPolicy = policy;
        s_PresentationPolicyDiagnostics.Requested = policy;
        if (NVRHIRenderBackend* backend = dynamic_cast<NVRHIRenderBackend*>(s_Backend.get()))
            backend->SetPresentationPolicy(policy);
    }

    PresentationPolicy Renderer::GetPresentationPolicy() { return s_PresentationPolicy; }

    RendererPresentationPolicyDiagnostics Renderer::GetPresentationPolicyDiagnostics()
    {
        if (const NVRHIRenderBackend* backend = dynamic_cast<const NVRHIRenderBackend*>(s_Backend.get()))
            if (const RendererPresentationPolicyDiagnostics* diagnostics = backend->GetPresentationPolicyDiagnostics())
                s_PresentationPolicyDiagnostics = *diagnostics;
        return s_PresentationPolicyDiagnostics;
    }

    void Renderer::RecordGpuCompletionObservation(u64 completedApplicationFrameIndex)
    {
        s_FrameTiming.HasGpuCompletionObservation = true;
        s_FrameTiming.LastGpuCompletionObservedFrameIndex = completedApplicationFrameIndex;
        AddLifecyclePhase(RendererFrameLifecyclePhase::GpuCompletionObservation);
    }

    void Renderer::PublishSceneRenderSnapshot(SceneRenderSnapshot snapshot)
    {
        std::shared_ptr<const SceneRenderSnapshot> published =
            std::make_shared<const SceneRenderSnapshot>(std::move(snapshot));
        s_SceneRenderSnapshot.Store(std::move(published));
    }

    std::shared_ptr<const SceneRenderSnapshot> Renderer::GetSceneRenderSnapshot()
    {
        return s_SceneRenderSnapshot.Load();
    }

    bool Renderer::PrepareCurrentSceneRasterFrame()
    {
        const std::shared_ptr<const SceneRenderSnapshot> snapshot = s_SceneRenderSnapshot.Load();
        if (!snapshot)
        {
            s_PreparedSceneRasterFrame.Store({});
            return true;
        }

        std::shared_ptr<const SceneRasterFrame> prepared =
            std::make_shared<const SceneRasterFrame>(PrepareSceneRasterFrame(*snapshot));
        s_PreparedSceneRasterFrame.Store(std::move(prepared));
        return true;
    }

    std::shared_ptr<const SceneRasterFrame> Renderer::GetPreparedSceneRasterFrame()
    {
        return s_PreparedSceneRasterFrame.Load();
    }

    void Renderer::PublishSceneRasterFrame(SceneRasterFrame frame)
    {
        std::shared_ptr<const SceneRasterFrame> published =
            std::make_shared<const SceneRasterFrame>(std::move(frame));
        s_SceneRasterFrame.Store(std::move(published));
    }

    std::shared_ptr<const SceneRasterFrame> Renderer::GetLastSceneRasterFrame()
    {
        return s_SceneRasterFrame.Load();
    }
}
