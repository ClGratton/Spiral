#include "Engine/Renderer/NVRHI/NVRHIRenderBackend.h"

#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Application.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"

#include <algorithm>
#include <cmath>

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
            if (args.HasFlag("--vulkan-rhi-indexed-draw-smoke") && !RunVulkanRHIIndexedDrawSmoke())
            {
                Log::Error("Vulkan RHI indexed draw smoke failed");
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

    bool NVRHIRenderBackend::RunVulkanRHIIndexedDrawSmoke()
    {
        RHI::Device* device = m_VulkanContext ? m_VulkanContext->GetRHIDevice() : nullptr;
        constexpr u32 width = 32, height = 24;
        struct Vertex { float Position[3]; float Color[3]; };
        struct Constants { float ViewProjection[16]; };
        const std::array<Vertex, 3> vertices {{{ { -0.7f, -0.6f, 0.5f }, { 0.9f, 0.2f, 0.1f } }, { { 0.7f, -0.6f, 0.5f }, { 0.9f, 0.2f, 0.1f } }, { { 0.0f, 0.7f, 0.5f }, { 0.9f, 0.2f, 0.1f } } }};
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
            if (stage == RHI::ShaderStage::Vertex) request.ExpectedVertexInputs = {{ "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 }, { "Color", "COLOR", 0, 1, "float32x3", 12, 1, 3 }};
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
        pipelineDescription.VertexInputs = {{ "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Position) }, { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Color) }};
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
        const bool interior = readbackOk && pixelMatches(16, 12, {{ 209, 59, 40, 255 }}); const bool background = readbackOk && pixelMatches(2, 2, {{ 10, 13, 15, 255 }});
        const std::array<u8, 4> interiorPixel = readbackOk ? std::array<u8, 4> {{ readback.Data[12 * readback.RowPitchBytes + 16 * 4], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 1], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 2], readback.Data[12 * readback.RowPitchBytes + 16 * 4 + 3] }} : std::array<u8, 4> {};
        u32 foregroundPixels = 0;
        for (u32 y = 0; readbackOk && y < readback.Extent.Height; ++y)
            for (u32 x = 0; x < readback.Extent.Width; ++x)
                foregroundPixels += !pixelMatches(x, y, {{ 10, 13, 15, 255 }}) ? 1u : 0u;
        Log::Info("VulkanRHIIndexedDrawV1 package=", packageOk ? "pass" : "fail", " reflection=", packageOk ? "pass" : "fail", " pipeline=", pipeline ? "pass" : "fail", " constants=", uploads ? "pass" : "fail", " draw=", submitted ? "pass" : "fail", " submit=", submitted ? "pass" : "fail", " readback=", readbackOk ? "pass" : "fail", " interior=", interior ? "pass" : "fail", " background=", background ? "pass" : "fail", " actualInterior=", static_cast<u32>(interiorPixel[0]), ",", static_cast<u32>(interiorPixel[1]), ",", static_cast<u32>(interiorPixel[2]), ",", static_cast<u32>(interiorPixel[3]), " foregroundPixels=", foregroundPixels, " rowPitch=", readback.RowPitchBytes, " vertexKey=", vertexPackage.Key, " pixelKey=", pixelPackage.Key, " validation=", validationError);
        return interior && background;
    }
}
