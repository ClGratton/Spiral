#pragma once

#include "Engine/RHI/Device.h"
#include "Engine/RHI/NVRHI/NVRHIAdapter.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanContext.h"
#include "Engine/Renderer/NVRHI/NVRHID3D12Presentation.h"
#include "Engine/Renderer/NVRHI/NVRHIVulkanPresentation.h"
#include "Engine/Renderer/NVRHI/NVRHIVulkanViewportSceneRenderer.h"
#include "Engine/Renderer/RenderBackend.h"

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
        bool PrepareViewportTexture(u32 width, u32 height);
        u64 GetViewportTextureId() const;
        void MarkViewportTextureQueued(u64 textureId);
        bool CaptureViewportToFile(std::string_view path);
        bool RunVulkanRHICoreSmoke();
        bool RunRHIBufferTransitionSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHICompletionSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIQueueDependencySmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIBufferOwnershipSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIResourceOwnershipSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHIResourceStateSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRHITextureReadbackSmoke(RHI::Device& device, std::string_view backendName);
        bool RunRenderGraphExecutionSmoke(RHI::Device& device, std::string_view backendName);
        bool RunVulkanRHIIndexedDrawSmoke();
        bool RunVulkanSceneViewportRasterSmoke();

        const RHI::NVRHIAdapterInfo& GetAdapterInfo() const { return m_AdapterInfo; }
        RendererBackend GetRendererBackend() const { return m_RendererBackend; }
        const RHI::DeviceCapabilities* GetDeviceCapabilities() const;
        const RendererPresentationTiming* GetPresentationTiming() const;

    private:
        RHI::NVRHIAdapterInfo m_AdapterInfo;
        RHI::NVRHID3D12NativeHandles m_D3D12NativeHandles;
        Scope<RHI::Device> m_Device;
        Scope<NVRHID3D12Presentation> m_D3D12Presentation;
        Scope<RHI::NVRHIVulkanContext> m_VulkanContext;
        Scope<NVRHIVulkanPresentation> m_VulkanPresentation;
        Scope<NVRHIVulkanViewportSceneRenderer> m_VulkanSceneRenderer;
        u64 m_VulkanOutputCaptureGeneration = 0;
        RHI::Backend m_RequestedBackend = RHI::Backend::None;
        RendererBackend m_RendererBackend = RendererBackend::NVRHICommon;
    };
}
