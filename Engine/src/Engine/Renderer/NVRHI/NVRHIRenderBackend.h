#pragma once

#include "Engine/RHI/Device.h"
#include "Engine/RHI/NVRHI/NVRHIAdapter.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanContext.h"
#include "Engine/Renderer/NVRHI/NVRHID3D12Presentation.h"
#include "Engine/Renderer/NVRHI/NVRHIVulkanPresentation.h"
#include "Engine/Renderer/NVRHI/NVRHIVulkanViewportSceneRenderer.h"
#include "Engine/Renderer/RenderBackend.h"
#include "Engine/Renderer/UiTextureService.h"

#include <string>
#include <vector>

struct ImDrawData;

namespace Engine
{
    class NVRHIRenderBackend final : public RenderBackend
    {
    public:
        const char* GetName() const override;
        bool Initialize() override;
        void SetRequestedBackend(RHI::Backend backend) { m_RequestedBackend = backend; }
        void Shutdown() override;
        void BeginFrame(const ClearColor& clearColor) override;
        void EndFrame() override;
        bool InitializeImGui(void* nativeWindow, u32 width, u32 height);
        void ShutdownImGui();
        bool IsNativeImGuiEnabled() const;
        void BeginImGuiFrame();
        void RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height);
        // Detachable OS-window panels (Vulkan only today; see UiViewportPolicy.h).
        UiViewportRendererKind GetUiViewportRendererKind() const;
        const std::vector<int>& GetUiViewportSurfacePresentModes() const;
        bool EnableUiViewports();
        void RenderUiPlatformWindows();
        UiViewportDiagnostics GetUiViewportDiagnostics() const;
        bool CaptureUiDrawDataOffscreen(ImDrawData* drawData, u32 width, u32 height, std::vector<u8>& outRgba);
        bool PrepareViewportTexture(u32 width, u32 height);
        u64 GetViewportTextureId() const;
        void MarkViewportTextureQueued(u64 textureId);
        bool CaptureViewportToFile(std::string_view path);

        // Renderer UI-texture service (Renderer::CreateUiTexture and friends).
        // Present only while native ImGui is enabled. Main thread only.
        UiTextureHandle CreateUiTexture(u32 width, u32 height, std::string_view debugName);
        bool UpdateUiTexture(UiTextureHandle handle, const UiTextureUpdate& update);
        bool ResizeUiTexture(UiTextureHandle handle, u32 width, u32 height);
        bool DestroyUiTexture(UiTextureHandle handle);
        u64 GetUiTextureImGuiId(UiTextureHandle handle) const;
        bool GetUiTextureExtent(UiTextureHandle handle, u32& outWidth, u32& outHeight) const;
        UiTextureCounters GetUiTextureCounters() const;
        UiTextureError GetLastUiTextureError() const;
        bool RunVulkanRHICoreSmoke();
        bool RunRHIBufferTransitionSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHICompletionSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHITimestampQuerySmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIQueueDependencySmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIBufferOwnershipSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHITextureOwnershipSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIResourceOwnershipSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIResourceStateSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHITextureReadbackSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHITextureUploadSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHITextureUpdateSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHISampledTextureTableSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIFixedStructuredBufferSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIMaterialTextureShaderSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRenderGraphExecutionSmoke(RHI::Device& device, std::string_view backendName);
        bool RunVulkanRHIIndexedDrawSmoke();
        bool RunVulkanSceneViewportRasterSmoke();

        const RHI::NVRHIAdapterInfo& GetAdapterInfo() const { return m_AdapterInfo; }
        RendererBackend GetRendererBackend() const { return m_RendererBackend; }
        const RHI::DeviceCapabilities* GetDeviceCapabilities() const;
        const RendererPresentationTiming* GetPresentationTiming() const;
        void SetPresentationPolicy(PresentationPolicy policy);
        const RendererPresentationPolicyDiagnostics* GetPresentationPolicyDiagnostics() const;

    private:
        enum class UiTextureSmokeStage : u8
        {
            Disabled,
            Pending,
            AwaitingRetirement,
            Finished
        };

        // Evidence gathered by the offscreen ImGui captures of --ui-texture-smoke.
        struct UiTextureSmokeEvidence
        {
            u32 Captures = 0;
            u64 PixelsCompared = 0;
            u64 Mismatches = 0;
            u32 MaximumChannelDelta = 0;
            bool IdChangedOnResize = false;
        };

        void InitializeUiTextureService(RHI::Device& device, Scope<UiTextureNativeBridge> bridge, std::string_view backendName);
        void ShutdownUiTextureService();
        void CollectUiTextureRetirements();
        void RunUiTextureSmoke();
        void FinishUiTextureSmokeIfRetired();

        RHI::NVRHIAdapterInfo m_AdapterInfo;
        RHI::NVRHID3D12NativeHandles m_D3D12NativeHandles;
        Scope<RHI::Device> m_Device;
        Scope<NVRHID3D12Presentation> m_D3D12Presentation;
        Scope<RHI::NVRHIVulkanContext> m_VulkanContext;
        Scope<NVRHIVulkanPresentation> m_VulkanPresentation;
        Scope<NVRHIVulkanViewportSceneRenderer> m_VulkanSceneRenderer;
        u64 m_VulkanOutputCaptureGeneration = 0;
        // The bridge must outlive the service that calls into it.
        Scope<UiTextureNativeBridge> m_UiTextureBridge;
        Scope<UiTextureService> m_UiTextureService;
        bool m_ImGuiFrameOpen = false;
        std::string m_UiTextureBackendName;
        UiTextureSmokeStage m_UiTextureSmokeStage = UiTextureSmokeStage::Disabled;
        UiTextureSmokeEvidence m_UiTextureSmokeEvidence;
        RHI::Backend m_RequestedBackend = RHI::Backend::None;
        RendererBackend m_RendererBackend = RendererBackend::NVRHICommon;
    };
}
