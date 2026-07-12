#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/RHICommon.h"
#include "Engine/Scene/Camera.h"

#include <string>
#include <string_view>
#include <vector>

struct ImDrawData;

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
    };

    struct RendererFrameTiming
    {
        u64 FrameIndex = 0;
        double CpuMilliseconds = 0.0;
        double GpuMilliseconds = 0.0;
        RendererTimingStatus GpuStatus = RendererTimingStatus::Unavailable;
        RendererPresentationTiming Presentation;
        std::vector<RendererPassTiming> Passes;
    };

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
        static void BeginFrame();
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
        static bool CaptureViewportToFile(std::string_view path);
        static RendererBackend GetActiveBackend();
        static const char* GetActiveBackendName();
        static const std::vector<RendererBackendOption>& GetBackendOptions();
        static bool RequestBackend(RendererBackend backend);
        static const RendererCapabilities& GetCapabilities();
        static const RendererBuildInfo& GetBuildInfo();
        static const RendererFrameTiming& GetLastFrameTiming();
        static void SetCameraView(const CameraView& cameraView);
        static const CameraView& GetCameraView();
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
