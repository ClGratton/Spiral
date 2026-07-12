#include "Engine/Renderer/NVRHI/NVRHIRenderBackend.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Application.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"

namespace Engine
{
    const char* NVRHIRenderBackend::GetName() const
    {
        switch (m_RendererBackend)
        {
            case RendererBackend::NVRHID3D12: return "NVRHI D3D12";
            case RendererBackend::NVRHIVulkan: return "NVRHI Vulkan";
            case RendererBackend::NVRHICommon: return "NVRHI Common";
            default: return "NVRHI";
        }
    }

    bool NVRHIRenderBackend::Initialize()
    {
        m_AdapterInfo = RHI::QueryNVRHIAdapter();
        if (!m_AdapterInfo.Available)
            return false;

        RHI::DeviceDescription description {};
        description.RequestedBackend = RHI::Backend::NVRHID3D12;
#if defined(GE_DEBUG)
        description.EnableValidation = true;
#else
        description.EnableValidation = false;
#endif

        if (m_RequestedBackend == RHI::Backend::NVRHIVulkan)
        {
            m_VulkanContext = CreateScope<RHI::NVRHIVulkanContext>();
            if (!m_VulkanContext->Initialize(
                    Application::Get().GetWindow().GetNativeWindow(),
                    description.EnableValidation,
                    m_AdapterInfo))
            {
                m_VulkanContext.reset();
                return false;
            }

            m_RendererBackend = RendererBackend::NVRHIVulkan;
            return true;
        }

        m_Device = RHI::CreateNVRHID3D12Device(std::move(description), m_AdapterInfo, &m_D3D12NativeHandles);
        if (m_Device)
        {
            RHI::BufferDescription copyQueueProbeDescription;
            copyQueueProbeDescription.DebugName = "Renderer Startup Copy Queue Upload Probe";
            copyQueueProbeDescription.SizeBytes = sizeof(u32);
            copyQueueProbeDescription.Usage = RHI::BufferUsage::CopyDest;
            Scope<RHI::Buffer> copyQueueProbe = m_Device->CreateBuffer(copyQueueProbeDescription);
            const u32 copyQueueProbeValue = 0x53504952u;
            if (!copyQueueProbe || !m_Device->UploadBuffer(*copyQueueProbe, &copyQueueProbeValue, sizeof(copyQueueProbeValue)))
            {
                Log::Error("Could not create and submit the D3D12 copy-queue upload probe");
                m_Device.reset();
                return false;
            }

            m_RendererBackend = RendererBackend::NVRHID3D12;
            return true;
        }

        Log::Warn("NVRHI D3D12 device is unavailable; using NVRHI common probe backend");
        m_RendererBackend = RendererBackend::NVRHICommon;
        return m_AdapterInfo.Available;
    }

    void NVRHIRenderBackend::Shutdown()
    {
        ShutdownImGui();

        if (m_VulkanContext)
        {
            m_VulkanContext->WaitIdle();
            m_VulkanContext->Shutdown();
            m_VulkanContext.reset();
        }

        if (m_Device)
        {
            m_Device->WaitIdle();
            m_Device.reset();
        }

        m_AdapterInfo = {};
        m_RendererBackend = RendererBackend::NVRHICommon;
        m_RequestedBackend = RHI::Backend::None;
    }

    void NVRHIRenderBackend::BeginFrame(const ClearColor& clearColor)
    {
        (void)clearColor;
        if (m_D3D12Presentation)
            m_D3D12Presentation->WaitForFrameLatency();
    }

    void NVRHIRenderBackend::EndFrame()
    {
    }

    const RHI::DeviceCapabilities* NVRHIRenderBackend::GetDeviceCapabilities() const
    {
        if (m_Device)
            return &m_Device->GetCapabilities();
        return m_VulkanContext ? &m_VulkanContext->GetCapabilities() : nullptr;
    }

    const RendererPresentationTiming* NVRHIRenderBackend::GetPresentationTiming() const
    {
        if (m_D3D12Presentation)
            return &m_D3D12Presentation->GetTiming();
        return m_VulkanPresentation ? &m_VulkanPresentation->GetTiming() : nullptr;
    }

    bool NVRHIRenderBackend::InitializeImGui(void* nativeWindow, u32 width, u32 height)
    {
        if (m_RendererBackend == RendererBackend::NVRHIVulkan && m_VulkanContext)
        {
            if (!m_VulkanPresentation)
                m_VulkanPresentation = CreateScope<NVRHIVulkanPresentation>();
            return m_VulkanPresentation->Initialize(m_VulkanContext.get(), nativeWindow, width, height);
        }

        if (m_RendererBackend != RendererBackend::NVRHID3D12 || !m_Device)
            return false;

        if (!m_D3D12Presentation)
            m_D3D12Presentation = CreateScope<NVRHID3D12Presentation>();

        return m_D3D12Presentation->Initialize(nativeWindow, m_Device.get(), m_D3D12NativeHandles, width, height);
    }

    void NVRHIRenderBackend::ShutdownImGui()
    {
        if (m_D3D12Presentation)
        {
            m_D3D12Presentation->Shutdown();
            m_D3D12Presentation.reset();
        }
        if (m_VulkanPresentation)
        {
            m_VulkanPresentation->Shutdown();
            m_VulkanPresentation.reset();
        }
    }

    bool NVRHIRenderBackend::IsNativeImGuiEnabled() const
    {
        return (m_D3D12Presentation && m_D3D12Presentation->IsInitialized())
            || (m_VulkanPresentation && m_VulkanPresentation->IsInitialized());
    }

    void NVRHIRenderBackend::BeginImGuiFrame()
    {
        if (m_D3D12Presentation)
            m_D3D12Presentation->BeginImGuiFrame();
        else if (m_VulkanPresentation)
            m_VulkanPresentation->BeginImGuiFrame();
    }

    void NVRHIRenderBackend::RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height)
    {
        if (m_D3D12Presentation)
            m_D3D12Presentation->RenderImGuiDrawData(drawData, clearColor, width, height);
        else if (m_VulkanPresentation)
            m_VulkanPresentation->RenderImGuiDrawData(drawData, clearColor, width, height);
    }

    bool NVRHIRenderBackend::PrepareViewportTexture(u32 width, u32 height)
    {
        return m_D3D12Presentation && m_D3D12Presentation->PrepareViewportTexture(width, height);
    }

    u64 NVRHIRenderBackend::GetViewportTextureId() const
    {
        return m_D3D12Presentation ? m_D3D12Presentation->GetViewportTextureId() : 0;
    }

    bool NVRHIRenderBackend::CaptureViewportToFile(std::string_view path)
    {
        return m_D3D12Presentation && m_D3D12Presentation->CaptureViewportToFile(path);
    }
}
