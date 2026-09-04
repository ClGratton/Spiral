#include "Engine/Renderer/NVRHI/NVRHIRenderBackend.h"

#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"
#include "Engine/Renderer/TextureGpuResourceCache.h"
#include "Engine/Renderer/TextureRuntimePublication.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SceneDebugVisualization.h"
#include "Engine/Renderer/SceneSurfaceConstants.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Application.h"
#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/RHI/TextureBindingTable.h"
#include "Engine/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>

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
        const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
        description.PreferredAdapterName = args.GetOptionValue("--renderer-adapter");
        description.RequirePreferredAdapter = args.HasFlag("--renderer-adapter-strict");
        description.ForceGraphicsQueueFallback = args.HasFlag("--rhi-force-graphics-queue-fallback");
        if (description.RequirePreferredAdapter && description.PreferredAdapterName.empty())
        {
            Log::Error("--renderer-adapter-strict requires --renderer-adapter=<exact adapter name>");
            return false;
        }

        if (m_RequestedBackend == RHI::Backend::NVRHIVulkan)
        {
            m_VulkanContext = CreateScope<RHI::NVRHIVulkanContext>();
            if (!m_VulkanContext->Initialize(
                    Application::Get().GetWindow().GetNativeWindow(),
                    description,
                    m_AdapterInfo))
            {
                m_VulkanContext.reset();
                return false;
            }

            m_RendererBackend = RendererBackend::NVRHIVulkan;
            if (args.HasFlag("--vulkan-rhi-core-smoke") && !RunVulkanRHICoreSmoke())
            {
                Log::Error("Vulkan RHI core smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--rhi-buffer-transition-smoke")
                && !RunRHIBufferTransitionSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan RHI buffer-transition smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--rhi-completion-smoke")
                && !RunRHICompletionSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan RHI completion smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--rhi-timestamp-query-smoke")
                && !RunRHITimestampQuerySmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan RHI timestamp-query smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--rhi-queue-dependency-smoke")
                && !RunRHIQueueDependencySmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan RHI queue-dependency smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--rhi-buffer-ownership-smoke")
                && !RunRHIBufferOwnershipSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan RHI buffer-ownership smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--rhi-texture-ownership-smoke") && !RunRHITextureOwnershipSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            { Log::Error("Vulkan RHI texture-ownership smoke failed"); m_VulkanContext->Shutdown(); m_VulkanContext.reset(); return false; }
            if (args.HasFlag("--rhi-resource-ownership-smoke")
                && !RunRHIResourceOwnershipSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan RHI resource-ownership smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--rhi-resource-state-smoke")
                && !RunRHIResourceStateSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan RHI resource-state smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--rhi-texture-upload-smoke") && !RunRHITextureUploadSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan RHI texture-upload smoke failed"); m_VulkanContext->Shutdown(); m_VulkanContext.reset(); return false;
            }
            if (args.HasFlag("--rhi-sampled-table-smoke")
                && (!RunRHISampledTextureTableSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan")
                    || !RunRHIMaterialTextureShaderSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan")))
            { Log::Error("Vulkan RHI sampled-table smoke failed"); m_VulkanContext->Shutdown(); m_VulkanContext.reset(); return false; }
            if (args.HasFlag("--rhi-fixed-structured-buffer-smoke")
                && !RunRHIFixedStructuredBufferSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            { Log::Error("Vulkan RHI fixed structured-buffer smoke failed"); m_VulkanContext->Shutdown(); m_VulkanContext.reset(); return false; }
            if (args.HasFlag("--render-graph-execution-smoke") && !RunRenderGraphExecutionSmoke(*m_VulkanContext->GetRHIDevice(), "Vulkan"))
            {
                Log::Error("Vulkan render-graph execution smoke failed"); m_VulkanContext->Shutdown(); m_VulkanContext.reset(); return false;
            }
            if (args.HasFlag("--vulkan-rhi-indexed-draw-smoke") && !RunVulkanRHIIndexedDrawSmoke())
            {
                Log::Error("Vulkan RHI indexed draw smoke failed");
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            if (args.HasFlag("--vulkan-scene-viewport-raster-smoke") && !RunVulkanSceneViewportRasterSmoke())
            {
                Log::Error("Vulkan Scene viewport raster smoke failed");
                m_VulkanSceneRenderer.reset();
                m_VulkanContext->Shutdown();
                m_VulkanContext.reset();
                return false;
            }
            return true;
        }

        const bool requirePreferredAdapter = description.RequirePreferredAdapter;
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
            if (args.HasFlag("--rhi-buffer-transition-smoke") && !RunRHIBufferTransitionSmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI buffer-transition smoke failed");
                m_Device.reset();
                return false;
            }
            if (args.HasFlag("--rhi-completion-smoke") && !RunRHICompletionSmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI completion smoke failed");
                m_Device.reset();
                return false;
            }
            if (args.HasFlag("--rhi-timestamp-query-smoke") && !RunRHITimestampQuerySmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI timestamp-query smoke failed");
                m_Device.reset();
                return false;
            }
            if (args.HasFlag("--rhi-queue-dependency-smoke") && !RunRHIQueueDependencySmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI queue-dependency smoke failed");
                m_Device.reset();
                return false;
            }
            if (args.HasFlag("--rhi-buffer-ownership-smoke") && !RunRHIBufferOwnershipSmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI buffer-ownership smoke failed");
                m_Device.reset();
                return false;
            }
            if (args.HasFlag("--rhi-texture-ownership-smoke") && !RunRHITextureOwnershipSmoke(*m_Device, "D3D12"))
            { Log::Error("D3D12 RHI texture-ownership smoke failed"); m_Device.reset(); return false; }
            if (args.HasFlag("--rhi-resource-ownership-smoke") && !RunRHIResourceOwnershipSmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI resource-ownership smoke failed");
                m_Device.reset();
                return false;
            }
            if (args.HasFlag("--rhi-resource-state-smoke") && !RunRHIResourceStateSmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI resource-state smoke failed");
                m_Device.reset();
                return false;
            }
            if (args.HasFlag("--rhi-texture-readback-smoke") && !RunRHITextureReadbackSmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI texture-readback smoke failed");
                m_Device.reset();
                return false;
            }
            if (args.HasFlag("--rhi-texture-upload-smoke") && !RunRHITextureUploadSmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 RHI texture-upload smoke failed"); m_Device.reset(); return false;
            }
            if (args.HasFlag("--rhi-sampled-table-smoke")
                && (!RunRHISampledTextureTableSmoke(*m_Device, "D3D12")
                    || !RunRHIMaterialTextureShaderSmoke(*m_Device, "D3D12")))
            { Log::Error("D3D12 RHI sampled-table smoke failed"); m_Device.reset(); return false; }
            if (args.HasFlag("--rhi-fixed-structured-buffer-smoke")
                && !RunRHIFixedStructuredBufferSmoke(*m_Device, "D3D12"))
            { Log::Error("D3D12 RHI fixed structured-buffer smoke failed"); m_Device.reset(); return false; }
            if (args.HasFlag("--render-graph-execution-smoke") && !RunRenderGraphExecutionSmoke(*m_Device, "D3D12"))
            {
                Log::Error("D3D12 render-graph execution smoke failed"); return false;
            }
            return true;
        }

        if (requirePreferredAdapter)
            return false;

        Log::Warn("NVRHI D3D12 device is unavailable; using NVRHI common probe backend");
        m_RendererBackend = RendererBackend::NVRHICommon;
        return m_AdapterInfo.Available;
    }

    void NVRHIRenderBackend::Shutdown()
    {
        ShutdownImGui();

        if (m_VulkanContext)
        {
            m_VulkanSceneRenderer.reset();
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
        {
            // The replacement waitable object must be installed before this
            // frame's mandatory latency wait, never halfway through render.
            if (!m_D3D12Presentation->ApplyPendingPresentationPolicy())
                return;
            m_D3D12Presentation->WaitForFrameLatency();
        }
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

    void NVRHIRenderBackend::SetPresentationPolicy(PresentationPolicy policy)
    {
        if (m_D3D12Presentation)
            m_D3D12Presentation->SetPresentationPolicy(policy);
        if (m_VulkanPresentation)
            m_VulkanPresentation->SetPresentationPolicy(policy);
    }

    const RendererPresentationPolicyDiagnostics* NVRHIRenderBackend::GetPresentationPolicyDiagnostics() const
    {
        if (m_D3D12Presentation)
            return &m_D3D12Presentation->GetPresentationPolicyDiagnostics();
        return m_VulkanPresentation ? &m_VulkanPresentation->GetPresentationPolicyDiagnostics() : nullptr;
    }

    bool NVRHIRenderBackend::InitializeImGui(void* nativeWindow, u32 width, u32 height)
    {
        if (m_RendererBackend == RendererBackend::NVRHIVulkan && m_VulkanContext)
        {
            if (!m_VulkanPresentation)
                m_VulkanPresentation = CreateScope<NVRHIVulkanPresentation>();
            m_VulkanPresentation->SetPresentationPolicy(Renderer::GetPresentationPolicy());
            return m_VulkanPresentation->Initialize(m_VulkanContext.get(), nativeWindow, width, height);
        }

        if (m_RendererBackend != RendererBackend::NVRHID3D12 || !m_Device)
            return false;

        if (!m_D3D12Presentation)
            m_D3D12Presentation = CreateScope<NVRHID3D12Presentation>();
        m_D3D12Presentation->SetPresentationPolicy(Renderer::GetPresentationPolicy());

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
        if (m_D3D12Presentation)
            return m_D3D12Presentation->PrepareViewportTexture(width, height);
        if (!m_VulkanPresentation || !m_VulkanContext || width == 0 || height == 0)
            return false;

        if (!m_VulkanSceneRenderer)
        {
            m_VulkanSceneRenderer = CreateScope<NVRHIVulkanViewportSceneRenderer>();
            if (!m_VulkanSceneRenderer->Initialize(m_VulkanContext->GetRHIDevice()))
            {
                m_VulkanSceneRenderer.reset();
                Log::Error("Could not initialize Vulkan Scene viewport renderer for ImGui handoff");
                return false;
            }
        }

        const bool replacingOutput = m_VulkanSceneRenderer->GetOutputGeneration() == 0
            || m_VulkanSceneRenderer->GetOutputWidth() != width
            || m_VulkanSceneRenderer->GetOutputHeight() != height;
        if (replacingOutput)
        {
            // The descriptor borrows the NVRHI image view. Retire all prior
            // ImGui use before removing it and replacing the renderer target.
            m_VulkanContext->WaitIdle();
            m_VulkanPresentation->ReleaseViewportOutput();
        }
        if (!m_VulkanSceneRenderer->RenderCurrentSnapshot(width, height, Renderer::GetClearColor()))
            return false;
        const u64 outputGeneration = m_VulkanSceneRenderer->GetOutputGeneration();
        if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--vulkan-render-smoke")
            && m_VulkanOutputCaptureGeneration != outputGeneration)
        {
            RHI::TextureReadback readback;
            const bool captured = m_VulkanSceneRenderer->ReadbackColor(readback);
            const bool dimensions = captured && readback.Extent.Width == width && readback.Extent.Height == height
                && readback.RowPitchBytes >= width * 4
                && readback.Data.size() >= static_cast<size_t>(readback.RowPitchBytes) * height;
            u32 nonBackground = 0;
            for (u32 y = 0; dimensions && y < height; ++y)
                for (u32 x = 0; x < width; ++x)
                {
                    const u8* pixel = readback.Data.data() + static_cast<size_t>(y) * readback.RowPitchBytes + static_cast<size_t>(x) * 4;
                    nonBackground += pixel[0] != 10 || pixel[1] != 13 || pixel[2] != 15 ? 1u : 0u;
                }
            const bool content = dimensions && nonBackground > 0;
            Log::Info("VulkanSceneOutputCaptureV1 outputGeneration=", outputGeneration,
                " capture=", content ? "pass" : "fail", " size=", readback.Extent.Width, "x", readback.Extent.Height,
                " foregroundPixels=", nonBackground);
            if (!content)
                return false;
            m_VulkanOutputCaptureGeneration = outputGeneration;
        }
        return m_VulkanPresentation->RegisterViewportOutput(
            m_VulkanSceneRenderer->GetOutputNativeHandles(), outputGeneration);
    }

    u64 NVRHIRenderBackend::GetViewportTextureId() const
    {
        return m_D3D12Presentation ? m_D3D12Presentation->GetViewportTextureId()
            : (m_VulkanPresentation ? m_VulkanPresentation->GetViewportTextureId() : 0);
    }

    void NVRHIRenderBackend::MarkViewportTextureQueued(u64 textureId)
    {
        if (m_VulkanPresentation)
            m_VulkanPresentation->MarkViewportTextureQueued(textureId);
    }

    bool NVRHIRenderBackend::CaptureViewportToFile(std::string_view path)
    {
        return m_D3D12Presentation && m_D3D12Presentation->CaptureViewportToFile(path);
    }

    bool NVRHIRenderBackend::RunVulkanRHICoreSmoke()
    {
        RHI::Device* device = m_VulkanContext ? m_VulkanContext->GetRHIDevice() : nullptr;
        constexpr u32 width = 16;
        constexpr u32 height = 12;
        if (!device)
            return false;

        RHI::BufferDescription bufferDescription;
        bufferDescription.DebugName = "VulkanRHICoreV1 Upload";
        bufferDescription.SizeBytes = sizeof(u32);
        bufferDescription.Usage = RHI::BufferUsage::CopyDest;
        Scope<RHI::Buffer> uploadBuffer = device->CreateBuffer(bufferDescription);
        RHI::BufferDescription noCpuMapBufferDescription;
        noCpuMapBufferDescription.DebugName = "VulkanRHICoreV1 No CPU Map";
        noCpuMapBufferDescription.SizeBytes = sizeof(u32);
        noCpuMapBufferDescription.Usage = RHI::BufferUsage::CopyDest;
        noCpuMapBufferDescription.CpuAccess = RHI::BufferCpuAccess::None;
        Scope<RHI::Buffer> noCpuMapBuffer = device->CreateBuffer(noCpuMapBufferDescription);
        const bool rejectedNoCpuMap = noCpuMapBuffer && !noCpuMapBuffer->Map();
        const u32 uploadValue = 0x564B5248u;
        const bool bufferUpload = uploadBuffer
            && device->UploadBuffer(*uploadBuffer, &uploadValue, sizeof(uploadValue));
        RHI::TextureDescription colorDescription;
        colorDescription.DebugName = "VulkanRHICoreV1 Color";
        colorDescription.Extent = { width, height };
        colorDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        colorDescription.Usage = static_cast<RHI::TextureUsage>(
            static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource));
        Scope<RHI::Texture> color = device->CreateTexture(colorDescription);
        RHI::TextureDescription depthDescription;
        depthDescription.DebugName = "VulkanRHICoreV1 Depth";
        depthDescription.Extent = { width, height };
        depthDescription.TextureFormat = RHI::Format::D32Float;
        depthDescription.Usage = RHI::TextureUsage::DepthStencil;
        Scope<RHI::Texture> depth = device->CreateTexture(depthDescription);
        Scope<RHI::CommandList> list = device->CreateCommandList(RHI::QueueType::Graphics, "VulkanRHICoreV1 Clear");
        RHI::ViewportClear clear;
        clear.Color[0] = 0.25f;
        clear.Color[1] = 0.5f;
        clear.Color[2] = 0.75f;
        clear.Color[3] = 1.0f;
        const bool opened = bufferUpload && color && depth && list && list->Begin();
        const bool rejectedOpenSubmission = opened && !device->SubmitAndWait(*list);
        if (opened)
            list->BeginDebugMarker(std::string_view("VulkanRHICoreV1 Marker", sizeof("VulkanRHICoreV1 Marker") - 1));
        const bool rejectedUnbalancedMarkerEnd = opened && !list->End();
        if (opened)
            list->EndDebugMarker();
        const bool submitted = rejectedNoCpuMap && rejectedOpenSubmission && rejectedUnbalancedMarkerEnd
            && list->BindViewportOutputs(*color, depth.get())
            && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget)
            && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite)
            && list->ClearViewportOutputs(clear)
            && list->TransitionTexture(*color, RHI::ResourceState::CopySource)
            && list->End() && device->SubmitAndWait(*list);
        const bool rejectedDuplicateSubmission = submitted && !device->SubmitAndWait(*list);
        RHI::TextureReadback readback;
        const bool readbackOk = submitted && device->ReadbackTexture(*color, readback);
        bool pixelsOk = readbackOk && readback.Extent.Width == width && readback.Extent.Height == height
            && readback.RowPitchBytes >= width * 4 && readback.Data.size() >= static_cast<size_t>(readback.RowPitchBytes) * height;
        const u8 expected[] { 64u, 128u, 191u, 255u };
        for (u32 y = 0; y < height && pixelsOk; ++y)
            for (u32 x = 0; x < width && pixelsOk; ++x)
                for (u32 channel = 0; channel < 4; ++channel)
                    if (std::abs(static_cast<int>(readback.Data[y * readback.RowPitchBytes + x * 4 + channel]) - expected[channel]) > 1)
                        pixelsOk = false;
        const RHI::DeviceCapabilities& capabilities = device->GetCapabilities();
        Log::Info("VulkanRHICoreV1 adapter=", capabilities.Identity.Name,
            ", deviceClass=", RHI::ToString(capabilities.Identity.Type), ", size=", width, "x", height,
            ", bufferUpload=", bufferUpload ? "pass" : "fail", ", clear=", submitted ? "pass" : "fail", ", readback=", readbackOk ? "pass" : "fail",
            ", pixels=", pixelsOk ? "pass" : "fail", ", nvrhiSubmission=", submitted ? "pass" : "fail",
            ", lifecycle=", (rejectedOpenSubmission && rejectedDuplicateSubmission) ? "pass" : "fail", ", cpuMapNone=", rejectedNoCpuMap ? "pass" : "fail",
            ", markers=", (opened && rejectedUnbalancedMarkerEnd) ? "executed-balanced" : "not-executed");
        return pixelsOk && rejectedDuplicateSubmission;
    }

    bool NVRHIRenderBackend::RunRHIBufferTransitionSmoke(RHI::Device& device, std::string_view backendName)
    {
        RHI::BufferDescription description;
        description.DebugName = "RHIBufferTransitionSmokeV1";
        description.SizeBytes = sizeof(u32);
        description.Usage = static_cast<RHI::BufferUsage>(
            static_cast<u32>(RHI::BufferUsage::CopySource) | static_cast<u32>(RHI::BufferUsage::CopyDest));
        Scope<RHI::Buffer> buffer = device.CreateBuffer(description);
        Scope<RHI::CommandList> list = buffer ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIBufferTransitionSmokeV1") : nullptr;
        const bool rejectedOutsideRecording = list && !list->TransitionBuffer(*buffer, RHI::ResourceState::CopyDest);
        const bool recording = list && list->Begin();
        const bool rejectedInvalidState = recording && !list->TransitionBuffer(*buffer, RHI::ResourceState::RenderTarget);
        const bool transitions = recording
            && list->TransitionBuffer(*buffer, RHI::ResourceState::CopyDest)
            && list->TransitionBuffer(*buffer, RHI::ResourceState::CopySource)
            && list->TransitionBuffer(*buffer, RHI::ResourceState::CopySource);
        const bool closed = transitions && list->End();
        const bool rejectedAfterClose = closed && !list->TransitionBuffer(*buffer, RHI::ResourceState::CopyDest);
        const bool submitted = closed && device.SubmitAndWait(*list);
        const bool passed = rejectedOutsideRecording && rejectedInvalidState && rejectedAfterClose && submitted;
        Log::Info("RHIBufferTransitionSmokeV1 backend=", backendName,
            ", invalid=", rejectedInvalidState ? "rejected" : "accepted",
            ", lifecycle=", (rejectedOutsideRecording && rejectedAfterClose) ? "pass" : "fail",
            ", submission=", submitted ? "pass" : "fail",
            ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHICompletionSmoke(RHI::Device& device, std::string_view backendName)
    {
        Scope<RHI::CommandList> list = device.CreateCommandList(RHI::QueueType::Graphics, "RHICompletionSmokeV1");
        const bool closed = list && list->Begin() && list->End();
        const RHI::CompletionToken token = closed ? device.Submit(*list) : RHI::CompletionToken {};
        const RHI::CompletionStatus initial = token.IsValid() ? device.QueryCompletion(token) : RHI::CompletionStatus::Invalid;
        const RHI::CompletionToken crossDevice { token.DeviceId + 1, token.SubmissionId };
        const RHI::CompletionToken stale { token.DeviceId, token.SubmissionId + 1 };
        const bool invalidRejected = device.QueryCompletion({}) == RHI::CompletionStatus::Invalid;
        const bool crossDeviceRejected = token.IsValid() && device.QueryCompletion(crossDevice) == RHI::CompletionStatus::Invalid;
        const bool staleRejected = token.IsValid() && device.QueryCompletion(stale) == RHI::CompletionStatus::Invalid;
        const bool initialValid = initial == RHI::CompletionStatus::Incomplete || initial == RHI::CompletionStatus::Complete;
        const bool waitCompleted = token.IsValid() && device.WaitForCompletion(token, 5000);
        const bool finalComplete = waitCompleted && device.QueryCompletion(token) == RHI::CompletionStatus::Complete;
        const bool reused = finalComplete && list->Begin() && list->End();
        const RHI::CompletionToken reuseToken = reused ? device.Submit(*list) : RHI::CompletionToken {};
        const bool reuseRetired = reuseToken.IsValid() && device.WaitForCompletion(reuseToken, 5000);
        const bool passed = invalidRejected && crossDeviceRejected && staleRejected && initialValid
            && finalComplete && reuseRetired;
        Log::Info("RHICompletionSmokeV1 backend=", backendName,
            ", tokenValidation=", (invalidRejected && crossDeviceRejected && staleRejected) ? "pass" : "fail",
            ", query=nonblocking-", initial == RHI::CompletionStatus::Incomplete ? "incomplete" : (initial == RHI::CompletionStatus::Complete ? "complete" : "failed"),
            ", wait=", finalComplete ? "pass" : "fail",
            ", reuse=", reuseRetired ? "pass" : "fail",
            ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHITimestampQuerySmoke(RHI::Device& device, std::string_view backendName)
    {
        RHI::QueryPoolDescription description;
        description.DebugName = "RHITimestampQuerySmokeV1";
        description.Type = RHI::QueryType::Timestamp;
        description.Count = 2;
        Scope<RHI::QueryPool> pool = device.CreateQueryPool(description);
        const double periodNanoseconds = pool ? pool->GetTimestampPeriodNanoseconds() : 0.0;
        const bool allocated = pool && device.OwnsQueryPool(pool.get()) && periodNanoseconds > 0.0;

        Scope<RHI::CommandList> firstList = allocated
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITimestampQuerySmokeV1 First") : nullptr;
        const bool firstRecorded = firstList && firstList->Begin() && firstList->ResetQueryPool(*pool, 0, 2)
            && firstList->WriteTimestamp(*pool, 0) && firstList->WriteTimestamp(*pool, 1)
            && firstList->ResolveQueryPool(*pool, 0, 2) && firstList->End();
        const RHI::CompletionToken firstToken = firstRecorded ? device.Submit(*firstList) : RHI::CompletionToken {};
        const RHI::QueryResult firstPending = firstToken.IsValid() ? pool->ReadResult(0) : RHI::QueryResult {};
        const bool pending = firstPending.Status == RHI::QueryResultStatus::Pending;
        const bool firstRetired = firstToken.IsValid() && device.WaitForCompletion(firstToken, 5000);
        const RHI::QueryResult firstBegin = pool ? pool->ReadResult(0) : RHI::QueryResult {};
        const RHI::QueryResult firstEnd = pool ? pool->ReadResult(1) : RHI::QueryResult {};
        const bool readback = firstRetired && firstBegin.Status == RHI::QueryResultStatus::Ready
            && firstEnd.Status == RHI::QueryResultStatus::Ready && firstEnd.Value >= firstBegin.Value;

        Scope<RHI::CommandList> reuseList = readback
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITimestampQuerySmokeV1 Reuse") : nullptr;
        const bool reuseRecorded = reuseList && reuseList->Begin() && reuseList->ResetQueryPool(*pool, 0, 2)
            && reuseList->WriteTimestamp(*pool, 0) && reuseList->WriteTimestamp(*pool, 1)
            && reuseList->ResolveQueryPool(*pool, 0, 2) && reuseList->End();
        const RHI::CompletionToken reuseToken = reuseRecorded ? device.Submit(*reuseList) : RHI::CompletionToken {};
        const bool reusePending = reuseToken.IsValid() && pool->ReadResult(0).Status == RHI::QueryResultStatus::Pending;
        const bool reused = reusePending && device.WaitForCompletion(reuseToken, 5000)
            && pool->ReadResult(0).Status == RHI::QueryResultStatus::Ready
            && pool->ReadResult(0).Generation > firstBegin.Generation;

        RHI::QueryPoolDescription destructionDescription = description;
        destructionDescription.DebugName = "RHITimestampQuerySmokeV1 Destruction";
        destructionDescription.Count = 1;
        Scope<RHI::QueryPool> destructionPool = device.CreateQueryPool(destructionDescription);
        Scope<RHI::CommandList> destructionList = destructionPool
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITimestampQuerySmokeV1 Destruction") : nullptr;
        const bool destructionRecorded = destructionList && destructionList->Begin()
            && destructionList->ResetQueryPool(*destructionPool, 0, 1)
            && destructionList->WriteTimestamp(*destructionPool, 0)
            && destructionList->ResolveQueryPool(*destructionPool, 0, 1) && destructionList->End();
        const RHI::CompletionToken destructionToken = destructionRecorded
            ? device.Submit(*destructionList) : RHI::CompletionToken {};
        destructionPool.reset();
        const bool destructionRetired = destructionToken.IsValid() && device.WaitForCompletion(destructionToken, 5000);

        const bool passed = allocated && firstRecorded && pending && readback && reused && destructionRetired;
        Log::Info("RHITimestampQuerySmokeV1 backend=", backendName,
            ", allocation=", allocated ? "pass" : "fail",
            ", periodNanoseconds=", periodNanoseconds,
            ", writeResolve=", firstRecorded ? "pass" : "fail",
            ", pending=", pending ? "pass" : "fail",
            ", readback=", readback ? "pass" : "fail",
            ", reuse=", reused ? "retired-pass" : "fail",
            ", destruction=", destructionRetired ? "retained-pass" : "fail",
            ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHIQueueDependencySmoke(RHI::Device& device, std::string_view backendName)
    {
        const RHI::QueueResolution copy = device.ResolveQueue(RHI::QueueType::Copy);
        const RHI::QueueResolution graphics = device.ResolveQueue(RHI::QueueType::Graphics);
        const RHI::QueueResolution compute = device.ResolveQueue(RHI::QueueType::Compute);
        if (backendName == "Vulkan")
        {
            Scope<RHI::CommandList> copyList = device.CreateCommandList(RHI::QueueType::Copy, "RHIQueueDependencyCopyV1");
            const bool copyClosed = copyList && copyList->Begin() && copyList->End();
            const RHI::CompletionToken copyToken = copyClosed ? device.Submit(*copyList) : RHI::CompletionToken {};
            Scope<RHI::CommandList> graphicsList = copyToken.IsValid()
                ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIQueueDependencyGraphicsV1") : nullptr;
            const bool graphicsClosed = graphicsList && graphicsList->Begin() && graphicsList->End();
            const RHI::CompletionToken graphicsToken = graphicsClosed
                ? device.Submit(*graphicsList, { copyToken }) : RHI::CompletionToken {};
            Scope<RHI::CommandList> computeList = graphicsToken.IsValid()
                ? device.CreateCommandList(RHI::QueueType::Compute, "RHIQueueDependencyComputeV1") : nullptr;
            const bool computeClosed = computeList && computeList->Begin() && computeList->End();
            const RHI::CompletionToken computeToken = computeClosed
                ? device.Submit(*computeList, { graphicsToken }) : RHI::CompletionToken {};
            RHI::BufferDescription ownedDescription;
            ownedDescription.DebugName = "RHIQueueDependencyForeignFamilyV1";
            ownedDescription.SizeBytes = 16;
            ownedDescription.Usage = RHI::BufferUsage::CopyDest;
            Scope<RHI::Buffer> graphicsOwned = device.CreateBuffer(ownedDescription);
            Scope<RHI::CommandList> forbiddenList = copy.Independent && graphicsOwned
                ? device.CreateCommandList(RHI::QueueType::Copy, "RHIQueueDependencyForeignFamilyV1") : nullptr;
            const bool mustReject = copy.Independent && !device.CanQueuesShareResources(RHI::QueueType::Graphics, RHI::QueueType::Copy);
            const bool foreignFamilyPolicy = !copy.Independent || (forbiddenList && forbiddenList->Begin()
                && (mustReject ? !forbiddenList->TransitionBuffer(*graphicsOwned, RHI::ResourceState::CopyDest)
                    : forbiddenList->TransitionBuffer(*graphicsOwned, RHI::ResourceState::CopyDest)) && forbiddenList->End());
            const bool retired = computeToken.IsValid() && device.WaitForCompletion(computeToken, 5000)
                && device.QueryCompletion(copyToken) == RHI::CompletionStatus::Complete
                && device.QueryCompletion(graphicsToken) == RHI::CompletionStatus::Complete;
            const bool topology = graphics.Requested == RHI::QueueType::Graphics
                && graphics.Effective == RHI::QueueType::Graphics && graphics.Independent
                && (copy.Independent ? copy.Effective == RHI::QueueType::Copy : copy.Effective == RHI::QueueType::Graphics)
                && (compute.Independent ? compute.Effective == RHI::QueueType::Compute : compute.Effective == RHI::QueueType::Graphics);
            const bool passed = topology && retired && foreignFamilyPolicy;
            Log::Info("RHIQueueDependencySmokeV1 backend=Vulkan, copy=", copy.Independent ? "independent" : "graphics-fallback",
                ", compute=", compute.Independent ? "independent" : "graphics-fallback",
                ", copyToGraphics=", copy.Effective == graphics.Effective ? "ordered-elided" : "gpu-wait",
                ", graphicsToCompute=", graphics.Effective == compute.Effective ? "ordered-elided" : "gpu-wait",
                ", cpuWaitBetween=no, queueLocal=yes, sharedResources=", mustReject ? "rejected" : "permitted-or-elided", ", retirement=", retired ? "pass" : "fail",
                ", result=", passed ? "pass" : "fail");
            return passed;
        }
        constexpr u32 valueCount = 1024;
        constexpr u64 byteCount = valueCount * sizeof(u32);
        std::array<u32, valueCount> expected {};
        for (u32 index = 0; index < valueCount; ++index)
            expected[index] = 0x51A70000u ^ (index * 2654435761u);

        RHI::BufferDescription uploadDescription;
        uploadDescription.DebugName = "RHIQueueDependencyV1 Upload";
        uploadDescription.SizeBytes = byteCount;
        uploadDescription.Usage = RHI::BufferUsage::CopySource;
        uploadDescription.CpuAccess = RHI::BufferCpuAccess::Write;
        RHI::BufferDescription intermediateDescription;
        intermediateDescription.DebugName = "RHIQueueDependencyV1 Intermediate";
        intermediateDescription.SizeBytes = byteCount;
        intermediateDescription.Usage = static_cast<RHI::BufferUsage>(
            static_cast<u32>(RHI::BufferUsage::CopySource) | static_cast<u32>(RHI::BufferUsage::CopyDest));
        intermediateDescription.InitialState = RHI::ResourceState::CopyDest;
        RHI::BufferDescription readbackDescription;
        readbackDescription.DebugName = "RHIQueueDependencyV1 Readback";
        readbackDescription.SizeBytes = byteCount;
        readbackDescription.Usage = RHI::BufferUsage::CopyDest;
        readbackDescription.CpuAccess = RHI::BufferCpuAccess::Read;
        Scope<RHI::Buffer> upload = device.CreateBuffer(uploadDescription);
        Scope<RHI::Buffer> intermediate = device.CreateBuffer(intermediateDescription);
        Scope<RHI::Buffer> readback = device.CreateBuffer(readbackDescription);
        void* uploadData = upload ? upload->Map() : nullptr;
        if (uploadData)
        {
            std::memcpy(uploadData, expected.data(), static_cast<size_t>(byteCount));
            upload->Unmap();
        }

        Scope<RHI::CommandList> copyList = uploadData && intermediate && readback
            ? device.CreateCommandList(RHI::QueueType::Copy, "RHIQueueDependencyCopyV1") : nullptr;
        const bool copyClosed = copyList && copyList->Begin()
            && copyList->CopyBuffer(*intermediate, 0, *upload, 0, byteCount)
            && copyList->TransitionBuffer(*intermediate, RHI::ResourceState::CopySource)
            && copyList->End();
        const RHI::CompletionToken copyToken = copyClosed ? device.Submit(*copyList) : RHI::CompletionToken {};
        RHI::ResourceState finalState = RHI::ResourceState::Unknown;
        const bool statePublished = copyToken.IsValid()
            && device.QueryResourceState(intermediate.get(), finalState) && finalState == RHI::ResourceState::CopySource;

        Scope<RHI::CommandList> graphicsList = statePublished
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIQueueDependencyGraphicsV1") : nullptr;
        const bool graphicsClosed = graphicsList && graphicsList->Begin()
            && graphicsList->CopyBuffer(*readback, 0, *intermediate, 0, byteCount) && graphicsList->End();
        const RHI::CompletionToken graphicsToken = graphicsClosed
            ? device.Submit(*graphicsList, { copyToken }) : RHI::CompletionToken {};

        Scope<RHI::CommandList> computeList = graphicsToken.IsValid()
            ? device.CreateCommandList(RHI::QueueType::Compute, "RHIQueueDependencyComputeV1") : nullptr;
        const bool computeClosed = computeList && computeList->Begin() && computeList->End();
        const RHI::CompletionToken computeToken = computeClosed
            ? device.Submit(*computeList, { graphicsToken }) : RHI::CompletionToken {};
        const bool retired = computeToken.IsValid() && device.WaitForCompletion(computeToken, 5000)
            && device.QueryCompletion(copyToken) == RHI::CompletionStatus::Complete
            && device.QueryCompletion(graphicsToken) == RHI::CompletionStatus::Complete;
        const void* readbackData = retired ? readback->Map() : nullptr;
        const bool bytesMatch = readbackData && std::memcmp(readbackData, expected.data(), static_cast<size_t>(byteCount)) == 0;
        if (readbackData)
            readback->Unmap();
        const bool topology = graphics.Effective == RHI::QueueType::Graphics && graphics.Independent
            && (!copy.Independent ? copy.Effective == RHI::QueueType::Graphics : copy.Effective == RHI::QueueType::Copy)
            && (!compute.Independent ? compute.Effective == RHI::QueueType::Graphics : compute.Effective == RHI::QueueType::Compute);
        const bool passed = topology && statePublished && graphicsToken.IsValid() && computeToken.IsValid() && retired && bytesMatch;
        Log::Info("RHIQueueDependencySmokeV1 backend=", backendName,
            ", copy=", copy.Independent ? "independent" : "graphics-fallback",
            ", compute=", compute.Independent ? "independent" : "graphics-fallback",
            ", copyToGraphics=", copy.Effective == graphics.Effective ? "ordered-elided" : "gpu-wait",
            ", graphicsToCompute=", graphics.Effective == compute.Effective ? "ordered-elided" : "gpu-wait",
            ", cpuWaitBetween=no, bytes=", bytesMatch ? "pass" : "fail",
            ", finalState=", statePublished ? "CopySource" : "fail",
            ", retirement=", retired ? "pass" : "fail", ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHIBufferOwnershipSmoke(RHI::Device& device, std::string_view backendName)
    {
        const RHI::QueueResolution graphics = device.ResolveQueue(RHI::QueueType::Graphics);
        const RHI::QueueResolution copy = device.ResolveQueue(RHI::QueueType::Copy);
        RHI::BufferDescription description;
        description.DebugName = "RHIBufferOwnershipSmokeV1 Transfer";
        description.SizeBytes = 4096;
        description.Usage = static_cast<RHI::BufferUsage>(
            static_cast<u32>(RHI::BufferUsage::CopySource) | static_cast<u32>(RHI::BufferUsage::CopyDest));
        description.InitialState = RHI::ResourceState::CopyDest;
        Scope<RHI::Buffer> transfer = device.CreateBuffer(description);

        const bool fallback = !copy.Independent;
        if (fallback)
        {
            Scope<RHI::CommandList> release = transfer
                ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIBufferOwnershipFallbackReleaseV1") : nullptr;
            const bool rejected = release && release->Begin()
                && !release->ReleaseBufferOwnership({ transfer.get(), RHI::QueueType::Graphics, RHI::QueueType::Copy,
                    RHI::ResourceState::CopyDest, RHI::ResourceState::CopySource })
                && release->End();
            const bool pending = transfer && device.HasPendingBufferOwnershipTransfer(transfer.get());
            const bool passed = graphics.Effective == RHI::QueueType::Graphics
                && copy.Effective == RHI::QueueType::Graphics && !copy.Independent && rejected && !pending;
            Log::Info("RHIBufferOwnershipSmokeV1 backend=", backendName,
                ", mode=graphics-fallback, transfer=rejected, pending=", pending ? "yes" : "no",
                ", result=", passed ? "pass" : "fail");
            return passed;
        }

        std::array<u32, 1024> expected {};
        for (u32 index = 0; index < expected.size(); ++index)
            expected[index] = 0x0B1E0000u ^ (index * 2246822519u);
        RHI::BufferDescription validationDescription = description;
        validationDescription.DebugName = "RHIBufferOwnershipSmokeV1 Validation";
        validationDescription.Usage = RHI::BufferUsage::CopyDest;
        Scope<RHI::Buffer> validation = device.CreateBuffer(validationDescription);
        RHI::BufferDescription readbackDescription = validationDescription;
        readbackDescription.DebugName = "RHIBufferOwnershipSmokeV1 Readback";
        readbackDescription.CpuAccess = RHI::BufferCpuAccess::Read;
        Scope<RHI::Buffer> readback = device.CreateBuffer(readbackDescription);
        const bool uploaded = transfer && device.UploadBuffer(*transfer, expected.data(), sizeof(expected));

        Scope<RHI::CommandList> release = uploaded
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIBufferOwnershipReleaseV1") : nullptr;
        const bool releaseClosed = release && release->Begin()
            && release->ReleaseBufferOwnership({ transfer.get(), RHI::QueueType::Graphics, RHI::QueueType::Copy,
                RHI::ResourceState::CopyDest, RHI::ResourceState::CopySource })
            && release->End();
        const RHI::CompletionToken releaseToken = releaseClosed ? device.Submit(*release) : RHI::CompletionToken {};
        release.reset();
        const bool pendingAfterRelease = releaseToken.IsValid() && device.HasPendingBufferOwnershipTransfer(transfer.get());

        // CPU-visible buffers are intentionally ineligible for ownership transfer.
        // The GPU-only validation target follows the paired lifecycle in both
        // directions before its Graphics-owned result is copied to readback.
        Scope<RHI::CommandList> validationRelease = uploaded && validation
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIBufferOwnershipValidationReleaseV1") : nullptr;
        const bool validationReleaseClosed = validationRelease && validationRelease->Begin()
            && validationRelease->ReleaseBufferOwnership({ validation.get(), RHI::QueueType::Graphics, RHI::QueueType::Copy,
                RHI::ResourceState::CopyDest, RHI::ResourceState::CopyDest }) && validationRelease->End();
        const RHI::CompletionToken validationReleaseToken = validationReleaseClosed ? device.Submit(*validationRelease) : RHI::CompletionToken {};
        validationRelease.reset();
        RHI::BufferOwnershipAcquire validationAcquireDescription;
        validationAcquireDescription.Resource = validation.get(); validationAcquireDescription.SourceQueue = RHI::QueueType::Graphics;
        validationAcquireDescription.DestinationQueue = RHI::QueueType::Copy; validationAcquireDescription.Before = RHI::ResourceState::CopyDest;
        validationAcquireDescription.After = RHI::ResourceState::CopyDest; validationAcquireDescription.ReleaseToken = validationReleaseToken;
        Scope<RHI::CommandList> validationAcquire = validationReleaseToken.IsValid()
            ? device.CreateCommandList(RHI::QueueType::Copy, "RHIBufferOwnershipValidationAcquireV1") : nullptr;
        const bool validationAcquireClosed = validationAcquire && validationAcquire->Begin()
            && validationAcquire->AcquireBufferOwnership(validationAcquireDescription) && validationAcquire->End();
        const RHI::CompletionToken validationAcquireToken = validationAcquireClosed ? device.Submit(*validationAcquire, { validationReleaseToken }) : RHI::CompletionToken {};

        Scope<RHI::CommandList> acquire = pendingAfterRelease
            ? device.CreateCommandList(RHI::QueueType::Copy, "RHIBufferOwnershipAcquireV1") : nullptr;
        RHI::BufferOwnershipAcquire acquireDescription;
        acquireDescription.Resource = transfer.get();
        acquireDescription.SourceQueue = RHI::QueueType::Graphics;
        acquireDescription.DestinationQueue = RHI::QueueType::Copy;
        acquireDescription.Before = RHI::ResourceState::CopyDest;
        acquireDescription.After = RHI::ResourceState::CopySource;
        acquireDescription.ReleaseToken = releaseToken;
        const bool acquireClosed = acquire && acquire->Begin()
            && acquire->AcquireBufferOwnership(acquireDescription)
            && acquire->End();
        const RHI::CompletionToken acquireToken = acquireClosed ? device.Submit(*acquire, { releaseToken }) : RHI::CompletionToken {};
        RHI::QueueType finalOwner = RHI::QueueType::Graphics;
        RHI::ResourceState finalState = RHI::ResourceState::Unknown;
        const bool acquired = acquireToken.IsValid() && !device.HasPendingBufferOwnershipTransfer(transfer.get())
            && device.QueryBufferQueueOwner(transfer.get(), finalOwner) && finalOwner == RHI::QueueType::Copy
            && device.QueryResourceState(transfer.get(), finalState) && finalState == RHI::ResourceState::CopySource;

        Scope<RHI::CommandList> validationCopy = acquired && validationAcquireToken.IsValid()
            ? device.CreateCommandList(RHI::QueueType::Copy, "RHIBufferOwnershipValidationCopyV1") : nullptr;
        const bool validationCopyClosed = validationCopy && validationCopy->Begin()
            && validationCopy->CopyBuffer(*validation, 0, *transfer, 0, sizeof(expected)) && validationCopy->End();
        const RHI::CompletionToken validationCopyToken = validationCopyClosed
            ? device.Submit(*validationCopy, { acquireToken, validationAcquireToken }) : RHI::CompletionToken {};
        Scope<RHI::CommandList> validationReturnRelease = validationCopyToken.IsValid()
            ? device.CreateCommandList(RHI::QueueType::Copy, "RHIBufferOwnershipValidationReturnReleaseV1") : nullptr;
        const bool validationReturnReleaseClosed = validationReturnRelease && validationReturnRelease->Begin()
            && validationReturnRelease->ReleaseBufferOwnership({ validation.get(), RHI::QueueType::Copy, RHI::QueueType::Graphics,
                RHI::ResourceState::CopyDest, RHI::ResourceState::CopyDest }) && validationReturnRelease->End();
        const RHI::CompletionToken validationReturnReleaseToken = validationReturnReleaseClosed
            ? device.Submit(*validationReturnRelease, { validationCopyToken }) : RHI::CompletionToken {};
        RHI::BufferOwnershipAcquire validationReturnAcquireDescription;
        validationReturnAcquireDescription.Resource = validation.get(); validationReturnAcquireDescription.SourceQueue = RHI::QueueType::Copy;
        validationReturnAcquireDescription.DestinationQueue = RHI::QueueType::Graphics; validationReturnAcquireDescription.Before = RHI::ResourceState::CopyDest;
        validationReturnAcquireDescription.After = RHI::ResourceState::CopyDest; validationReturnAcquireDescription.ReleaseToken = validationReturnReleaseToken;
        Scope<RHI::CommandList> validationReturnAcquire = validationReturnReleaseToken.IsValid()
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIBufferOwnershipValidationReturnAcquireV1") : nullptr;
        const bool validationReturnAcquireClosed = validationReturnAcquire && validationReturnAcquire->Begin()
            && validationReturnAcquire->AcquireBufferOwnership(validationReturnAcquireDescription) && validationReturnAcquire->End();
        const RHI::CompletionToken validationReturnAcquireToken = validationReturnAcquireClosed
            ? device.Submit(*validationReturnAcquire, { validationReturnReleaseToken }) : RHI::CompletionToken {};
        Scope<RHI::CommandList> readbackCopy = validationReturnAcquireToken.IsValid()
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIBufferOwnershipReadbackV1") : nullptr;
        const bool readbackClosed = readbackCopy && readbackCopy->Begin()
            && readbackCopy->CopyBuffer(*readback, 0, *validation, 0, sizeof(expected)) && readbackCopy->End();
        const RHI::CompletionToken readbackToken = readbackClosed
            ? device.Submit(*readbackCopy, { validationReturnAcquireToken }) : RHI::CompletionToken {};
        const bool retired = readbackToken.IsValid() && device.WaitForCompletion(readbackToken, 5000)
            && device.QueryCompletion(releaseToken) == RHI::CompletionStatus::Complete
            && device.QueryCompletion(acquireToken) == RHI::CompletionStatus::Complete
            && device.QueryCompletion(validationReleaseToken) == RHI::CompletionStatus::Complete
            && device.QueryCompletion(validationAcquireToken) == RHI::CompletionStatus::Complete
            && device.QueryCompletion(validationCopyToken) == RHI::CompletionStatus::Complete
            && device.QueryCompletion(validationReturnReleaseToken) == RHI::CompletionStatus::Complete
            && device.QueryCompletion(validationReturnAcquireToken) == RHI::CompletionStatus::Complete;
        const void* mapped = retired ? readback->Map() : nullptr;
        const bool bytesMatch = mapped && std::memcmp(mapped, expected.data(), sizeof(expected)) == 0;
        if (mapped)
            readback->Unmap();

        Scope<RHI::Buffer> abandoned = device.CreateBuffer(description);
        const bool abandonedUploaded = abandoned && device.UploadBuffer(*abandoned, expected.data(), sizeof(expected));
        Scope<RHI::CommandList> abandonedRelease = abandonedUploaded
            ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIBufferOwnershipRecoveryReleaseV1") : nullptr;
        const bool abandonedClosed = abandonedRelease && abandonedRelease->Begin()
            && abandonedRelease->ReleaseBufferOwnership({ abandoned.get(), RHI::QueueType::Graphics, RHI::QueueType::Copy,
                RHI::ResourceState::CopyDest, RHI::ResourceState::CopySource })
            && abandonedRelease->End();
        const RHI::CompletionToken abandonedToken = abandonedClosed ? device.Submit(*abandonedRelease) : RHI::CompletionToken {};
        const bool recovery = abandonedToken.IsValid() && device.HasPendingBufferOwnershipTransfer(abandoned.get())
            && device.WaitForCompletion(abandonedToken, 5000)
            && device.RecoverAbandonedBufferOwnershipTransfer(*abandoned, abandonedToken)
            && !device.HasPendingBufferOwnershipTransfer(abandoned.get())
            && device.QueryBufferQueueOwner(abandoned.get(), finalOwner) && finalOwner == RHI::QueueType::Graphics
            && device.QueryResourceState(abandoned.get(), finalState) && finalState == RHI::ResourceState::CopyDest;
        const bool passed = graphics.Independent && copy.Independent && graphics.Effective == RHI::QueueType::Graphics
            && copy.Effective == RHI::QueueType::Copy && uploaded && pendingAfterRelease && acquired && bytesMatch && retired && recovery;
        Log::Info("RHIBufferOwnershipSmokeV1 backend=", backendName,
            ", mode=independent, release=accepted, acquire=gpu-wait, cpuWaitBetween=no, bytes=", bytesMatch ? "pass" : "fail",
            ", finalOwner=Copy, finalState=CopySource, recovery=", recovery ? "pass" : "fail",
            ", retirement=", retired ? "pass" : "fail", ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHITextureOwnershipSmoke(RHI::Device& device, std::string_view backendName)
    {
        const RHI::QueueResolution graphics = device.ResolveQueue(RHI::QueueType::Graphics);
        const RHI::QueueResolution copy = device.ResolveQueue(RHI::QueueType::Copy);
        RHI::TextureDescription description;
        description.DebugName = "RHITextureOwnershipSmokeV1 Transfer";
        description.Extent = { 3, 2 }; description.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        description.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource));
        description.InitialState = RHI::ResourceState::CopySource;
        Scope<RHI::Texture> transfer = device.CreateTexture(description);
        const bool fallback = !copy.Independent;
        if (fallback)
        {
            Scope<RHI::CommandList> release = transfer ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITextureOwnershipFallbackReleaseV1") : nullptr;
            const bool rejected = release && release->Begin() && !release->ReleaseTextureOwnership({ transfer.get(), RHI::QueueType::Graphics, RHI::QueueType::Copy, RHI::ResourceState::CopySource, RHI::ResourceState::CopySource }) && release->End();
            const bool pending = transfer && device.HasPendingTextureOwnershipTransfer(transfer.get());
            const bool passed = graphics.Effective == RHI::QueueType::Graphics && copy.Effective == RHI::QueueType::Graphics && !copy.Independent && rejected && !pending;
            Log::Info("RHITextureOwnershipSmokeV1 backend=", backendName, ", mode=graphics-fallback, transfer=rejected, pending=", pending ? "yes" : "no", ", result=", passed ? "pass" : "fail");
            return passed;
        }
        RHI::TextureDescription depthDescription;
        depthDescription.DebugName = "RHITextureOwnershipSmokeV1 Depth";
        depthDescription.Extent = description.Extent;
        depthDescription.TextureFormat = RHI::Format::D32Float;
        depthDescription.Usage = RHI::TextureUsage::DepthStencil;
        Scope<RHI::Texture> depth = transfer ? device.CreateTexture(depthDescription) : nullptr;
        Scope<RHI::CommandList> clear = depth ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITextureOwnershipClearV1") : nullptr;
        RHI::ViewportClear clearValue;
        clearValue.Color[0] = 0.25f; clearValue.Color[1] = 0.5f; clearValue.Color[2] = 0.75f; clearValue.Color[3] = 1.0f;
        clearValue.ClearDepth = false;
        const bool initialized = clear && clear->Begin() && clear->BindViewportOutputs(*transfer, depth.get())
            && clear->TransitionTexture(*transfer, RHI::ResourceState::RenderTarget) && clear->ClearViewportOutputs(clearValue)
            && clear->TransitionTexture(*transfer, RHI::ResourceState::CopySource) && clear->End() && device.SubmitAndWait(*clear);
        Scope<RHI::CommandList> release = initialized ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITextureOwnershipReleaseV1") : nullptr;
        const bool releaseClosed = release && release->Begin() && release->ReleaseTextureOwnership({ transfer.get(), RHI::QueueType::Graphics, RHI::QueueType::Copy, RHI::ResourceState::CopySource, RHI::ResourceState::CopySource }) && release->End();
        const RHI::CompletionToken releaseToken = releaseClosed ? device.Submit(*release) : RHI::CompletionToken {};
        RHI::TextureOwnershipAcquire acquire; acquire.Resource = transfer.get(); acquire.SourceQueue = RHI::QueueType::Graphics; acquire.DestinationQueue = RHI::QueueType::Copy; acquire.Before = RHI::ResourceState::CopySource; acquire.After = RHI::ResourceState::CopySource; acquire.ReleaseToken = releaseToken;
        Scope<RHI::CommandList> acquireList = releaseToken.IsValid() ? device.CreateCommandList(RHI::QueueType::Copy, "RHITextureOwnershipAcquireV1") : nullptr;
        const bool acquireClosed = acquireList && acquireList->Begin() && acquireList->AcquireTextureOwnership(acquire) && acquireList->End();
        const RHI::CompletionToken acquireToken = acquireClosed ? device.Submit(*acquireList, { releaseToken }) : RHI::CompletionToken {};
        RHI::QueueType owner = RHI::QueueType::Graphics; RHI::ResourceState state = RHI::ResourceState::Unknown;
        const bool acquired = acquireToken.IsValid() && !device.HasPendingTextureOwnershipTransfer(transfer.get()) && device.QueryTextureQueueOwner(transfer.get(), owner) && owner == RHI::QueueType::Copy && device.QueryResourceState(transfer.get(), state) && state == RHI::ResourceState::CopySource;
        // The only CPU wait occurs after the paired submissions. Readback uses
        // the established RHI path and must not mutate the ownership authority.
        const bool acquireRetired = acquired && device.WaitForCompletion(acquireToken, 5000);
        RHI::TextureReadback readback;
        const bool readbackOk = acquireRetired && device.ReadbackTexture(*transfer, readback);
        const std::array<u8, 4> expected { 64u, 128u, 191u, 255u };
        bool bytesMatch = readbackOk && readback.Extent.Width == description.Extent.Width && readback.Extent.Height == description.Extent.Height
            && readback.TextureFormat == description.TextureFormat && readback.RowPitchBytes == description.Extent.Width * 4
            && readback.Data.size() == static_cast<size_t>(readback.RowPitchBytes) * description.Extent.Height;
        for (u32 y = 0; bytesMatch && y < description.Extent.Height; ++y)
            for (u32 x = 0; bytesMatch && x < description.Extent.Width; ++x)
                for (u32 channel = 0; channel < expected.size(); ++channel)
                    if (std::abs(static_cast<int>(readback.Data[static_cast<size_t>(y) * readback.RowPitchBytes + x * 4 + channel]) - expected[channel]) > 1) bytesMatch = false;
        const bool finalState = bytesMatch && device.QueryTextureQueueOwner(transfer.get(), owner) && owner == RHI::QueueType::Copy
            && device.QueryResourceState(transfer.get(), state) && state == RHI::ResourceState::CopySource;
        const bool retired = acquireRetired && device.QueryCompletion(releaseToken) == RHI::CompletionStatus::Complete
            && device.QueryCompletion(acquireToken) == RHI::CompletionStatus::Complete;
        Scope<RHI::Texture> abandoned = device.CreateTexture(description);
        Scope<RHI::CommandList> abandonedRelease = abandoned ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITextureOwnershipRecoveryReleaseV1") : nullptr;
        const bool abandonedClosed = abandonedRelease && abandonedRelease->Begin() && abandonedRelease->ReleaseTextureOwnership({ abandoned.get(), RHI::QueueType::Graphics, RHI::QueueType::Copy, RHI::ResourceState::CopySource, RHI::ResourceState::CopySource }) && abandonedRelease->End();
        const RHI::CompletionToken abandonedToken = abandonedClosed ? device.Submit(*abandonedRelease) : RHI::CompletionToken {};
        const bool recovery = abandonedToken.IsValid() && device.HasPendingTextureOwnershipTransfer(abandoned.get())
            && device.WaitForCompletion(abandonedToken, 5000)
            && device.QueryCompletion(abandonedToken) == RHI::CompletionStatus::Complete
            && device.RecoverAbandonedTextureOwnershipTransfer(*abandoned, abandonedToken)
            && !device.HasPendingTextureOwnershipTransfer(abandoned.get())
            && device.QueryTextureQueueOwner(abandoned.get(), owner) && owner == RHI::QueueType::Graphics
            && device.QueryResourceState(abandoned.get(), state) && state == RHI::ResourceState::CopySource;
        const bool passed = graphics.Independent && copy.Independent && initialized && acquired && bytesMatch && finalState && retired && recovery;
        Log::Info("RHITextureOwnershipSmokeV1 backend=", backendName, ", mode=independent, release=accepted, acquire=gpu-wait, cpuWaitBetween=no, bytes=", bytesMatch ? "pass" : "fail", ", finalOwner=Copy, finalState=CopySource, recovery=", recovery ? "pass" : "fail", ", retirement=", retired ? "pass" : "fail", ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHIResourceOwnershipSmoke(RHI::Device& device, std::string_view backendName)
    {
        RHI::BufferDescription bufferDescription;
        bufferDescription.DebugName = "RHIResourceOwnershipSmokeV1 Buffer";
        bufferDescription.SizeBytes = sizeof(u32);
        bufferDescription.Usage = RHI::BufferUsage::CopyDest;
        RHI::TextureDescription textureDescription;
        textureDescription.DebugName = "RHIResourceOwnershipSmokeV1 Texture";
        textureDescription.Extent = { 4, 4 };
        textureDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        textureDescription.Usage = RHI::TextureUsage::CopyDest;
        Scope<RHI::Buffer> buffer = device.CreateBuffer(bufferDescription);
        Scope<RHI::Texture> texture = device.CreateTexture(textureDescription);
        const bool owned = buffer && texture && device.OwnsResource(buffer.get()) && device.OwnsResource(texture.get());
        const bool nullRejected = !device.OwnsResource(static_cast<const RHI::Buffer*>(nullptr))
            && !device.OwnsResource(static_cast<const RHI::Texture*>(nullptr));
        const bool passed = owned && nullRejected;
        Log::Info("RHIResourceOwnershipSmokeV1 backend=", backendName,
            ", owned=", owned ? "pass" : "fail",
            ", null=rejected", nullRejected ? "" : "-failed",
            ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHIResourceStateSmoke(RHI::Device& device, std::string_view backendName)
    {
        RHI::BufferDescription bufferDescription;
        bufferDescription.DebugName = "RHIResourceStateSmokeV1 Buffer";
        bufferDescription.SizeBytes = sizeof(u32);
        bufferDescription.Usage = static_cast<RHI::BufferUsage>(static_cast<u32>(RHI::BufferUsage::CopyDest) | static_cast<u32>(RHI::BufferUsage::CopySource));
        bufferDescription.InitialState = RHI::ResourceState::CopyDest;
        RHI::TextureDescription textureDescription;
        textureDescription.DebugName = "RHIResourceStateSmokeV1 Texture";
        textureDescription.Extent = { 4, 4 };
        textureDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        textureDescription.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::CopyDest) | static_cast<u32>(RHI::TextureUsage::CopySource));
        textureDescription.InitialState = RHI::ResourceState::CopyDest;
        Scope<RHI::Buffer> buffer = device.CreateBuffer(bufferDescription);
        Scope<RHI::Texture> texture = device.CreateTexture(textureDescription);
        RHI::ResourceState observed = RHI::ResourceState::Unknown;
        const bool initial = buffer && texture
            && device.QueryResourceState(buffer.get(), observed) && observed == RHI::ResourceState::CopyDest
            && device.QueryResourceState(texture.get(), observed) && observed == RHI::ResourceState::CopyDest;
        Scope<RHI::CommandList> list = initial ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIResourceStateSmokeV1") : nullptr;
        const bool recording = list && list->Begin();
        const bool rejectedInvalidRecord = recording && !list->TransitionTexture(*texture, RHI::ResourceState::Unknown)
            && device.QueryResourceState(texture.get(), observed) && observed == RHI::ResourceState::CopyDest;
        const bool transitions = rejectedInvalidRecord && list->TransitionTexture(*texture, RHI::ResourceState::CopySource)
            && list->TransitionBuffer(*buffer, RHI::ResourceState::CopySource);
        const bool pendingInvisible = transitions
            && device.QueryResourceState(buffer.get(), observed) && observed == RHI::ResourceState::CopyDest
            && device.QueryResourceState(texture.get(), observed) && observed == RHI::ResourceState::CopyDest;
        const bool submitted = pendingInvisible && list->End() && device.SubmitAndWait(*list);
        const bool final = submitted
            && device.QueryResourceState(buffer.get(), observed) && observed == RHI::ResourceState::CopySource
            && device.QueryResourceState(texture.get(), observed) && observed == RHI::ResourceState::CopySource;
        RHI::TextureDescription unknownDescription = textureDescription;
        unknownDescription.DebugName = "RHIResourceStateSmokeV1 Unknown";
        unknownDescription.InitialState = RHI::ResourceState::Unknown;
        Scope<RHI::Texture> unknownTexture = device.CreateTexture(unknownDescription);
        const bool invalid = !device.QueryResourceState(static_cast<const RHI::Buffer*>(nullptr), observed)
            && !device.QueryResourceState(static_cast<const RHI::Texture*>(nullptr), observed)
            && !unknownTexture;
        const bool passed = initial && rejectedInvalidRecord && pendingInvisible && final && invalid;
        Log::Info("RHIResourceStateSmokeV1 backend=", backendName,
            ", initial=pass", initial ? "" : "-failed",
            ", pending=", pendingInvisible ? "hidden" : "visible",
            ", invalid=", invalid ? "rejected" : "accepted",
            ", submission=", submitted ? "pass" : "fail",
            ", final=", final ? "pass" : "fail",
            ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHITextureReadbackSmoke(RHI::Device& device, std::string_view backendName)
    {
        constexpr u32 width = 3;
        constexpr u32 height = 2;
        RHI::TextureDescription colorDescription;
        colorDescription.DebugName = "RHITextureReadbackSmokeV1 Color";
        colorDescription.Extent = { width, height };
        colorDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        colorDescription.Usage = static_cast<RHI::TextureUsage>(
            static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource));
        Scope<RHI::Texture> color = device.CreateTexture(colorDescription);
        RHI::TextureReadback rejectedStateResult;
        const bool rejectedState = color && !device.ReadbackTexture(*color, rejectedStateResult);

        RHI::TextureDescription unsupportedDescription = colorDescription;
        unsupportedDescription.DebugName = "RHITextureReadbackSmokeV1 Unsupported";
        unsupportedDescription.TextureFormat = RHI::Format::R8Unorm;
        unsupportedDescription.InitialState = RHI::ResourceState::CopySource;
        Scope<RHI::Texture> unsupported = device.CreateTexture(unsupportedDescription);
        const bool rejectedFormat = unsupported && !device.ReadbackTexture(*unsupported, rejectedStateResult);

        RHI::TextureDescription depthDescription;
        depthDescription.DebugName = "RHITextureReadbackSmokeV1 Depth";
        depthDescription.Extent = { width, height };
        depthDescription.TextureFormat = RHI::Format::D32Float;
        depthDescription.Usage = RHI::TextureUsage::DepthStencil;
        Scope<RHI::Texture> depth = device.CreateTexture(depthDescription);
        Scope<RHI::CommandList> list = color && depth ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITextureReadbackSmokeV1") : nullptr;
        RHI::ViewportClear clear;
        clear.Color[0] = 0.25f;
        clear.Color[1] = 0.5f;
        clear.Color[2] = 0.75f;
        clear.Color[3] = 1.0f;
        clear.ClearDepth = false;
        const bool draw = list && list->Begin() && list->BindViewportOutputs(*color, depth.get())
            && list->ClearViewportOutputs(clear)
            && list->TransitionTexture(*color, RHI::ResourceState::CopySource)
            && list->End() && device.SubmitAndWait(*list);
        RHI::TextureReadback readback;
        const bool readbackOk = draw && device.ReadbackTexture(*color, readback);
        const std::array<u8, 4> expected { 64u, 128u, 191u, 255u };
        bool pixelsOk = readbackOk && readback.Extent.Width == width && readback.Extent.Height == height
            && readback.TextureFormat == RHI::Format::R8G8B8A8Unorm && readback.RowPitchBytes == width * 4
            && readback.Data.size() == static_cast<size_t>(readback.RowPitchBytes) * height;
        for (u32 y = 0; pixelsOk && y < height; ++y)
            for (u32 x = 0; pixelsOk && x < width; ++x)
                for (u32 channel = 0; channel < expected.size(); ++channel)
                    if (std::abs(static_cast<int>(readback.Data[static_cast<size_t>(y) * readback.RowPitchBytes + x * 4 + channel]) - expected[channel]) > 1)
                        pixelsOk = false;
        const bool passed = rejectedState && rejectedFormat && pixelsOk;
        Log::Info("RHITextureReadbackSmokeV1 backend=", backendName,
            ", invalidState=", rejectedState ? "rejected" : "accepted",
            ", unsupportedFormat=", rejectedFormat ? "rejected" : "accepted",
            ", submit=", draw ? "pass" : "fail",
            ", readback=", readbackOk ? "pass" : "fail",
            ", layout=", pixelsOk ? "tight" : "invalid",
            ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunRHITextureUploadSmoke(RHI::Device& device, std::string_view backendName)
    {
        constexpr u32 width = 3;
        constexpr u32 height = 2;
        RHI::TextureDescription description;
        description.DebugName = "RHITextureUploadSmokeV1";
        description.Extent = { width, height };
        description.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        description.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::CopyDest)
            | static_cast<u32>(RHI::TextureUsage::CopySource) | static_cast<u32>(RHI::TextureUsage::ShaderResource));
        description.InitialState = RHI::ResourceState::CopyDest;
        Scope<RHI::Texture> texture = device.CreateTexture(description);
        const Ref<std::vector<u8>> bytes = CreateRef<std::vector<u8>>(height * 16u, 0u);
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x)
            {
                const size_t offset = static_cast<size_t>(y) * 16u + x * 4u;
                (*bytes)[offset + 0] = static_cast<u8>(16u + x + y * 3u);
                (*bytes)[offset + 1] = static_cast<u8>(64u + x + y * 3u);
                (*bytes)[offset + 2] = static_cast<u8>(128u + x + y * 3u);
                (*bytes)[offset + 3] = 255u;
            }
        RHI::TextureUpload upload { { width, height }, RHI::Format::R8G8B8A8Unorm, 16u, bytes };
        RHI::ResourceState state = RHI::ResourceState::Unknown;
        const bool uploaded = texture && device.UploadTexture(*texture, upload)
            && device.QueryResourceState(texture.get(), state) && state == RHI::ResourceState::ShaderResource;
        Scope<RHI::CommandList> readbackList = uploaded ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITextureUploadSmokeV1 Readback") : nullptr;
        const bool transitioned = readbackList && readbackList->Begin()
            && readbackList->TransitionTexture(*texture, RHI::ResourceState::CopySource)
            && readbackList->End() && device.SubmitAndWait(*readbackList);
        RHI::TextureReadback readback;
        const bool read = transitioned && device.ReadbackTexture(*texture, readback);
        bool bytesMatch = read && readback.RowPitchBytes == width * 4u && readback.Data.size() == static_cast<size_t>(width * height * 4u);
        for (u32 y = 0; bytesMatch && y < height; ++y)
            for (u32 x = 0; bytesMatch && x < width; ++x)
                for (u32 channel = 0; channel < 4; ++channel)
                    if (readback.Data[static_cast<size_t>(y) * readback.RowPitchBytes + x * 4u + channel] != (*bytes)[static_cast<size_t>(y) * 16u + x * 4u + channel])
                        bytesMatch = false;
        const bool singleMipPassed = uploaded && transitioned && read && bytesMatch;
        Log::Info("RHITextureUploadSmokeV1 backend=", backendName,
            ", shaderResource=", uploaded ? "pass" : "fail",
            ", readback=", read ? "pass" : "fail",
            ", bytes=", bytesMatch ? "pass" : "fail",
            ", result=", singleMipPassed ? "pass" : "fail");

        const auto runMipChain = [&](RHI::Format format)
        {
            const auto fail = [&](std::string_view stage)
            {
                Log::Error("RHITextureUploadSmokeV2 backend=", backendName,
                    ", format=", RHI::ToString(format), ", failedStage=", stage);
                return false;
            };
            const RHI::FormatUsage requiredUsages = RHI::FormatUsage::Sampled
                | RHI::FormatUsage::CopySource | RHI::FormatUsage::CopyDestination;
            const bool supported = std::any_of(device.GetCapabilities().Formats.begin(), device.GetCapabilities().Formats.end(),
                [&](const RHI::FormatCapability& capability)
                {
                    return capability.Value == format && RHI::HasAllFormatUsages(capability.Usages, requiredUsages);
                });
            if (!supported)
                return fail("capability");

            RHI::TextureDescription mipDescription;
            mipDescription.DebugName = std::string("RHITextureUploadSmokeV2 ") + RHI::ToString(format);
            mipDescription.Extent = { 8, 8 };
            mipDescription.TextureFormat = format;
            mipDescription.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::CopyDest)
                | static_cast<u32>(RHI::TextureUsage::CopySource) | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            mipDescription.InitialState = RHI::ResourceState::CopyDest;
            mipDescription.MipLevels = 4;

            RHI::TextureUploadBatch mipUpload;
            mipUpload.TextureFormat = format;
            const Ref<std::vector<u8>> mipBytes = CreateRef<std::vector<u8>>();
            RHI::Extent2D mipExtent = mipDescription.Extent;
            u64 byteOffset = 0;
            for (u32 mipLevel = 0; mipLevel < mipDescription.MipLevels; ++mipLevel)
            {
                u64 rowPitch = 0, byteSize = 0;
                if (!RHI::CalculateTextureSubresourceStorage(format, mipExtent, rowPitch, byteSize)
                    || byteSize > std::numeric_limits<size_t>::max()
                    || byteOffset > std::numeric_limits<size_t>::max() - byteSize)
                    return fail("batch-layout");
                mipBytes->resize(static_cast<size_t>(byteOffset + byteSize));
                for (u64 byte = 0; byte < byteSize; ++byte)
                    (*mipBytes)[static_cast<size_t>(byteOffset + byte)] = static_cast<u8>(
                        17u + mipLevel * 37u + static_cast<u32>(byte % 193u));
                mipUpload.Subresources.push_back({ mipLevel, 0, mipExtent, byteOffset, rowPitch, byteSize });
                byteOffset += byteSize;
                mipExtent.Width = mipExtent.Width > 1 ? mipExtent.Width / 2 : 1;
                mipExtent.Height = mipExtent.Height > 1 ? mipExtent.Height / 2 : 1;
            }
            mipUpload.Bytes = mipBytes;

            Scope<RHI::Texture> mipTexture = device.CreateTexture(mipDescription);
            RHI::ResourceState mipState = RHI::ResourceState::Unknown;
            const bool mipUploaded = mipTexture && device.UploadTexture(*mipTexture, mipUpload)
                && device.QueryResourceState(mipTexture.get(), mipState) && mipState == RHI::ResourceState::ShaderResource;
            if (!mipUploaded)
                return fail(mipTexture ? "upload-or-state" : "create");
            Scope<RHI::CommandList> mipReadbackList = mipUploaded
                ? device.CreateCommandList(RHI::QueueType::Graphics, "RHITextureUploadSmokeV2 Readback") : nullptr;
            const bool mipTransitioned = mipReadbackList && mipReadbackList->Begin()
                && mipReadbackList->TransitionTexture(*mipTexture, RHI::ResourceState::CopySource)
                && mipReadbackList->End() && device.SubmitAndWait(*mipReadbackList);
            if (!mipTransitioned)
                return fail("copy-source-transition");

            for (const RHI::TextureSubresourceUpload& subresource : mipUpload.Subresources)
            {
                RHI::TextureReadback mipReadback;
                if (!device.ReadbackTexture(*mipTexture, subresource.MipLevel, mipReadback)
                    || mipReadback.Extent.Width != subresource.Extent.Width
                    || mipReadback.Extent.Height != subresource.Extent.Height
                    || mipReadback.TextureFormat != format
                    || mipReadback.RowPitchBytes != subresource.RowPitchBytes
                    || mipReadback.Data.size() != static_cast<size_t>(subresource.ByteSize)
                    || !std::equal(mipReadback.Data.begin(), mipReadback.Data.end(),
                        mipBytes->begin() + static_cast<std::ptrdiff_t>(subresource.ByteOffset)))
                    return fail(std::string("readback-mip-") + std::to_string(subresource.MipLevel));
            }
            return true;
        };

        const bool bc5MipChain = runMipChain(RHI::Format::BC5Unorm);
        const bool bc7MipChain = runMipChain(RHI::Format::BC7Unorm);
        const bool bc7SrgbMipChain = runMipChain(RHI::Format::BC7UnormSrgb);
        const bool mipUploadPassed = singleMipPassed && bc5MipChain && bc7MipChain && bc7SrgbMipChain;
        Log::Info("RHITextureUploadSmokeV2 backend=", backendName,
            ", mips=4, bc5Bytes=", bc5MipChain ? "pass" : "fail",
            ", bc7Bytes=", bc7MipChain ? "pass" : "fail",
            ", bc7SrgbBytes=", bc7SrgbMipChain ? "pass" : "fail",
            ", finalState=ShaderResource, result=", mipUploadPassed ? "pass" : "fail");

        const auto makeArtifact = [](AssetHandle asset, TextureTargetProfile profile,
            TextureCookedFormat cookedFormat, RHI::Format format, u8 seed)
        {
            TextureArtifact artifact;
            artifact.Asset = asset;
            artifact.SourcePath = "Engine/Generated/TextureGpuResourceCacheSmoke.ktx2";
            artifact.Role = TextureRole::Normal;
            artifact.ColorSpace = TextureColorSpace::Linear;
            artifact.TargetProfile = profile;
            artifact.CookedFormat = cookedFormat;
            RHI::Extent2D extent { 8, 8 };
            u64 offset = 0;
            for (u32 mip = 0; mip < 4; ++mip)
            {
                u64 rowPitch = 0, byteSize = 0;
                if (!RHI::CalculateTextureSubresourceStorage(format, extent, rowPitch, byteSize))
                    return TextureArtifact {};
                artifact.Mips.push_back({ extent.Width, extent.Height, offset, byteSize });
                artifact.Payload.resize(static_cast<size_t>(offset + byteSize));
                for (u64 byte = 0; byte < byteSize; ++byte)
                    artifact.Payload[static_cast<size_t>(offset + byte)] = static_cast<u8>(seed + mip * 29u + byte % 181u);
                offset += byteSize;
                extent.Width = extent.Width > 1 ? extent.Width / 2 : 1;
                extent.Height = extent.Height > 1 ? extent.Height / 2 : 1;
            }
            return artifact;
        };

        TextureArtifact preferred = makeArtifact(501, TextureTargetProfile::DesktopBC,
            TextureCookedFormat::Bc5Unorm, RHI::Format::BC5Unorm, 11);
        TextureGpuResourceCache cache(3);
        Ref<const TextureGpuResourceBundle> first, reused, replacement, fallbackBundle;
        std::string cacheError;
        RHI::ResourceState preferredState = RHI::ResourceState::Unknown;
        const bool preferredPublished = cache.Acquire(device, preferred, nullptr, first, cacheError)
            && first && !first->UsedExplicitRgbaFallback && first->Generation == 1
            && first->UploadPlan.Payload && *first->UploadPlan.Payload == preferred.Payload
            && device.OwnsResource(first->Texture.get())
            && device.QueryResourceState(first->Texture.get(), preferredState)
            && preferredState == RHI::ResourceState::ShaderResource;
        const bool exactReuse = preferredPublished
            && cache.Acquire(device, preferred, nullptr, reused, cacheError) && reused == first;
        TextureArtifact changed = preferred;
        if (!changed.Payload.empty()) changed.Payload[17] ^= 0x5au;
        const bool replaced = exactReuse && cache.Acquire(device, changed, nullptr, replacement, cacheError)
            && replacement && replacement != first && replacement->Generation > first->Generation;

        TextureArtifact unsupported = makeArtifact(502, TextureTargetProfile::Astc,
            TextureCookedFormat::Astc4x4Unorm, RHI::Format::ASTC4x4Unorm, 47);
        TextureArtifact rgbaFallback = makeArtifact(502, TextureTargetProfile::RGBAFallback,
            TextureCookedFormat::R8G8B8A8Unorm, RHI::Format::R8G8B8A8Unorm, 83);
        RHI::ResourceState fallbackState = RHI::ResourceState::Unknown;
        const bool fallbackPublished = replaced
            && cache.Acquire(device, unsupported, &rgbaFallback, fallbackBundle, cacheError)
            && fallbackBundle && fallbackBundle->UsedExplicitRgbaFallback
            && fallbackBundle->UploadPlan.TargetProfile == TextureTargetProfile::RGBAFallback
            && fallbackBundle->UploadPlan.Payload && *fallbackBundle->UploadPlan.Payload == rgbaFallback.Payload
            && device.OwnsResource(fallbackBundle->Texture.get())
            && device.QueryResourceState(fallbackBundle->Texture.get(), fallbackState)
            && fallbackState == RHI::ResourceState::ShaderResource;
        cache.Clear();
        const bool retainedAfterClear = cache.GetEntryCount() == 0 && first && replacement && fallbackBundle
            && first->Texture && replacement->Texture && fallbackBundle->Texture
            && first->UploadPlan.Payload && replacement->UploadPlan.Payload && fallbackBundle->UploadPlan.Payload;
        const bool cachePassed = preferredPublished && exactReuse && replaced && fallbackPublished && retainedAfterClear;
        Log::Info("TextureGpuResourceCacheSmokeV1 backend=", backendName,
            ", preferred=", preferredPublished ? "pass" : "fail",
            ", reuse=", exactReuse ? "exact" : "fail",
            ", replacement=", replaced ? "pass" : "fail",
            ", fallback=", fallbackPublished ? "RGBA8" : "fail",
            ", shaderResource=", preferredState == RHI::ResourceState::ShaderResource
                && fallbackState == RHI::ResourceState::ShaderResource ? "pass" : "fail",
            ", cacheCleared=", cache.GetEntryCount() == 0 ? "pass" : "fail",
            ", retained=", retainedAfterClear ? "pass" : "fail",
            ", result=", cachePassed ? "pass" : "fail");

        AssetRegistry runtimeRegistry;
        const std::string runtimeSource = AssetRegistry::NormalizeSourcePath(
            "Engine/Generated/TextureRuntimePublicationSmoke.ktx2");
        const AssetHandle runtimeAsset = runtimeRegistry.RegisterAsset(
            AssetType::Texture, runtimeSource, "Texture Runtime Publication Smoke");
        TextureArtifact runtimePreferred = makeArtifact(runtimeAsset,
            TextureTargetProfile::DesktopBC, TextureCookedFormat::Bc5Unorm,
            RHI::Format::BC5Unorm, 101);
        runtimePreferred.SourcePath = runtimeSource;
        TextureArtifact runtimeFallback = makeArtifact(runtimeAsset,
            TextureTargetProfile::RGBAFallback, TextureCookedFormat::R8G8B8A8Unorm,
            RHI::Format::R8G8B8A8Unorm, 151);
        runtimeFallback.SourcePath = runtimeSource;
        const std::filesystem::path runtimePreferredPath = GetCookedTextureArtifactPath(
            runtimeAsset, TextureTargetProfile::DesktopBC);
        const std::filesystem::path runtimeFallbackPath = GetCookedTextureArtifactPath(
            runtimeAsset, TextureTargetProfile::RGBAFallback);
        std::error_code filesystemError;
        std::filesystem::remove(runtimePreferredPath, filesystemError);
        std::filesystem::remove(runtimeFallbackPath, filesystemError);

        std::string runtimeError;
        const bool artifactsStored = runtimeAsset != kInvalidAssetHandle
            && StoreTextureArtifact(runtimePreferredPath, runtimePreferred, runtimeError)
            && StoreTextureArtifact(runtimeFallbackPath, runtimeFallback, runtimeError);
        if (artifactsStored)
            Renderer::PublishArtifactResolvers(runtimeRegistry);
        Scope<TextureRuntimePublication> runtime = artifactsStored
            ? TextureRuntimePublication::Create(device, TextureTargetProfile::DesktopBC, 3, 3)
            : nullptr;
        const RHI::TextureBindingHandle errorHandle = runtime
            ? runtime->GetErrorHandle() : RHI::TextureBindingHandle {};
        const auto isError = [errorHandle](RHI::TextureBindingHandle handle)
        {
            return handle.Index == errorHandle.Index && handle.Generation == errorHandle.Generation;
        };
        const RHI::TextureBindingHandle initialHandle = runtime
            ? runtime->Resolve(runtimeAsset, RHI::TextureSampler::LinearWrap, runtimeError)
            : RHI::TextureBindingHandle {};
        const RHI::TextureBindingHandle alternateHandle = runtime
            ? runtime->Resolve(runtimeAsset, RHI::TextureSampler::PointClamp, runtimeError)
            : RHI::TextureBindingHandle {};
        const RHI::Texture* initialTexture = runtime && runtime->GetBindingTable()
            ? runtime->GetBindingTable()->Resolve(initialHandle).TextureResource.get() : nullptr;
        const RHI::TextureBindingView initialView = runtime && runtime->GetBindingTable()
            ? runtime->GetBindingTable()->Resolve(initialHandle) : RHI::TextureBindingView {};
        const RHI::TextureBindingView alternateView = runtime && runtime->GetBindingTable()
            ? runtime->GetBindingTable()->Resolve(alternateHandle) : RHI::TextureBindingView {};

        Scope<RHI::CommandList> firstUseList = runtime
            ? device.CreateCommandList(RHI::QueueType::Graphics, "TextureRuntimePublicationSmokeV1 First Use")
            : nullptr;
        const bool firstClosed = firstUseList && firstUseList->Begin() && firstUseList->End();
        const RHI::CompletionToken firstUse = firstClosed
            ? device.Submit(*firstUseList) : RHI::CompletionToken {};
        const bool initialPublished = runtime && !isError(initialHandle) && initialTexture
            && !isError(alternateHandle) && alternateHandle.Index != initialHandle.Index
            && alternateView.TextureResource == initialView.TextureResource
            && initialView.Sampler == RHI::TextureSampler::LinearWrap
            && alternateView.Sampler == RHI::TextureSampler::PointClamp
            && runtime->GetPublishedViewCount() == 2
            && firstUse.IsValid() && runtime->RetainAcceptedFrame(
                firstUse, { errorHandle, initialHandle, alternateHandle }, runtimeError);

        TextureArtifact runtimeChanged = runtimePreferred;
        if (runtimeChanged.Payload.size() > 17)
            runtimeChanged.Payload[17] ^= 0x5au;
        const bool replacementStored = initialPublished
            && StoreTextureArtifact(runtimePreferredPath, runtimeChanged, runtimeError);
        if (replacementStored)
            Renderer::PublishArtifactResolvers(runtimeRegistry);
        const RHI::TextureBindingHandle pendingReplacement = replacementStored
            ? runtime->Resolve(runtimeAsset, RHI::TextureSampler::LinearWrap, runtimeError)
            : RHI::TextureBindingHandle {};
        const RHI::TextureBindingHandle pendingAlternateReplacement = replacementStored
            ? runtime->Resolve(runtimeAsset, RHI::TextureSampler::PointClamp, runtimeError)
            : RHI::TextureBindingHandle {};
        const bool replacementQueued = replacementStored && isError(pendingReplacement)
            && isError(pendingAlternateReplacement)
            && runtime->GetPendingOperationCount() == 2
            && runtime->GetBindingTable()->Resolve(initialHandle).TextureResource.get() == initialTexture;
        const bool firstComplete = firstUse.IsValid() && device.WaitForCompletion(firstUse, 5000);
        const bool replacementRetired = replacementQueued && firstComplete
            && runtime->Retire(firstUse, runtimeError);
        const RHI::TextureBindingHandle replacementHandle = replacementRetired
            ? runtime->Resolve(runtimeAsset, RHI::TextureSampler::LinearWrap, runtimeError)
            : RHI::TextureBindingHandle {};
        const RHI::TextureBindingHandle alternateReplacementHandle = replacementRetired
            ? runtime->Resolve(runtimeAsset, RHI::TextureSampler::PointClamp, runtimeError)
            : RHI::TextureBindingHandle {};
        const RHI::Texture* replacementTexture = runtime && runtime->GetBindingTable()
            ? runtime->GetBindingTable()->Resolve(replacementHandle).TextureResource.get() : nullptr;
        const RHI::TextureBindingView alternateReplacementView = runtime && runtime->GetBindingTable()
            ? runtime->GetBindingTable()->Resolve(alternateReplacementHandle) : RHI::TextureBindingView {};
        const bool replacementPublished = replacementRetired && !isError(replacementHandle)
            && replacementHandle.Index == initialHandle.Index
            && replacementHandle.Generation == initialHandle.Generation
            && !isError(alternateReplacementHandle)
            && alternateReplacementHandle.Index == alternateHandle.Index
            && alternateReplacementHandle.Generation == alternateHandle.Generation
            && replacementTexture && replacementTexture != initialTexture
            && alternateReplacementView.TextureResource.get() == replacementTexture
            && alternateReplacementView.Sampler == RHI::TextureSampler::PointClamp;

        Scope<RHI::CommandList> secondUseList = replacementPublished
            ? device.CreateCommandList(RHI::QueueType::Graphics, "TextureRuntimePublicationSmokeV1 Second Use")
            : nullptr;
        const bool secondClosed = secondUseList && secondUseList->Begin() && secondUseList->End();
        const RHI::CompletionToken secondUse = secondClosed
            ? device.Submit(*secondUseList) : RHI::CompletionToken {};
        const bool secondRetained = secondUse.IsValid()
            && runtime->RetainAcceptedFrame(secondUse,
                { replacementHandle, alternateReplacementHandle }, runtimeError);
        const bool registryRemoved = secondRetained && runtimeRegistry.RemoveAsset(runtimeAsset);
        if (registryRemoved)
            Renderer::PublishArtifactResolvers(runtimeRegistry);
        const RHI::TextureBindingHandle pendingRemoval = registryRemoved
            ? runtime->Resolve(runtimeAsset, RHI::TextureSampler::LinearWrap, runtimeError)
            : RHI::TextureBindingHandle {};
        const RHI::TextureBindingHandle pendingAlternateRemoval = registryRemoved
            ? runtime->Resolve(runtimeAsset, RHI::TextureSampler::PointClamp, runtimeError)
            : RHI::TextureBindingHandle {};
        const bool removalQueued = registryRemoved && isError(pendingRemoval)
            && isError(pendingAlternateRemoval)
            && runtime->GetPendingOperationCount() == 2;
        const bool secondComplete = secondUse.IsValid() && device.WaitForCompletion(secondUse, 5000);
        const bool removalRetired = removalQueued && secondComplete
            && runtime->Retire(secondUse, runtimeError)
            && runtime->GetPublishedViewCount() == 0
            && runtime->GetBindingTable()->Resolve(replacementHandle).IsError
            && runtime->GetBindingTable()->Resolve(alternateReplacementHandle).IsError
            && isError(runtime->Resolve(runtimeAsset, RHI::TextureSampler::LinearWrap, runtimeError));
        const bool failureUsesError = runtime
            && isError(runtime->Resolve(kInvalidAssetHandle,
                RHI::TextureSampler::LinearClamp, runtimeError));

        device.WaitIdle();
        if (runtime)
            runtime->ReleaseAfterDeviceIdle();
        const bool idleReleased = runtime && runtime->GetBindingTable() == nullptr
            && runtime->GetCachedResourceCount() == 0
            && runtime->GetRetainedFrameCount() == 0;
        Renderer::ClearArtifactResolvers();
        std::filesystem::remove(runtimePreferredPath, filesystemError);
        std::filesystem::remove(runtimeFallbackPath, filesystemError);

        const bool runtimePassed = artifactsStored && initialPublished
            && replacementQueued && replacementPublished && removalQueued
            && removalRetired && failureUsesError && idleReleased;
        Log::Info("TextureRuntimePublicationSmokeV1 backend=", backendName,
            ", catalog=", artifactsStored ? "pass" : "fail",
            ", upload=", initialPublished ? "pass" : "fail",
            ", table=", initialHandle.IsValid() && !isError(initialHandle) ? "pass" : "fail",
            ", replacement=exact-token-", replacementPublished ? "pass" : "fail",
            ", removal=exact-token-", removalRetired ? "pass" : "fail",
            ", failure=", failureUsesError ? "error-resource" : "invalid",
            ", idleRelease=", idleReleased ? "pass" : "fail",
            ", result=", runtimePassed ? "pass" : "fail");
        return mipUploadPassed && cachePassed && runtimePassed;
    }

    bool NVRHIRenderBackend::RunRHISampledTextureTableSmoke(RHI::Device& device, std::string_view backendName)
    {
        constexpr u32 width = 32, height = 24, tableCapacity = 2;
        const bool vulkan = device.GetCapabilities().ActiveBackend == RHI::Backend::NVRHIVulkan;
        struct Vertex { float Position[3]; float Color[3]; float UV[2]; };
        struct Constants { float ViewProjection[16]; };
        const std::array<Vertex, 3> vertices {{{ { -0.7f, -0.6f, 0.5f }, {}, { 0.0f, 1.0f } }, { { 0.7f, -0.6f, 0.5f }, {}, { 1.0f, 1.0f } }, { { 0.0f, 0.7f, 0.5f }, {}, { 0.5f, 0.0f } } }};
        const std::array<u16, 3> indices {{ 0, 1, 2 }};
        const Constants constants {{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }};
        const std::string source = R"(
cbuffer ViewportConstants : register(b0, space0) { row_major float4x4 ViewProjection; };
Texture2D ReadOnlyTextures[2] : register(t0, space1);
SamplerState ReadOnlySamplers[2] : register(s0, space1);
struct VertexInput { float3 Position : POSITION; float3 Color : COLOR; float2 UV : TEXCOORD; };
struct PixelInput { float4 Position : SV_Position; float2 UV : TEXCOORD; };
PixelInput VSMain(VertexInput input) { PixelInput output; output.Position = mul(float4(input.Position, 1.0), ViewProjection); output.UV = input.UV; return output; }
float4 PSMain(PixelInput input) : SV_Target { return ReadOnlyTextures[1].SampleLevel(ReadOnlySamplers[1], input.UV, 0.0); }
)";
        auto makeRequest = [&source](RHI::ShaderStage stage, const char* entry)
        {
            PortableShaderRequest request;
            request.SourceName = "RHIReadOnlySampledTableSmokeV1.slang"; request.Source = source; request.EntryPoint = entry; request.Stage = stage;
#ifdef _WIN32
            request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
            request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
#else
            request.Targets = { PortableShaderTarget::Spirv };
#endif
            request.CompilerIdentity = "Slang"; request.CompilerVersion = "2026.13.1"; request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
            request.ExpectedLayout = {
                { "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0}", 1, 64, 0, 0 },
                { "ReadOnlySamplers", 's', 0, 1, stage, "SamplerState", "sampler", tableCapacity, 0, 0, 0 },
                { "ReadOnlyTextures", 't', 0, 1, stage, "Texture2D", "float32x4", tableCapacity, 0, 1, 4 }
            };
            if (stage == RHI::ShaderStage::Vertex)
                request.ExpectedVertexInputs = {{ "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 }, { "Color", "COLOR", 0, 1, "float32x3", 12, 1, 3 }, { "UV", "TEXCOORD", 0, 2, "float32x2", 8, 1, 2 }};
            return request;
        };
        SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
        const PortableShaderRequest vertexRequest = makeRequest(RHI::ShaderStage::Vertex, "VSMain");
        const PortableShaderRequest pixelRequest = makeRequest(RHI::ShaderStage::Pixel, "PSMain");
        const PortableShaderPackage vertexPackage = compiler.Compile(vertexRequest);
        const PortableShaderPackage pixelPackage = compiler.Compile(pixelRequest);
        for (const PortableShaderDiagnostic& diagnostic : vertexPackage.Diagnostics)
            Log::Error("RHIReadOnlySampledTableSmokeV1 vertex diagnostic: ", diagnostic.Target, ": ", diagnostic.Message);
        for (const PortableShaderDiagnostic& diagnostic : pixelPackage.Diagnostics)
            Log::Error("RHIReadOnlySampledTableSmokeV1 pixel diagnostic: ", diagnostic.Target, ": ", diagnostic.Message);
        std::string validationError;
        const bool packages = PortableShaderContract::ValidatePackage(vertexRequest, vertexPackage, validationError)
            && PortableShaderContract::ValidatePackage(pixelRequest, pixelPackage, validationError);
        RHI::ShaderDescription vs; vs.DebugName = "RHIReadOnlySampledTableSmokeV1 VS"; vs.SourceName = vertexRequest.SourceName; vs.EntryPoint = vulkan ? "main" : "VSMain"; vs.Stage = RHI::ShaderStage::Vertex; vs.BinaryFormat = vulkan ? RHI::ShaderBinaryFormat::Spirv : RHI::ShaderBinaryFormat::Dxil; vs.Binary = vulkan ? vertexPackage.Spirv : vertexPackage.Dxil; vs.Reflection = vertexPackage.Reflection;
        RHI::ShaderDescription ps; ps.DebugName = "RHIReadOnlySampledTableSmokeV1 PS"; ps.SourceName = pixelRequest.SourceName; ps.EntryPoint = vulkan ? "main" : "PSMain"; ps.Stage = RHI::ShaderStage::Pixel; ps.BinaryFormat = vulkan ? RHI::ShaderBinaryFormat::Spirv : RHI::ShaderBinaryFormat::Dxil; ps.Binary = vulkan ? pixelPackage.Spirv : pixelPackage.Dxil; ps.Reflection = pixelPackage.Reflection;
        Scope<RHI::Shader> vertexShader = packages ? device.CreateShader(vs) : nullptr;
        Scope<RHI::Shader> pixelShader = packages ? device.CreateShader(ps) : nullptr;
        RHI::PipelineDescription pipelineDescription; pipelineDescription.DebugName = "RHIReadOnlySampledTableSmokeV1 Pipeline"; pipelineDescription.VertexShader = vertexShader.get(); pipelineDescription.PixelShader = pixelShader.get();
        pipelineDescription.VertexInputs = {{ "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Position) }, { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Color) }, { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(Vertex, UV) }};
        pipelineDescription.VertexStrideBytes = sizeof(Vertex);
        pipelineDescription.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }}; pipelineDescription.SampledTextureTable = RHI::SampledTextureTableBinding { tableCapacity }; pipelineDescription.ColorFormat = RHI::Format::R8G8B8A8Unorm; pipelineDescription.DepthFormat = RHI::Format::D32Float; pipelineDescription.DepthTestEnable = false; pipelineDescription.DepthWriteEnable = false; pipelineDescription.RasterCullMode = RHI::CullMode::None;
        Scope<RHI::Pipeline> pipeline = vertexShader && pixelShader ? device.CreatePipeline(pipelineDescription) : nullptr;
        auto createBuffer = [&device](const char* name, u64 size, u32 stride, RHI::BufferUsage usage) { RHI::BufferDescription d; d.DebugName = name; d.SizeBytes = size; d.StrideBytes = stride; d.Usage = static_cast<RHI::BufferUsage>(static_cast<u32>(usage) | static_cast<u32>(RHI::BufferUsage::CopyDest)); return device.CreateBuffer(d); };
        Scope<RHI::Buffer> vertexBuffer = createBuffer("RHIReadOnlySampledTableSmokeV1 Vertices", sizeof(vertices), sizeof(Vertex), RHI::BufferUsage::Vertex);
        Scope<RHI::Buffer> indexBuffer = createBuffer("RHIReadOnlySampledTableSmokeV1 Indices", sizeof(indices), sizeof(u16), RHI::BufferUsage::Index);
        Scope<RHI::Buffer> constantBuffer = createBuffer("RHIReadOnlySampledTableSmokeV1 Constants", sizeof(constants), 0, RHI::BufferUsage::Constant);
        auto createSampledTexture = [&device](const char* name, const std::array<u8, 4>& pixel) -> Ref<RHI::Texture>
        {
            RHI::TextureDescription description; description.DebugName = name; description.Extent = { 1, 1 }; description.TextureFormat = RHI::Format::R8G8B8A8Unorm; description.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::CopyDest) | static_cast<u32>(RHI::TextureUsage::ShaderResource)); description.InitialState = RHI::ResourceState::CopyDest;
            Scope<RHI::Texture> texture = device.CreateTexture(description); if (!texture) return nullptr;
            const Ref<std::vector<u8>> bytes = CreateRef<std::vector<u8>>(pixel.begin(), pixel.end());
            if (!device.UploadTexture(*texture, { { 1, 1 }, RHI::Format::R8G8B8A8Unorm, 4, bytes })) return nullptr;
            return Ref<RHI::Texture>(texture.release());
        };
        const Ref<RHI::Texture> errorTexture = createSampledTexture("RHIReadOnlySampledTableSmokeV1 Error", {{ 255, 0, 255, 255 }});
        const Ref<RHI::Texture> sampledTexture = createSampledTexture("RHIReadOnlySampledTableSmokeV1 Sample", {{ 51, 102, 204, 255 }});
        Scope<RHI::TextureBindingTable> table = errorTexture ? RHI::TextureBindingTable::Create(device, { tableCapacity, errorTexture, RHI::TextureSampler::PointClamp }) : nullptr;
        const RHI::TextureBindingHandle sampledHandle = table && sampledTexture ? table->Allocate(sampledTexture, RHI::TextureSampler::LinearClamp) : RHI::TextureBindingHandle {};
        RHI::TextureDescription colorDescription; colorDescription.DebugName = "RHIReadOnlySampledTableSmokeV1 Color"; colorDescription.Extent = { width, height }; colorDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm; colorDescription.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource));
        RHI::TextureDescription depthDescription = colorDescription; depthDescription.DebugName = "RHIReadOnlySampledTableSmokeV1 Depth"; depthDescription.TextureFormat = RHI::Format::D32Float; depthDescription.Usage = RHI::TextureUsage::DepthStencil;
        Scope<RHI::Texture> color = device.CreateTexture(colorDescription); Scope<RHI::Texture> depth = device.CreateTexture(depthDescription);
        const bool uploads = vertexBuffer && indexBuffer && constantBuffer && sampledHandle.Index == 1
            && device.UploadBuffer(*vertexBuffer, vertices.data(), sizeof(vertices)) && device.UploadBuffer(*indexBuffer, indices.data(), sizeof(indices)) && device.UploadBuffer(*constantBuffer, &constants, sizeof(constants));
        Scope<RHI::CommandList> list = uploads && pipeline && table && color && depth ? device.CreateCommandList(RHI::QueueType::Graphics, "RHIReadOnlySampledTableSmokeV1") : nullptr;
        RHI::ViewportClear clear; clear.Color[0] = 0.04f; clear.Color[1] = 0.05f; clear.Color[2] = 0.06f; clear.Color[3] = 1.0f;
        const bool recording = list && list->Begin() && list->BindViewportOutputs(*color, depth.get())
            && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget)
            && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite)
            && list->ClearViewportOutputs(clear)
            && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget)
            && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite);
        bool bound = false;
        if (recording) { list->SetGraphicsPipeline(*pipeline); list->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); list->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) }); list->SetVertexBuffer(0, *vertexBuffer); list->SetIndexBuffer(*indexBuffer, RHI::IndexFormat::Uint16); list->SetGraphicsConstantBuffer(0, *constantBuffer); bound = list->BindGraphicsSampledTextureTable(*table); if (bound) list->DrawIndexed(3, 1, 0, 0, 0); }
        const bool submitted = recording && bound && list->TransitionTexture(*color, RHI::ResourceState::CopySource) && list->End() && device.SubmitAndWait(*list);
        RHI::TextureReadback readback; const bool read = submitted && device.ReadbackTexture(*color, readback);
        const size_t center = read ? static_cast<size_t>(12) * readback.RowPitchBytes + 16u * 4u : 0;
        const std::array<u8, 4> actual = read && readback.Data.size() >= center + 4
            ? std::array<u8, 4> {{ readback.Data[center], readback.Data[center + 1], readback.Data[center + 2], readback.Data[center + 3] }} : std::array<u8, 4> {};
        const bool sampled = read && readback.Data.size() >= center + 4 && std::abs(static_cast<int>(readback.Data[center]) - 51) <= 2 && std::abs(static_cast<int>(readback.Data[center + 1]) - 102) <= 2 && std::abs(static_cast<int>(readback.Data[center + 2]) - 204) <= 2 && readback.Data[center + 3] == 255;
        Log::Info("RHIReadOnlySampledTableSmokeV1 backend=", backendName, ", package=", packages ? "pass" : "fail", ", pipeline=", pipeline ? "pass" : "fail", ", capacity=", tableCapacity, ", bind=", bound ? "pass" : "fail", ", submit=", submitted ? "pass" : "fail", ", readback=", read ? "pass" : "fail", ", sampledPixel=", sampled ? "pass" : "fail", ", actual=", static_cast<u32>(actual[0]), ",", static_cast<u32>(actual[1]), ",", static_cast<u32>(actual[2]), ",", static_cast<u32>(actual[3]), ", result=", sampled ? "pass" : "fail", ", validation=", validationError);
        return sampled;
    }

    bool NVRHIRenderBackend::RunRHIFixedStructuredBufferSmoke(
        RHI::Device& device, std::string_view backendName)
    {
        constexpr u32 width = 32, height = 24, tableCapacity = 2;
        const bool vulkan = device.GetCapabilities().ActiveBackend == RHI::Backend::NVRHIVulkan;
        struct Vertex { float Position[3]; };
        struct Constants { float ViewProjection[16]; };
        const std::array<Vertex, 3> vertices {{
            {{ -0.7f, -0.6f, 0.5f }}, {{ 0.7f, -0.6f, 0.5f }}, {{ 0.0f, 0.7f, 0.5f }}
        }};
        const std::array<u16, 3> indices {{ 0, 1, 2 }};
        const Constants constants {{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        }};
        const std::array<std::array<u32, 4>, 2> words {{
            {{ 0u, 0u, 0u, 0u }}, {{ 18u, 52u, 86u, 120u }}
        }};
        const std::array<u8, 4> expectedPixel {{ 33, 82, 154, 255 }};
        const std::string vertexSource = R"(
cbuffer ViewportConstants : register(b0, space0) { row_major float4x4 ViewProjection; };
struct VertexInput { float3 Position : POSITION; };
struct PixelInput { float4 Position : SV_Position; };
PixelInput VSMain(VertexInput input) { PixelInput output; output.Position = mul(float4(input.Position, 1.0), ViewProjection); return output; }
)";
        const std::string pixelSource = R"(
