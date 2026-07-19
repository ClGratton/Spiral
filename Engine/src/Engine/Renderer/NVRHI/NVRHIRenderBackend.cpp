#include "Engine/Renderer/NVRHI/NVRHIRenderBackend.h"

#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Application.h"
#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
        struct Vertex { float Position[3]; float Color[3]; float UV[2]; };
        struct Constants { float ViewProjection[16]; };
        const std::array<Vertex, 3> vertices {{{ { -0.7f, -0.6f, 0.5f }, { 0.9f, 0.2f, 0.1f }, { 0.0f, 1.0f } }, { { 0.7f, -0.6f, 0.5f }, { 0.9f, 0.2f, 0.1f }, { 1.0f, 1.0f } }, { { 0.0f, 0.7f, 0.5f }, { 0.9f, 0.2f, 0.1f }, { 0.5f, 0.0f } } }};
        const std::array<u16, 3> indices {{ 0, 1, 2 }};
        const Constants constants {{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }};
        ShaderSourceFile source = ShaderLibrary::LoadSource("Engine/Shaders/EditorViewport.hlsl", "Vulkan indexed draw smoke");
        auto makeRequest = [&source](RHI::ShaderStage stage, const char* entry) {
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
            request.ExpectedLayout = {{ "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0}", 1, 64, 0, 0 }};
            if (stage == RHI::ShaderStage::Vertex) request.ExpectedVertexInputs = {{ "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 }, { "Color", "COLOR", 0, 1, "float32x3", 12, 1, 3 }, { "UV", "TEXCOORD", 0, 2, "float32x2", 8, 1, 2 }};
            return request;
        };
        SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
        PortableShaderPackage vertexPackage = source.Status == ShaderSourceStatus::Loaded ? compiler.Compile(makeRequest(RHI::ShaderStage::Vertex, "VSMain")) : PortableShaderPackage {};
        PortableShaderPackage pixelPackage = source.Status == ShaderSourceStatus::Loaded ? compiler.Compile(makeRequest(RHI::ShaderStage::Pixel, "PSMain")) : PortableShaderPackage {};
        std::string validationError;
        const bool packageOk = device && PortableShaderContract::ValidatePackage(makeRequest(RHI::ShaderStage::Vertex, "VSMain"), vertexPackage, validationError)
            && PortableShaderContract::ValidatePackage(makeRequest(RHI::ShaderStage::Pixel, "PSMain"), pixelPackage, validationError);
        // Slang preserves the source entry point in reflection but emits the linked
        // SPIR-V entry point as `main`; NVRHI forwards this name to Vulkan.
        RHI::ShaderDescription vs; vs.DebugName = "VulkanRHIIndexedDrawV1 VS"; vs.SourceName = source.ResolvedPath.string(); vs.EntryPoint = "main"; vs.Stage = RHI::ShaderStage::Vertex; vs.BinaryFormat = RHI::ShaderBinaryFormat::Spirv; vs.Binary = vertexPackage.Spirv;
        RHI::ShaderDescription ps = vs; ps.DebugName = "VulkanRHIIndexedDrawV1 PS"; ps.EntryPoint = "main"; ps.Stage = RHI::ShaderStage::Pixel; ps.Binary = pixelPackage.Spirv;
        Scope<RHI::Shader> vertexShader = packageOk ? device->CreateShader(vs) : nullptr;
        Scope<RHI::Shader> pixelShader = packageOk ? device->CreateShader(ps) : nullptr;
        RHI::PipelineDescription pipelineDescription; pipelineDescription.DebugName = "VulkanRHIIndexedDrawV1 Pipeline"; pipelineDescription.VertexShader = vertexShader.get(); pipelineDescription.PixelShader = pixelShader.get();
        pipelineDescription.VertexInputs = {{ "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Position) }, { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Color) }, { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(Vertex, UV) }};
        pipelineDescription.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }}; pipelineDescription.ColorFormat = RHI::Format::R8G8B8A8Unorm; pipelineDescription.DepthFormat = RHI::Format::D32Float; pipelineDescription.DepthTestEnable = false; pipelineDescription.DepthWriteEnable = false; pipelineDescription.RasterCullMode = RHI::CullMode::None;
        Scope<RHI::Pipeline> pipeline = vertexShader && pixelShader ? device->CreatePipeline(pipelineDescription) : nullptr;
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
        Scope<RHI::CommandList> list = uploads && pipeline && color && depth ? device->CreateCommandList(RHI::QueueType::Graphics, "VulkanRHIIndexedDrawV1") : nullptr;
        RHI::ViewportClear clear; clear.Color[0] = 0.04f; clear.Color[1] = 0.05f; clear.Color[2] = 0.06f; clear.Color[3] = 1.0f;
        const bool draw = list && list->Begin() && list->BindViewportOutputs(*color, depth.get()) && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget) && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite) && list->ClearViewportOutputs(clear) && list->TransitionTexture(*color, RHI::ResourceState::RenderTarget) && list->TransitionTexture(*depth, RHI::ResourceState::DepthWrite);
        if (draw) { list->SetGraphicsPipeline(*pipeline); list->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); list->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) }); list->SetVertexBuffer(0, *vertexBuffer); list->SetIndexBuffer(*indexBuffer, RHI::IndexFormat::Uint16); list->SetGraphicsConstantBuffer(0, *constantBuffer); list->DrawIndexed(3, 1, 0, 0, 0); }
        const bool submitted = draw && list->TransitionTexture(*color, RHI::ResourceState::CopySource) && list->End() && device->SubmitAndWait(*list);
        RHI::TextureReadback readback; const bool readbackOk = submitted && device->ReadbackTexture(*color, readback);
        auto pixelMatches = [&readback](u32 x, u32 y, const std::array<u8, 4>& expected) { if (readback.Data.size() < static_cast<size_t>(readback.RowPitchBytes) * readback.Extent.Height) return false; for (u32 c = 0; c < 4; ++c) if (std::abs(static_cast<int>(readback.Data[y * readback.RowPitchBytes + x * 4 + c]) - expected[c]) > 3) return false; return true; };
        const bool interior = readbackOk && pixelMatches(16, 12, {{ 101, 28, 19, 255 }}); const bool background = readbackOk && pixelMatches(2, 2, {{ 10, 13, 15, 255 }});
        const std::array<u8, 4> interiorPixel = readbackOk ? std::array<u8, 4> {{ readback.Data[12 * readback.RowPitchBytes + 16 * 4], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 1], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 2], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 3] }} : std::array<u8, 4> {};
        u32 foregroundPixels = 0;
        for (u32 y = 0; readbackOk && y < readback.Extent.Height; ++y)
            for (u32 x = 0; x < readback.Extent.Width; ++x)
                foregroundPixels += !pixelMatches(x, y, {{ 10, 13, 15, 255 }}) ? 1u : 0u;
        Log::Info("VulkanRHIIndexedDrawV1 package=", packageOk ? "pass" : "fail", " reflection=", packageOk ? "pass" : "fail", " pipeline=", pipeline ? "pass" : "fail", " constants=", uploads ? "pass" : "fail", " draw=", submitted ? "pass" : "fail", " submit=", submitted ? "pass" : "fail", " readback=", readbackOk ? "pass" : "fail", " interior=", interior ? "pass" : "fail", " background=", background ? "pass" : "fail", " actualInterior=", static_cast<u32>(interiorPixel[0]), ",", static_cast<u32>(interiorPixel[1]), ",", static_cast<u32>(interiorPixel[2]), ",", static_cast<u32>(interiorPixel[3]), " foregroundPixels=", foregroundPixels, " rowPitch=", readback.RowPitchBytes, " vertexKey=", vertexPackage.Key, " pixelKey=", pixelPackage.Key, " validation=", validationError);
        return interior && background;
    }

    bool NVRHIRenderBackend::RunVulkanSceneViewportRasterSmoke()
    {
        RHI::Device* device = m_VulkanContext ? m_VulkanContext->GetRHIDevice() : nullptr;
        if (!device)
            return false;
        m_VulkanSceneRenderer = CreateScope<NVRHIVulkanViewportSceneRenderer>();
        if (!m_VulkanSceneRenderer->Initialize(device))
            return false;

        AssetRegistry smokeRegistry;
        AssetHandle smokeMesh = kInvalidAssetHandle;
        std::string artifactError;
        if (!EnsureDefaultSceneMeshArtifact(smokeRegistry, smokeMesh, artifactError))
        {
            Log::Error("Vulkan Scene viewport smoke could not publish its default mesh artifact: ", artifactError);
            return false;
        }
        Renderer::PublishMeshArtifactResolver(smokeRegistry);

        SceneRenderSnapshot snapshot;
        snapshot.FrameIndex = 1;
        snapshot.WorldGridPolicy = Math::WorldGridPolicy {};
        SceneRenderView view;
        view.Camera.Valid = true;
        view.Camera.TranslationOrigin = { 0.0, 0.0, 0.0 };
        view.Camera.HasCanonicalTranslationOrigin = Math::TryDecomposeWorldPosition(view.Camera.TranslationOrigin, snapshot.WorldGridPolicy, view.Camera.TranslationOriginPosition);
        view.Camera.ViewProjection = Math::Mat4::Identity();
        snapshot.Views.push_back(view);
        SceneRenderMesh mesh;
        mesh.SourceEntity = 1;
        mesh.MeshAsset = smokeMesh;
        mesh.Transform.Position = view.Camera.TranslationOriginPosition;
        snapshot.Meshes.push_back(mesh);
        Renderer::PublishSceneRenderSnapshot(std::move(snapshot));
        if (!Renderer::PrepareCurrentSceneRasterFrame())
            return false;
        const ClearColor background { 0.04f, 0.05f, 0.06f, 1.0f };
        const bool firstRaster = m_VulkanSceneRenderer->RenderCurrentSnapshot(48, 36, background);
        const u64 firstGeneration = m_VulkanSceneRenderer->GetOutputGeneration();
        const bool resizedRaster = firstRaster && m_VulkanSceneRenderer->RenderCurrentSnapshot(64, 48, background);
        const u64 outputGeneration = m_VulkanSceneRenderer->GetOutputGeneration();
        RHI::TextureReadback readback;
        const bool readbackOk = resizedRaster && m_VulkanSceneRenderer->ReadbackColor(readback);
        const std::array<u8, 4> expectedBackground { 10, 13, 15, 255 };
        auto pixel = [&readback](u32 x, u32 y) { return &readback.Data[static_cast<size_t>(y) * readback.RowPitchBytes + static_cast<size_t>(x) * 4]; };
        bool backgroundOk = readbackOk && readback.Extent.Width == 64 && readback.Extent.Height == 48 && readback.RowPitchBytes >= 64 * 4 && readback.Data.size() >= static_cast<size_t>(readback.RowPitchBytes) * 48;
        if (backgroundOk) for (u32 channel = 0; channel < 4; ++channel) if (std::abs(static_cast<int>(pixel(2, 2)[channel]) - expectedBackground[channel]) > 3) backgroundOk = false;
        u32 foregroundPixels = 0;
        for (u32 y = 0; readbackOk && y < readback.Extent.Height; ++y) for (u32 x = 0; x < readback.Extent.Width; ++x) { const u8* value = pixel(x, y); const int delta = std::abs(static_cast<int>(value[0]) - expectedBackground[0]) + std::abs(static_cast<int>(value[1]) - expectedBackground[1]) + std::abs(static_cast<int>(value[2]) - expectedBackground[2]); foregroundPixels += delta > 24 ? 1u : 0u; }
        const bool geometryOk = foregroundPixels > 300 && foregroundPixels < 2600;
        const bool resizeOk = firstGeneration == 1 && outputGeneration == 2;
        Log::Info("VulkanSceneViewportRasterV1 snapshot=pass artifact=pass pipeline=pass raster=", resizedRaster ? "pass" : "fail", " readback=", readbackOk ? "pass" : "fail", " geometry=", geometryOk ? "pass" : "fail", " background=", backgroundOk ? "pass" : "fail", " resize=", resizeOk ? "pass" : "fail", " outputGeneration=", outputGeneration, " size=", readback.Extent.Width, "x", readback.Extent.Height, " foregroundPixels=", foregroundPixels, " rowPitch=", readback.RowPitchBytes);
        return resizedRaster && readbackOk && geometryOk && backgroundOk && resizeOk;
    }
}
