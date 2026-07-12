#include "Engine/Renderer/Renderer.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/RHI/NVRHI/NVRHIAdapter.h"
#include "Engine/Renderer/NVRHI/NVRHIRenderBackend.h"
#include "Engine/Renderer/RenderBackend.h"

#include <chrono>
#include <stdexcept>

namespace Engine
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        RendererCapabilities s_Capabilities;
        ClearColor s_ClearColor;
        RenderViewportRect s_ViewportRect;
        CameraView s_CameraView;
        RendererBuildInfo s_BuildInfo;
        RendererFrameTiming s_FrameTiming;
        Scope<RenderBackend> s_Backend;
        std::vector<RendererBackendOption> s_BackendOptions;
        Clock::time_point s_RendererFrameStart;
        u64 s_RendererTimingFrameIndex = 0;
        RendererBackend s_ActiveBackend = RendererBackend::Headless;
        bool s_Initialized = false;
        bool s_RendererFrameTimingActive = false;

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
            const NVRHIRenderBackend* backend = dynamic_cast<const NVRHIRenderBackend*>(s_Backend.get());
            const RHI::DeviceCapabilities* capabilities = backend ? backend->GetDeviceCapabilities() : nullptr;
            return capabilities && capabilities->GetFeature(RHI::DeviceFeature::Timestamps).Implemented
                ? RendererTimingStatus::Pending
                : RendererTimingStatus::Unavailable;
        }

        void BeginTimingFrame()
        {
            s_FrameTiming = {};
            s_FrameTiming.FrameIndex = ++s_RendererTimingFrameIndex;
            s_FrameTiming.GpuStatus = GetBackendGpuTimingStatus();
            s_RendererFrameStart = Clock::now();
            s_RendererFrameTimingActive = true;
        }

        void RefreshTimingFrameTotal()
        {
            if (!s_RendererFrameTimingActive)
                return;

            s_FrameTiming.CpuMilliseconds = ToMilliseconds(Clock::now() - s_RendererFrameStart);
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
            if (const RendererPresentationTiming* timing = backend ? backend->GetPresentationTiming() : nullptr)
                s_FrameTiming.Presentation = *timing;
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
                    s_Capabilities.HasNativeRayTracing = capabilities->GetFeature(RHI::DeviceFeature::RayTracing).IsUsable();
                    s_Capabilities.HasWorkGraphs = capabilities->GetFeature(RHI::DeviceFeature::WorkGraphs).IsUsable();
                    s_Capabilities.HasNeuralAccelerators = capabilities->GetFeature(RHI::DeviceFeature::NeuralShaders).IsUsable();
                }
            }
        }

        if (!s_Backend)
            s_ActiveBackend = RendererBackend::Headless;
        RebuildBackendOptions();
        s_CameraView = EditorCamera().GetCameraView();

        s_Initialized = true;
        Log::Info("Renderer initialized with backend: ", s_Backend ? s_Backend->GetName() : "Headless");

        const RHI::NVRHIAdapterInfo nvrhiInfo = RHI::QueryNVRHIAdapter();
        if (nvrhiInfo.Available)
            Log::Info("NVRHI core linked; format probe: ", nvrhiInfo.ProbeFormatName);
    }

    void Renderer::Shutdown()
    {
        if (!s_Initialized)
            return;

        if (s_Backend)
        {
            s_Backend->Shutdown();
            s_Backend.reset();
        }

        s_Capabilities = {};
        s_ActiveBackend = RendererBackend::Headless;
        RebuildBackendOptions();

        Log::Info("Renderer shutdown");
        s_Initialized = false;
    }

    void Renderer::BeginFrame()
    {
        if (!s_Initialized || !s_Backend)
            return;

        BeginTimingFrame();
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

    void Renderer::SetCameraView(const CameraView& cameraView)
    {
        if (cameraView.Valid)
            s_CameraView = cameraView;
    }

    const CameraView& Renderer::GetCameraView()
    {
        if (!s_CameraView.Valid)
            s_CameraView = EditorCamera().GetCameraView();

        return s_CameraView;
    }
}