Texture2D ReadOnlyTextures[2] : register(t0, space1);
SamplerState ReadOnlySamplers[2] : register(s0, space1);
StructuredBuffer<uint4> FixedWords : register(t0, space3);
struct PixelInput { float4 Position : SV_Position; };
float4 PSMain(PixelInput input) : SV_Target
{
    float3 sampled = ReadOnlyTextures[1].SampleLevel(ReadOnlySamplers[1], float2(0.5, 0.5), 0.0).rgb;
    uint3 sampledBytes = uint3(round(saturate(sampled) * 255.0));
    uint3 resultBytes = sampledBytes ^ FixedWords[1].xyz;
    return float4(float3(resultBytes) / 255.0, 1.0);
}
)";
        auto makeRequest = [](const std::string& source, const char* sourceName,
                               RHI::ShaderStage stage, const char* entry)
        {
            PortableShaderRequest request;
            request.SourceName = sourceName;
            request.Source = source;
            request.EntryPoint = entry;
            request.Stage = stage;
#ifdef _WIN32
            request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
            request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
#else
            request.Targets = { PortableShaderTarget::Spirv };
#endif
            request.CompilerIdentity = "Slang";
            request.CompilerVersion = "2026.13.1";
            request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
            return request;
        };
        PortableShaderRequest vertexRequest = makeRequest(vertexSource,
            "RHIFixedStructuredBufferV1.vs.slang", RHI::ShaderStage::Vertex, "VSMain");
        vertexRequest.ExpectedLayout = {
            { "ViewportConstants", 'b', 0, 0, RHI::ShaderStage::Vertex,
                "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0}", 1, 64, 0, 0 }
        };
        vertexRequest.ExpectedVertexInputs = {
            { "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 }
        };
        PortableShaderRequest pixelRequest = makeRequest(pixelSource,
            "RHIFixedStructuredBufferV1.ps.slang", RHI::ShaderStage::Pixel, "PSMain");
        pixelRequest.ExpectedLayout = {
            { "ReadOnlySamplers", 's', 0, 1, RHI::ShaderStage::Pixel,
                "SamplerState", "sampler", tableCapacity, 0, 0, 0 },
            { "ReadOnlyTextures", 't', 0, 1, RHI::ShaderStage::Pixel,
                "Texture2D", "float32x4", tableCapacity, 0, 1, 4 },
            { "FixedWords", 't', 0, 3, RHI::ShaderStage::Pixel,
                "StructuredBuffer", "uint32x4", 1, 0, 1, 4 }
        };

        SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
        const PortableShaderPackage vertexPackage = compiler.Compile(vertexRequest);
        const PortableShaderPackage pixelPackage = compiler.Compile(pixelRequest);
        for (const PortableShaderDiagnostic& diagnostic : vertexPackage.Diagnostics)
            Log::Error("RHIFixedStructuredBufferV1 vertex diagnostic: ",
                diagnostic.Target, ": ", diagnostic.Message);
        for (const PortableShaderDiagnostic& diagnostic : pixelPackage.Diagnostics)
            Log::Error("RHIFixedStructuredBufferV1 pixel diagnostic: ",
                diagnostic.Target, ": ", diagnostic.Message);
        std::string validationError;
        const bool packages = PortableShaderContract::ValidatePackage(
                vertexRequest, vertexPackage, validationError)
            && PortableShaderContract::ValidatePackage(
                pixelRequest, pixelPackage, validationError);

        RHI::ShaderDescription vs;
        vs.DebugName = "RHIFixedStructuredBufferV1 VS";
        vs.SourceName = vertexRequest.SourceName;
        vs.EntryPoint = vulkan ? "main" : "VSMain";
        vs.Stage = RHI::ShaderStage::Vertex;
        vs.BinaryFormat = vulkan ? RHI::ShaderBinaryFormat::Spirv : RHI::ShaderBinaryFormat::Dxil;
        vs.Binary = vulkan ? vertexPackage.Spirv : vertexPackage.Dxil;
        vs.Reflection = vertexPackage.Reflection;
        RHI::ShaderDescription ps;
        ps.DebugName = "RHIFixedStructuredBufferV1 PS";
        ps.SourceName = pixelRequest.SourceName;
        ps.EntryPoint = vulkan ? "main" : "PSMain";
        ps.Stage = RHI::ShaderStage::Pixel;
        ps.BinaryFormat = vulkan ? RHI::ShaderBinaryFormat::Spirv : RHI::ShaderBinaryFormat::Dxil;
        ps.Binary = vulkan ? pixelPackage.Spirv : pixelPackage.Dxil;
        ps.Reflection = pixelPackage.Reflection;
        Scope<RHI::Shader> vertexShader = packages ? device.CreateShader(vs) : nullptr;
        Scope<RHI::Shader> pixelShader = packages ? device.CreateShader(ps) : nullptr;

        RHI::PipelineDescription pipelineDescription;
        pipelineDescription.DebugName = "RHIFixedStructuredBufferV1 Pipeline";
        pipelineDescription.VertexShader = vertexShader.get();
        pipelineDescription.PixelShader = pixelShader.get();
        pipelineDescription.VertexInputs = {
            { "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Position) }
        };
        pipelineDescription.VertexStrideBytes = sizeof(Vertex);
        pipelineDescription.ConstantBufferBindings = {
            { 0, 0, RHI::ShaderStage::AllGraphics }
        };
        pipelineDescription.SampledTextureTable = RHI::SampledTextureTableBinding { tableCapacity };
        pipelineDescription.FixedReadOnlyStructuredBuffer =
            RHI::FixedReadOnlyStructuredBufferBinding {};
        pipelineDescription.ColorFormat = RHI::Format::R8G8B8A8Unorm;
        pipelineDescription.DepthFormat = RHI::Format::Unknown;
        pipelineDescription.DepthTestEnable = false;
        pipelineDescription.DepthWriteEnable = false;
        pipelineDescription.RasterCullMode = RHI::CullMode::None;
        Scope<RHI::Pipeline> pipeline = vertexShader && pixelShader
            ? device.CreatePipeline(pipelineDescription) : nullptr;

        RHI::ShaderDescription malformedPs = ps;
        const auto malformedBinding = std::find_if(malformedPs.Reflection.begin(),
            malformedPs.Reflection.end(), [](const auto& binding)
            {
                return binding.ResourceKind == "StructuredBuffer";
            });
        if (malformedBinding != malformedPs.Reflection.end())
        {
            malformedBinding->TypeShape = "uint32x3";
            malformedBinding->Columns = 3;
        }
        Scope<RHI::Shader> malformedPixelShader = packages
            ? device.CreateShader(malformedPs) : nullptr;
        RHI::PipelineDescription malformedPipelineDescription = pipelineDescription;
        malformedPipelineDescription.PixelShader = malformedPixelShader.get();
        Scope<RHI::Pipeline> malformedPipeline = malformedPixelShader
            ? device.CreatePipeline(malformedPipelineDescription) : nullptr;
        const bool malformedReflectionRejected = malformedPixelShader && !malformedPipeline;

        auto createBuffer = [&device](const char* name, u64 size, u32 stride,
                                RHI::BufferUsage usage, RHI::ResourceState initialState)
        {
            RHI::BufferDescription description;
            description.DebugName = name;
            description.SizeBytes = size;
            description.StrideBytes = stride;
            description.Usage = usage;
            description.InitialState = initialState;
            return device.CreateBuffer(description);
        };
        const auto copyDestinationUsage = [](RHI::BufferUsage usage)
        {
            return static_cast<RHI::BufferUsage>(static_cast<u32>(usage)
                | static_cast<u32>(RHI::BufferUsage::CopyDest));
        };
        Scope<RHI::Buffer> vertexBuffer = createBuffer("RHIFixedStructuredBufferV1 Vertices",
            sizeof(vertices), sizeof(Vertex), copyDestinationUsage(RHI::BufferUsage::Vertex),
            RHI::ResourceState::Common);
        Scope<RHI::Buffer> indexBuffer = createBuffer("RHIFixedStructuredBufferV1 Indices",
            sizeof(indices), sizeof(u16), copyDestinationUsage(RHI::BufferUsage::Index),
            RHI::ResourceState::Common);
        Scope<RHI::Buffer> constantBuffer = createBuffer("RHIFixedStructuredBufferV1 Constants",
            sizeof(constants), 0, copyDestinationUsage(RHI::BufferUsage::Constant),
            RHI::ResourceState::Common);
        Scope<RHI::Buffer> structuredBuffer = createBuffer("RHIFixedStructuredBufferV1 Words",
            sizeof(words), RHI::kFixedReadOnlyStructuredBufferStrideBytes,
            copyDestinationUsage(RHI::BufferUsage::Structured), RHI::ResourceState::CopyDest);
        Scope<RHI::Buffer> wrongUsageBuffer = createBuffer("RHIFixedStructuredBufferV1 Wrong Usage",
            sizeof(words), RHI::kFixedReadOnlyStructuredBufferStrideBytes,
            RHI::BufferUsage::CopyDest, RHI::ResourceState::ShaderResource);
        Scope<RHI::Buffer> wrongStrideBuffer = createBuffer("RHIFixedStructuredBufferV1 Wrong Stride",
            sizeof(words), 8, copyDestinationUsage(RHI::BufferUsage::Structured),
            RHI::ResourceState::CopyDest);
        Scope<RHI::Buffer> wrongShapeBuffer = createBuffer("RHIFixedStructuredBufferV1 Wrong Shape",
            20, RHI::kFixedReadOnlyStructuredBufferStrideBytes,
            copyDestinationUsage(RHI::BufferUsage::Structured), RHI::ResourceState::CopyDest);
        const bool malformedBuffersRejected = !wrongStrideBuffer && !wrongShapeBuffer;

        auto createSampledTexture = [&device](const char* name,
                                        const std::array<u8, 4>& pixel) -> Ref<RHI::Texture>
        {
            RHI::TextureDescription description;
            description.DebugName = name;
            description.Extent = { 1, 1 };
            description.TextureFormat = RHI::Format::R8G8B8A8Unorm;
            description.Usage = static_cast<RHI::TextureUsage>(
                static_cast<u32>(RHI::TextureUsage::CopyDest)
                | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            description.InitialState = RHI::ResourceState::CopyDest;
            Scope<RHI::Texture> texture = device.CreateTexture(description);
            if (!texture) return nullptr;
            const Ref<std::vector<u8>> bytes = CreateRef<std::vector<u8>>(
                pixel.begin(), pixel.end());
            if (!device.UploadTexture(*texture,
                    { { 1, 1 }, RHI::Format::R8G8B8A8Unorm, 4, bytes }))
                return nullptr;
            return Ref<RHI::Texture>(texture.release());
        };
        const Ref<RHI::Texture> errorTexture = createSampledTexture(
            "RHIFixedStructuredBufferV1 Error", {{ 255, 0, 255, 255 }});
        const Ref<RHI::Texture> sampledTexture = createSampledTexture(
            "RHIFixedStructuredBufferV1 Sample", {{ 51, 102, 204, 255 }});
        Scope<RHI::TextureBindingTable> table = errorTexture
            ? RHI::TextureBindingTable::Create(device,
                { tableCapacity, errorTexture, RHI::TextureSampler::PointClamp })
            : nullptr;
        const RHI::TextureBindingHandle sampledHandle = table && sampledTexture
            ? table->Allocate(sampledTexture, RHI::TextureSampler::LinearClamp)
            : RHI::TextureBindingHandle {};
        const bool uploaded = vertexBuffer && indexBuffer && constantBuffer && structuredBuffer
            && wrongUsageBuffer && sampledHandle.Index == 1
            && device.UploadBuffer(*vertexBuffer, vertices.data(), sizeof(vertices))
            && device.UploadBuffer(*indexBuffer, indices.data(), sizeof(indices))
            && device.UploadBuffer(*constantBuffer, &constants, sizeof(constants))
            && device.UploadBuffer(*structuredBuffer, words.data(), sizeof(words));
        Scope<RHI::CommandList> transitionList = uploaded
            ? device.CreateCommandList(RHI::QueueType::Graphics,
                "RHIFixedStructuredBufferV1 Transition")
            : nullptr;
        const bool transitioned = transitionList && transitionList->Begin()
            && transitionList->TransitionBuffer(
                *structuredBuffer, RHI::ResourceState::ShaderResource)
            && transitionList->End() && device.SubmitAndWait(*transitionList);

        enum class DrawMode { Missing, Stale, Valid };
        std::array<u8, 4> validActual {};
        bool beforePipelineRejected = false;
        bool wrongUsageRejected = false;
        bool tableCoexistence = true;
        auto runDraw = [&](DrawMode mode)
        {
            RHI::TextureDescription colorDescription;
            colorDescription.DebugName = "RHIFixedStructuredBufferV1 Color";
            colorDescription.Extent = { width, height };
            colorDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm;
            colorDescription.Usage = static_cast<RHI::TextureUsage>(
                static_cast<u32>(RHI::TextureUsage::RenderTarget)
                | static_cast<u32>(RHI::TextureUsage::CopySource));
            Scope<RHI::Texture> color = device.CreateTexture(colorDescription);
            Scope<RHI::CommandList> list = color && pipeline && table
                ? device.CreateCommandList(RHI::QueueType::Graphics,
                    "RHIFixedStructuredBufferV1 Draw")
                : nullptr;
            RHI::ViewportClear clear;
            // Zero is exactly representable through both float clear and UNORM
            // readback, so a rejected draw has one backend-independent oracle.
            clear.Color[0] = 0.0f;
            clear.Color[1] = 0.0f;
            clear.Color[2] = 0.0f;
            clear.Color[3] = 1.0f;
            clear.ClearDepth = false;
            const bool recording = list && list->Begin()
                && list->BindViewportOutputs(*color, nullptr)
                && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget)
                && list->ClearViewportOutputs(clear);
            if (!recording) return false;
            if (mode == DrawMode::Missing)
                beforePipelineRejected = !list->BindGraphicsReadOnlyStructuredBuffer(
                    *structuredBuffer);
            const auto bindCommon = [&]()
            {
                list->SetGraphicsPipeline(*pipeline);
                list->SetViewport({ 0.0f, 0.0f, static_cast<float>(width),
                    static_cast<float>(height), 0.0f, 1.0f });
                list->SetScissorRect({ 0, 0, static_cast<int>(width),
                    static_cast<int>(height) });
                list->SetVertexBuffer(0, *vertexBuffer);
                list->SetIndexBuffer(*indexBuffer, RHI::IndexFormat::Uint16);
                list->SetGraphicsConstantBuffer(0, *constantBuffer);
                const bool tableBound = list->BindGraphicsSampledTextureTable(*table);
                tableCoexistence = tableCoexistence && tableBound;
                return tableBound;
            };
            bool bindings = bindCommon();
            if (mode == DrawMode::Stale)
            {
                bindings = bindings
                    && list->BindGraphicsReadOnlyStructuredBuffer(*structuredBuffer)
                    && bindCommon();
            }
            else if (mode == DrawMode::Valid)
            {
                wrongUsageRejected = !list->BindGraphicsReadOnlyStructuredBuffer(
                    *wrongUsageBuffer);
                bindings = bindings && wrongUsageRejected
                    && list->BindGraphicsReadOnlyStructuredBuffer(*structuredBuffer);
            }
            if (bindings)
                list->DrawIndexed(3, 1, 0, 0, 0);
            const bool submitted = bindings
                && list->TransitionTexture(*color, RHI::ResourceState::CopySource)
                && list->End() && device.SubmitAndWait(*list);
            RHI::TextureReadback readback;
            const bool read = submitted && device.ReadbackTexture(*color, readback);
            const bool readbackShape = read
                && readback.Extent.Width == width && readback.Extent.Height == height
                && readback.TextureFormat == RHI::Format::R8G8B8A8Unorm
                && readback.RowPitchBytes == width * 4u
                && readback.Data.size() == static_cast<size_t>(readback.RowPitchBytes) * height;
            const size_t center = readbackShape
                ? static_cast<size_t>(height / 2) * readback.RowPitchBytes
                    + static_cast<size_t>(width / 2) * 4
                : 0;
            if (!readbackShape || readback.Data.size() < center + 4) return false;
            const std::array<u8, 4> actual {{
                readback.Data[center], readback.Data[center + 1],
                readback.Data[center + 2], readback.Data[center + 3]
            }};
            if (mode == DrawMode::Valid)
            {
                validActual = actual;
                return std::equal(actual.begin(), actual.end(), expectedPixel.begin());
            }
            const std::array<u8, 4> expectedClear {{
                static_cast<u8>(std::lround(clear.Color[0] * 255.0f)),
                static_cast<u8>(std::lround(clear.Color[1] * 255.0f)),
                static_cast<u8>(std::lround(clear.Color[2] * 255.0f)),
                255
            }};
            return std::equal(actual.begin(), actual.end(), expectedClear.begin());
        };

        const bool missingRejected = transitioned && runDraw(DrawMode::Missing);
        const bool staleRejected = missingRejected && runDraw(DrawMode::Stale);
        const bool readbackPassed = staleRejected && runDraw(DrawMode::Valid);
        const bool passed = packages && pipeline && malformedReflectionRejected
            && malformedBuffersRejected && beforePipelineRejected && wrongUsageRejected
            && missingRejected && staleRejected && tableCoexistence && readbackPassed;
        Log::Info("RHIFixedStructuredBufferV1 backend=", backendName,
            " declaration=exact pixel=t0-space3-uint4 stride=16",
            " malformedReflection=", malformedReflectionRejected ? "rejected" : "accepted",
            " malformedBuffer=", malformedBuffersRejected ? "rejected" : "accepted",
            " missing=", missingRejected && beforePipelineRejected ? "rejected" : "accepted",
            " wrongUsage=", wrongUsageRejected ? "rejected" : "accepted",
            " pipelineInvalidation=", staleRejected ? "rejected-stale" : "failed",
            " coexistence=", tableCoexistence ? "sampled-table-preserved" : "failed",
            " readback=", static_cast<u32>(validActual[0]), ",",
            static_cast<u32>(validActual[1]), ",", static_cast<u32>(validActual[2]), ",",
            static_cast<u32>(validActual[3]), " expected=33,82,154,255 result=",
            passed ? "pass" : "fail", " validation=", validationError);
        return passed;
    }

    bool NVRHIRenderBackend::RunRHIMaterialTextureShaderSmoke(
        RHI::Device& device, std::string_view backendName)
    {
        constexpr u32 width = 64, height = 12, bandHeight = 4, tableCapacity = 8;
        const bool vulkan = device.GetCapabilities().ActiveBackend == RHI::Backend::NVRHIVulkan;
        struct Vertex { float Position[3]; float Normal[3]; float Color[3]; float UV[2]; };
        using Constants = SceneSurfaceConstants;
        const std::array<Vertex, 3> vertices {{
            {{ -1.0f, -1.0f, 0.5f }, { 0.0f, 0.0f, 1.0f }, {}, {}},
            {{ -1.0f, 3.0f, 0.5f }, { 0.0f, 0.0f, 1.0f }, {}, {}},
            {{ 3.0f, -1.0f, 0.5f }, { 0.0f, 0.0f, 1.0f }, {}, {}}
        }};
        const std::array<u16, 3> indices {{ 0, 1, 2 }};

        ShaderSourceFile source = ShaderLibrary::LoadSource(
            "Engine/Shaders/EditorViewport.hlsl", "Material texture shader readback");
        auto makeRequest = [&source](RHI::ShaderStage stage, const char* entry)
        {
            PortableShaderRequest request;
            request.SourceName = source.ResolvedPath.string();
            request.Source = source.Source;
            request.EntryPoint = entry;
            request.Stage = stage;
            request.Defines = { "GE_READ_ONLY_TEXTURE_CAPACITY=8" };
#ifdef _WIN32
            request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
            request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
#else
            request.Targets = { PortableShaderTarget::Spirv };
#endif
            request.CompilerIdentity = "Slang";
            request.CompilerVersion = "2026.13.1";
            request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
            request.ExpectedLayout = {
                { "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0,NormalTransform:float32x4x4:row-major@64,BaseColorAndAlphaCutoff:float32x4@128,EmissiveAndStrength:float32x4@144,SurfaceFactors:float32x4@160,CallistoFactors:float32x4@176,TextureIndices0:uint32x4@192,TextureIndices1:uint32x4@208,TextureState:uint32x4@224,MaterialState:uint32x4@240,ModelView:float32x4x4:row-major@256,NormalViewTransform:float32x4x4:row-major@320,ShadowViewProjection:float32x4x4:row-major@384,ShadowParameters:float32x4@448,ShadowState:uint32x4@464,SkyIrradianceUpper:float32x4@480,SkyIrradianceLower:float32x4@496}", 1, 512, 0, 0 },
                { "ReadOnlySamplers", 's', 0, 1, stage, "SamplerState", "sampler", tableCapacity, 0, 0, 0 },
                { "ReadOnlyTextures", 't', 0, 1, stage, "Texture2D", "float32x4", tableCapacity, 0, 1, 4 }
            };
            if (stage == RHI::ShaderStage::Vertex)
            {
                request.ExpectedVertexInputs = {
                    { "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 },
                    { "Normal", "NORMAL", 0, 1, "float32x3", 12, 1, 3 },
                    { "Color", "COLOR", 0, 2, "float32x3", 12, 1, 3 },
                    { "UV", "TEXCOORD", 0, 3, "float32x2", 8, 1, 2 }
                };
            }
            return request;
        };
        const PortableShaderRequest vertexRequest = makeRequest(RHI::ShaderStage::Vertex, "VSMain");
        const PortableShaderRequest pixelRequest = makeRequest(RHI::ShaderStage::Pixel, "PSMaterialProbe");
        SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
        const PortableShaderPackage vertexPackage = source.Status == ShaderSourceStatus::Loaded
            ? compiler.Compile(vertexRequest) : PortableShaderPackage {};
        const PortableShaderPackage pixelPackage = source.Status == ShaderSourceStatus::Loaded
            ? compiler.Compile(pixelRequest) : PortableShaderPackage {};
        for (const PortableShaderDiagnostic& diagnostic : vertexPackage.Diagnostics)
            Log::Error("SceneMaterialTextureShaderReadbackV1 vertex diagnostic: ", diagnostic.Target, ": ", diagnostic.Message);
        for (const PortableShaderDiagnostic& diagnostic : pixelPackage.Diagnostics)
            Log::Error("SceneMaterialTextureShaderReadbackV1 pixel diagnostic: ", diagnostic.Target, ": ", diagnostic.Message);
        std::string validationError;
        const bool packages = PortableShaderContract::ValidatePackage(
                vertexRequest, vertexPackage, validationError)
            && PortableShaderContract::ValidatePackage(
                pixelRequest, pixelPackage, validationError);

        RHI::ShaderDescription vs;
        vs.DebugName = "SceneMaterialTextureShaderReadbackV1 VS";
        vs.SourceName = vertexRequest.SourceName;
        vs.EntryPoint = vulkan ? "main" : "VSMain";
        vs.Stage = RHI::ShaderStage::Vertex;
        vs.BinaryFormat = vulkan ? RHI::ShaderBinaryFormat::Spirv : RHI::ShaderBinaryFormat::Dxil;
        vs.Binary = vulkan ? vertexPackage.Spirv : vertexPackage.Dxil;
        vs.Reflection = vertexPackage.Reflection;
        RHI::ShaderDescription ps;
        ps.DebugName = "SceneMaterialTextureShaderReadbackV1 PS";
        ps.SourceName = pixelRequest.SourceName;
        ps.EntryPoint = vulkan ? "main" : "PSMaterialProbe";
        ps.Stage = RHI::ShaderStage::Pixel;
        ps.BinaryFormat = vulkan ? RHI::ShaderBinaryFormat::Spirv : RHI::ShaderBinaryFormat::Dxil;
        ps.Binary = vulkan ? pixelPackage.Spirv : pixelPackage.Dxil;
        ps.Reflection = pixelPackage.Reflection;
        Scope<RHI::Shader> vertexShader = packages ? device.CreateShader(vs) : nullptr;
        Scope<RHI::Shader> pixelShader = packages ? device.CreateShader(ps) : nullptr;
        RHI::PipelineDescription pipelineDescription;
        pipelineDescription.DebugName = "SceneMaterialTextureShaderReadbackV1 Pipeline";
        pipelineDescription.VertexShader = vertexShader.get();
        pipelineDescription.PixelShader = pixelShader.get();
        pipelineDescription.VertexInputs = {
            { "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Position) },
            { "NORMAL", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Normal) },
            { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Color) },
            { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(Vertex, UV) }
        };
        pipelineDescription.VertexStrideBytes = sizeof(Vertex);
        pipelineDescription.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }};
        pipelineDescription.SampledTextureTable = RHI::SampledTextureTableBinding { tableCapacity };
        pipelineDescription.ColorFormat = RHI::Format::R8G8B8A8Unorm;
        pipelineDescription.DepthFormat = RHI::Format::D32Float;
        pipelineDescription.DepthTestEnable = false;
        pipelineDescription.DepthWriteEnable = false;
        pipelineDescription.RasterCullMode = RHI::CullMode::None;
        Scope<RHI::Pipeline> pipeline = vertexShader && pixelShader
            ? device.CreatePipeline(pipelineDescription) : nullptr;

        AssetRegistry registry;
        MaterialLibrary materials;
        std::vector<std::filesystem::path> artifactPaths;
        std::string artifactError;
        bool artifactsStored = true;
        const auto addTexture = [&](std::string_view sourcePath, TextureRole role,
                                    TextureColorSpace colorSpace, RHI::Extent2D extent,
                                    std::vector<u8> mip0, std::vector<u8> mip1 = {})
        {
            const std::string normalized = AssetRegistry::NormalizeSourcePath(sourcePath);
            const AssetHandle handle = registry.RegisterAsset(
                AssetType::Texture, normalized, std::filesystem::path(normalized).stem().string());
            TextureArtifact artifact;
            artifact.Asset = handle;
            artifact.SourcePath = normalized;
            artifact.Role = role;
            artifact.ColorSpace = colorSpace;
            artifact.TargetProfile = TextureTargetProfile::RGBAFallback;
            artifact.CookedFormat = colorSpace == TextureColorSpace::Srgb
                ? TextureCookedFormat::R8G8B8A8Srgb : TextureCookedFormat::R8G8B8A8Unorm;
            artifact.Mips.push_back({ extent.Width, extent.Height, 0, mip0.size() });
            artifact.Payload = std::move(mip0);
            if (!mip1.empty())
            {
                artifact.Mips.push_back({ 1, 1, artifact.Payload.size(), mip1.size() });
                artifact.Payload.insert(artifact.Payload.end(), mip1.begin(), mip1.end());
            }
            const std::filesystem::path path = GetCookedTextureArtifactPath(
                handle, TextureTargetProfile::RGBAFallback);
            artifactPaths.push_back(path);
            artifactsStored = artifactsStored && handle != kInvalidAssetHandle
                && StoreTextureArtifact(path, artifact, artifactError);
            return handle;
        };
        const AssetHandle base = addTexture(
            "Engine/Generated/MaterialShaderProbeBase.rgba8", TextureRole::BaseColor,
            TextureColorSpace::Srgb, { 2, 2 },
            { 188, 0, 0, 255, 188, 0, 0, 255, 188, 0, 0, 255, 188, 0, 0, 255 },
            { 0, 188, 0, 255 });
        const AssetHandle normal = addTexture(
            "Engine/Generated/MaterialShaderProbeNormal.rgba8", TextureRole::Normal,
            TextureColorSpace::Linear, { 1, 1 }, { 64, 128, 192, 255 });
        const AssetHandle orm = addTexture(
            "Engine/Generated/MaterialShaderProbeOrm.rgba8", TextureRole::Orm,
            TextureColorSpace::Linear, { 1, 1 }, { 32, 96, 224, 255 });
        const AssetHandle emissive = addTexture(
            "Engine/Generated/MaterialShaderProbeEmissive.rgba8", TextureRole::Emissive,
            TextureColorSpace::Srgb, { 1, 1 }, { 0, 0, 188, 255 });
        const AssetHandle mask = addTexture(
            "Engine/Generated/MaterialShaderProbeMask.rgba8", TextureRole::Mask,
            TextureColorSpace::Linear, { 2, 2 },
            { 48, 160, 240, 255, 200, 80, 120, 255,
              48, 160, 240, 255, 200, 80, 120, 255 });

        MaterialAsset completeMaterial;
        completeMaterial.Name = "Complete Shader Probe";
        completeMaterial.Textures = { base, normal, orm, emissive, mask, mask };
        completeMaterial.Samplers.BaseColor = MaterialTextureSampler::LinearWrap;
        completeMaterial.Samplers.Normal = MaterialTextureSampler::LinearClamp;
        completeMaterial.Samplers.Orm = MaterialTextureSampler::PointWrap;
        completeMaterial.Samplers.Emissive = MaterialTextureSampler::PointClamp;
        completeMaterial.Samplers.Opacity = MaterialTextureSampler::LinearWrap;
        completeMaterial.Samplers.CallistoControl = MaterialTextureSampler::PointClamp;
        MaterialAsset emptyMaterial;
        emptyMaterial.Name = "Empty Shader Probe";
        MaterialAsset mismatchMaterial;
        mismatchMaterial.Name = "Mismatch Shader Probe";
        mismatchMaterial.Textures.BaseColor = normal;
        const AssetHandle completeMaterialAsset = registry.RegisterAsset(
            AssetType::Material, "Engine/Generated/CompleteShaderProbe.spiralmat", completeMaterial.Name);
        const AssetHandle emptyMaterialAsset = registry.RegisterAsset(
            AssetType::Material, "Engine/Generated/EmptyShaderProbe.spiralmat", emptyMaterial.Name);
        const AssetHandle mismatchMaterialAsset = registry.RegisterAsset(
            AssetType::Material, "Engine/Generated/MismatchShaderProbe.spiralmat", mismatchMaterial.Name);
        const bool materialsStored = materials.Set(completeMaterialAsset, completeMaterial)
            && materials.Set(emptyMaterialAsset, emptyMaterial)
            && materials.Set(mismatchMaterialAsset, mismatchMaterial);
        if (artifactsStored && materialsStored)
            Renderer::PublishArtifactResolvers(registry, materials);
        Scope<TextureRuntimePublication> runtime = artifactsStored && materialsStored && pipeline
            ? TextureRuntimePublication::Create(
                device, TextureTargetProfile::RGBAFallback, 5, tableCapacity)
            : nullptr;
        MaterialTextureBindingSet completeBindings, emptyBindings, mismatchBindings;
        std::string bindingError;
        const bool bindingsResolved = runtime
            && runtime->ResolveMaterialTextures(completeMaterialAsset, completeBindings, bindingError)
            && completeBindings.DeclaredMask == 0x3fu && completeBindings.ErrorMask == 0u
            && runtime->ResolveMaterialTextures(emptyMaterialAsset, emptyBindings, bindingError)
            && emptyBindings.DeclaredMask == 0u && emptyBindings.ErrorMask == 0u
            && runtime->ResolveMaterialTextures(mismatchMaterialAsset, mismatchBindings, bindingError)
            && mismatchBindings.DeclaredMask == 1u && mismatchBindings.ErrorMask == 1u;

        const auto makeConstants = [](const MaterialTextureBindingSet& bindings)
        {
            Constants constants {};
            constants.ViewProjection[0] = constants.ViewProjection[5]
                = constants.ViewProjection[10] = constants.ViewProjection[15] = 1.0f;
            constants.NormalTransform[0] = constants.NormalTransform[5]
                = constants.NormalTransform[10] = constants.NormalTransform[15] = 1.0f;
            constants.TextureIndices0[0] = bindings.Handles[0].Index;
            constants.TextureIndices0[1] = bindings.Handles[1].Index;
            constants.TextureIndices0[2] = bindings.Handles[2].Index;
            constants.TextureIndices0[3] = bindings.Handles[3].Index;
            constants.TextureIndices1[0] = bindings.Handles[4].Index;
            constants.TextureIndices1[1] = bindings.Handles[5].Index;
            constants.TextureState[0] = bindings.DeclaredMask;
            constants.TextureState[1] = bindings.ErrorMask;
            return constants;
        };
        const std::array<Constants, 3> constants {{
            makeConstants(completeBindings), makeConstants(emptyBindings), makeConstants(mismatchBindings)
        }};
        const auto createBuffer = [&device](const char* name, u64 size, u32 stride, RHI::BufferUsage usage)
        {
            RHI::BufferDescription description;
            description.DebugName = name;
            description.SizeBytes = size;
            description.StrideBytes = stride;
            description.Usage = static_cast<RHI::BufferUsage>(
                static_cast<u32>(usage) | static_cast<u32>(RHI::BufferUsage::CopyDest));
            return device.CreateBuffer(description);
        };
        Scope<RHI::Buffer> vertexBuffer = packages
            ? createBuffer("SceneMaterialTextureShaderReadbackV1 Vertices",
                sizeof(vertices), sizeof(Vertex), RHI::BufferUsage::Vertex) : nullptr;
        Scope<RHI::Buffer> indexBuffer = packages
            ? createBuffer("SceneMaterialTextureShaderReadbackV1 Indices",
                sizeof(indices), sizeof(u16), RHI::BufferUsage::Index) : nullptr;
        std::array<Scope<RHI::Buffer>, 3> constantBuffers;
        bool buffersUploaded = bindingsResolved && vertexBuffer && indexBuffer
            && device.UploadBuffer(*vertexBuffer, vertices.data(), sizeof(vertices))
            && device.UploadBuffer(*indexBuffer, indices.data(), sizeof(indices));
        for (u32 row = 0; row < constantBuffers.size(); ++row)
        {
            constantBuffers[row] = buffersUploaded
                ? createBuffer("SceneMaterialTextureShaderReadbackV1 Constants",
                    sizeof(Constants), 0, RHI::BufferUsage::Constant) : nullptr;
            buffersUploaded = buffersUploaded && constantBuffers[row]
                && device.UploadBuffer(*constantBuffers[row], &constants[row], sizeof(Constants));
        }

        RHI::TextureDescription colorDescription;
        colorDescription.DebugName = "SceneMaterialTextureShaderReadbackV1 Color";
        colorDescription.Extent = { width, height };
        colorDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        colorDescription.Usage = static_cast<RHI::TextureUsage>(
            static_cast<u32>(RHI::TextureUsage::RenderTarget)
            | static_cast<u32>(RHI::TextureUsage::CopySource));
        RHI::TextureDescription depthDescription = colorDescription;
        depthDescription.DebugName = "SceneMaterialTextureShaderReadbackV1 Depth";
        depthDescription.TextureFormat = RHI::Format::D32Float;
        depthDescription.Usage = RHI::TextureUsage::DepthStencil;
        Scope<RHI::Texture> color = buffersUploaded ? device.CreateTexture(colorDescription) : nullptr;
        Scope<RHI::Texture> depth = buffersUploaded ? device.CreateTexture(depthDescription) : nullptr;
        Scope<RHI::CommandList> list = color && depth && pipeline && runtime
            ? device.CreateCommandList(RHI::QueueType::Graphics,
                "SceneMaterialTextureShaderReadbackV1") : nullptr;
        RHI::ViewportClear clear;
        const bool recording = list && list->Begin()
            && list->BindViewportOutputs(*color, depth.get())
            && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget)
            && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite)
            && list->ClearViewportOutputs(clear)
            && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget)
            && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite);
        bool tableBound = false;
        if (recording)
        {
            list->SetGraphicsPipeline(*pipeline);
            list->SetViewport({ 0.0f, 0.0f, static_cast<float>(width),
                static_cast<float>(height), 0.0f, 1.0f });
            list->SetVertexBuffer(0, *vertexBuffer);
            list->SetIndexBuffer(*indexBuffer, RHI::IndexFormat::Uint16);
            tableBound = runtime->GetBindingTable()
                && list->BindGraphicsSampledTextureTable(*runtime->GetBindingTable());
            for (u32 row = 0; tableBound && row < constantBuffers.size(); ++row)
            {
                list->SetGraphicsConstantBuffer(0, *constantBuffers[row]);
                list->SetScissorRect({ 0, static_cast<int>(row * bandHeight),
                    static_cast<int>(width), static_cast<int>((row + 1) * bandHeight) });
                list->DrawIndexed(3, 1, 0, 0, 0);
            }
        }
        const bool closed = recording && tableBound
            && list->TransitionTexture(*color, RHI::ResourceState::CopySource)
            && list->End();
        const RHI::CompletionToken token = closed
            ? device.Submit(*list) : RHI::CompletionToken {};
        std::vector<RHI::TextureBindingHandle> usedHandles(
            completeBindings.Handles.begin(), completeBindings.Handles.end());
        usedHandles.insert(usedHandles.end(),
            mismatchBindings.Handles.begin(), mismatchBindings.Handles.end());
        const bool retained = token.IsValid()
            && runtime->RetainAcceptedFrame(token, usedHandles, bindingError);
        const bool completed = token.IsValid() && device.WaitForCompletion(token, 5000);
        const bool retired = retained && completed && runtime->Retire(token, bindingError);
        RHI::TextureReadback readback;
        const bool read = completed && device.ReadbackTexture(*color, readback);
        const auto pixelMatches = [&readback](u32 x, u32 y, const std::array<u8, 4>& expected)
        {
            if (readback.Data.size() < static_cast<size_t>(readback.RowPitchBytes) * readback.Extent.Height)
                return false;
            for (u32 channel = 0; channel < expected.size(); ++channel)
                if (std::abs(static_cast<int>(readback.Data[y * readback.RowPitchBytes + x * 4 + channel])
                        - static_cast<int>(expected[channel])) > 4)
                    return false;
            return true;
        };
        const std::array<std::array<std::array<u8, 4>, 8>, 3> expected {{
            {{{{128,0,0,255}},{{0,128,0,255}},{{64,128,192,255}},{{32,96,224,255}},{{0,0,128,255}},{{48,160,240,255}},{{200,80,120,255}},{{0,255,0,255}}}},
            {{{{255,255,255,255}},{{255,255,255,255}},{{128,128,255,255}},{{255,255,255,255}},{{0,0,0,255}},{{255,255,255,255}},{{255,255,255,255}},{{0,255,0,255}}}},
            {{{{255,0,255,255}},{{255,0,255,255}},{{128,128,255,255}},{{255,255,255,255}},{{0,0,0,255}},{{255,255,255,255}},{{255,255,255,255}},{{255,0,0,255}}}}
        }};
        bool rowsMatch = read;
        for (u32 row = 0; rowsMatch && row < expected.size(); ++row)
            for (u32 cell = 0; rowsMatch && cell < expected[row].size(); ++cell)
                rowsMatch = pixelMatches(cell * 8 + 4, row * bandHeight + 2, expected[row][cell]);

        const bool passed = packages && pipeline && artifactsStored && materialsStored
            && bindingsResolved && buffersUploaded && tableBound && token.IsValid()
            && retained && completed && retired && read && rowsMatch;
        Log::Info("SceneMaterialTextureShaderReadbackV1 backend=", backendName,
            " roles=", rowsMatch ? "exact-pass" : "fail",
            " colorSpace=", rowsMatch ? "sRGB-linear-pass" : "fail",
            " samplers=", rowsMatch ? "declared-pass" : "fail",
            " mip1=", rowsMatch ? "pass" : "fail",
            " missing=", rowsMatch ? "semantic-defaults-pass" : "fail",
            " invalid=", rowsMatch ? "error-resource-pass" : "fail",
            " retention=", retained && retired ? "exact-token-pass" : "fail",
            " result=", passed ? "pass" : "fail",
            " validation=", validationError,
            " artifactError=", artifactError,
            " bindingError=", bindingError);
        device.WaitIdle();
        if (runtime)
            runtime->ReleaseAfterDeviceIdle();
        Renderer::ClearArtifactResolvers();
        std::error_code filesystemError;
        for (const std::filesystem::path& path : artifactPaths)
            std::filesystem::remove(path, filesystemError);
        return passed;
    }

    bool NVRHIRenderBackend::RunRenderGraphExecutionSmoke(RHI::Device& device, std::string_view backendName)
    {
        RHI::TextureDescription colorDescription;
        colorDescription.DebugName = "RenderGraphExecutionSmokeV1 Color";
        colorDescription.Extent = { 3, 2 };
        colorDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm;
        colorDescription.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource) | static_cast<u32>(RHI::TextureUsage::CopyDest));
        colorDescription.InitialState = RHI::ResourceState::CopyDest;
        RHI::TextureDescription depthDescription;
        depthDescription.DebugName = "RenderGraphExecutionSmokeV1 Depth";
        depthDescription.Extent = colorDescription.Extent;
        depthDescription.TextureFormat = RHI::Format::D32Float;
        depthDescription.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::DepthStencil) | static_cast<u32>(RHI::TextureUsage::CopyDest));
        depthDescription.InitialState = RHI::ResourceState::CopyDest;
        Scope<RHI::Texture> color = device.CreateTexture(colorDescription);
        Scope<RHI::Texture> depth = device.CreateTexture(depthDescription);
        const RHI::QueueResolution copyQueue = device.ResolveQueue(RHI::QueueType::Copy);
        RenderGraph graph;
        const auto graphColor = graph.AddTexture(colorDescription, RenderGraph::ResourceLifetimeKind::Imported);
        const auto graphDepth = graph.AddTexture(depthDescription, RenderGraph::ResourceLifetimeKind::Imported);
        const auto clear = graph.AddPass("Clear");
        const auto finalize = graph.AddPass("Finalize", RHI::QueueType::Copy);
        graph.AddWrite(clear, graphColor, RHI::ResourceState::RenderTarget);
        graph.AddWrite(clear, graphDepth, RHI::ResourceState::DepthWrite);
        graph.AddRead(finalize, graphColor, RHI::ResourceState::CopySource, RHI::ShaderStage::Pixel);
        u32 callbackStep = 0;
        bool ordered = true;
        graph.SetPassCallback(clear, [&callbackStep, &ordered, graphColor, graphDepth](RenderGraph::ExecutionContext& context)
        {
            RHI::Texture* colorTarget = context.GetTexture(graphColor); RHI::Texture* depthTarget = context.GetTexture(graphDepth);
            RHI::ViewportClear clearValue; clearValue.Color[0] = 0.25f; clearValue.Color[1] = 0.5f; clearValue.Color[2] = 0.75f;
            ordered = ordered && callbackStep % 2 == 0;
            ++callbackStep;
            return ordered && colorTarget && depthTarget && context.GetCommandList().BindViewportOutputs(*colorTarget, depthTarget) && context.GetCommandList().ClearViewportOutputs(clearValue);
        });
        graph.SetPassCallback(finalize, [&callbackStep, &ordered, graphColor, graphDepth](RenderGraph::ExecutionContext& context)
        {
            ordered = ordered && callbackStep % 2 == 1
                && context.GetTexture(graphColor) != nullptr && context.GetTexture(graphDepth) == nullptr;
            ++callbackStep;
            return ordered;
        });
        const RenderGraph::CompileResult compiled = graph.Compile();
        const bool bound = color && depth && graph.BindTexture(graphColor, *color) && graph.BindTexture(graphDepth, *depth);
        const RenderGraph::ExecuteResult executed = bound ? graph.Execute(device, compiled) : RenderGraph::ExecuteResult {};
        if (!executed.Success) Log::Error("RenderGraphExecutionSmokeV1 execution error: ", executed.Error);
        RHI::TextureReadback readback;
        const bool readbackOk = executed.Success && device.ReadbackTexture(*color, readback);
        const std::array<u8, 4> expected { 64u, 128u, 191u, 255u };
        const auto pixelsMatch = [&expected](const RHI::TextureReadback& value)
        {
            bool matches = value.Data.size() == 24 && value.RowPitchBytes == 12;
            for (u32 y = 0; matches && y < 2; ++y) for (u32 x = 0; matches && x < 3; ++x) for (u32 c = 0; c < 4; ++c)
                if (std::abs(static_cast<int>(value.Data[y * value.RowPitchBytes + x * 4 + c]) - expected[c]) > 1) matches = false;
            return matches;
        };
        const bool firstPixels = readbackOk && pixelsMatch(readback);
        const bool firstRetired = firstPixels && device.QueryCompletion(executed.Completion) == RHI::CompletionStatus::Complete;
        // Rebind fresh imported resources for the second execution. This keeps
        // the smoke focused on graph context retirement/reuse instead of
        // adding an out-of-graph reverse ownership transfer merely to reset
        // the first Copy-owned color texture back to Graphics.
        Scope<RHI::Texture> reusedColor = device.CreateTexture(colorDescription);
        Scope<RHI::Texture> reusedDepth = device.CreateTexture(depthDescription);
        const bool rebound = firstRetired && reusedColor && reusedDepth
            && graph.BindTexture(graphColor, *reusedColor) && graph.BindTexture(graphDepth, *reusedDepth);
        const RenderGraph::ExecuteResult reused = rebound ? graph.Execute(device, compiled) : RenderGraph::ExecuteResult {};
        if (!reused.Success) Log::Error("RenderGraphExecutionSmokeV1 reuse error: ", reused.Error);
        RHI::TextureReadback reusedReadback;
        const bool reusedReadbackOk = reused.Success && device.ReadbackTexture(*reusedColor, reusedReadback);
        const bool reusedPixels = reusedReadbackOk && pixelsMatch(reusedReadback);
        const bool sameRetiredContext = reused.Success && reused.ReusedRetiredContext
            && reused.RecordingContextIndex == executed.RecordingContextIndex;
        RHI::BufferDescription transientDescription;
        transientDescription.DebugName = "RenderGraphTransientAllocationSmokeV1 Buffer";
        transientDescription.SizeBytes = 64;
        transientDescription.Usage = RHI::BufferUsage::CopyDest;
        transientDescription.InitialState = RHI::ResourceState::CopyDest;
        RenderGraph transientGraph;
        const auto transientFirst = transientGraph.AddBuffer(transientDescription);
        const auto transientSecond = transientGraph.AddBuffer(transientDescription);
        const auto transientFirstPass = transientGraph.AddPass("Transient Allocation First");
        const auto transientSecondPass = transientGraph.AddPass("Transient Allocation Second");
        transientGraph.AddWrite(transientFirstPass, transientFirst, RHI::ResourceState::CopyDest);
        transientGraph.AddWrite(transientSecondPass, transientSecond, RHI::ResourceState::CopyDest);
        RHI::Buffer* transientFirstPhysical = nullptr;
        RHI::Buffer* transientSecondPhysical = nullptr;
        transientGraph.SetPassCallback(transientFirstPass, [&](RenderGraph::ExecutionContext& context)
        { transientFirstPhysical = context.GetBuffer(transientFirst); return transientFirstPhysical != nullptr; });
        transientGraph.SetPassCallback(transientSecondPass, [&](RenderGraph::ExecutionContext& context)
        { transientSecondPhysical = context.GetBuffer(transientSecond); return transientSecondPhysical != nullptr; });
        const RenderGraph::CompileResult transientCompiled = transientGraph.Compile();
        const RenderGraph::ExecuteResult transientInitial = transientGraph.Execute(device, transientCompiled);
        bool transientRetired = transientInitial.Success;
        for (const RHI::CompletionToken& token : transientInitial.Completions)
            transientRetired = transientRetired && device.WaitForCompletion(token, 5000);
        const RenderGraph::ExecuteResult transientReused = transientRetired ? transientGraph.Execute(device, transientCompiled) : RenderGraph::ExecuteResult {};
        const bool transientPassed = transientCompiled.Success && transientInitial.Success && transientRetired && transientReused.Success
            && transientFirstPhysical == transientSecondPhysical
            && transientInitial.TransientAllocationMode == RHI::CapabilityPath::NonAliasedGpuRetiredPool
            && transientInitial.TransientResourceCount == 2 && transientInitial.EstimatedTransientAllocatedBytes == 64
            && transientInitial.EstimatedTransientPooledBytes == 64 && transientReused.EstimatedTransientAllocatedBytes == 0
            && transientReused.ReusedRetiredTransientCount == 1;
        Log::Info("RenderGraphTransientAllocationSmokeV1 backend=", backendName,
            ", mode=", transientInitial.TransientAllocationMode == RHI::CapabilityPath::NonAliasedGpuRetiredPool ? "NonAliasedGpuRetiredPool" : "unexpected",
            ", lifetime=compatible-sequential-", transientFirstPhysical == transientSecondPhysical ? "pass" : "fail",
            ", estimatedLogicalAllocatedBytes=", transientInitial.EstimatedTransientAllocatedBytes,
            ", estimatedLogicalPooledBytes=", transientInitial.EstimatedTransientPooledBytes,
            ", retirement=exact-token-", transientRetired ? "pass" : "fail",
            ", reuse=", transientReused.ReusedRetiredTransientCount == 1 ? "retired-pass" : "fail",
            ", result=", transientPassed ? "pass" : "fail");
        const bool passed = compiled.Success && ordered && callbackStep == 4u && executed.Success && firstPixels
            && firstRetired && rebound && sameRetiredContext && reusedPixels && transientPassed;
        Log::Info("RenderGraphExecutionSmokeV1 backend=", backendName,
            ", barriers=", compiled.Barriers.size(),
            ", callbacks=ordered-", ordered && callbackStep == 4u ? "pass" : "fail",
            ", undeclared=rejected, submission=", executed.Success && reused.Success ? "pass" : "fail",
            ", topology=", copyQueue.Effective == RHI::QueueType::Graphics ? "graphics-fallback" : "independent-copy",
            ", dependency=", copyQueue.Effective == RHI::QueueType::Graphics ? "ordered-elided" : "gpu-wait",
            ", readback=", firstPixels && reusedPixels ? "pass" : "fail",
            ", reuse=", sameRetiredContext ? "retired-same-context" : "fail",
            ", result=", passed ? "pass" : "fail");
        return passed;
    }

    bool NVRHIRenderBackend::RunVulkanRHIIndexedDrawSmoke()
    {
        RHI::Device* device = m_VulkanContext ? m_VulkanContext->GetRHIDevice() : nullptr;
        constexpr u32 width = 32, height = 24;
        const u32 tableCapacity = device
            ? RHI::SelectReadOnlyTextureTableCapacity(device->GetCapabilities()) : 0;
        struct Vertex { float Position[3]; float Color[3]; float UV[2]; float Normal[3]; };
        static_assert(sizeof(Vertex) == 44 && offsetof(Vertex, Normal) == 32);
        using Constants = SceneSurfaceConstants;
        const std::array<Vertex, 3> vertices {{
            {{ -0.7f, -0.6f, 0.5f }, { 0.2f, 0.4f, 0.6f }, { 0.25f, 0.75f }, { 0.8f, 0.1f, 0.3f }},
            {{ 0.7f, -0.6f, 0.5f }, { 0.2f, 0.4f, 0.6f }, { 0.25f, 0.75f }, { 0.8f, 0.1f, 0.3f }},
            {{ 0.0f, 0.7f, 0.5f }, { 0.2f, 0.4f, 0.6f }, { 0.25f, 0.75f }, { 0.8f, 0.1f, 0.3f }}
        }};
        const std::array<u16, 3> indices {{ 0, 1, 2 }};
        Constants constants {};
        constants.ViewProjection[0] = constants.ViewProjection[5]
            = constants.ViewProjection[10] = constants.ViewProjection[15] = 1.0f;
        constants.NormalTransform[0] = constants.NormalTransform[5]
            = constants.NormalTransform[10] = constants.NormalTransform[15] = 1.0f;
        constants.BaseColorAndAlphaCutoff[0] = 0.4f;
        constants.BaseColorAndAlphaCutoff[1] = 0.1f;
        constants.BaseColorAndAlphaCutoff[2] = 0.075f;
        constants.BaseColorAndAlphaCutoff[3] = 0.5f;
        constants.SurfaceFactors[1] = 0.5f;
        constants.SurfaceFactors[2] = 1.0f;
        constants.SurfaceFactors[3] = 1.0f;
        constants.CallistoFactors[0] = constants.CallistoFactors[1] = 1.0f;
        constants.CallistoFactors[2] = constants.CallistoFactors[3] = 0.75f;
        constants.TextureState[3] = 1u;
        ShaderSourceFile source = ShaderLibrary::LoadSource("Engine/Shaders/EditorViewport.hlsl", "Vulkan indexed draw smoke");
        if (source.Status == ShaderSourceStatus::Loaded)
        {
            source.Source += R"(

struct RHIVertexStrideInput
{
    float3 Position : POSITION;
    float3 Color : COLOR;
    float2 UV : TEXCOORD;
    float3 Normal : NORMAL;
};

struct RHIVertexStrideOutput
{
    float4 Position : SV_Position;
    float3 Color : COLOR0;
    float2 UV : TEXCOORD0;
    float3 Normal : NORMAL0;
};

RHIVertexStrideOutput VSVertexStrideSmoke(RHIVertexStrideInput input)
{
    RHIVertexStrideOutput output;
    output.Position = mul(float4(input.Position, 1.0f), ViewProjection);
    output.Color = input.Color;
    output.UV = input.UV;
    output.Normal = input.Normal;
    return output;
}

float4 PSVertexStrideSmoke(RHIVertexStrideOutput input) : SV_Target0
{
    const float3 fetched = input.Normal * 0.5f + input.Color * 0.25f
        + float3(input.UV, 1.0f) * 0.25f;
    const float tableAlpha = ReadOnlyTextures[TextureIndices0.x]
        .Sample(ReadOnlySamplers[TextureIndices0.x], input.UV).a;
    return float4(saturate(fetched), tableAlpha);
}
)";
        }
        auto makeRequest = [&source, tableCapacity](RHI::ShaderStage stage, const char* entry) {
            PortableShaderRequest request;
            request.SourceName = source.ResolvedPath.string(); request.Source = source.Source; request.EntryPoint = entry; request.Stage = stage;
            #ifdef _WIN32
            request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
            request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
            #else
            request.Targets = { PortableShaderTarget::Spirv };
            #endif
            request.CompilerIdentity = "Slang"; request.CompilerVersion = "2026.13.1";
            request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
            request.Defines = { "GE_READ_ONLY_TEXTURE_CAPACITY=" + std::to_string(tableCapacity) };
            request.ExpectedLayout = {
                { "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0,NormalTransform:float32x4x4:row-major@64,BaseColorAndAlphaCutoff:float32x4@128,EmissiveAndStrength:float32x4@144,SurfaceFactors:float32x4@160,CallistoFactors:float32x4@176,TextureIndices0:uint32x4@192,TextureIndices1:uint32x4@208,TextureState:uint32x4@224,MaterialState:uint32x4@240,ModelView:float32x4x4:row-major@256,NormalViewTransform:float32x4x4:row-major@320,ShadowViewProjection:float32x4x4:row-major@384,ShadowParameters:float32x4@448,ShadowState:uint32x4@464,SkyIrradianceUpper:float32x4@480,SkyIrradianceLower:float32x4@496}", 1, 512, 0, 0 },
                { "ReadOnlySamplers", 's', 0, 1, stage, "SamplerState", "sampler", tableCapacity, 0, 0, 0 },
                { "ReadOnlyTextures", 't', 0, 1, stage, "Texture2D", "float32x4", tableCapacity, 0, 1, 4 }
            };
            if (stage == RHI::ShaderStage::Vertex) request.ExpectedVertexInputs = {{ "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 }, { "Color", "COLOR", 0, 1, "float32x3", 12, 1, 3 }, { "UV", "TEXCOORD", 0, 2, "float32x2", 8, 1, 2 }, { "Normal", "NORMAL", 0, 3, "float32x3", 12, 1, 3 }};
            return request;
        };
        SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
        PortableShaderPackage vertexPackage = source.Status == ShaderSourceStatus::Loaded ? compiler.Compile(makeRequest(RHI::ShaderStage::Vertex, "VSVertexStrideSmoke")) : PortableShaderPackage {};
        PortableShaderPackage pixelPackage = source.Status == ShaderSourceStatus::Loaded ? compiler.Compile(makeRequest(RHI::ShaderStage::Pixel, "PSVertexStrideSmoke")) : PortableShaderPackage {};
        std::string validationError;
        const bool packageOk = device && PortableShaderContract::ValidatePackage(makeRequest(RHI::ShaderStage::Vertex, "VSVertexStrideSmoke"), vertexPackage, validationError)
            && PortableShaderContract::ValidatePackage(makeRequest(RHI::ShaderStage::Pixel, "PSVertexStrideSmoke"), pixelPackage, validationError);
        // Slang preserves the source entry point in reflection but emits the linked
        // SPIR-V entry point as `main`; NVRHI forwards this name to Vulkan.
        RHI::ShaderDescription vs; vs.DebugName = "VulkanRHIIndexedDrawV1 VS"; vs.SourceName = source.ResolvedPath.string(); vs.EntryPoint = "main"; vs.Stage = RHI::ShaderStage::Vertex; vs.BinaryFormat = RHI::ShaderBinaryFormat::Spirv; vs.Binary = vertexPackage.Spirv; vs.Reflection = vertexPackage.Reflection;
        RHI::ShaderDescription ps = vs; ps.DebugName = "VulkanRHIIndexedDrawV1 PS"; ps.EntryPoint = "main"; ps.Stage = RHI::ShaderStage::Pixel; ps.Binary = pixelPackage.Spirv; ps.Reflection = pixelPackage.Reflection;
        Scope<RHI::Shader> vertexShader = packageOk ? device->CreateShader(vs) : nullptr;
        Scope<RHI::Shader> pixelShader = packageOk ? device->CreateShader(ps) : nullptr;
        RHI::PipelineDescription pipelineDescription; pipelineDescription.DebugName = "VulkanRHIIndexedDrawV1 Pipeline"; pipelineDescription.VertexShader = vertexShader.get(); pipelineDescription.PixelShader = pixelShader.get();
        pipelineDescription.VertexInputs = {{ "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Position) }, { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Color) }, { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(Vertex, UV) }, { "NORMAL", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Normal) }};
        pipelineDescription.VertexStrideBytes = sizeof(Vertex);
        pipelineDescription.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }}; pipelineDescription.SampledTextureTable = RHI::SampledTextureTableBinding { tableCapacity }; pipelineDescription.ColorFormat = RHI::Format::R8G8B8A8Unorm; pipelineDescription.DepthFormat = RHI::Format::D32Float; pipelineDescription.DepthTestEnable = false; pipelineDescription.DepthWriteEnable = false; pipelineDescription.RasterCullMode = RHI::CullMode::None;
        Scope<RHI::Pipeline> pipeline = vertexShader && pixelShader ? device->CreatePipeline(pipelineDescription) : nullptr;
        Scope<TextureRuntimePublication> textureRuntime = pipeline
            ? TextureRuntimePublication::Create(*device, TextureTargetProfile::RGBAFallback,
                1, tableCapacity)
            : nullptr;
        auto createBuffer = [device](const char* name, u64 size, u32 stride, RHI::BufferUsage usage) { RHI::BufferDescription d; d.DebugName = name; d.SizeBytes = size; d.StrideBytes = stride; d.Usage = static_cast<RHI::BufferUsage>(static_cast<u32>(usage) | static_cast<u32>(RHI::BufferUsage::CopyDest)); return device->CreateBuffer(d); };
        Scope<RHI::Buffer> vertexBuffer = packageOk ? createBuffer("VulkanRHIIndexedDrawV1 Vertices", sizeof(vertices), sizeof(Vertex), RHI::BufferUsage::Vertex) : nullptr;
        Scope<RHI::Buffer> indexBuffer = packageOk ? createBuffer("VulkanRHIIndexedDrawV1 Indices", sizeof(indices), sizeof(u16), RHI::BufferUsage::Index) : nullptr;
        Scope<RHI::Buffer> constantBuffer = packageOk ? createBuffer("VulkanRHIIndexedDrawV1 Constants", sizeof(constants), 0, RHI::BufferUsage::Constant) : nullptr;
        RHI::TextureDescription colorDescription; colorDescription.DebugName = "VulkanRHIIndexedDrawV1 Color"; colorDescription.Extent = { width, height }; colorDescription.TextureFormat = RHI::Format::R8G8B8A8Unorm; colorDescription.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource));
        RHI::TextureDescription depthDescription = colorDescription; depthDescription.DebugName = "VulkanRHIIndexedDrawV1 Depth"; depthDescription.TextureFormat = RHI::Format::D32Float; depthDescription.Usage = RHI::TextureUsage::DepthStencil;
        Scope<RHI::Texture> color = packageOk ? device->CreateTexture(colorDescription) : nullptr; Scope<RHI::Texture> depth = packageOk ? device->CreateTexture(depthDescription) : nullptr;
        const bool uploads = vertexBuffer && indexBuffer && constantBuffer
            && device->UploadBuffer(*vertexBuffer, vertices.data(), sizeof(vertices))
            && device->UploadBuffer(*indexBuffer, indices.data(), sizeof(indices))
            && device->UploadBuffer(*constantBuffer, &constants, sizeof(constants));
        Scope<RHI::CommandList> list = uploads && pipeline && textureRuntime && color && depth ? device->CreateCommandList(RHI::QueueType::Graphics, "VulkanRHIIndexedDrawV1") : nullptr;
        RHI::ViewportClear clear; clear.Color[0] = 0.04f; clear.Color[1] = 0.05f; clear.Color[2] = 0.06f; clear.Color[3] = 1.0f;
        const bool draw = list && list->Begin() && list->BindViewportOutputs(*color, depth.get()) && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget) && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite) && list->ClearViewportOutputs(clear) && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget) && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite);
        bool tableBound = false;
        if (draw) { list->SetGraphicsPipeline(*pipeline); list->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); list->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) }); list->SetVertexBuffer(0, *vertexBuffer); list->SetIndexBuffer(*indexBuffer, RHI::IndexFormat::Uint16); list->SetGraphicsConstantBuffer(0, *constantBuffer); tableBound = textureRuntime->GetBindingTable() && list->BindGraphicsSampledTextureTable(*textureRuntime->GetBindingTable()); if (tableBound) list->DrawIndexed(3, 1, 0, 0, 0); }
        const bool submitted = draw && tableBound && list->TransitionTexture(*color, RHI::ResourceState::CopySource) && list->End() && device->SubmitAndWait(*list);
        RHI::TextureReadback readback; const bool readbackOk = submitted && device->ReadbackTexture(*color, readback);
        auto pixelMatches = [&readback](u32 x, u32 y, const std::array<u8, 4>& expected) { if (readback.Data.size() < static_cast<size_t>(readback.RowPitchBytes) * readback.Extent.Height) return false; for (u32 c = 0; c < 4; ++c) if (std::abs(static_cast<int>(readback.Data[y * readback.RowPitchBytes + x * 4 + c]) - expected[c]) > 3) return false; return true; };
        const auto encodeUnorm = [](float value) { return static_cast<u8>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
        const std::array<float, 3> expectedFetched {{
            0.8f * 0.5f + 0.2f * 0.25f + 0.25f * 0.25f,
            0.1f * 0.5f + 0.4f * 0.25f + 0.75f * 0.25f,
            0.3f * 0.5f + 0.6f * 0.25f + 1.0f * 0.25f
        }};
        const std::array<u8, 4> expectedInterior {{ encodeUnorm(expectedFetched[0]),
            encodeUnorm(expectedFetched[1]), encodeUnorm(expectedFetched[2]), 255 }};
        const bool interior = readbackOk && pixelMatches(16, 12, expectedInterior); const bool background = readbackOk && pixelMatches(2, 2, {{ 10, 13, 15, 255 }});
        const std::array<u8, 4> interiorPixel = readbackOk ? std::array<u8, 4> {{ readback.Data[12 * readback.RowPitchBytes + 16 * 4], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 1], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 2], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 3] }} : std::array<u8, 4> {};
        u32 foregroundPixels = 0;
        for (u32 y = 0; readbackOk && y < readback.Extent.Height; ++y)
            for (u32 x = 0; x < readback.Extent.Width; ++x)
                foregroundPixels += !pixelMatches(x, y, {{ 10, 13, 15, 255 }}) ? 1u : 0u;
        Log::Info("VulkanRHIIndexedDrawV1 package=", packageOk ? "pass" : "fail", " reflection=", packageOk ? "pass" : "fail", " pipeline=", pipeline ? "pass" : "fail", " constants=", uploads ? "pass" : "fail", " draw=", submitted ? "pass" : "fail", " submit=", submitted ? "pass" : "fail", " readback=", readbackOk ? "pass" : "fail", " interior=", interior ? "pass" : "fail", " background=", background ? "pass" : "fail", " table=", tableBound ? "bound" : "fail", " actualInterior=", static_cast<u32>(interiorPixel[0]), ",", static_cast<u32>(interiorPixel[1]), ",", static_cast<u32>(interiorPixel[2]), ",", static_cast<u32>(interiorPixel[3]), " foregroundPixels=", foregroundPixels, " rowPitch=", readback.RowPitchBytes, " vertexKey=", vertexPackage.Key, " pixelKey=", pixelPackage.Key, " validation=", validationError);
        const bool vertexStridePassed = packageOk && pipeline && uploads && submitted && interior && background
            && pipelineDescription.VertexInputs.size() == 4 && pipelineDescription.VertexStrideBytes == 44;
        Log::Info("RHIVertexStrideV1 backend=Vulkan attributes=4 stride=44 fetch=exact result=",
            vertexStridePassed ? "pass" : "fail");
        device->WaitIdle();
        if (textureRuntime) textureRuntime->ReleaseAfterDeviceIdle();
        return vertexStridePassed;
    }

    bool NVRHIRenderBackend::RunVulkanSceneViewportRasterSmoke()
    {
        RHI::Device* device = m_VulkanContext ? m_VulkanContext->GetRHIDevice() : nullptr;
        if (!device)
            return false;
        const bool surfaceProbeRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag(
            "--scene-surface-basis-material-id-smoke");
        const bool pbrProbeRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag(
            "--scene-basic-pbr-material-id-smoke");
        const bool lightPayloadProbeRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag(
            "--scene-light-payload-smoke");
        const bool directLightingProbeRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag(
            "--scene-photometric-direct-light-smoke");
        const bool shadowMapProbeRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag(
            "--scene-shadow-map-smoke");
        const bool skyAtmosphereProbeRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag(
            "--scene-sky-atmosphere-smoke");
        const bool debugVisualizationProbeRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag(
            "--scene-debug-visualization-smoke");
        const u32 dedicatedProbeCount = static_cast<u32>(surfaceProbeRequested)
            + static_cast<u32>(pbrProbeRequested)
            + static_cast<u32>(lightPayloadProbeRequested)
            + static_cast<u32>(directLightingProbeRequested)
            + static_cast<u32>(shadowMapProbeRequested)
            + static_cast<u32>(skyAtmosphereProbeRequested)
            + static_cast<u32>(debugVisualizationProbeRequested);
        if (dedicatedProbeCount > 1)
            return false;
        m_VulkanSceneRenderer = CreateScope<NVRHIVulkanViewportSceneRenderer>();
        const bool rendererInitialized = surfaceProbeRequested
            ? m_VulkanSceneRenderer->InitializeSurfaceBasisProbe(device)
            : lightPayloadProbeRequested
                ? m_VulkanSceneRenderer->InitializeLightPayloadProbe(device)
                : m_VulkanSceneRenderer->Initialize(device);
        if (!rendererInitialized)
            return false;

        const SceneDebugVisualizationPublication previousDebugVisualization =
            Renderer::GetSceneDebugVisualization();
        struct ScopedDebugVisualizationRestore
        {
            SceneDebugVisualizationSettings Settings;
            ~ScopedDebugVisualizationRestore()
            {
                Renderer::SetSceneDebugVisualization(Settings);
            }
        } debugVisualizationRestore { previousDebugVisualization.Settings };
        // Smoke fixtures are renderer-owned and must not inherit Editor startup
        // selection. This keeps the ordinary comparator at exactly seven passes.
        if (!Renderer::SetSceneDebugVisualization({ SceneDebugView::Lit,
                kInvalidEntityId, false }))
            return false;

        AssetRegistry smokeRegistry;
        AssetHandle smokeMesh = kInvalidAssetHandle;
        AssetHandle smokeTexture = kInvalidAssetHandle;
        std::string artifactError;
        if (surfaceProbeRequested || lightPayloadProbeRequested)
        {
            const std::string sourcePath = lightPayloadProbeRequested
                ? "Engine/Generated/VulkanSceneLightPayloadProbe.mesh"
                : "Engine/Generated/VulkanSurfaceBasisProbe.mesh";
            smokeMesh = smokeRegistry.RegisterAsset(AssetType::Mesh, sourcePath,
                lightPayloadProbeRequested
                    ? "Vulkan Scene Light Payload Probe" : "Vulkan Surface Basis Probe");
            MeshArtifact probeArtifact;
            probeArtifact.Asset = smokeMesh;
            probeArtifact.SourcePath = sourcePath;
            constexpr float normalX = 0.26726124f;
            constexpr float normalY = 0.53452248f;
            constexpr float normalZ = 0.80178373f;
            const auto vertex = [=](float x, float y, float z, float u, float v)
            {
                MeshArtifactVertex result;
                result.Position[0] = x; result.Position[1] = y; result.Position[2] = z;
                result.Normal[0] = normalX; result.Normal[1] = normalY; result.Normal[2] = normalZ;
                result.Color[0] = result.Color[1] = result.Color[2] = 1.0f;
                result.UV[0] = u; result.UV[1] = v;
                return result;
            };
            probeArtifact.Vertices = {
                vertex(-1.0f, -1.0f, 0.125f, 0.0f, 1.0f),
                vertex(-1.0f, 3.0f, 0.125f, 0.0f, 0.0f),
                vertex(3.0f, -1.0f, 0.125f, 1.0f, 1.0f)
            };
            probeArtifact.Indices = { 0, 1, 2 };
            probeArtifact.Primitives = {{ 0, 0, 0,
                sizeof(MeshArtifactVertex) * probeArtifact.Vertices.size(),
                0, sizeof(u32) * probeArtifact.Indices.size() }};
            if (smokeMesh == kInvalidAssetHandle
                || !StoreMeshArtifact(GetCookedMeshArtifactPath(smokeMesh), probeArtifact, artifactError))
            {
                Log::Error(lightPayloadProbeRequested
                    ? "Vulkan light-payload probe could not publish its full-screen artifact: "
                    : "Vulkan surface-basis probe could not publish its authored-normal artifact: ",
                    artifactError);
                return false;
            }
        }
        else if (pbrProbeRequested || directLightingProbeRequested
            || shadowMapProbeRequested)
        {
            const std::string sourcePath = shadowMapProbeRequested
                ? "Engine/Generated/VulkanSceneShadowMapProbe.mesh"
                : directLightingProbeRequested
                    ? "Engine/Generated/VulkanPhotometricDirectLightProbe.mesh"
                    : "Engine/Generated/VulkanBasicPbrProbe.mesh";
            smokeMesh = smokeRegistry.RegisterAsset(AssetType::Mesh, sourcePath,
                shadowMapProbeRequested
                    ? "Vulkan Scene Shadow Map Probe"
                    : directLightingProbeRequested
                    ? "Vulkan Photometric Direct Light Probe" : "Vulkan Basic PBR Probe");
            MeshArtifact probeArtifact;
            probeArtifact.Asset = smokeMesh;
            probeArtifact.SourcePath = sourcePath;
            const auto vertex = [](float x, float y, float u, float v)
            {
                MeshArtifactVertex result;
                result.Position[0] = x; result.Position[1] = y; result.Position[2] = 0.25f;
                result.Normal[0] = 0.0f; result.Normal[1] = 0.0f; result.Normal[2] = -1.0f;
                result.Color[0] = result.Color[1] = result.Color[2] = 1.0f;
                result.UV[0] = u; result.UV[1] = v;
                return result;
            };
            probeArtifact.Vertices = (directLightingProbeRequested || shadowMapProbeRequested)
                ? std::vector<MeshArtifactVertex> {
                    vertex(-1.0f, -1.0f, 0.0f, 1.0f),
                    vertex(-1.0f, 1.0f, 0.0f, 0.0f),
                    vertex(1.0f, 1.0f, 1.0f, 0.0f),
                    vertex(1.0f, -1.0f, 1.0f, 1.0f)
                }
                : std::vector<MeshArtifactVertex> {
                    vertex(-0.16f, -0.4f, 0.0f, 1.0f),
                    vertex(-0.16f, 0.4f, 0.0f, 0.0f),
                    vertex(0.16f, 0.4f, 1.0f, 0.0f),
                    vertex(0.16f, -0.4f, 1.0f, 1.0f)
                };
            probeArtifact.Indices = { 0, 1, 2, 0, 2, 3 };
            probeArtifact.Primitives = {{ 0, 0, 0,
                sizeof(MeshArtifactVertex) * probeArtifact.Vertices.size(),
                0, sizeof(u32) * probeArtifact.Indices.size() }};
            if (smokeMesh == kInvalidAssetHandle
                || !StoreMeshArtifact(GetCookedMeshArtifactPath(smokeMesh), probeArtifact, artifactError))
            {
                Log::Error(directLightingProbeRequested
                    ? "Vulkan photometric direct-light probe could not publish its surface artifact: "
                    : shadowMapProbeRequested
                        ? "Vulkan shadow-map probe could not publish its surface artifact: "
                    : "Vulkan basic-PBR probe could not publish its panel artifact: ", artifactError);
                return false;
            }
        }
        else if (!EnsureDefaultSceneMeshArtifact(smokeRegistry, smokeMesh, artifactError))
        {
            Log::Error("Vulkan Scene viewport smoke could not publish its default mesh artifact: ", artifactError);
            return false;
        }
        if (!EnsureDefaultSceneTextureArtifact(smokeRegistry, smokeTexture, artifactError))
        {
            Log::Error("Vulkan Scene viewport smoke could not publish its default texture artifact: ", artifactError);
            return false;
        }
        MaterialLibrary materials;
        AssetHandle smokeMaterial = kInvalidAssetHandle;
        AssetHandle pbrOrmTexture = kInvalidAssetHandle;
        std::array<AssetHandle, 5> pbrMaterialHandles {};
        std::array<MaterialAsset, 4> pbrMaterials {};
        if (pbrProbeRequested)
        {
            const std::string ormPath = "Engine/Generated/VulkanBasicPbrOrm.rgba8";
            pbrOrmTexture = smokeRegistry.RegisterAsset(
                AssetType::Texture, ormPath, "Vulkan Basic PBR ORM");
            TextureArtifact ormArtifact;
            ormArtifact.Asset = pbrOrmTexture;
            ormArtifact.SourcePath = ormPath;
            ormArtifact.Role = TextureRole::Orm;
            ormArtifact.ColorSpace = TextureColorSpace::Linear;
            ormArtifact.TargetProfile = TextureTargetProfile::RGBAFallback;
            ormArtifact.CookedFormat = TextureCookedFormat::R8G8B8A8Unorm;
            ormArtifact.Mips.push_back({ 1, 1, 0, 4 });
            ormArtifact.Payload = { 255, 156, 0, 255 };
            if (pbrOrmTexture == kInvalidAssetHandle
                || !StoreTextureArtifact(GetCookedTextureArtifactPath(
                    pbrOrmTexture, TextureTargetProfile::RGBAFallback), ormArtifact, artifactError))
            {
                Log::Error("Vulkan basic-PBR probe could not publish its linear ORM artifact: ", artifactError);
                return false;
            }
            const std::array<const char*, 5> names {
                "Smooth Dielectric", "Rough Dielectric", "Metallic",
                "Rough Metallic", "Row Zero Error"
            };
            for (size_t index = 0; index < names.size(); ++index)
            {
                const std::string path = "Engine/Generated/VulkanBasicPbr" + std::to_string(index) + ".spiralmat";
                pbrMaterialHandles[index] = smokeRegistry.RegisterAsset(
                    AssetType::Material, path, names[index]);
                if (pbrMaterialHandles[index] == kInvalidAssetHandle)
                    return false;
            }
            pbrMaterials[0].Name = names[0];
            pbrMaterials[0].BaseColor = { 0.55f, 0.55f, 0.55f };
            pbrMaterials[0].Metallic = 0.0f;
            pbrMaterials[0].Roughness = 0.12f;
            pbrMaterials[1] = pbrMaterials[0];
            pbrMaterials[1].Name = names[1];
            pbrMaterials[1].BaseColor = { 0.71f, 0.33f, 0.09f };
            pbrMaterials[1].Metallic = 1.0f;
            pbrMaterials[1].Roughness = 1.0f;
            pbrMaterials[1].Textures.Orm = pbrOrmTexture;
            pbrMaterials[2].Name = names[2];
            pbrMaterials[2].BaseColor = { 0.92f, 0.24f, 0.08f };
            pbrMaterials[2].Metallic = 1.0f;
            pbrMaterials[2].Roughness = 0.28f;
            pbrMaterials[3].Name = names[3];
            pbrMaterials[3].BaseColor = { 0.18f, 0.52f, 0.10f };
            pbrMaterials[3].Metallic = 1.0f;
            pbrMaterials[3].Roughness = 1.0f;
            for (size_t index = 0; index < pbrMaterials.size(); ++index)
                if (!materials.Set(pbrMaterialHandles[index], pbrMaterials[index]))
                    return false;
            // The fifth registered material deliberately has no library row,
            // so immutable resolution selects the visible row-zero error state.
        }
        else
        {
            smokeMaterial = smokeRegistry.RegisterAsset(AssetType::Material,
                shadowMapProbeRequested
                    ? "Engine/Generated/VulkanSceneShadowMapProbe.spiralmat"
                    : directLightingProbeRequested
                    ? "Engine/Generated/VulkanPhotometricDirectLightProbe.spiralmat"
                    : "Engine/Generated/VulkanSceneSmoke.spiralmat",
                shadowMapProbeRequested
                    ? "Vulkan Scene Shadow Map Probe"
                    : directLightingProbeRequested
                    ? "Vulkan Photometric Direct Light Probe" : "Vulkan Scene Smoke");
            MaterialAsset material;
            material.Name = shadowMapProbeRequested
                ? "Vulkan Scene Shadow Map Probe"
                : directLightingProbeRequested
                ? "Vulkan Photometric Direct Light Probe" : "Vulkan Scene Smoke";
            material.BaseColor = (directLightingProbeRequested || shadowMapProbeRequested)
                ? Math::Vec3 { 0.5f, 0.5f, 0.5f }
                : Math::Vec3 { 0.72f, 0.78f, 0.92f };
            material.Roughness = (directLightingProbeRequested || shadowMapProbeRequested)
                ? 0.5f : 0.45f;
            if (!directLightingProbeRequested && !shadowMapProbeRequested)
            {
                material.Textures.BaseColor = smokeTexture;
                material.Samplers.BaseColor = MaterialTextureSampler::LinearWrap;
            }
            if (smokeMaterial == kInvalidAssetHandle || !materials.Set(smokeMaterial, material))
                return false;
        }
        Renderer::PublishArtifactResolvers(smokeRegistry, materials);

        SceneRenderSnapshot snapshot;
        snapshot.FrameIndex = 1;
        snapshot.WorldGridPolicy = Math::WorldGridPolicy {};
        SceneRenderView view;
        view.Camera.Valid = true;
        view.Camera.TranslationOrigin = { 0.0, 0.0, 0.0 };
        view.Camera.HasCanonicalTranslationOrigin = Math::TryDecomposeWorldPosition(view.Camera.TranslationOrigin, snapshot.WorldGridPolicy, view.Camera.TranslationOriginPosition);
        view.Camera.View = Math::Mat4::Identity();
        view.Camera.Projection = Math::PerspectiveLH(
            Math::DegreesToRadians(60.0f), 4.0f / 3.0f, 0.1f, 100.0f);
        view.Camera.ViewProjection = Math::Mat4::Identity();
        snapshot.Views.push_back(view);
        SceneRenderLight directionalLight;
        directionalLight.SourceEntity = 2;
        directionalLight.Transform.Position = view.Camera.TranslationOriginPosition;
        directionalLight.PhotometricValue = 30000.0;
        directionalLight.Color = { 0.25f, 0.5f, 1.0f };
        // The ordinary regression fixture retains a clear-color background;
        // the dedicated production-pass fixture below owns sky visibility.
        directionalLight.Transform.RotationDegrees = { 0.0f, -31.0f, 0.0f };
        directionalLight.CastsShadows = false;
        SceneRenderLight pointLight;
        pointLight.SourceEntity = 3;
        pointLight.Type = LightType::Point;
        pointLight.PhotometricValue = 2000.0;
        pointLight.PhotometricUnit = LightPhotometricUnit::Lumens;
        pointLight.Range = 10.0f;
        pointLight.Color = { 1.0f, 0.5f, 0.25f };
        pointLight.CastsShadows = false;
        if (!Math::TryDecomposeWorldPosition(
                { 0.0, 0.0, 5.0 }, snapshot.WorldGridPolicy, pointLight.Transform.Position))
            return false;
        if (lightPayloadProbeRequested)
        {
            SceneRenderLight spotLight;
            spotLight.SourceEntity = 4;
            spotLight.Type = LightType::Spot;
            spotLight.Color = { 0.125f, 0.75f, 0.375f };
            spotLight.PhotometricValue = 6789.25;
            spotLight.PhotometricUnit = LightPhotometricUnit::Lumens;
            spotLight.Range = 10.0f;
            spotLight.InnerConeDegrees = 21.0f;
            spotLight.OuterConeDegrees = 47.0f;
            spotLight.Transform.RotationDegrees = { -12.0f, 23.0f, 0.0f };
            if (!Math::TryDecomposeWorldPosition(
                    { 0.5, -0.25, 6.0 }, snapshot.WorldGridPolicy,
                    spotLight.Transform.Position))
                return false;
            // Nonzero directional and local indices make the table readback
            // discriminating instead of accepting zero-filled padding.
            snapshot.Lights = { pointLight, directionalLight, spotLight };
        }
        else if (pbrProbeRequested)
        {
            // Preserve the established BRDF oracle with a real typed Scene
            // directional light equivalent to the retired preview source.
            directionalLight.Color = { 1.0f, 1.0f, 1.0f };
            directionalLight.PhotometricValue = 4.0;
            directionalLight.Transform.RotationDegrees = { 0.0f,
                static_cast<float>(std::atan2(-0.96, 0.28)
                    * 180.0 / 3.14159265358979323846), 0.0f };
            snapshot.Lights = { directionalLight };
        }
        else if (directLightingProbeRequested)
        {
            snapshot.Lights.clear();
        }
        else if (shadowMapProbeRequested)
        {
            directionalLight.SourceEntity = 31;
            directionalLight.Color = { 1.0f, 1.0f, 1.0f };
            directionalLight.PhotometricValue = 6.0;
            directionalLight.Transform.RotationDegrees = { 0.0f, 30.0f, 0.0f };
            directionalLight.CastsShadows = true;
            snapshot.Lights = { directionalLight };
        }
        else if (skyAtmosphereProbeRequested || debugVisualizationProbeRequested)
        {
            directionalLight.SourceEntity = 51;
            directionalLight.Color = { 1.0f, 1.0f, 1.0f };
            directionalLight.PhotometricValue = 100000.0;
            directionalLight.Transform.RotationDegrees = { 25.0f, 180.0f, 0.0f };
            directionalLight.CastsShadows = debugVisualizationProbeRequested;
            snapshot.Lights = { directionalLight };
        }
        else
        {
            snapshot.Lights = { directionalLight, pointLight };
        }
        if (pbrProbeRequested)
        {
            constexpr std::array<double, 5> centers { -0.8, -0.4, 0.0, 0.4, 0.8 };
            for (size_t index = 0; index < centers.size(); ++index)
            {
                SceneRenderMesh panel;
                panel.SourceEntity = static_cast<EntityId>(10 + index);
                panel.MeshAsset = smokeMesh;
                panel.MaterialAsset = pbrMaterialHandles[index];
                if (!Math::TryDecomposeWorldPosition({ centers[index], 0.0, 0.0 },
                    snapshot.WorldGridPolicy, panel.Transform.Position))
                    return false;
                snapshot.Meshes.push_back(panel);
            }
        }
        else if (shadowMapProbeRequested)
        {
            SceneRenderMesh receiver;
            receiver.SourceEntity = 32;
            receiver.MeshAsset = smokeMesh;
            receiver.MaterialAsset = smokeMaterial;
            receiver.CastsShadows = false;
            receiver.Transform.Scale = { 0.8f, 0.8f, 1.0f };
            if (!Math::TryDecomposeWorldPosition({ 0.0, 0.0, 0.5 },
                snapshot.WorldGridPolicy, receiver.Transform.Position))
                return false;
            SceneRenderMesh caster;
            caster.SourceEntity = 33;
            caster.MeshAsset = smokeMesh;
            caster.MaterialAsset = smokeMaterial;
            caster.CastsShadows = true;
            caster.Transform.Scale = { 0.12f, 0.12f, 1.0f };
            if (!Math::TryDecomposeWorldPosition({ -0.3, 0.0, 0.0 },
                snapshot.WorldGridPolicy, caster.Transform.Position))
                return false;
            snapshot.Meshes = { receiver, caster };
        }
        else
        {
            SceneRenderMesh mesh;
            mesh.SourceEntity = 1;
            mesh.MeshAsset = smokeMesh;
            mesh.MaterialAsset = smokeMaterial;
            mesh.Transform.Position = view.Camera.TranslationOriginPosition;
            if (surfaceProbeRequested)
            {
                mesh.Transform.Scale = { 1.3f, 0.7f, 2.0f };
                mesh.Transform.RotationDegrees = { 0.0f, 0.0f, 23.0f };
            }
            snapshot.Meshes.push_back(mesh);
        }
        if (debugVisualizationProbeRequested
            && !Renderer::SetSceneDebugVisualization({
                SceneDebugView::MaterialId, 1, true }))
            return false;
        Renderer::PublishSceneRenderSnapshot(snapshot);
        if (!Renderer::PrepareCurrentSceneRasterFrame())
            return false;
        const std::shared_ptr<const SceneRasterFrame> retainedDebugFrame =
            debugVisualizationProbeRequested
            ? Renderer::GetPreparedSceneRasterFrame() : nullptr;
        SceneDebugVisualizationPublication liveDebugVisualization;
        if (debugVisualizationProbeRequested)
        {
            // The already-prepared frame must retain bounds-on even though the
            // live publication is changed before either GPU submission.
            if (!Renderer::SetSceneDebugVisualization({
                    SceneDebugView::MaterialId, 1, false }))
                return false;
            liveDebugVisualization = Renderer::GetSceneDebugVisualization();
        }
        bool liveCatalogPublished = dedicatedProbeCount != 0;
        u64 retainedCatalogGeneration = 0;
        u64 liveCatalogGeneration = 0;
        if (dedicatedProbeCount == 0)
        {
            const std::shared_ptr<const SceneRasterFrame> retainedFrame =
                Renderer::GetPreparedSceneRasterFrame();
            MaterialLibrary liveEditedMaterials = materials;
            MaterialAsset* liveEditedMaterial =
                liveEditedMaterials.Get(smokeMaterial);
            retainedCatalogGeneration = retainedFrame
                ? retainedFrame->MaterialCatalogGeneration : 0;
            if (retainedFrame && retainedFrame->ArtifactResolvers
                && liveEditedMaterial)
            {
                liveEditedMaterial->Roughness = 0.83f;
                Renderer::PublishArtifactResolvers(
                    smokeRegistry, liveEditedMaterials);
                liveCatalogGeneration =
                    Renderer::GetPublishedArtifactResolverGeneration();
                liveCatalogPublished = liveCatalogGeneration
                    > retainedCatalogGeneration;
            }
        }
        const RendererColorPipelineSettings previousColorSettings = Renderer::GetColorPipelineSettings();
        struct ScopedColorPipelineRestore
        {
            RendererColorPipelineSettings Settings;
            ~ScopedColorPipelineRestore()
            {
                Renderer::SetColorPipelineSettings(Settings);
            }
        } colorPipelineRestore { previousColorSettings };
        if (!Renderer::SetColorPipelineSettings({
            (skyAtmosphereProbeRequested || debugVisualizationProbeRequested)
                ? 11.0 : 0.0 }))
            return false;
        const ClearColor background { 0.04f, 0.05f, 0.06f, 1.0f };
        const u32 firstWidth = skyAtmosphereProbeRequested ? 256u : 48u;
        const u32 firstHeight = skyAtmosphereProbeRequested ? 144u
            : debugVisualizationProbeRequested ? 36u
            : pbrProbeRequested ? 24u : directLightingProbeRequested ? 48u : 36u;
        const u32 secondWidth = skyAtmosphereProbeRequested ? 320u : 64u;
        const u32 secondHeight = skyAtmosphereProbeRequested ? 180u
            : debugVisualizationProbeRequested ? 48u
            : pbrProbeRequested ? 32u : directLightingProbeRequested ? 64u : 48u;
        const bool firstRaster = m_VulkanSceneRenderer->RenderCurrentSnapshot(
            firstWidth, firstHeight, background);
        const u64 firstGeneration = m_VulkanSceneRenderer->GetOutputGeneration();
        RHI::TextureReadback firstDedicatedReadback;
        const bool firstDedicatedRetired = !(
            surfaceProbeRequested || pbrProbeRequested || lightPayloadProbeRequested
                || directLightingProbeRequested || shadowMapProbeRequested
                || skyAtmosphereProbeRequested || debugVisualizationProbeRequested)
            || (firstRaster && m_VulkanSceneRenderer->ReadbackColor(firstDedicatedReadback));
        const bool resizedRaster = firstRaster && firstDedicatedRetired
            && m_VulkanSceneRenderer->RenderCurrentSnapshot(
                secondWidth, secondHeight, background);
        const u64 outputGeneration = m_VulkanSceneRenderer->GetOutputGeneration();
        RHI::TextureReadback hdrReadback;
        bool hdrReadbackOk = !(pbrProbeRequested || lightPayloadProbeRequested
                || directLightingProbeRequested || skyAtmosphereProbeRequested)
            || (resizedRaster && m_VulkanSceneRenderer->ReadbackHdr(hdrReadback));
        RHI::TextureReadback readback;
        bool readbackOk = resizedRaster && hdrReadbackOk
            && m_VulkanSceneRenderer->ReadbackColor(readback);
        bool nextLiveCatalogPrepared = dedicatedProbeCount != 0;
        if (dedicatedProbeCount == 0)
        {
            SceneRenderSnapshot nextSnapshot = snapshot;
            nextSnapshot.FrameIndex = 2;
            Renderer::PublishSceneRenderSnapshot(std::move(nextSnapshot));
            const bool nextPrepared =
                Renderer::PrepareCurrentSceneRasterFrame();
            const std::shared_ptr<const SceneRasterFrame> nextFrame =
                Renderer::GetPreparedSceneRasterFrame();
            const auto editedRow = nextFrame
                ? std::find_if(nextFrame->MaterialRows.begin(),
                    nextFrame->MaterialRows.end(),
                    [smokeMaterial](const SceneMaterialRow& row)
                    { return row.SourceAsset == smokeMaterial; })
                : std::vector<SceneMaterialRow>::const_iterator {};
            nextLiveCatalogPrepared = nextPrepared && nextFrame
                && nextFrame->MaterialCatalogGeneration
                    == liveCatalogGeneration
                && editedRow != nextFrame->MaterialRows.end()
                && editedRow->Material.Roughness == 0.83f;
            const bool liveCatalogContinuity = liveCatalogPublished
                && firstRaster && resizedRaster && readbackOk
                && nextLiveCatalogPrepared;
            Log::Info("SceneArtifactSnapshotContinuityV1 backend=Vulkan publication=after-prepare inFlight=retained-generation-rendered nextFrame=new-generation-prepared retainedGeneration=",
                retainedCatalogGeneration, " liveGeneration=",
                liveCatalogGeneration, " result=",
                liveCatalogContinuity ? "pass" : "fail");
        }
        if (debugVisualizationProbeRequested)
        {
            const RHI::TextureReadback materialIdWithBounds = readback;
            const auto validColorReadback = [secondWidth, secondHeight](
                const RHI::TextureReadback& value)
            {
                return value.Extent.Width == secondWidth
                    && value.Extent.Height == secondHeight
                    && value.RowPitchBytes >= secondWidth * 4u
                    && value.Data.size() >= static_cast<size_t>(
                        value.RowPitchBytes) * secondHeight;
            };
            const auto pixel = [](const RHI::TextureReadback& value,
                u32 x, u32 y)
            {
                return &value.Data[static_cast<size_t>(y)
                    * value.RowPitchBytes + static_cast<size_t>(x) * 4u];
            };
            const auto colorDiffers = [](const u8* first, const u8* second,
                u32 threshold)
            {
                for (u32 channel = 0; channel < 3; ++channel)
                    if (std::abs(static_cast<int>(first[channel])
                            - static_cast<int>(second[channel]))
                        > static_cast<int>(threshold))
                        return true;
                return false;
            };
            const auto renderDebugView = [&](u64 frameIndex,
                SceneDebugView debugView, EntityId selectedEntity,
                bool showSelectedBounds, RHI::TextureReadback& outReadback,
                std::shared_ptr<const SceneRasterFrame>& outFrame)
            {
                if (!Renderer::SetSceneDebugVisualization({ debugView,
                        selectedEntity, showSelectedBounds }))
                    return false;
                snapshot.FrameIndex = frameIndex;
                Renderer::PublishSceneRenderSnapshot(snapshot);
                if (!Renderer::PrepareCurrentSceneRasterFrame())
                    return false;
                outFrame = Renderer::GetPreparedSceneRasterFrame();
                return m_VulkanSceneRenderer->RenderCurrentSnapshot(
                        secondWidth, secondHeight, background)
                    && m_VulkanSceneRenderer->ReadbackColor(outReadback);
            };

            RHI::TextureReadback materialIdWithoutBounds;
            RHI::TextureReadback geometricNormal;
            RHI::TextureReadback shadowCaster;
            RHI::TextureReadback lit;
            std::shared_ptr<const SceneRasterFrame> materialIdWithoutBoundsFrame;
            std::shared_ptr<const SceneRasterFrame> geometricNormalFrame;
            std::shared_ptr<const SceneRasterFrame> shadowCasterFrame;
            std::shared_ptr<const SceneRasterFrame> litFrame;
            const bool viewsRendered = readbackOk
                && renderDebugView(2, SceneDebugView::MaterialId, 1, false,
                    materialIdWithoutBounds, materialIdWithoutBoundsFrame)
                && renderDebugView(3, SceneDebugView::GeometricNormal, 1, false,
                    geometricNormal, geometricNormalFrame)
                && renderDebugView(4, SceneDebugView::ShadowCaster, 1, false,
                    shadowCaster, shadowCasterFrame)
                && renderDebugView(5, SceneDebugView::Lit, kInvalidEntityId,
                    false, lit, litFrame);
            const bool shapesValid = viewsRendered
                && validColorReadback(materialIdWithBounds)
                && validColorReadback(materialIdWithoutBounds)
                && validColorReadback(geometricNormal)
                && validColorReadback(shadowCaster)
                && validColorReadback(lit);

            u32 changedOverlayPixels = 0;
            u32 fullOpacityBlendPixels = 0;
            if (shapesValid)
            {
                constexpr std::array<u32, 3> overlayBytes { 69u, 133u, 179u };
                constexpr double overlayOpacity = 0.92;
                for (u32 y = 0; y < secondHeight; ++y)
                {
                    for (u32 x = 0; x < secondWidth; ++x)
                    {
                        const u8* withBounds = pixel(materialIdWithBounds, x, y);
                        const u8* withoutBounds = pixel(materialIdWithoutBounds, x, y);
                        if (!colorDiffers(withBounds, withoutBounds, 2u))
                            continue;
                        ++changedOverlayPixels;
                        bool fullOpacityBlend = withBounds[3] == 255u
                            && withoutBounds[3] == 255u;
                        for (u32 channel = 0; fullOpacityBlend && channel < 3; ++channel)
                        {
                            const int expected = static_cast<int>(std::lround(
                                static_cast<double>(withoutBounds[channel])
                                    * (1.0 - overlayOpacity)
                                + static_cast<double>(overlayBytes[channel])
                                    * overlayOpacity));
                            fullOpacityBlend = std::abs(
                                static_cast<int>(withBounds[channel]) - expected) <= 2;
                        }
                        if (fullOpacityBlend)
                            ++fullOpacityBlendPixels;
                    }
                }
            }
            const u8* materialCenter = shapesValid
                ? pixel(materialIdWithoutBounds, secondWidth / 2u,
                    secondHeight / 2u) : nullptr;
            const u8* normalCenter = shapesValid
                ? pixel(geometricNormal, secondWidth / 2u,
                    secondHeight / 2u) : nullptr;
            const u8* shadowCenter = shapesValid
                ? pixel(shadowCaster, secondWidth / 2u,
                    secondHeight / 2u) : nullptr;
            const u8* litCenter = shapesValid
                ? pixel(lit, secondWidth / 2u, secondHeight / 2u) : nullptr;
            using DebugColor = std::array<double, 3>;
            const auto encodeDebugColor = [](DebugColor color)
            {
                for (double& channel : color)
                    channel = std::min(channel, 6.25);
                const double minimum = std::min(color[0],
                    std::min(color[1], color[2]));
                const double offset = minimum < 0.08
                    ? minimum - 6.25 * minimum * minimum : 0.04;
                for (double& channel : color)
                    channel -= offset;
                const double peak = std::max(color[0],
                    std::max(color[1], color[2]));
                if (peak >= 0.76)
                {
                    constexpr double distance = 0.24;
                    const double newPeak = 1.0 - distance * distance
                        / (peak + distance - 0.76);
                    for (double& channel : color)
                        channel *= newPeak / peak;
                    const double amount = 1.0
                        - 1.0 / (0.15 * (peak - newPeak) + 1.0);
                    for (double& channel : color)
                        channel += (newPeak - channel) * amount;
                }
                std::array<u8, 3> encoded {};
                for (size_t channel = 0; channel < encoded.size(); ++channel)
                {
                    const double value = std::clamp(color[channel], 0.0, 1.0);
                    const double srgb = value <= 0.0031308 ? value * 12.92
                        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
                    encoded[channel] = static_cast<u8>(std::clamp(
                        static_cast<int>(std::lround(srgb * 255.0)), 0, 255));
                }
                return encoded;
            };
            const auto debugPixelMatches = [](const u8* actual,
                const std::array<u8, 3>& expected)
            {
                if (!actual || actual[3] != 255u)
                    return false;
                for (size_t channel = 0; channel < expected.size(); ++channel)
                    if (std::abs(static_cast<int>(actual[channel])
                            - static_cast<int>(expected[channel])) > 4)
                        return false;
                return true;
            };
            u32 persistentMaterialHash = static_cast<u32>(smokeMaterial)
                ^ static_cast<u32>(smokeMaterial >> 32u);
            u32 materialColorHash = persistentMaterialHash * 0x9e3779b9u
                + 0x7f4a7c15u;
            materialColorHash ^= materialColorHash >> 16u;
            materialColorHash *= 0x85ebca6bu;
            materialColorHash ^= materialColorHash >> 13u;
            materialColorHash *= 0xc2b2ae35u;
            materialColorHash ^= materialColorHash >> 16u;
            const std::array<u8, 3> expectedMaterialId = encodeDebugColor({
                0.15 + 0.5 * static_cast<double>(materialColorHash & 255u) / 255.0,
                0.15 + 0.5 * static_cast<double>((materialColorHash >> 8u) & 255u) / 255.0,
                0.15 + 0.5 * static_cast<double>((materialColorHash >> 16u) & 255u) / 255.0
            });
            const std::array<u8, 3> expectedGeometricNormal =
                encodeDebugColor({ 0.35, 0.35, 0.70 });
            const std::array<u8, 3> expectedShadowCaster =
                encodeDebugColor({ 0.12, 0.62, 0.26 });
            const bool exactDiagnosticPixels = shapesValid
                && debugPixelMatches(materialCenter, expectedMaterialId)
                && debugPixelMatches(normalCenter, expectedGeometricNormal)
                && debugPixelMatches(shadowCenter, expectedShadowCaster);
            const bool centerUnaffectedByBounds = shapesValid
                && !colorDiffers(pixel(materialIdWithBounds, secondWidth / 2u,
                        secondHeight / 2u), materialCenter, 1u);
            const bool debugModesDistinct = shapesValid
                && colorDiffers(materialCenter, normalCenter, 8u)
                && colorDiffers(materialCenter, shadowCenter, 8u)
                && colorDiffers(materialCenter, litCenter, 8u)
                && colorDiffers(normalCenter, shadowCenter, 8u)
                && colorDiffers(normalCenter, litCenter, 8u)
                && colorDiffers(shadowCenter, litCenter, 8u);

            const bool retainedFrameCoherent = retainedDebugFrame
                && retainedDebugFrame->DebugVisualization.View
                    == SceneDebugView::MaterialId
                && retainedDebugFrame->DebugVisualization.SelectedEntity == 1
                && retainedDebugFrame->DebugVisualization.ShowSelectedBounds
                && retainedDebugFrame->DebugVisualizationGeneration
                    < liveDebugVisualization.Generation;
            const bool nextFrameAdopted = materialIdWithoutBoundsFrame
                && materialIdWithoutBoundsFrame->DebugVisualization.View
                    == SceneDebugView::MaterialId
                && materialIdWithoutBoundsFrame->DebugVisualization.SelectedEntity == 1
                && !materialIdWithoutBoundsFrame->DebugVisualization.ShowSelectedBounds
                && materialIdWithoutBoundsFrame->DebugVisualizationGeneration
                    == liveDebugVisualization.Generation;
            const bool postToneMapOverlay = changedOverlayPixels >= 32u
                && fullOpacityBlendPixels >= 8u && centerUnaffectedByBounds;

            snapshot.FrameIndex = 6;
            snapshot.Meshes.clear();
            Renderer::PublishSceneRenderSnapshot(snapshot);
            RHI::TextureReadback emptyVisibleMeshReadback;
            const bool emptyFramePrepared = Renderer::SetSceneDebugVisualization({
                    SceneDebugView::Lit, kInvalidEntityId, false })
                && Renderer::PrepareCurrentSceneRasterFrame();
            const std::shared_ptr<const SceneRasterFrame> emptyFrame =
                emptyFramePrepared ? Renderer::GetPreparedSceneRasterFrame() : nullptr;
            const bool emptyFrameRendered = emptyFramePrepared
                && m_VulkanSceneRenderer->RenderCurrentSnapshot(
                    secondWidth, secondHeight, background)
                && m_VulkanSceneRenderer->ReadbackColor(emptyVisibleMeshReadback);
            const bool emptyShape = emptyFrameRendered
                && validColorReadback(emptyVisibleMeshReadback);
            const u8* emptyUpper = emptyShape
                ? pixel(emptyVisibleMeshReadback, secondWidth / 8u,
                    secondHeight / 8u) : nullptr;
            const u8* emptyGround = emptyShape
                ? pixel(emptyVisibleMeshReadback, secondWidth / 8u,
                    secondHeight * 7u / 8u) : nullptr;
            const bool emptySkyRetained = emptyFrame
                && emptyFrame->HasValidView && emptyFrame->Instances.empty()
                && emptyFrame->SkyAtmosphere.Enabled
                && emptyFrame->DebugVisualization.View == SceneDebugView::Lit
                && emptyUpper && emptyGround && emptyUpper[3] == 255u
                && emptyGround[3] == 255u
                && colorDiffers(emptyUpper, emptyGround, 4u);

            const bool debugVisualizationOracle = shapesValid
                && debugModesDistinct && exactDiagnosticPixels
                && retainedFrameCoherent
                && nextFrameAdopted && postToneMapOverlay
                && emptyFrameRendered && emptySkyRetained;
            Renderer::SetColorPipelineSettings(previousColorSettings);
            Log::Info("SceneDebugVisualizationV1 backend=Vulkan modes=Lit,MaterialId,GeometricNormal,ShadowCaster settings=immutable-prepared readback=exact-diagnostic selectedBounds=post-tone-map overlayPixels=",
                changedOverlayPixels, " fullOpacityBlendPixels=",
                fullOpacityBlendPixels,
                " graphPasses=8-on,7-off emptyVisibleMeshes=sky-retained result=",
                debugVisualizationOracle ? "pass" : "fail");
            return debugVisualizationOracle;
        }
        bool finalRaster = resizedRaster;
        if (lightPayloadProbeRequested)
        {
            // Keep the ABI size stable but change sampled words before the
            // third frame. The exact oracle therefore proves a reused staging
            // slot is rewritten rather than merely observing old GPU bytes.
            snapshot.FrameIndex = 2;
            snapshot.Lights[0].PhotometricValue = 3456.75;
            snapshot.Lights[2].Color = { 0.625f, 0.25f, 0.875f };
            Renderer::PublishSceneRenderSnapshot(snapshot);
            const bool changedFramePrepared =
                Renderer::PrepareCurrentSceneRasterFrame();
            finalRaster = readbackOk && changedFramePrepared
                && m_VulkanSceneRenderer->RenderCurrentSnapshot(
                    64u, 48u, background);
            hdrReadbackOk = finalRaster
                && m_VulkanSceneRenderer->ReadbackHdr(hdrReadback);
            readbackOk = hdrReadbackOk
                && m_VulkanSceneRenderer->ReadbackColor(readback);
        }
        if (lightPayloadProbeRequested)
        {
            ClusteredLightGrid oracleGrid;
            std::string oracleGridError;
            const bool oracleGridBuilt = BuildClusteredLightGrid(
                snapshot, 0, 64u, 48u, {}, oracleGrid, oracleGridError);
            const ClusteredLightGrid* grid = oracleGridBuilt ? &oracleGrid : nullptr;
            const auto packedWords = [](size_t count)
            {
                return static_cast<u32>((count + 3u) / 4u);
            };
            const auto floatBits = [](float value) { return std::bit_cast<u32>(value); };
            const auto doubleLow = [](double value)
            {
                return static_cast<u32>(std::bit_cast<u64>(value));
            };
            const auto doubleHigh = [](double value)
            {
                return static_cast<u32>(std::bit_cast<u64>(value) >> 32u);
            };
            const auto prepared = [](const ClusteredLightRecord& light)
            {
                constexpr double pi = 3.14159265358979323846;
                const double luminance = 0.2126 * static_cast<double>(light.Color.X)
                    + 0.7152 * static_cast<double>(light.Color.Y)
                    + 0.0722 * static_cast<double>(light.Color.Z);
                const double normalization = luminance > 0.0 ? 1.0 / luminance : 0.0;
                double axialValue = light.PhotometricValue;
                double inverseSpan = 0.0;
                if (light.Type == LightType::Point)
                    axialValue /= 4.0 * pi;
                else if (light.Type == LightType::Spot)
                {
                    const double inner = static_cast<double>(light.InnerConeCosine);
                    const double outer = static_cast<double>(light.OuterConeCosine);
                    const double span = inner - outer;
                    const double solidAngle = span == 0.0
                        ? 2.0 * pi * (1.0 - outer)
                        : 2.0 * pi * ((1.0 - inner) + span / 3.0);
                    axialValue = solidAngle > 0.0
                        ? light.PhotometricValue / solidAngle : 0.0;
                    inverseSpan = span > 0.0 ? 1.0 / span : 0.0;
                }
                return std::array<float, 5> {
                    static_cast<float>(static_cast<double>(light.Color.X)
                        * normalization * axialValue),
                    static_cast<float>(static_cast<double>(light.Color.Y)
                        * normalization * axialValue),
                    static_cast<float>(static_cast<double>(light.Color.Z)
                        * normalization * axialValue),
                    static_cast<float>(inverseSpan),
                    light.Type != LightType::Directional && light.Range > 0.0f
                        ? 1.0f / light.Range : 0.0f
                };
            };
            const auto recordWord = [&](const ClusteredLightRecord& light, u32 word)
                -> SceneLightPayloadWord
            {
                const std::array<float, 5> preparedLight = prepared(light);
                switch (word)
                {
                    case 0: return { light.SourceEntity, static_cast<u32>(light.Type),
                        static_cast<u32>(light.PhotometricUnit), light.CastsShadows ? 1u : 0u };
                    case 1: return { doubleLow(light.PhotometricValue),
                        doubleHigh(light.PhotometricValue),
                        floatBits(static_cast<float>(light.PhotometricValue)), 0 };
                    case 2: return { floatBits(light.ViewPosition.X),
                        floatBits(light.ViewPosition.Y), floatBits(light.ViewPosition.Z),
                        floatBits(light.Range) };
                    case 3: return { floatBits(light.WorldDirection.X),
                        floatBits(light.WorldDirection.Y), floatBits(light.WorldDirection.Z),
                        floatBits(light.InnerConeCosine) };
                    case 4: return { floatBits(light.ViewDirection.X),
                        floatBits(light.ViewDirection.Y), floatBits(light.ViewDirection.Z),
                        floatBits(light.OuterConeCosine) };
                    case 5: return { floatBits(light.Color.X), floatBits(light.Color.Y),
                        floatBits(light.Color.Z), floatBits(preparedLight[3]) };
                    default: return { floatBits(preparedLight[0]), floatBits(preparedLight[1]),
                        floatBits(preparedLight[2]), floatBits(preparedLight[4]) };
                }
            };
            const auto scalarWord = [](const std::vector<u32>& values, size_t word)
            {
                SceneLightPayloadWord result { 0, 0, 0, 0 };
                for (size_t component = 0; component < result.size(); ++component)
                {
                    const size_t index = word * result.size() + component;
                    if (index < values.size())
                        result[component] = values[index];
                }
                return result;
            };
            bool fixtureValid = finalRaster && readbackOk && grid
                && grid->Lights.size() == 3
                && grid->Lights[0].Type == LightType::Point
                && grid->Lights[1].Type == LightType::Directional
                && grid->Lights[2].Type == LightType::Spot
                && grid->GlobalLightIndices == std::vector<u32> { 1 }
                && grid->ClusterOffsets.size() >= 8
                && grid->LocalLightIndices.size() >= 4;
            std::array<SceneLightPayloadWord, 16> expectedWords {};
            if (fixtureValid)
            {
                const u32 records = SceneLightPayload::HeaderWordCount;
                const u32 directionalOffset = records
                    + static_cast<u32>(grid->Lights.size())
                        * SceneLightPayload::LightRecordWordCount;
                const u32 offsetsOffset = directionalOffset
                    + packedWords(grid->GlobalLightIndices.size());
                const u32 localOffset = offsetsOffset
                    + packedWords(grid->ClusterOffsets.size());
                const u32 total = localOffset
                    + packedWords(grid->LocalLightIndices.size());
                expectedWords[0] = { 0x504C5347u, SceneLightPayload::Version,
                    total, SceneLightPayload::HeaderWordCount };
                expectedWords[1] = { records, static_cast<u32>(grid->Lights.size()),
                    directionalOffset, static_cast<u32>(grid->GlobalLightIndices.size()) };
                expectedWords[2] = { offsetsOffset,
                    static_cast<u32>(grid->ClusterOffsets.size()), localOffset,
                    static_cast<u32>(grid->LocalLightIndices.size()) };
                expectedWords[3] = { grid->MaximumLocalLightsPerCluster,
                    grid->OverflowedLocalLightReferences,
                    SceneLightPayload::LightRecordWordCount,
                    floatBits(1.0f) };
                expectedWords[4] = recordWord(grid->Lights[0], 0);
                expectedWords[5] = recordWord(grid->Lights[0], 1);
                expectedWords[6] = recordWord(grid->Lights[0], 6);
                expectedWords[7] = recordWord(grid->Lights[1], 0);
                expectedWords[8] = recordWord(grid->Lights[1], 6);
                expectedWords[9] = recordWord(grid->Lights[2], 0);
                expectedWords[10] = recordWord(grid->Lights[2], 3);
                expectedWords[11] = recordWord(grid->Lights[2], 4);
                expectedWords[12] = recordWord(grid->Lights[2], 6);
                expectedWords[13] = scalarWord(grid->GlobalLightIndices, 0);
                expectedWords[14] = scalarWord(grid->ClusterOffsets, 1);
                expectedWords[15] = scalarWord(grid->LocalLightIndices, 0);
            }
            const auto floatToHalf = [](float value)
            {
                const u32 bits = std::bit_cast<u32>(value);
                const u32 sign = (bits >> 16) & 0x8000u;
                const u32 exponent = (bits >> 23) & 0xffu;
                const u32 mantissa = bits & 0x7fffffu;
                if (exponent == 0xffu)
                    return static_cast<u16>(sign | 0x7c00u
                        | (mantissa != 0 ? 0x0200u : 0u));
                const int halfExponent = static_cast<int>(exponent) - 127 + 15;
                if (halfExponent >= 31)
                    return static_cast<u16>(sign | 0x7c00u);
                if (halfExponent <= 0)
                {
                    if (halfExponent < -10)
                        return static_cast<u16>(sign);
                    u32 subnormal = (mantissa | 0x800000u) >> (1 - halfExponent);
                    subnormal += 0x0fffu + ((subnormal >> 13) & 1u);
                    return static_cast<u16>(sign | (subnormal >> 13));
                }
                u32 rounded = mantissa + 0x0fffu + ((mantissa >> 13) & 1u);
                u32 resultExponent = static_cast<u32>(halfExponent);
                if ((rounded & 0x800000u) != 0)
                {
                    rounded = 0;
                    ++resultExponent;
                    if (resultExponent >= 31)
                        return static_cast<u16>(sign | 0x7c00u);
                }
                return static_cast<u16>(sign | (resultExponent << 10)
                    | (rounded >> 13));
            };
            bool cpuGpuExact = fixtureValid && hdrReadbackOk
                && hdrReadback.Extent.Width == 64 && hdrReadback.Extent.Height == 48
                && hdrReadback.TextureFormat == RHI::Format::R16G16B16A16Float
                && hdrReadback.RowPitchBytes >= 64u * 8u
                && hdrReadback.Data.size()
                    >= static_cast<size_t>(hdrReadback.RowPitchBytes) * 48u;
            size_t mismatchCell = expectedWords.size();
            size_t mismatchByte = 0;
            size_t mismatchComponent = 0;
            u16 mismatchActual = 0;
            u16 mismatchExpected = 0;
            for (size_t cell = 0; cpuGpuExact && cell < expectedWords.size(); ++cell)
            {
                for (size_t byte = 0; cpuGpuExact && byte < 4; ++byte)
                {
                    const size_t pixelOffset = static_cast<size_t>(24)
                        * hdrReadback.RowPitchBytes + (cell * 4u + byte) * 8u;
                    for (size_t component = 0; component < 4; ++component)
                    {
                        const u16 actual = static_cast<u16>(hdrReadback.Data[
                            pixelOffset + component * 2u])
                            | static_cast<u16>(static_cast<u16>(hdrReadback.Data[
                                pixelOffset + component * 2u + 1u]) << 8u);
                        const u32 expectedByte = (expectedWords[cell][component]
                            >> static_cast<u32>(byte * 8u)) & 255u;
                        const u16 expected = floatToHalf(static_cast<float>(expectedByte));
                        if (actual != expected)
                        {
                            mismatchCell = cell;
                            mismatchByte = byte;
                            mismatchComponent = component;
                            mismatchActual = actual;
                            mismatchExpected = expected;
                            cpuGpuExact = false;
                            break;
                        }
                    }
                }
            }
            if (!cpuGpuExact)
                Log::Error("Scene light payload CPU/GPU oracle mismatch: fixture=",
                    fixtureValid ? "valid" : "invalid", ", hdrReadback=",
                    hdrReadbackOk ? "valid" : "invalid", ", cell=", mismatchCell,
                    ", byte=", mismatchByte, ", component=", mismatchComponent,
                    ", expectedHalf=", mismatchExpected,
                    ", actualHalf=", mismatchActual, ", finalRaster=",
                    finalRaster ? "yes" : "no", ", colorReadback=",
                    readbackOk ? "yes" : "no", ", lights=",
                    grid ? grid->Lights.size() : 0, ", global=",
                    grid ? grid->GlobalLightIndices.size() : 0, ", offsets=",
                    grid ? grid->ClusterOffsets.size() : 0, ", local=",
                    grid ? grid->LocalLightIndices.size() : 0);
            const SceneLightPayloadPublicationDiagnostics diagnostics =
                m_VulkanSceneRenderer->GetLightPayloadPublicationDiagnostics();
            const bool reusableSlots = diagnostics.AllocationCount == 2
                && diagnostics.ReuseCount == 1
                && diagnostics.CapacityRejectionCount == 0
                && diagnostics.CommitCount == 3;
            const bool payloadProbeOk = cpuGpuExact && reusableSlots;
            Renderer::SetColorPipelineSettings(previousColorSettings);
            Log::Info("SceneLightPayloadV3 backend=Vulkan layout=versioned-uint4 records=directional-point-spot-prepared tables=global-csr-local preExposure=header-scale cpuGpu=",
                cpuGpuExact ? "exact-pass" : "fail",
                " copy=graph staging=cpu-write gpu=structured-copydest slots=4 allocations=",
                diagnostics.AllocationCount, " reuses=", diagnostics.ReuseCount,
                " retention=exact-graph-token productionPSMain=separate lightingEvaluation=not-exercised result=",
                payloadProbeOk ? "pass" : "fail");
            return payloadProbeOk;
        }
        if (shadowMapProbeRequested)
        {
            const RHI::TextureReadback shadowed = readback;
            snapshot.FrameIndex = 2;
            if (snapshot.Meshes.size() != 2)
                return false;
            snapshot.Meshes[1].CastsShadows = false;
            Renderer::PublishSceneRenderSnapshot(snapshot);
            RHI::TextureReadback unshadowed;
            const bool unshadowedRendered = Renderer::PrepareCurrentSceneRasterFrame()
                && m_VulkanSceneRenderer->RenderCurrentSnapshot(64u, 48u, background)
                && m_VulkanSceneRenderer->ReadbackColor(unshadowed);
            const bool shapeValid = readbackOk && unshadowedRendered
                && shadowed.Extent.Width == 64 && shadowed.Extent.Height == 48
                && unshadowed.Extent.Width == shadowed.Extent.Width
                && unshadowed.Extent.Height == shadowed.Extent.Height
                && shadowed.RowPitchBytes >= 64u * 4u
                && unshadowed.RowPitchBytes >= 64u * 4u
                && shadowed.Data.size()
                    >= static_cast<size_t>(shadowed.RowPitchBytes) * 48u
                && unshadowed.Data.size()
                    >= static_cast<size_t>(unshadowed.RowPitchBytes) * 48u;
            const auto brightnessAt = [](const RHI::TextureReadback& value,
                                          u32 x, u32 y)
            {
                const u8* pixel = &value.Data[static_cast<size_t>(y)
                    * value.RowPitchBytes + static_cast<size_t>(x) * 4u];
                return static_cast<u32>(pixel[0]) + static_cast<u32>(pixel[1])
                    + static_cast<u32>(pixel[2]);
            };
            u32 darkerPixels = 0;
            u32 maximumBrightnessDelta = 0;
            if (shapeValid)
            {
                for (u32 y = 18; y <= 30; ++y)
                {
                    for (u32 x = 27; x <= 37; ++x)
                    {
                        const u32 withShadow = brightnessAt(shadowed, x, y);
                        const u32 withoutShadow = brightnessAt(unshadowed, x, y);
                        if (withoutShadow > withShadow + 24u)
                        {
                            ++darkerPixels;
                            maximumBrightnessDelta = std::max(maximumBrightnessDelta,
                                withoutShadow - withShadow);
                        }
                    }
                }
            }
            const u32 shadowedTarget = shapeValid ? brightnessAt(shadowed, 32, 24) : 0u;
            const u32 unshadowedTarget = shapeValid ? brightnessAt(unshadowed, 32, 24) : 0u;
            const u32 shadowedControl = shapeValid ? brightnessAt(shadowed, 50, 24) : 0u;
            const u32 unshadowedControl = shapeValid ? brightnessAt(unshadowed, 50, 24) : 0u;
            const u32 controlDelta = shadowedControl > unshadowedControl
                ? shadowedControl - unshadowedControl
                : unshadowedControl - shadowedControl;
            const bool shadowOracle = shapeValid && darkerPixels >= 12u
                && unshadowedTarget > shadowedTarget + 60u
                && maximumBrightnessDelta > 60u && controlDelta <= 9u;
            Renderer::SetColorPipelineSettings(previousColorSettings);
            Log::Info("SceneShadowMapVisualV1 backend=Vulkan fixture=receiver-plus-offset-caster light=directional-30deg resolution=1024 stabilization=texel-snapped filter=3x3-pcf casterToggle=component target=32,24 targetBrightness=",
                shadowedTarget, ",", unshadowedTarget, " darkerPixels=", darkerPixels,
                " maximumDelta=", maximumBrightnessDelta, " controlDelta=", controlDelta,
                " depthOnly=production-pass finalColor=readback-differential result=",
                shadowOracle ? "pass" : "fail");
            return shadowOracle;
        }
        if (skyAtmosphereProbeRequested)
        {
            const std::shared_ptr<const SceneRasterFrame> skyFrame =
                Renderer::GetPreparedSceneRasterFrame();
            snapshot.FrameIndex = 2;
            snapshot.Lights.clear();
            Renderer::PublishSceneRenderSnapshot(snapshot);
            RHI::TextureReadback noSkyReadback;
            const bool noSkyRendered = Renderer::PrepareCurrentSceneRasterFrame()
                && m_VulkanSceneRenderer->RenderCurrentSnapshot(
                    secondWidth, secondHeight, background)
                && m_VulkanSceneRenderer->ReadbackColor(noSkyReadback);
            const bool ldrShape = readbackOk
                && readback.Extent.Width == secondWidth
                && readback.Extent.Height == secondHeight
                && readback.RowPitchBytes >= secondWidth * 4u
                && readback.Data.size() >= static_cast<size_t>(
                    readback.RowPitchBytes) * secondHeight;
            const bool noSkyShape = noSkyRendered
                && noSkyReadback.Extent.Width == secondWidth
                && noSkyReadback.Extent.Height == secondHeight
                && noSkyReadback.RowPitchBytes >= secondWidth * 4u
                && noSkyReadback.Data.size() >= static_cast<size_t>(
                    noSkyReadback.RowPitchBytes) * secondHeight;
            const bool hdrShape = hdrReadbackOk
                && hdrReadback.Extent.Width == secondWidth
                && hdrReadback.Extent.Height == secondHeight
                && hdrReadback.TextureFormat == RHI::Format::R16G16B16A16Float
                && hdrReadback.RowPitchBytes >= secondWidth * 8u
                && hdrReadback.Data.size() >= static_cast<size_t>(
                    hdrReadback.RowPitchBytes) * secondHeight;
            const auto ldrPixel = [&readback](u32 x, u32 y)
            {
                return &readback.Data[static_cast<size_t>(y)
                    * readback.RowPitchBytes + static_cast<size_t>(x) * 4u];
            };
            const auto brightness = [](const u8* pixel)
            {
                return static_cast<u32>(pixel[0]) + static_cast<u32>(pixel[1])
                    + static_cast<u32>(pixel[2]);
            };
            const u8* upper = ldrShape ? ldrPixel(40u, 25u) : nullptr;
            const u8* ground = ldrShape ? ldrPixel(40u, 155u) : nullptr;
            u32 sunBrightness = 0;
            u32 sunX = 0;
            u32 sunY = 0;
            if (ldrShape)
            {
                for (u32 y = 4; y <= 36; ++y)
                {
                    for (u32 x = 140; x <= 180; ++x)
                    {
                        const u32 candidate = brightness(ldrPixel(x, y));
                        if (candidate > sunBrightness)
                        {
                            sunBrightness = candidate;
                            sunX = x;
                            sunY = y;
                        }
                    }
                }
            }
            bool hdrFinite = hdrShape;
            u32 saturatedSunPixels = 0;
            u32 saturatedPixels = 0;
            for (u32 y = 0; hdrFinite && y < secondHeight; ++y)
            {
                const size_t row = static_cast<size_t>(y)
                    * hdrReadback.RowPitchBytes;
                for (u32 x = 0; hdrFinite && x < secondWidth; ++x)
                {
                    bool saturatedRgb = true;
                    for (u32 channel = 0; channel < 4; ++channel)
                    {
                        const size_t offset = row + static_cast<size_t>(x) * 8u
                            + static_cast<size_t>(channel) * 2u;
                        const u16 value = static_cast<u16>(hdrReadback.Data[offset])
                            | static_cast<u16>(static_cast<u16>(
                                hdrReadback.Data[offset + 1u]) << 8u);
                        if ((value & 0x7c00u) == 0x7c00u)
                        {
                            hdrFinite = false;
                            break;
                        }
                        if (channel < 3 && value != 0x7bffu)
                            saturatedRgb = false;
                    }
                    if (hdrFinite && saturatedRgb)
                    {
                        ++saturatedPixels;
                        if (x >= 140u && x <= 180u && y >= 4u && y <= 36u)
                            ++saturatedSunPixels;
                    }
                }
            }
            const u32 upperBrightness = upper ? brightness(upper) : 0u;
            const u32 groundBrightness = ground ? brightness(ground) : 0u;
            const u8* litSurface = ldrShape ? ldrPixel(160u, 90u) : nullptr;
            const u8* unlitSurface = noSkyShape
                ? &noSkyReadback.Data[static_cast<size_t>(90u)
                    * noSkyReadback.RowPitchBytes + static_cast<size_t>(160u) * 4u]
                : nullptr;
            const u32 litSurfaceBrightness = litSurface
                ? brightness(litSurface) : 0u;
            const u32 unlitSurfaceBrightness = unlitSurface
                ? brightness(unlitSurface) : 0u;
            const bool frameValid = skyFrame
                && skyFrame->SkyAtmosphere.Enabled
                && skyFrame->SkyAtmosphere.SunEntity == 51
                && skyFrame->SkyAtmosphere.SunLightIndex == 0
                && skyFrame->SkyAtmosphere.Turbidity == kBasicSkyTurbidity
                && skyFrame->SkyAtmosphere.GroundAlbedo == kBasicSkyGroundAlbedo
                && skyFrame->SkyAtmosphere.UpperDiffuseIrradiance.X
                    > skyFrame->SkyAtmosphere.LowerDiffuseIrradiance.X
                && skyFrame->SkyAtmosphere.UpperDiffuseIrradiance.Y
                    > skyFrame->SkyAtmosphere.LowerDiffuseIrradiance.Y
                && skyFrame->SkyAtmosphere.UpperDiffuseIrradiance.Z
                    > skyFrame->SkyAtmosphere.LowerDiffuseIrradiance.Z;
            const bool visual = upper && ground && upper[3] == 255
                && ground[3] == 255 && upper[2] > upper[0]
                && upperBrightness > groundBrightness + 20u
                && sunBrightness > upperBrightness + 10u
                && saturatedSunPixels > 0u
                && saturatedPixels == saturatedSunPixels
                && litSurface && unlitSurface
                && litSurfaceBrightness > unlitSurfaceBrightness + 10u;
            const bool skyOracle = resizedRaster && firstGeneration == 1
                && outputGeneration == 2 && frameValid && hdrFinite
                && noSkyRendered && visual;
            Renderer::SetColorPipelineSettings(previousColorSettings);
            Log::Info("SceneSkyAtmosphereVisualV1 backend=Vulkan model=Preetham1999 turbidity=3 groundAlbedo=0.1 sun=first-directional angularRadiusDegrees=0.266 exposureEV100=11 skyDome=production-pass diffuseIrradiance=first-order-zonal ormOcclusion=indirect-only upper=",
                upper ? static_cast<u32>(upper[0]) : 0u, ",",
                upper ? static_cast<u32>(upper[1]) : 0u, ",",
                upper ? static_cast<u32>(upper[2]) : 0u,
                " upperBrightness=", upperBrightness, " ground=",
                ground ? static_cast<u32>(ground[0]) : 0u, ",",
                ground ? static_cast<u32>(ground[1]) : 0u, ",",
                ground ? static_cast<u32>(ground[2]) : 0u,
                " groundBrightness=", groundBrightness,
                " sunPeak=", sunBrightness, " sunPixel=", sunX, ",", sunY,
                " sunHdrSaturatedPixels=", saturatedSunPixels,
                " totalHdrSaturatedPixels=", saturatedPixels,
                " surfaceAmbientBrightness=", litSurfaceBrightness, ",",
                unlitSurfaceBrightness,
                " hdrFinite=", hdrFinite ? "pass" : "fail",
                " resize=", firstGeneration == 1 && outputGeneration == 2
                    ? "pass" : "fail", " result=",
                skyOracle ? "pass" : "fail");
            return skyOracle;
        }
        if (directLightingProbeRequested)
        {
            using DVec = std::array<double, 3>;
            constexpr double pi = 3.14159265358979323846;
            constexpr u32 sampleX = 32;
            constexpr u32 sampleY = 32;
            constexpr double sampleCoordinate = 1.0 / 64.0;
            const DVec surfacePosition { sampleCoordinate, -sampleCoordinate, 0.25 };
            const DVec surfaceNormal { 0.0, 0.0, -1.0 };
            const auto dot = [](const DVec& left, const DVec& right)
            {
                return left[0] * right[0] + left[1] * right[1]
                    + left[2] * right[2];
            };
            const auto normalize = [&dot](DVec value)
            {
                const double length = std::sqrt(dot(value, value));
                if (!(length > 0.0) || !std::isfinite(length))
                    return DVec {};
                for (double& component : value)
                    component /= length;
                return value;
            };
            const DVec viewDirection = normalize({ -surfacePosition[0],
                -surfacePosition[1], -surfacePosition[2] });
            const auto evaluateBrdf = [dot, normalize, surfaceNormal, viewDirection](
                DVec directionToLight, double incident)
            {
                constexpr double localPi = 3.14159265358979323846;
                constexpr double base = 0.5;
                constexpr double metallic = 0.0;
                constexpr double perceptualRoughness = 0.5;
                const DVec halfVector = normalize({
                    viewDirection[0] + directionToLight[0],
                    viewDirection[1] + directionToLight[1],
                    viewDirection[2] + directionToLight[2]
                });
                const double noV = std::clamp(dot(surfaceNormal, viewDirection), 0.0, 1.0);
                const double noL = std::clamp(dot(surfaceNormal, directionToLight), 0.0, 1.0);
                const double noH = std::clamp(dot(surfaceNormal, halfVector), 0.0, 1.0);
                const double voH = std::clamp(dot(viewDirection, halfVector), 0.0, 1.0);
                const double alpha = perceptualRoughness * perceptualRoughness;
                const double alphaSquared = alpha * alpha;
                const double denominator = noH * noH * (alphaSquared - 1.0) + 1.0;
                const double distribution = alphaSquared
                    / std::max(localPi * denominator * denominator, 0.000001);
                const double smithV = noL * std::sqrt(
                    noV * noV * (1.0 - alphaSquared) + alphaSquared);
                const double smithL = noV * std::sqrt(
                    noL * noL * (1.0 - alphaSquared) + alphaSquared);
                const double visibility = 0.5 / std::max(smithV + smithL, 0.000001);
                const double fresnel = 0.04 + 0.96 * std::pow(1.0 - voH, 5.0);
                const double specular = distribution * visibility * fresnel;
                const double fd90 = 0.5 + 2.0 * alpha * voH * voH;
                const double lightScatter = 1.0 + (fd90 - 1.0)
                    * std::pow(1.0 - noL, 5.0);
                const double viewScatter = 1.0 + (fd90 - 1.0)
                    * std::pow(1.0 - noV, 5.0);
                const double diffuse = base * (1.0 - metallic)
                    * lightScatter * viewScatter / localPi;
                const double result = noV > 0.0 && noL > 0.0
                    ? (diffuse + specular) * incident * noL : 0.0;
                return DVec { result, result, result };
            };
            const auto floatToHalf = [](float value)
            {
                const u32 bits = std::bit_cast<u32>(value);
                const u32 sign = (bits >> 16) & 0x8000u;
                const u32 exponent = (bits >> 23) & 0xffu;
                const u32 mantissa = bits & 0x7fffffu;
                if (exponent == 0xffu)
                    return static_cast<u16>(sign | 0x7c00u
                        | (mantissa != 0 ? 0x0200u : 0u));
                const int halfExponent = static_cast<int>(exponent) - 127 + 15;
                if (halfExponent >= 31)
                    return static_cast<u16>(sign | 0x7c00u);
                if (halfExponent <= 0)
                {
                    if (halfExponent < -10)
                        return static_cast<u16>(sign);
                    u32 subnormal = (mantissa | 0x800000u) >> (1 - halfExponent);
                    subnormal += 0x0fffu + ((subnormal >> 13) & 1u);
                    return static_cast<u16>(sign | (subnormal >> 13));
                }
                u32 rounded = mantissa + 0x0fffu + ((mantissa >> 13) & 1u);
                u32 resultExponent = static_cast<u32>(halfExponent);
                if ((rounded & 0x800000u) != 0)
                {
                    rounded = 0;
                    ++resultExponent;
                    if (resultExponent >= 31)
                        return static_cast<u16>(sign | 0x7c00u);
                }
                return static_cast<u16>(sign | (resultExponent << 10)
                    | (rounded >> 13));
            };
            const auto halfToDouble = [](u16 value)
            {
                const double sign = (value & 0x8000u) != 0 ? -1.0 : 1.0;
                const u16 exponent = static_cast<u16>((value >> 10) & 0x1fu);
                const u16 mantissa = static_cast<u16>(value & 0x03ffu);
                if (exponent == 0)
                    return sign * std::ldexp(static_cast<double>(mantissa), -24);
                if (exponent == 31)
                    return sign * std::numeric_limits<double>::infinity();
                return sign * std::ldexp(1.0
                    + static_cast<double>(mantissa) / 1024.0,
                    static_cast<int>(exponent) - 15);
            };
            const auto toneMap = [](DVec color)
            {
                for (double& channel : color)
                    channel = std::min(channel, 6.25);
                const double minimum = std::min(color[0], std::min(color[1], color[2]));
                const double offset = minimum < 0.08
                    ? minimum - 6.25 * minimum * minimum : 0.04;
                for (double& channel : color)
                    channel -= offset;
                const double peak = std::max(color[0], std::max(color[1], color[2]));
                if (peak >= 0.76)
                {
                    const double distance = 0.24;
                    const double newPeak = 1.0 - distance * distance
                        / (peak + distance - 0.76);
                    for (double& channel : color)
                        channel *= newPeak / peak;
                    const double amount = 1.0
                        - 1.0 / (0.15 * (peak - newPeak) + 1.0);
                    for (double& channel : color)
                        channel += (newPeak - channel) * amount;
                }
                return color;
            };
            const auto encode = [](DVec color)
            {
                std::array<u8, 3> result {};
                for (size_t channel = 0; channel < result.size(); ++channel)
                {
                    const double value = std::clamp(color[channel], 0.0, 1.0);
                    const double srgb = value <= 0.0031308 ? value * 12.92
                        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
                    result[channel] = static_cast<u8>(std::clamp(
                        static_cast<int>(std::lround(srgb * 255.0)), 0, 255));
                }
                return result;
            };
            const auto expectedHalves = [floatToHalf](const DVec& value)
            {
                return std::array<u16, 3> {
                    floatToHalf(static_cast<float>(value[0])),
                    floatToHalf(static_cast<float>(value[1])),
                    floatToHalf(static_cast<float>(value[2]))
                };
            };
            const auto actualHalves = [](const RHI::TextureReadback& value)
            {
                std::array<u16, 3> result {};
                if (value.Extent.Width != 64 || value.Extent.Height != 64
                    || value.TextureFormat != RHI::Format::R16G16B16A16Float
                    || value.RowPitchBytes < 64u * 8u
                    || value.Data.size() < static_cast<size_t>(value.RowPitchBytes) * 64u)
                    return result;
                const size_t offset = static_cast<size_t>(sampleY)
                    * value.RowPitchBytes + static_cast<size_t>(sampleX) * 8u;
                for (size_t channel = 0; channel < result.size(); ++channel)
                    result[channel] = static_cast<u16>(value.Data[offset + channel * 2u])
                        | static_cast<u16>(static_cast<u16>(
                            value.Data[offset + channel * 2u + 1u]) << 8u);
                return result;
            };
            const auto closeHalves = [](const std::array<u16, 3>& left,
                const std::array<u16, 3>& right)
            {
                for (size_t channel = 0; channel < left.size(); ++channel)
                    if (std::abs(static_cast<int>(left[channel])
                        - static_cast<int>(right[channel])) > 2)
                        return false;
                return true;
            };
            const auto ldrMatches = [halfToDouble, toneMap, encode](
                const RHI::TextureReadback& value,
                const std::array<u16, 3>& expected)
            {
                if (value.Extent.Width != 64 || value.Extent.Height != 64
                    || value.RowPitchBytes < 64u * 4u
                    || value.Data.size() < static_cast<size_t>(value.RowPitchBytes) * 64u)
                    return false;
                DVec quantized {};
                for (size_t channel = 0; channel < quantized.size(); ++channel)
                    quantized[channel] = halfToDouble(expected[channel]);
                const std::array<u8, 3> expectedBytes = encode(toneMap(quantized));
                const u8* actual = &value.Data[static_cast<size_t>(sampleY)
                    * value.RowPitchBytes + static_cast<size_t>(sampleX) * 4u];
                for (size_t channel = 0; channel < expectedBytes.size(); ++channel)
                    if (std::abs(static_cast<int>(actual[channel])
                        - static_cast<int>(expectedBytes[channel])) > 3)
                        return false;
                return actual[3] == 255;
            };
            const auto actualLdrBytes = [](const RHI::TextureReadback& value)
            {
                std::array<u8, 3> result {};
                if (value.Extent.Width != 64 || value.Extent.Height != 64
                    || value.RowPitchBytes < 64u * 4u
                    || value.Data.size() < static_cast<size_t>(value.RowPitchBytes) * 64u)
                    return result;
                const u8* actual = &value.Data[static_cast<size_t>(sampleY)
                    * value.RowPitchBytes + static_cast<size_t>(sampleX) * 4u];
                std::copy_n(actual, result.size(), result.begin());
                return result;
            };
            const auto expectedLdrBytes = [halfToDouble, toneMap, encode](
                const std::array<u16, 3>& expected)
            {
                DVec quantized {};
                for (size_t channel = 0; channel < quantized.size(); ++channel)
                    quantized[channel] = halfToDouble(expected[channel]);
                return encode(toneMap(quantized));
            };

            const RHI::TextureReadback zeroHdr = hdrReadback;
            const RHI::TextureReadback zeroLdr = readback;
            ClusteredLightGrid zeroGrid;
            std::string gridError;
            const bool zeroGridBuilt = BuildClusteredLightGrid(
                snapshot, 0, 64, 64, {}, zeroGrid, gridError);
            const auto renderCase = [&](u64 frameIndex,
                const std::vector<SceneRenderLight>& lights,
                RHI::TextureReadback& outHdr, RHI::TextureReadback& outLdr,
                ClusteredLightGrid& outGrid)
            {
                snapshot.FrameIndex = frameIndex;
                snapshot.Lights = lights;
                Renderer::PublishSceneRenderSnapshot(snapshot);
                std::string caseGridError;
                return Renderer::PrepareCurrentSceneRasterFrame()
                    && BuildClusteredLightGrid(snapshot, 0, 64, 64, {},
                        outGrid, caseGridError)
                    && m_VulkanSceneRenderer->RenderCurrentSnapshot(
                        64, 64, background)
                    && m_VulkanSceneRenderer->ReadbackHdr(outHdr)
                    && m_VulkanSceneRenderer->ReadbackColor(outLdr);
            };

            SceneRenderLight directional;
            directional.SourceEntity = 20;
            directional.Transform.Position = view.Camera.TranslationOriginPosition;
            directional.Color = { 1.0f, 1.0f, 1.0f };
            directional.PhotometricValue = 1.0;
            directional.CastsShadows = false;
            SceneRenderLight point;
            point.SourceEntity = 21;
            point.Type = LightType::Point;
            point.PhotometricUnit = LightPhotometricUnit::Lumens;
            point.PhotometricValue = pi / 4.0;
            point.Color = { 1.0f, 1.0f, 1.0f };
            point.Range = 0.5f;
            point.CastsShadows = false;
            if (!Math::TryDecomposeWorldPosition({ sampleCoordinate,
                    -sampleCoordinate, 0.0 }, snapshot.WorldGridPolicy,
                    point.Transform.Position))
                return false;
            SceneRenderLight spot = point;
            spot.SourceEntity = 22;
            spot.Type = LightType::Spot;
            spot.InnerConeDegrees = 60.0f;
            spot.OuterConeDegrees = 90.0f;
            const float authoredInner = std::cos(
                Math::DegreesToRadians(spot.InnerConeDegrees));
            const float authoredOuter = std::cos(
                Math::DegreesToRadians(spot.OuterConeDegrees));
            const double authoredSpan = static_cast<double>(authoredInner)
                - static_cast<double>(authoredOuter);
            const double authoredSolidAngle = 2.0 * pi
                * ((1.0 - static_cast<double>(authoredInner))
                    + authoredSpan / 3.0);
            spot.PhotometricValue = 0.25 * authoredSolidAngle;
            const double targetCosine = 0.5
                * (static_cast<double>(authoredInner)
                    + static_cast<double>(authoredOuter));
            spot.Transform.RotationDegrees.Y = static_cast<float>(
                std::acos(targetCosine) * 180.0 / pi);

            RHI::TextureReadback directionalHdr, directionalLdr;
            RHI::TextureReadback pointHdr, pointLdr;
            RHI::TextureReadback spotHdr, spotLdr;
            ClusteredLightGrid directionalGrid, pointGrid, spotGrid;
            const bool casesRendered = zeroGridBuilt
                && renderCase(2, { directional }, directionalHdr,
                    directionalLdr, directionalGrid)
                && renderCase(3, { point }, pointHdr, pointLdr, pointGrid)
                && renderCase(4, { spot }, spotHdr, spotLdr, spotGrid);
            const auto localMembership = [](const ClusteredLightGrid& grid)
            {
                if (grid.TileCountX == 0 || grid.TileCountY == 0
                    || grid.ClusterOffsets.empty())
                    return false;
                const u32 depth = grid.SelectDepthSlice(0.25f);
                const size_t cluster = grid.GetClusterIndex(0, 0, depth);
                if (cluster + 1 >= grid.ClusterOffsets.size())
                    return false;
                for (u32 cursor = grid.ClusterOffsets[cluster];
                    cursor < grid.ClusterOffsets[cluster + 1]; ++cursor)
                    if (cursor < grid.LocalLightIndices.size()
                        && grid.LocalLightIndices[cursor] == 0u)
                        return true;
                return false;
            };
            const bool gridValid = casesRendered && zeroGrid.Lights.empty()
                && directionalGrid.GlobalLightIndices == std::vector<u32> { 0u }
                && localMembership(pointGrid) && localMembership(spotGrid)
                && zeroGrid.OverflowedLocalLightReferences == 0
                && directionalGrid.OverflowedLocalLightReferences == 0
                && pointGrid.OverflowedLocalLightReferences == 0
                && spotGrid.OverflowedLocalLightReferences == 0;

            const DVec directionalL = { 0.0, 0.0, -1.0 };
            const DVec wrongDirectionalL = { 0.0, 0.0, 1.0 };
            const DVec directionalExpected = evaluateBrdf(directionalL, 1.0);
            const DVec wrongDirectional = evaluateBrdf(wrongDirectionalL, 1.0);
            DVec localDelta {
                static_cast<double>(pointGrid.Lights.empty()
                    ? 0.0f : pointGrid.Lights[0].ViewPosition.X) - surfacePosition[0],
                static_cast<double>(pointGrid.Lights.empty()
                    ? 0.0f : pointGrid.Lights[0].ViewPosition.Y) - surfacePosition[1],
                static_cast<double>(pointGrid.Lights.empty()
                    ? 0.0f : pointGrid.Lights[0].ViewPosition.Z) - surfacePosition[2]
            };
            const double distanceSquared = dot(localDelta, localDelta);
            const DVec localL = normalize(localDelta);
            const float inverseRange = 1.0f / point.Range;
            const double normalizedDistanceSquared = distanceSquared
                * static_cast<double>(inverseRange) * inverseRange;
            const double windowBase = std::clamp(1.0
                - normalizedDistanceSquared * normalizedDistanceSquared, 0.0, 1.0);
            const double attenuation = windowBase * windowBase
                / std::max(distanceSquared, 0.0001);
            const float pointCandela = static_cast<float>(point.PhotometricValue
                / (4.0 * pi));
            const DVec pointExpected = evaluateBrdf(localL,
                static_cast<double>(pointCandela) * attenuation);
            const DVec pointWithoutSphere = evaluateBrdf(localL,
                static_cast<double>(static_cast<float>(point.PhotometricValue))
                    * attenuation);
            const DVec pointWithoutAttenuation = evaluateBrdf(localL,
                static_cast<double>(pointCandela));

            const ClusteredLightRecord& packedSpot = spotGrid.Lights.empty()
                ? ClusteredLightRecord {} : spotGrid.Lights[0];
            DVec spotEmission { packedSpot.ViewDirection.X,
                packedSpot.ViewDirection.Y, packedSpot.ViewDirection.Z };
            spotEmission = normalize(spotEmission);
            const double spotCosine = dot(spotEmission,
                { -localL[0], -localL[1], -localL[2] });
            const double spotSpan = static_cast<double>(packedSpot.InnerConeCosine)
                - static_cast<double>(packedSpot.OuterConeCosine);
            const float spotInverseSpan = static_cast<float>(1.0 / spotSpan);
            const double spotQ = std::clamp((spotCosine
                    - static_cast<double>(packedSpot.OuterConeCosine))
                * static_cast<double>(spotInverseSpan), 0.0, 1.0);
            const double spotOmega = 2.0 * pi
                * ((1.0 - static_cast<double>(packedSpot.InnerConeCosine))
                    + spotSpan / 3.0);
            const float spotCandela = static_cast<float>(
                spot.PhotometricValue / spotOmega);
            const double spotBase = static_cast<double>(spotCandela) * attenuation;
            const DVec spotExpected = evaluateBrdf(localL,
                spotBase * spotQ * spotQ);
            const DVec spotLinear = evaluateBrdf(localL, spotBase * spotQ);
            const DVec spotCubic = evaluateBrdf(localL,
                spotBase * spotQ * spotQ * (3.0 - 2.0 * spotQ));
            const DVec spotHard = evaluateBrdf(localL, spotBase);
            const DVec spotUnnormalized = evaluateBrdf(localL,
                static_cast<double>(static_cast<float>(spot.PhotometricValue))
                    * attenuation * spotQ * spotQ);

            const std::array<u16, 3> zeroExpected { 0u, 0u, 0u };
            const std::array<u16, 3> directionalHalf = expectedHalves(directionalExpected);
            const std::array<u16, 3> pointHalf = expectedHalves(pointExpected);
            const std::array<u16, 3> spotHalf = expectedHalves(spotExpected);
            const std::array<u16, 3> zeroActual = actualHalves(zeroHdr);
            const std::array<u16, 3> directionalActual = actualHalves(directionalHdr);
            const std::array<u16, 3> pointActual = actualHalves(pointHdr);
            const std::array<u16, 3> spotActual = actualHalves(spotHdr);
            const std::array<u8, 3> zeroLdrActual = actualLdrBytes(zeroLdr);
            const std::array<u8, 3> directionalLdrActual = actualLdrBytes(directionalLdr);
            const std::array<u8, 3> pointLdrActual = actualLdrBytes(pointLdr);
            const std::array<u8, 3> spotLdrActual = actualLdrBytes(spotLdr);
            const std::array<u8, 3> zeroLdrExpected = expectedLdrBytes(zeroExpected);
            const std::array<u8, 3> directionalLdrExpected = expectedLdrBytes(directionalHalf);
            const std::array<u8, 3> pointLdrExpected = expectedLdrBytes(pointHalf);
            const std::array<u8, 3> spotLdrExpected = expectedLdrBytes(spotHalf);
            const bool hdrMatches = closeHalves(zeroActual, zeroExpected)
                && closeHalves(directionalActual, directionalHalf)
                && closeHalves(pointActual, pointHalf)
                && closeHalves(spotActual, spotHalf);
            const bool ldrMatchesAll = ldrMatches(zeroLdr, zeroExpected)
                && ldrMatches(directionalLdr, directionalHalf)
                && ldrMatches(pointLdr, pointHalf)
                && ldrMatches(spotLdr, spotHalf);
            const bool counterfactualsRejected = !closeHalves(directionalActual,
                    expectedHalves(wrongDirectional))
                && !closeHalves(pointActual, expectedHalves(pointWithoutSphere))
                && !closeHalves(pointActual, expectedHalves(pointWithoutAttenuation))
                && !closeHalves(spotActual, expectedHalves(spotLinear))
                && !closeHalves(spotActual, expectedHalves(spotCubic))
                && !closeHalves(spotActual, expectedHalves(spotHard))
                && !closeHalves(spotActual, expectedHalves(spotUnnormalized));
            const bool directLightProbeOk = gridValid && hdrMatches
                && ldrMatchesAll && counterfactualsRejected;
            Log::Info("ScenePhotometricDirectLightingDiagnosticsV1 zeroHalf=",
                zeroActual[0], ",", zeroActual[1], ",", zeroActual[2],
                " directionalHalf=", directionalActual[0], ",",
                directionalActual[1], ",", directionalActual[2],
                " pointHalf=", pointActual[0], ",", pointActual[1], ",",
                pointActual[2], " spotHalf=", spotActual[0], ",",
                spotActual[1], ",", spotActual[2], " q=", spotQ,
                " grid=", gridValid ? "pass" : "fail",
                " hdr=", hdrMatches ? "pass" : "fail",
                " ldr=", ldrMatchesAll ? "pass" : "fail",
                " ldrActual=", static_cast<u32>(zeroLdrActual[0]), ",",
                static_cast<u32>(directionalLdrActual[0]), ",",
                static_cast<u32>(pointLdrActual[0]), ",",
                static_cast<u32>(spotLdrActual[0]),
                " ldrExpected=", static_cast<u32>(zeroLdrExpected[0]), ",",
                static_cast<u32>(directionalLdrExpected[0]), ",",
                static_cast<u32>(pointLdrExpected[0]), ",",
                static_cast<u32>(spotLdrExpected[0]),
                " counterfactuals=", counterfactualsRejected ? "rejected" : "fail");
            Renderer::SetColorPipelineSettings(previousColorSettings);
            Log::Info("ScenePhotometricDirectLightingV1 backend=Vulkan productionPSMain=exercised types=directional-point-spot units=lux-lumens color=Rec709-luminance-normalized point=lm-over-4pi spot=flux-normalized-squared-cosine distance=inverse-square-smooth-range clusters=bounded-csr direction=emission-forward hdr=independent-half-pass ldr=independent-pass overflow=0 retention=exact-graph-token result=",
                directLightProbeOk ? "pass" : "fail");
            return directLightProbeOk;
        }
        if (pbrProbeRequested)
        {
            using DVec = std::array<double, 3>;
            const auto dot = [](const DVec& a, const DVec& b)
            { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
            const auto normalize = [&dot](DVec value)
            {
                const double length = std::sqrt(dot(value, value));
                for (double& component : value)
                    component /= length;
                return value;
            };
            const auto evaluate = [dot, normalize](const MaterialAsset& probeMaterial,
                DVec viewPosition, bool fixedView, bool correlatedSmith = true)
            {
                constexpr double pi = 3.14159265358979323846;
                const DVec normal { 0.0, 0.0, -1.0 };
                const DVec light = normalize({ 0.96, 0.0, -0.28 });
                DVec view = fixedView ? DVec { 0.0, 0.0, -1.0 }
                                      : normalize({ -viewPosition[0], -viewPosition[1], -viewPosition[2] });
                const DVec halfVector = normalize({
                    view[0] + light[0], view[1] + light[1], view[2] + light[2]
                });
                const double noV = std::clamp(dot(normal, view), 0.0, 1.0);
                const double noL = std::clamp(dot(normal, light), 0.0, 1.0);
                const double noH = std::clamp(dot(normal, halfVector), 0.0, 1.0);
                const double voH = std::clamp(dot(view, halfVector), 0.0, 1.0);
                const double perceptualRoughness = std::max(std::clamp(
                    static_cast<double>(probeMaterial.Roughness), 0.0, 1.0), 0.045);
                const double alpha = perceptualRoughness * perceptualRoughness;
                const double alphaSquared = alpha * alpha;
                const double distributionDenominator = noH * noH * (alphaSquared - 1.0) + 1.0;
                const double distribution = alphaSquared
                    / (pi * distributionDenominator * distributionDenominator);
                const double smithV = noL * std::sqrt(noV * noV * (1.0 - alphaSquared) + alphaSquared);
                const double smithL = noV * std::sqrt(noL * noL * (1.0 - alphaSquared) + alphaSquared);
                const double correlatedVisibility = 0.5 / std::max(smithV + smithL, 0.000001);
                const double uncorrelatedVisibility = 1.0 / std::max(
                    (noV + std::sqrt(noV * noV * (1.0 - alphaSquared) + alphaSquared))
                    * (noL + std::sqrt(noL * noL * (1.0 - alphaSquared) + alphaSquared)),
                    0.000001);
                const double visibility = correlatedSmith
                    ? correlatedVisibility : uncorrelatedVisibility;
                const double fresnelFactor = std::pow(1.0 - voH, 5.0);
                const double fd90 = 0.5 + 2.0 * alpha * voH * voH;
                const double lightScatter = 1.0 + (fd90 - 1.0) * std::pow(1.0 - noL, 5.0);
                const double viewScatter = 1.0 + (fd90 - 1.0) * std::pow(1.0 - noV, 5.0);
                const double metallic = std::clamp(static_cast<double>(probeMaterial.Metallic), 0.0, 1.0);
                const DVec base {
                    probeMaterial.BaseColor.X, probeMaterial.BaseColor.Y, probeMaterial.BaseColor.Z
                };
                DVec result {};
                for (size_t channel = 0; channel < result.size(); ++channel)
                {
                    const double f0 = 0.04 * (1.0 - metallic) + base[channel] * metallic;
                    const double fresnel = f0 + (1.0 - f0) * fresnelFactor;
                    const double specular = distribution * visibility * fresnel;
                    const double diffuse = base[channel] * (1.0 - metallic)
                        * lightScatter * viewScatter / pi;
                    result[channel] = noV > 0.0 && noL > 0.0
                        ? (diffuse + specular) * 4.0 * noL : 0.0;
                }
                return result;
            };
            const auto toneMap = [](DVec color)
            {
                for (double& channel : color)
                    channel = std::min(channel, 6.25);
                const double minimum = std::min(color[0], std::min(color[1], color[2]));
                const double offset = minimum < 0.08 ? minimum - 6.25 * minimum * minimum : 0.04;
                for (double& channel : color)
                    channel -= offset;
                const double peak = std::max(color[0], std::max(color[1], color[2]));
                if (peak >= 0.76)
                {
                    const double distance = 0.24;
                    const double newPeak = 1.0 - distance * distance / (peak + distance - 0.76);
                    for (double& channel : color)
                        channel *= newPeak / peak;
                    const double amount = 1.0 - 1.0 / (0.15 * (peak - newPeak) + 1.0);
                    for (double& channel : color)
                        channel += (newPeak - channel) * amount;
                }
                return color;
            };
            const auto encode = [](DVec displayLinear)
            {
                std::array<u8, 3> bytes {};
                for (size_t channel = 0; channel < bytes.size(); ++channel)
                {
                    const double value = std::clamp(displayLinear[channel], 0.0, 1.0);
                    const double srgb = value <= 0.0031308 ? value * 12.92
                        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
                    bytes[channel] = static_cast<u8>(std::clamp(
                        static_cast<int>(std::lround(srgb * 255.0)), 0, 255));
                }
                return bytes;
            };
            const auto floatToHalf = [](float value)
            {
                u32 bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                const u32 sign = (bits >> 16) & 0x8000u;
                const u32 exponent = (bits >> 23) & 0xffu;
                const u32 mantissa = bits & 0x7fffffu;
                if (exponent == 0xffu)
                    return static_cast<u16>(sign | 0x7c00u | (mantissa != 0 ? 0x0200u : 0u));
                const int halfExponent = static_cast<int>(exponent) - 127 + 15;
                if (halfExponent >= 31)
                    return static_cast<u16>(sign | 0x7c00u);
                if (halfExponent <= 0)
                {
                    if (halfExponent < -10)
                        return static_cast<u16>(sign);
                    u32 subnormal = (mantissa | 0x800000u) >> (1 - halfExponent);
                    subnormal += 0x0fffu + ((subnormal >> 13) & 1u);
                    return static_cast<u16>(sign | (subnormal >> 13));
                }
                u32 rounded = mantissa + 0x0fffu + ((mantissa >> 13) & 1u);
                u32 resultExponent = static_cast<u32>(halfExponent);
                if ((rounded & 0x800000u) != 0)
                {
                    rounded = 0;
                    ++resultExponent;
                    if (resultExponent >= 31)
                        return static_cast<u16>(sign | 0x7c00u);
                }
                return static_cast<u16>(sign | (resultExponent << 10) | (rounded >> 13));
            };
            const auto halfToDouble = [](u16 value)
            {
                const double sign = (value & 0x8000u) != 0 ? -1.0 : 1.0;
                const u16 exponent = static_cast<u16>((value >> 10) & 0x1fu);
                const u16 mantissa = static_cast<u16>(value & 0x03ffu);
                if (exponent == 0)
                    return sign * std::ldexp(static_cast<double>(mantissa), -24);
                if (exponent == 31)
                    return sign * std::numeric_limits<double>::infinity();
                return sign * std::ldexp(1.0 + static_cast<double>(mantissa) / 1024.0,
                    static_cast<int>(exponent) - 15);
            };
            const std::shared_ptr<const SceneRasterFrame> pbrFrame = Renderer::GetPreparedSceneRasterFrame();
            constexpr std::array<u32, 5> sampleX { 6, 19, 32, 45, 58 };
            const u32 sampleY = 16;
            bool pixelsMatch = readbackOk && readback.Extent.Width == 64
                && readback.Extent.Height == 32 && readback.RowPitchBytes >= 64 * 4
                && readback.Data.size() >= static_cast<size_t>(readback.RowPitchBytes) * 32;
            bool hdrPixelsMatch = hdrReadbackOk && hdrReadback.Extent.Width == 64
                && hdrReadback.Extent.Height == 32 && hdrReadback.RowPitchBytes >= 64 * 8
                && hdrReadback.Data.size() >= static_cast<size_t>(hdrReadback.RowPitchBytes) * 32;
            const bool buffersValid = pixelsMatch && hdrPixelsMatch;
            bool fixedViewRejected = false;
            bool uncorrelatedSmithRejected = false;
            bool roughMetalGoldenMatch = false;
            std::array<std::array<u8, 3>, 5> expectedPixels {};
            for (size_t index = 0; buffersValid && index < sampleX.size(); ++index)
            {
                MaterialAsset oracleMaterial = index < pbrMaterials.size()
                    ? pbrMaterials[index] : MaterialAsset {};
                if (index == 1)
                {
                    oracleMaterial.Roughness *= 156.0f / 255.0f;
                    oracleMaterial.Metallic = 0.0f;
                }
                const DVec viewPosition {
                    (static_cast<double>(sampleX[index]) + 0.5) / 32.0 - 1.0,
                    0.03125, 0.25
                };
                DVec hdr = index == 4 ? DVec { 4.0, 0.0, 4.0 }
                    : evaluate(oracleMaterial, {
                        viewPosition[0], viewPosition[1], viewPosition[2]
                    }, false);
                DVec quantizedHdr {};
                const size_t hdrOffset = static_cast<size_t>(sampleY) * hdrReadback.RowPitchBytes
                    + static_cast<size_t>(sampleX[index]) * 8;
                std::array<u16, 3> actualHdrHalf {};
                std::array<u16, 3> expectedHdrHalf {};
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    const u16 expectedHalf = floatToHalf(static_cast<float>(hdr[channel]));
                    const u16 actualHalf = static_cast<u16>(hdrReadback.Data[hdrOffset + channel * 2])
                        | static_cast<u16>(static_cast<u16>(hdrReadback.Data[hdrOffset + channel * 2 + 1]) << 8);
                    actualHdrHalf[channel] = actualHalf;
                    expectedHdrHalf[channel] = expectedHalf;
                    hdrPixelsMatch = hdrPixelsMatch
                        && std::abs(static_cast<int>(actualHalf) - static_cast<int>(expectedHalf)) <= 2;
                    quantizedHdr[channel] = halfToDouble(expectedHalf);
                }
                expectedPixels[index] = encode(toneMap(quantizedHdr));
                if (index == 3)
                {
                    roughMetalGoldenMatch = expectedHdrHalf
                            == std::array<u16, 3> { 11025, 12296, 10480 }
                        && expectedPixels[index] == std::array<u8, 3> { 45, 88, 24 };
                }
                const u8* actual = &readback.Data[static_cast<size_t>(sampleY) * readback.RowPitchBytes
                    + static_cast<size_t>(sampleX[index]) * 4];
                for (size_t channel = 0; channel < 3; ++channel)
                    pixelsMatch = pixelsMatch
                        && std::abs(static_cast<int>(actual[channel])
                            - static_cast<int>(expectedPixels[index][channel])) <= 3;
                pixelsMatch = pixelsMatch && actual[3] == 255;
                Log::Info("SceneBasicPbrMaterialIdDiagnosticsV1 panel=", index,
                    " actual=", static_cast<u32>(actual[0]), ",", static_cast<u32>(actual[1]), ",", static_cast<u32>(actual[2]),
                    " expected=", static_cast<u32>(expectedPixels[index][0]), ",", static_cast<u32>(expectedPixels[index][1]), ",", static_cast<u32>(expectedPixels[index][2]),
                    " actualHalf=", actualHdrHalf[0], ",", actualHdrHalf[1], ",", actualHdrHalf[2],
                    " expectedHalf=", expectedHdrHalf[0], ",", expectedHdrHalf[1], ",", expectedHdrHalf[2]);
                if (index < pbrMaterials.size())
                {
                    const std::array<u8, 3> fixed = encode(toneMap(evaluate(oracleMaterial, {
                        viewPosition[0], viewPosition[1], viewPosition[2]
                    }, true)));
                    int delta = 0;
                    for (size_t channel = 0; channel < 3; ++channel)
                        delta += std::abs(static_cast<int>(fixed[channel])
                            - static_cast<int>(expectedPixels[index][channel]));
                    fixedViewRejected = fixedViewRejected || delta > 10;
                    if (index == 3)
                    {
                        const DVec uncorrelated = evaluate(
                            oracleMaterial, viewPosition, false, false);
                        std::array<u16, 3> uncorrelatedHalf {};
                        for (size_t channel = 0; channel < 3; ++channel)
                        {
                            const int correlatedHalf = static_cast<int>(
                                floatToHalf(static_cast<float>(hdr[channel])));
                            const int uncorrelatedHalfValue = static_cast<int>(
                                floatToHalf(static_cast<float>(uncorrelated[channel])));
                            uncorrelatedSmithRejected = uncorrelatedSmithRejected
                                || std::abs(correlatedHalf - uncorrelatedHalfValue) > 2;
                            uncorrelatedHalf[channel] = static_cast<u16>(uncorrelatedHalfValue);
                        }
                        uncorrelatedSmithRejected = uncorrelatedSmithRejected
                            && uncorrelatedHalf == std::array<u16, 3> { 10694, 11927, 10248 };
                    }
                }
            }
            bool idsStable = pbrFrame && pbrFrame->MaterialRows.size() == 5
                && pbrFrame->Instances.size() == 5;
            for (size_t index = 0; idsStable && index < pbrMaterials.size(); ++index)
                idsStable = pbrFrame->Instances[index].MaterialId != 0
                    && pbrFrame->Instances[index].MaterialId < pbrFrame->MaterialRows.size()
                    && pbrFrame->MaterialRows[pbrFrame->Instances[index].MaterialId].SourceAsset
                        == pbrMaterialHandles[index]
                    && !pbrFrame->MaterialRows[pbrFrame->Instances[index].MaterialId].IsError;
            idsStable = idsStable && pbrFrame->Instances[4].MaterialId == 0
                && pbrFrame->MaterialRows[0].IsError;
            const bool pbrProbeOk = pixelsMatch && hdrPixelsMatch
                && fixedViewRejected && uncorrelatedSmithRejected
                && roughMetalGoldenMatch && idsStable;
            Log::Info("SceneBasicPbrMaterialIdDiagnosticsV1 buffers=", buffersValid ? "pass" : "fail",
                " rgba8=", pixelsMatch ? "pass" : "fail",
                " rgba16f=", hdrPixelsMatch ? "pass" : "fail",
                " fixedViewRejected=", fixedViewRejected ? "pass" : "fail",
                " uncorrelatedSmithRejected=", uncorrelatedSmithRejected ? "pass" : "fail",
                " roughMetalGolden=", roughMetalGoldenMatch ? "pass" : "fail",
                " ids=", idsStable ? "pass" : "fail");
            Renderer::SetColorPipelineSettings(previousColorSettings);
            Log::Info("SceneBasicPbrMaterialIdV2 backend=Vulkan productionPSMain=exercised brdf=GGX-Smith-Schlick-Burley materialIds=stable rowZero=error view=per-pixel-view-space lighting=typed-directional-lux sceneLights=consumed hdr=float32-before-pre-exposed-finite-storage retention=exact-graph-token result=",
                pbrProbeOk ? "pass" : "fail");
            return pbrProbeOk;
        }
        if (surfaceProbeRequested)
        {
            const std::shared_ptr<const SceneRasterFrame> surfaceFrame = Renderer::GetPreparedSceneRasterFrame();
            const u8* surfacePixel = readbackOk && readback.RowPitchBytes >= readback.Extent.Width * 4
                && readback.Data.size() >= static_cast<size_t>(readback.RowPitchBytes) * readback.Extent.Height
                ? &readback.Data[static_cast<size_t>(readback.Extent.Height / 2) * readback.RowPitchBytes
                    + static_cast<size_t>(readback.Extent.Width / 2) * 4]
                : nullptr;
            constexpr double pi = 3.14159265358979323846;
            constexpr double authoredLength = 3.7416573867739413856;
            double expectedX = (1.0 / authoredLength) / 1.3;
            double expectedY = (2.0 / authoredLength) / 0.7;
            double expectedZ = (3.0 / authoredLength) / 2.0;
            const double roll = 23.0 * pi / 180.0;
            const double rotatedX = expectedX * std::cos(roll) - expectedY * std::sin(roll);
            const double rotatedY = expectedX * std::sin(roll) + expectedY * std::cos(roll);
            expectedX = rotatedX;
            expectedY = rotatedY;
            const double expectedLength = std::sqrt(
                expectedX * expectedX + expectedY * expectedY + expectedZ * expectedZ);
            expectedX /= expectedLength;
            expectedY /= expectedLength;
            expectedZ /= expectedLength;
            const auto expectedByte = [](double direction)
            {
                const double displayLinear = std::clamp(0.46 + direction * 0.25, 0.0, 1.0);
                const double encoded = displayLinear <= 0.0031308
                    ? displayLinear * 12.92
                    : 1.055 * std::pow(displayLinear, 1.0 / 2.4) - 0.055;
                return static_cast<u8>(std::clamp(
                    static_cast<int>(std::lround(encoded * 255.0)), 0, 255));
            };
            const std::array<u8, 3> expectedPixel {
                expectedByte(expectedX), expectedByte(expectedY), expectedByte(expectedZ)
            };
            const bool surfaceProbeOk = surfacePixel && surfaceFrame
                && surfaceFrame->MaterialRows.size() == 2
                && surfaceFrame->MaterialRows[1].SourceAsset == smokeMaterial
                && !surfaceFrame->MaterialRows[1].IsError
                && surfaceFrame->Instances.size() == 1
                && surfaceFrame->Instances[0].MaterialId == 1
                && std::abs(static_cast<int>(surfacePixel[0]) - expectedPixel[0]) <= 4
                && std::abs(static_cast<int>(surfacePixel[1]) - expectedPixel[1]) <= 4
                && std::abs(static_cast<int>(surfacePixel[2]) - expectedPixel[2]) <= 4
                && surfacePixel[3] == 255;
            Renderer::SetColorPipelineSettings(previousColorSettings);
            Log::Info("SceneSurfaceBasisMaterialIdV1 backend=Vulkan invocation=dedicated productionPSMain=preserved authoredNormal=1,2,3-normalized scale=1.3,0.7,2 rotationDegrees=0,0,23 expectedDirection=",
                expectedX, ",", expectedY, ",", expectedZ,
                " readback=", surfacePixel ? static_cast<u32>(surfacePixel[0]) : 0, ",",
                surfacePixel ? static_cast<u32>(surfacePixel[1]) : 0, ",",
                surfacePixel ? static_cast<u32>(surfacePixel[2]) : 0,
                " expected=", static_cast<u32>(expectedPixel[0]), ",",
                static_cast<u32>(expectedPixel[1]), ",", static_cast<u32>(expectedPixel[2]),
                " tolerance=4 materialId=1 normalTransform=S^-1*R interface=production-scene retention=exact-graph-token result=",
                surfaceProbeOk ? "pass" : "fail");
            return surfaceProbeOk;
        }
        // Linear clear color after EV100=0 PBR-neutral mapping and explicit
        // sRGB output encoding.
        const std::array<u8, 4> expectedBackground { 25, 39, 48, 255 };
        auto pixel = [&readback](u32 x, u32 y) { return &readback.Data[static_cast<size_t>(y) * readback.RowPitchBytes + static_cast<size_t>(x) * 4]; };
        bool backgroundOk = readbackOk && readback.Extent.Width == 64 && readback.Extent.Height == 48 && readback.RowPitchBytes >= 64 * 4 && readback.Data.size() >= static_cast<size_t>(readback.RowPitchBytes) * 48;
        if (backgroundOk) for (u32 channel = 0; channel < 4; ++channel) if (std::abs(static_cast<int>(pixel(2, 2)[channel]) - expectedBackground[channel]) > 3) backgroundOk = false;
        u32 foregroundPixels = 0;
        for (u32 y = 0; readbackOk && y < readback.Extent.Height; ++y) for (u32 x = 0; x < readback.Extent.Width; ++x) { const u8* value = pixel(x, y); const int delta = std::abs(static_cast<int>(value[0]) - expectedBackground[0]) + std::abs(static_cast<int>(value[1]) - expectedBackground[1]) + std::abs(static_cast<int>(value[2]) - expectedBackground[2]); foregroundPixels += delta > 24 ? 1u : 0u; }
        const bool geometryOk = foregroundPixels > 300 && foregroundPixels < 2600;
        const bool resizeOk = firstGeneration == 1 && outputGeneration == 2;
        Log::Info("VulkanSceneViewportRasterV1 snapshot=pass artifact=pass pipeline=pass raster=", resizedRaster ? "pass" : "fail", " readback=", readbackOk ? "pass" : "fail", " geometry=", geometryOk ? "pass" : "fail", " background=", backgroundOk ? "pass" : "fail", " resize=", resizeOk ? "pass" : "fail", " outputGeneration=", outputGeneration, " size=", readback.Extent.Width, "x", readback.Extent.Height, " foregroundPixels=", foregroundPixels, " rowPitch=", readback.RowPitchBytes);
        const auto sampleBrightness = [](const RHI::TextureReadback& sample)
        {
            const u8* value = &sample.Data[static_cast<size_t>(2) * sample.RowPitchBytes + static_cast<size_t>(2) * 4];
            return static_cast<u32>(value[0]) + static_cast<u32>(value[1]) + static_cast<u32>(value[2]);
        };
        const auto neutralToneMap = [](std::array<float, 3> color)
        {
            const float startCompression = 0.76f;
            const float desaturation = 0.15f;
            for (float& channel : color)
                channel = std::min(channel, 6.25f);
            const float minimum = std::min(color[0], std::min(color[1], color[2]));
            const float offset = minimum < 0.08f ? minimum - 6.25f * minimum * minimum : 0.04f;
            for (float& channel : color)
                channel -= offset;
            const float peak = std::max(color[0], std::max(color[1], color[2]));
            if (peak < startCompression)
                return color;
            const float distance = 1.0f - startCompression;
            const float newPeak = 1.0f - distance * distance / (peak + distance - startCompression);
            for (float& channel : color)
                channel *= newPeak / peak;
            const float desaturationAmount = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
            for (float& channel : color)
                channel = channel + (newPeak - channel) * desaturationAmount;
            return color;
        };
        const auto gradeDisplayLinear = [](std::array<float, 3> color, double saturation, double contrast)
        {
            const float luminance = color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
            for (float& channel : color)
            {
                const float saturated = luminance + (channel - luminance) * static_cast<float>(saturation);
                channel = (saturated - 0.5f) * static_cast<float>(contrast) + 0.5f;
                channel = std::clamp(channel, 0.0f, 1.0f);
            }
            return color;
        };
        const auto linearToSrgbByte = [](float value)
        {
            value = std::clamp(value, 0.0f, 1.0f);
            const float encoded = value <= 0.0031308f
                ? value * 12.92f
                : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
            return static_cast<u8>(std::clamp(static_cast<int>(std::lround(encoded * 255.0f)), 0, 255));
        };
        const auto independentCameraEV100 = [](double apertureFNumber, double shutterSeconds, double iso)
        {
            const double exposureValue = (apertureFNumber * apertureFNumber) / shutterSeconds * (100.0 / iso);
            return std::log(exposureValue) / std::log(2.0);
        };
        const auto expectedPostToneMapPixelAtEV = [&](ClearColor input, double ev100, double saturation, double contrast)
        {
            const float exposure = static_cast<float>(std::pow(2.0, -ev100));
            std::array<float, 3> color {
                std::max(input.R * exposure, 0.0f),
                std::max(input.G * exposure, 0.0f),
                std::max(input.B * exposure, 0.0f)
            };
            color = neutralToneMap(color);
            for (float& channel : color)
                channel = std::clamp(channel, 0.0f, 1.0f);
            color = gradeDisplayLinear(color, saturation, contrast);
            return std::array<u8, 4> { linearToSrgbByte(color[0]), linearToSrgbByte(color[1]), linearToSrgbByte(color[2]), 255 };
        };
        const auto expectedPostToneMapPixel = [&](ClearColor input, const RendererColorPipelineSettings& settings)
        {
            return expectedPostToneMapPixelAtEV(input, EffectiveExposureEV100(settings),
                settings.PostToneMapSaturation, settings.PostToneMapContrast);
        };
        const auto expectedPreToneMapPixel = [&](ClearColor input, const RendererColorPipelineSettings& settings)
        {
            const float exposure = static_cast<float>(ManualExposureScale(settings));
            std::array<float, 3> color {
                std::max(input.R * exposure, 0.0f),
                std::max(input.G * exposure, 0.0f),
                std::max(input.B * exposure, 0.0f)
            };
            color = gradeDisplayLinear(color, settings.PostToneMapSaturation, settings.PostToneMapContrast);
            color = neutralToneMap(color);
            for (float& channel : color)
                channel = std::clamp(channel, 0.0f, 1.0f);
            return std::array<u8, 4> { linearToSrgbByte(color[0]), linearToSrgbByte(color[1]), linearToSrgbByte(color[2]), 255 };
        };
        const auto backgroundPixelMatches = [&](const RHI::TextureReadback& sample, const std::array<u8, 4>& expected, int tolerance)
        {
            if (sample.Extent.Width < 3 || sample.Extent.Height < 3
                || sample.RowPitchBytes < sample.Extent.Width * 4
                || sample.Data.size() < static_cast<size_t>(sample.RowPitchBytes) * sample.Extent.Height)
                return false;
            const u8* value = &sample.Data[static_cast<size_t>(2) * sample.RowPitchBytes + static_cast<size_t>(2) * 4];
            for (u32 channel = 0; channel < 4; ++channel)
                if (std::abs(static_cast<int>(value[channel]) - static_cast<int>(expected[channel])) > tolerance)
                    return false;
            return true;
        };
        const auto backgroundPixelDiffers = [&](const RHI::TextureReadback& sample, const std::array<u8, 4>& expected, int minimumTotalDifference)
        {
            if (sample.Extent.Width < 3 || sample.Extent.Height < 3
                || sample.RowPitchBytes < sample.Extent.Width * 4
                || sample.Data.size() < static_cast<size_t>(sample.RowPitchBytes) * sample.Extent.Height)
                return false;
            const u8* value = &sample.Data[static_cast<size_t>(2) * sample.RowPitchBytes + static_cast<size_t>(2) * 4];
            int difference = 0;
            for (u32 channel = 0; channel < 3; ++channel)
                difference += std::abs(static_cast<int>(value[channel]) - static_cast<int>(expected[channel]));
            return difference >= minimumTotalDifference;
        };
        const auto floatToHalf = [](float value)
        {
            const u32 bits = std::bit_cast<u32>(value);
            const u32 sign = (bits >> 16) & 0x8000u;
            const u32 exponent = (bits >> 23) & 0xffu;
            const u32 mantissa = bits & 0x7fffffu;
            if (exponent == 0xffu)
                return static_cast<u16>(sign | 0x7c00u
                    | (mantissa != 0 ? 0x0200u : 0u));
            const int halfExponent = static_cast<int>(exponent) - 127 + 15;
            if (halfExponent >= 31)
                return static_cast<u16>(sign | 0x7c00u);
            if (halfExponent <= 0)
            {
                if (halfExponent < -10)
                    return static_cast<u16>(sign);
                u32 subnormal = (mantissa | 0x800000u) >> (1 - halfExponent);
                subnormal += 0x0fffu + ((subnormal >> 13) & 1u);
                return static_cast<u16>(sign | (subnormal >> 13));
            }
            u32 rounded = mantissa + 0x0fffu + ((mantissa >> 13) & 1u);
            u32 resultExponent = static_cast<u32>(halfExponent);
            if ((rounded & 0x800000u) != 0)
            {
                rounded = 0;
                ++resultExponent;
                if (resultExponent >= 31)
                    return static_cast<u16>(sign | 0x7c00u);
            }
            return static_cast<u16>(sign | (resultExponent << 10)
                | (rounded >> 13));
        };
        const auto hdrHalfAt = [](const RHI::TextureReadback& sample,
            u32 x, u32 y, u32 channel)
        {
            const size_t offset = static_cast<size_t>(y) * sample.RowPitchBytes
                + static_cast<size_t>(x) * 8u + static_cast<size_t>(channel) * 2u;
            return static_cast<u16>(sample.Data[offset])
                | static_cast<u16>(static_cast<u16>(sample.Data[offset + 1u]) << 8u);
        };
        const auto validHdrReadback = [](const RHI::TextureReadback& sample)
        {
            return sample.Extent.Width == 64 && sample.Extent.Height == 48
                && sample.TextureFormat == RHI::Format::R16G16B16A16Float
                && sample.RowPitchBytes >= 64u * 8u
                && sample.Data.size()
                    >= static_cast<size_t>(sample.RowPitchBytes) * 48u;
        };
        auto renderExposure = [&](double ev100, u64 frameIndex,
            RHI::TextureReadback& outReadback)
        {
            if (!Renderer::SetColorPipelineSettings({ ev100 }))
                return false;
            SceneRenderSnapshot exposureSnapshot = snapshot;
            exposureSnapshot.FrameIndex = frameIndex;
            Renderer::PublishSceneRenderSnapshot(std::move(exposureSnapshot));
            return Renderer::PrepareCurrentSceneRasterFrame()
                && m_VulkanSceneRenderer->RenderCurrentSnapshot(64, 48, background)
                && m_VulkanSceneRenderer->ReadbackColor(outReadback);
        };
        RHI::TextureReadback brightExposure;
        RHI::TextureReadback neutralExposure;
        RHI::TextureReadback darkExposure;
        const ToneMapPassConstantCacheDiagnostics baselineConstantCache =
            m_VulkanSceneRenderer->GetToneMapConstantCacheDiagnostics();
        const bool identityGradeOk = resizedRaster && backgroundOk
            && baselineConstantCache.CurrentSettings == RendererColorPipelineSettings {};
        const bool exposureRenders = renderExposure(-2.0, 2, brightExposure)
            && renderExposure(0.0, 3, neutralExposure)
            && renderExposure(2.0, 4, darkExposure);
        const ToneMapPassConstantCacheDiagnostics exposureConstantCache =
            m_VulkanSceneRenderer->GetToneMapConstantCacheDiagnostics();
        const bool exposureReadbacks = exposureRenders
            && brightExposure.Extent.Width == 64 && brightExposure.Extent.Height == 48
            && neutralExposure.Extent.Width == 64 && neutralExposure.Extent.Height == 48
            && darkExposure.Extent.Width == 64 && darkExposure.Extent.Height == 48
            && brightExposure.RowPitchBytes >= 64 * 4
            && neutralExposure.RowPitchBytes >= 64 * 4
            && darkExposure.RowPitchBytes >= 64 * 4;
        const bool monotonic = exposureReadbacks
            && sampleBrightness(brightExposure) > sampleBrightness(neutralExposure)
            && sampleBrightness(neutralExposure) > sampleBrightness(darkExposure);
        const bool constantCacheOk = baselineConstantCache.AllocationCount == 1
            && baselineConstantCache.ReuseCount >= 1
            && exposureConstantCache.AllocationCount == 4
            && exposureConstantCache.ReuseCount >= baselineConstantCache.ReuseCount
            && exposureConstantCache.CurrentGeneration == 4
            && exposureConstantCache.CurrentSettings.ManualExposureEV100 == 2.0;
        RendererColorPipelineSettings calibratedSettings { 0.0 };
        calibratedSettings.ExposureMode = RendererExposureMode::CameraCalibration;
        calibratedSettings.CameraApertureFNumber = 2.0;
        calibratedSettings.CameraShutterSeconds = 0.25;
        calibratedSettings.CameraISO = 200.0;
        RendererColorPipelineSettings calibratedSameEVSettings = calibratedSettings;
        calibratedSameEVSettings.CameraApertureFNumber = 4.0;
        calibratedSameEVSettings.CameraShutterSeconds = 1.0;
        calibratedSameEVSettings.CameraISO = 200.0;
        const double calibratedEV100 = independentCameraEV100(
            calibratedSettings.CameraApertureFNumber,
            calibratedSettings.CameraShutterSeconds,
            calibratedSettings.CameraISO);
        const double calibratedSameEV100 = independentCameraEV100(
            calibratedSameEVSettings.CameraApertureFNumber,
            calibratedSameEVSettings.CameraShutterSeconds,
            calibratedSameEVSettings.CameraISO);
        RHI::TextureReadback calibratedReadback;
        const bool calibratedSet = Renderer::SetColorPipelineSettings(calibratedSettings);
        SceneRenderSnapshot calibratedSnapshot = snapshot;
        calibratedSnapshot.FrameIndex = 5;
        Renderer::PublishSceneRenderSnapshot(std::move(calibratedSnapshot));
        const bool calibratedRendered = calibratedSet
            && Renderer::PrepareCurrentSceneRasterFrame()
            && m_VulkanSceneRenderer->RenderCurrentSnapshot(64, 48, background)
            && m_VulkanSceneRenderer->ReadbackColor(calibratedReadback);
        const ToneMapPassConstantCacheDiagnostics calibratedConstantCache =
            m_VulkanSceneRenderer->GetToneMapConstantCacheDiagnostics();
        const bool calibratedPixelOk = calibratedRendered
            && backgroundPixelMatches(calibratedReadback, expectedPostToneMapPixelAtEV(
                background, calibratedEV100, calibratedSettings.PostToneMapSaturation,
                calibratedSettings.PostToneMapContrast), 4);
        RHI::TextureReadback calibratedRepeatReadback;
        const bool calibratedRepeatSet = Renderer::SetColorPipelineSettings(calibratedSettings);
        SceneRenderSnapshot calibratedRepeatSnapshot = snapshot;
        calibratedRepeatSnapshot.FrameIndex = 6;
        Renderer::PublishSceneRenderSnapshot(std::move(calibratedRepeatSnapshot));
        const bool calibratedRepeatRendered = calibratedRepeatSet
            && Renderer::PrepareCurrentSceneRasterFrame()
            && m_VulkanSceneRenderer->RenderCurrentSnapshot(64, 48, background)
            && m_VulkanSceneRenderer->ReadbackColor(calibratedRepeatReadback);
        const ToneMapPassConstantCacheDiagnostics calibratedRepeatConstantCache =
            m_VulkanSceneRenderer->GetToneMapConstantCacheDiagnostics();
        RHI::TextureReadback calibratedSameEVReadback;
        const bool calibratedSameEVSet = Renderer::SetColorPipelineSettings(calibratedSameEVSettings);
        SceneRenderSnapshot calibratedSameEVSnapshot = snapshot;
        calibratedSameEVSnapshot.FrameIndex = 7;
        Renderer::PublishSceneRenderSnapshot(std::move(calibratedSameEVSnapshot));
        const bool calibratedSameEVRendered = calibratedSameEVSet
            && Renderer::PrepareCurrentSceneRasterFrame()
            && m_VulkanSceneRenderer->RenderCurrentSnapshot(64, 48, background)
            && m_VulkanSceneRenderer->ReadbackColor(calibratedSameEVReadback);
        const ToneMapPassConstantCacheDiagnostics calibratedSameEVConstantCache =
            m_VulkanSceneRenderer->GetToneMapConstantCacheDiagnostics();
        const bool calibratedConstantsOk = calibratedConstantCache.AllocationCount == 5
            && calibratedConstantCache.ReuseCount >= exposureConstantCache.ReuseCount
            && calibratedConstantCache.CurrentGeneration == 5
            && calibratedConstantCache.CurrentSettings == calibratedSettings
            && std::abs(EffectiveExposureEV100(calibratedConstantCache.CurrentSettings) - calibratedEV100) < 0.0001
            && std::abs(calibratedEV100 - 3.0) < 0.0001;
        const bool calibratedCacheIdentityOk = calibratedRepeatRendered && calibratedSameEVRendered
            && calibratedRepeatConstantCache.AllocationCount == 5
            && calibratedRepeatConstantCache.ReuseCount > calibratedConstantCache.ReuseCount
            && calibratedRepeatConstantCache.CurrentGeneration == 5
            && calibratedRepeatConstantCache.CurrentSettings == calibratedSettings
            && calibratedSameEVConstantCache.AllocationCount == 6
            && calibratedSameEVConstantCache.CurrentGeneration == 6
            && calibratedSameEVConstantCache.CurrentSettings == calibratedSameEVSettings
            && std::abs(calibratedSameEV100 - calibratedEV100) < 0.0001
            && std::abs(EffectiveExposureEV100(calibratedSameEVConstantCache.CurrentSettings)
                - calibratedSameEV100) < 0.0001;
        Renderer::SetColorPipelineSettings(previousColorSettings);
        Log::Info("SceneExposureControlV1 backend=Vulkan ev100=-2,0,+2 graph=exact-byte-pass monotonic=",
            monotonic ? "pass" : "fail", " constants=",
            constantCacheOk ? "immutable-retained-cached" : "fail", " allocations=",
            calibratedSameEVConstantCache.AllocationCount, " reuses=", calibratedSameEVConstantCache.ReuseCount,
            " generations=", calibratedSameEVConstantCache.CurrentGeneration,
            " calibratedEV100=", calibratedEV100,
            " calibratedISO=", calibratedSettings.CameraISO,
            " calibrated=camera-fnumber-shutter-iso-",
            (calibratedRendered && calibratedPixelOk && calibratedConstantsOk) ? "pass" : "fail",
            " calibratedCache=same-ev-distinct-settings-",
            calibratedCacheIdentityOk ? "pass" : "fail", " result=",
            (exposureRenders && exposureReadbacks && monotonic && constantCacheOk
                && calibratedRendered && calibratedPixelOk && calibratedConstantsOk
                && calibratedCacheIdentityOk) ? "pass" : "fail");

        const RendererColorPipelineSettings gradedSettings { 0.0, 0.25, 1.5 };
        const ClearColor gradingBackground { 1.40f, 0.23f, 0.05f, 1.0f };
        RHI::TextureReadback gradedReadback;
        const bool gradingSet = Renderer::SetColorPipelineSettings(gradedSettings);
        SceneRenderSnapshot gradingSnapshot = snapshot;
        gradingSnapshot.FrameIndex = 8;
        Renderer::PublishSceneRenderSnapshot(std::move(gradingSnapshot));
        const bool gradingRendered = gradingSet
            && Renderer::PrepareCurrentSceneRasterFrame()
            && m_VulkanSceneRenderer->RenderCurrentSnapshot(64, 48, gradingBackground)
            && m_VulkanSceneRenderer->ReadbackColor(gradedReadback);
        const ToneMapPassConstantCacheDiagnostics gradingConstantCache =
            m_VulkanSceneRenderer->GetToneMapConstantCacheDiagnostics();
        const std::array<u8, 4> postToneMapExpected = expectedPostToneMapPixel(
            gradingBackground, gradedSettings);
        const std::array<u8, 4> preToneMapExpected = expectedPreToneMapPixel(
            gradingBackground, gradedSettings);
        const bool postOrderMatched = gradingRendered
            && backgroundPixelMatches(gradedReadback, postToneMapExpected, 4);
        const bool preOrderRejected = gradingRendered
            && backgroundPixelDiffers(gradedReadback, preToneMapExpected, 12);
        const bool gradingConstantsOk = gradingConstantCache.AllocationCount == 7
            && gradingConstantCache.ReuseCount >= calibratedSameEVConstantCache.ReuseCount
            && gradingConstantCache.CurrentGeneration == 7
            && gradingConstantCache.CurrentSettings == gradedSettings;
        Log::Info("ScenePostToneMapGradingV1 backend=Vulkan identity=", identityGradeOk ? "pass" : "fail",
            " controls=saturation-contrast order=",
            postOrderMatched && preOrderRejected ? "after-tone-map" : "fail",
            " graph=", gradingRendered ? "exact-byte-pass" : "fail",
            " constants=", gradingConstantsOk ? "immutable-retained-cached" : "fail",
            " result=", (identityGradeOk && gradingRendered && postOrderMatched && preOrderRejected && gradingConstantsOk) ? "pass" : "fail",
            " postExpected=", static_cast<u32>(postToneMapExpected[0]), ",",
            static_cast<u32>(postToneMapExpected[1]), ",", static_cast<u32>(postToneMapExpected[2]),
            " preRejected=", preOrderRejected ? "pass" : "fail");
        const ClearColor exactExposureBackground { 0.25f, 0.5f, 1.0f, 1.0f };
        RHI::TextureReadback exactExposureHdr;
        RHI::TextureReadback exactExposureColor;
        const bool exactExposureSet = Renderer::SetColorPipelineSettings({ 2.0 });
        SceneRenderSnapshot exactExposureSnapshot = snapshot;
        exactExposureSnapshot.FrameIndex = 9;
        Renderer::PublishSceneRenderSnapshot(std::move(exactExposureSnapshot));
        const bool exactExposureRendered = exactExposureSet
            && Renderer::PrepareCurrentSceneRasterFrame()
            && m_VulkanSceneRenderer->RenderCurrentSnapshot(
                64, 48, exactExposureBackground)
            && m_VulkanSceneRenderer->ReadbackHdr(exactExposureHdr)
            && m_VulkanSceneRenderer->ReadbackColor(exactExposureColor);
        const std::array<u16, 4> expectedExactExposureHdr {
            floatToHalf(0.0625f), floatToHalf(0.125f),
            floatToHalf(0.25f), floatToHalf(1.0f)
        };
        bool exactExposureHdrExact = exactExposureRendered
            && validHdrReadback(exactExposureHdr);
        for (u32 channel = 0; exactExposureHdrExact && channel < 4; ++channel)
            exactExposureHdrExact = hdrHalfAt(exactExposureHdr, 2, 2, channel)
                == expectedExactExposureHdr[channel];
        const bool singleExposureOutput = exactExposureRendered
            && backgroundPixelMatches(exactExposureColor,
                expectedPostToneMapPixelAtEV(
                    exactExposureBackground, 2.0, 1.0, 1.0), 4);
        const bool doubleExposureRejected = exactExposureRendered
            && backgroundPixelDiffers(exactExposureColor,
                expectedPostToneMapPixelAtEV(
                    exactExposureBackground, 4.0, 1.0, 1.0), 12);

        const ClearColor finiteClampBackground { 2.0f, 1.0f, 0.5f, 1.0f };
        RHI::TextureReadback finiteClampHdr;
        RHI::TextureReadback finiteClampColor;
        const bool finiteClampSet = Renderer::SetColorPipelineSettings({ -16.0 });
        SceneRenderSnapshot finiteClampSnapshot = snapshot;
        finiteClampSnapshot.FrameIndex = 10;
        Renderer::PublishSceneRenderSnapshot(std::move(finiteClampSnapshot));
        const bool finiteClampRendered = finiteClampSet
            && Renderer::PrepareCurrentSceneRasterFrame()
            && m_VulkanSceneRenderer->RenderCurrentSnapshot(
                64, 48, finiteClampBackground)
            && m_VulkanSceneRenderer->ReadbackHdr(finiteClampHdr)
            && m_VulkanSceneRenderer->ReadbackColor(finiteClampColor);
        const std::array<u16, 4> expectedFiniteClampHdr {
            0x7bffu,
            0x7bffu,
            floatToHalf(32768.0f),
            floatToHalf(1.0f)
        };
        bool finiteClampExact = finiteClampRendered
            && validHdrReadback(finiteClampHdr);
        for (u32 channel = 0; finiteClampExact && channel < 4; ++channel)
            finiteClampExact = hdrHalfAt(finiteClampHdr, 2, 2, channel)
                == expectedFiniteClampHdr[channel];
        bool finiteHdrEverywhere = finiteClampRendered
            && validHdrReadback(finiteClampHdr);
        for (u32 y = 0; finiteHdrEverywhere && y < finiteClampHdr.Extent.Height; ++y)
        {
            for (u32 x = 0; finiteHdrEverywhere && x < finiteClampHdr.Extent.Width; ++x)
            {
                for (u32 channel = 0; channel < 4; ++channel)
                {
                    const u16 value = hdrHalfAt(finiteClampHdr, x, y, channel);
                    if ((value & 0x7c00u) == 0x7c00u)
                    {
                        finiteHdrEverywhere = false;
                        break;
                    }
                }
            }
        }
        Renderer::SetColorPipelineSettings(previousColorSettings);
        const bool preExposedHdrOk = exactExposureHdrExact
            && singleExposureOutput && doubleExposureRejected && finiteClampRendered
            && finiteClampExact && finiteHdrEverywhere;
        if (!preExposedHdrOk)
            Log::Error("Scene pre-exposed HDR oracle mismatch: hdrEV2=",
                exactExposureHdrExact ? "exact" : "fail", ", singleOutput=",
                singleExposureOutput ? "yes" : "no", ", doubleRejected=",
                doubleExposureRejected ? "yes" : "no", ", clampRender=",
                finiteClampRendered ? "yes" : "no", ", clampPixel=",
                finiteClampExact ? "exact" : "fail", ", finiteEverywhere=",
                finiteHdrEverywhere ? "yes" : "no", ", hdrEV2Actual=",
                validHdrReadback(exactExposureHdr) ? hdrHalfAt(exactExposureHdr, 2, 2, 0) : 0, ",",
                validHdrReadback(exactExposureHdr) ? hdrHalfAt(exactExposureHdr, 2, 2, 1) : 0, ",",
                validHdrReadback(exactExposureHdr) ? hdrHalfAt(exactExposureHdr, 2, 2, 2) : 0, ",",
                validHdrReadback(exactExposureHdr) ? hdrHalfAt(exactExposureHdr, 2, 2, 3) : 0,
                ", hdrEV2Expected=", expectedExactExposureHdr[0], ",",
                expectedExactExposureHdr[1], ",", expectedExactExposureHdr[2], ",",
                expectedExactExposureHdr[3]);
        Log::Info("ScenePreExposedHdrV1 backend=Vulkan placement=before-RGBA16F scale=exp2-negative-EV toneMapExposure=none finiteClamp=65504 hdrEV2=exact-half doubleApplication=rejected finiteEverywhere=",
            finiteHdrEverywhere ? "pass" : "fail", " singleApplication=",
            exactExposureHdrExact && singleExposureOutput && doubleExposureRejected
                ? "pass" : "fail",
            " result=", preExposedHdrOk ? "pass" : "fail");
        return resizedRaster && readbackOk && geometryOk && backgroundOk && resizeOk
            && liveCatalogPublished && nextLiveCatalogPrepared
            && exposureRenders && exposureReadbacks && monotonic && constantCacheOk
            && exactExposureHdrExact && singleExposureOutput && doubleExposureRejected
            && calibratedRendered && calibratedPixelOk && calibratedConstantsOk && calibratedCacheIdentityOk
            && identityGradeOk && gradingRendered && postOrderMatched && preOrderRejected && gradingConstantsOk
            && finiteClampRendered && finiteClampExact && finiteHdrEverywhere;
    }
}
