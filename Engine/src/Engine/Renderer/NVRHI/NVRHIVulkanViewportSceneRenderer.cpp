#include "Engine/Renderer/NVRHI/NVRHIVulkanViewportSceneRenderer.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"
#include "Engine/RenderGraph/RenderGraph.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <array>
    #include <cstddef>
    #include <cstring>
    #include <filesystem>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_VULKAN)
    namespace
    {
        constexpr u32 kConstantBufferSize = 256;

        struct Vertex
        {
            float Position[3];
            float Color[3];
            float UV[2];
        };

        struct Constants { float ViewProjection[16]; };

        constexpr std::array<Vertex, 24> kVertices = {
            Vertex{{ -0.75f, -0.75f, -0.75f }, { 0.22f, 0.68f, 1.00f }, { 0.0f, 1.0f }}, Vertex{{ -0.75f,  0.75f, -0.75f }, { 0.22f, 0.68f, 1.00f }, { 0.0f, 0.0f }}, Vertex{{  0.75f,  0.75f, -0.75f }, { 0.22f, 0.68f, 1.00f }, { 1.0f, 0.0f }}, Vertex{{  0.75f, -0.75f, -0.75f }, { 0.22f, 0.68f, 1.00f }, { 1.0f, 1.0f }},
            Vertex{{  0.75f, -0.75f,  0.75f }, { 0.95f, 0.72f, 0.28f }, { 0.0f, 1.0f }}, Vertex{{  0.75f,  0.75f,  0.75f }, { 0.95f, 0.72f, 0.28f }, { 0.0f, 0.0f }}, Vertex{{ -0.75f,  0.75f,  0.75f }, { 0.95f, 0.72f, 0.28f }, { 1.0f, 0.0f }}, Vertex{{ -0.75f, -0.75f,  0.75f }, { 0.95f, 0.72f, 0.28f }, { 1.0f, 1.0f }},
            Vertex{{ -0.75f, -0.75f,  0.75f }, { 0.26f, 0.88f, 0.55f }, { 0.0f, 1.0f }}, Vertex{{ -0.75f,  0.75f,  0.75f }, { 0.26f, 0.88f, 0.55f }, { 0.0f, 0.0f }}, Vertex{{ -0.75f,  0.75f, -0.75f }, { 0.26f, 0.88f, 0.55f }, { 1.0f, 0.0f }}, Vertex{{ -0.75f, -0.75f, -0.75f }, { 0.26f, 0.88f, 0.55f }, { 1.0f, 1.0f }},
            Vertex{{  0.75f, -0.75f, -0.75f }, { 0.88f, 0.35f, 0.37f }, { 0.0f, 1.0f }}, Vertex{{  0.75f,  0.75f, -0.75f }, { 0.88f, 0.35f, 0.37f }, { 0.0f, 0.0f }}, Vertex{{  0.75f,  0.75f,  0.75f }, { 0.88f, 0.35f, 0.37f }, { 1.0f, 0.0f }}, Vertex{{  0.75f, -0.75f,  0.75f }, { 0.88f, 0.35f, 0.37f }, { 1.0f, 1.0f }},
            Vertex{{ -0.75f,  0.75f, -0.75f }, { 0.72f, 0.52f, 0.96f }, { 0.0f, 1.0f }}, Vertex{{ -0.75f,  0.75f,  0.75f }, { 0.72f, 0.52f, 0.96f }, { 0.0f, 0.0f }}, Vertex{{  0.75f,  0.75f,  0.75f }, { 0.72f, 0.52f, 0.96f }, { 1.0f, 0.0f }}, Vertex{{  0.75f,  0.75f, -0.75f }, { 0.72f, 0.52f, 0.96f }, { 1.0f, 1.0f }},
            Vertex{{ -0.75f, -0.75f,  0.75f }, { 0.24f, 0.75f, 0.82f }, { 0.0f, 1.0f }}, Vertex{{ -0.75f, -0.75f, -0.75f }, { 0.24f, 0.75f, 0.82f }, { 0.0f, 0.0f }}, Vertex{{  0.75f, -0.75f, -0.75f }, { 0.24f, 0.75f, 0.82f }, { 1.0f, 0.0f }}, Vertex{{  0.75f, -0.75f,  0.75f }, { 0.24f, 0.75f, 0.82f }, { 1.0f, 1.0f }}
        };
        constexpr std::array<u16, 36> kIndices = { 0,1,2,0,2,3, 4,5,6,4,6,7, 8,9,10,8,10,11, 12,13,14,12,14,15, 16,17,18,16,18,19, 20,21,22,20,22,23 };

        RHI::BufferUsage WithUsage(RHI::BufferUsage first, RHI::BufferUsage second)
        {
            return static_cast<RHI::BufferUsage>(static_cast<u32>(first) | static_cast<u32>(second));
        }
    }

    struct NVRHIVulkanViewportSceneRenderer::Impl
    {
        bool RecordBootstrapReference(
            RHI::Texture& colorTexture,
            RHI::Texture& depthTexture,
            u32 width,
            u32 height,
            const RHI::ViewportClear& clear,
            const SceneRasterFrame& frame,
            const std::vector<Scope<RHI::Buffer>>& constants)
        {
            Scope<RHI::CommandList> commands = m_Device->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Bootstrap Reference");
            if (!commands || !commands->Begin()
                || !commands->TransitionTexture(colorTexture, RHI::ResourceState::RenderTarget)
                || !commands->TransitionTexture(depthTexture, RHI::ResourceState::DepthWrite)
                || !commands->BindViewportOutputs(colorTexture, &depthTexture)
                || !commands->ClearViewportOutputs(clear)) return false;
            commands->BeginDebugMarker("Scene Viewport Bootstrap Reference Raster");
            if (m_Pipeline && m_VertexBuffer && m_IndexBuffer && frame.HasValidView && !frame.Instances.empty())
            {
                commands->SetGraphicsPipeline(*m_Pipeline); commands->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); commands->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) }); commands->SetVertexBuffer(0, *m_VertexBuffer); commands->SetIndexBuffer(*m_IndexBuffer, RHI::IndexFormat::Uint16);
                for (const Scope<RHI::Buffer>& constant : constants) { commands->SetGraphicsConstantBuffer(0, *constant); commands->DrawIndexed(static_cast<u32>(kIndices.size()), 1, 0, 0, 0); }
            }
            commands->EndDebugMarker();
            return commands->TransitionTexture(colorTexture, RHI::ResourceState::CopySource)
                && commands->End() && m_Device->SubmitAndWait(*commands);
        }

        bool ReadbackGraphOutput(RHI::Texture& colorTexture, RHI::TextureReadback& readback)
        {
            Scope<RHI::CommandList> commands = m_Device->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Graph Comparison Readback");
            if (!commands || !commands->Begin() || !commands->TransitionTexture(colorTexture, RHI::ResourceState::CopySource)
                || !commands->End() || !m_Device->SubmitAndWait(*commands) || !m_Device->ReadbackTexture(colorTexture, readback)) return false;
            commands = m_Device->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Graph Comparison Restore");
            return commands && commands->Begin() && commands->TransitionTexture(colorTexture, RHI::ResourceState::ShaderResource)
                && commands->End() && m_Device->SubmitAndWait(*commands);
        }

        bool Initialize(RHI::Device* device)
        {
            m_Device = device;
            if (!m_Device)
                return false;

            ShaderSourceFile source = ShaderLibrary::LoadSource("Engine/Shaders/EditorViewport.hlsl", "Vulkan Scene viewport");
            if (source.Status != ShaderSourceStatus::Loaded)
                return false;
            auto makeRequest = [&source](RHI::ShaderStage stage, const char* entry) {
                PortableShaderRequest request;
                request.SourceName = source.ResolvedPath.string(); request.Source = source.Source; request.EntryPoint = entry; request.Stage = stage;
#ifdef _WIN32
                request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv }; request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
#else
                request.Targets = { PortableShaderTarget::Spirv };
#endif
                request.CompilerIdentity = "Slang"; request.CompilerVersion = "2026.13.1"; request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
                request.ExpectedLayout = {{ "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0}", 1, 64, 0, 0 }};
                if (stage == RHI::ShaderStage::Vertex) request.ExpectedVertexInputs = {{ "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 }, { "Color", "COLOR", 0, 1, "float32x3", 12, 1, 3 }, { "UV", "TEXCOORD", 0, 2, "float32x2", 8, 1, 2 }};
                return request;
            };
            SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
            PortableShaderPackage vertex = compiler.Compile(makeRequest(RHI::ShaderStage::Vertex, "VSMain"));
            PortableShaderPackage pixel = compiler.Compile(makeRequest(RHI::ShaderStage::Pixel, "PSMain"));
            std::string error;
            if (!PortableShaderContract::ValidatePackage(makeRequest(RHI::ShaderStage::Vertex, "VSMain"), vertex, error) || !PortableShaderContract::ValidatePackage(makeRequest(RHI::ShaderStage::Pixel, "PSMain"), pixel, error)) { Log::Error("Vulkan Scene viewport shader package validation failed: ", error); return false; }
            RHI::ShaderDescription vs; vs.DebugName = "Vulkan Scene Viewport VS"; vs.SourceName = source.ResolvedPath.string(); vs.EntryPoint = "main"; vs.Stage = RHI::ShaderStage::Vertex; vs.BinaryFormat = RHI::ShaderBinaryFormat::Spirv; vs.Binary = vertex.Spirv;
            RHI::ShaderDescription ps = vs; ps.DebugName = "Vulkan Scene Viewport PS"; ps.Stage = RHI::ShaderStage::Pixel; ps.Binary = pixel.Spirv;
            m_VertexShader = m_Device->CreateShader(vs); m_PixelShader = m_Device->CreateShader(ps);
            RHI::PipelineDescription pipeline; pipeline.DebugName = "Vulkan Scene Viewport Pipeline"; pipeline.VertexShader = m_VertexShader.get(); pipeline.PixelShader = m_PixelShader.get(); pipeline.VertexInputs = {{ "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Position) }, { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(Vertex, Color) }, { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(Vertex, UV) }}; pipeline.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }}; pipeline.ColorFormat = RHI::Format::R8G8B8A8Unorm; pipeline.DepthFormat = RHI::Format::D32Float; pipeline.DepthTestEnable = true; pipeline.DepthWriteEnable = true; pipeline.RasterCullMode = RHI::CullMode::None;
            m_Pipeline = m_VertexShader && m_PixelShader ? m_Device->CreatePipeline(pipeline) : nullptr;
            auto buffer = [this](const char* name, u64 size, u32 stride, RHI::BufferUsage usage) { RHI::BufferDescription d; d.DebugName = name; d.SizeBytes = size; d.StrideBytes = stride; d.Usage = WithUsage(usage, RHI::BufferUsage::CopyDest); return m_Device->CreateBuffer(d); };
            m_VertexBuffer = buffer("Vulkan Scene Viewport Vertices", sizeof(kVertices), sizeof(Vertex), RHI::BufferUsage::Vertex);
            m_IndexBuffer = buffer("Vulkan Scene Viewport Indices", sizeof(kIndices), sizeof(u16), RHI::BufferUsage::Index);
            return m_Pipeline && m_VertexBuffer && m_IndexBuffer && m_Device->UploadBuffer(*m_VertexBuffer, kVertices.data(), sizeof(kVertices)) && m_Device->UploadBuffer(*m_IndexBuffer, kIndices.data(), sizeof(kIndices));
        }

        bool EnsureOutputs(u32 width, u32 height)
        {
            if (m_Color && m_Depth && m_Width == width && m_Height == height) return true;
            // Render rejects replacement while the submitted-frame owner still
            // retains an exact token for the current output generation.
            m_Color.reset(); m_Depth.reset();
            RHI::TextureDescription color; color.DebugName = "Vulkan Scene Viewport Color"; color.Extent = { width, height }; color.TextureFormat = RHI::Format::R8G8B8A8Unorm; color.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource) | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            RHI::TextureDescription depth = color; depth.DebugName = "Vulkan Scene Viewport Depth"; depth.TextureFormat = RHI::Format::D32Float; depth.Usage = RHI::TextureUsage::DepthStencil;
            m_Color = m_Device->CreateTexture(color); m_Depth = m_Device->CreateTexture(depth);
            if (!m_Color || !m_Depth) return false;
            m_Width = width; m_Height = height; ++m_OutputGeneration; return true;
        }

        bool Render(const SceneRenderSnapshot& snapshot, u32 width, u32 height, const ClearColor& clearColor)
        {
            if (!m_Device || width == 0 || height == 0) return false;
            const SubmittedRenderGraphFrameOwner::PollResult retirement = m_SubmittedGraphFrames.Poll(*m_Device);
            if (!retirement.Success) { Log::Error("Vulkan Scene viewport RenderGraph retirement failed: ", retirement.Error); return false; }
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) for (const SubmittedRenderGraphFrameOwner::RetiredFrame& retired : retirement.Retired) if (!retired.TimestampScopes.empty()) { const bool ready = std::all_of(retired.TimestampScopes.begin(), retired.TimestampScopes.end(), [](const RenderGraph::RawTimestampScope& scope) { return scope.Start.Status == RHI::QueryResultStatus::Ready && scope.End.Status == RHI::QueryResultStatus::Ready; }); Log::Info("RenderGraphTimestampScopesV1 backend=Vulkan frame=", retired.FrameIndex, " scopes=", retired.TimestampScopes.size(), " raw=", ready ? "ready" : "disjoint", " cpuWaitBetween=no result=", ready ? "pass" : "fail"); }
            if (!m_SubmittedGraphFrames.HasCapacity()) { Log::Error("Vulkan Scene viewport RenderGraph retirement capacity exhausted without a CPU wait"); return false; }
            if ((m_Width != width || m_Height != height) && m_SubmittedGraphFrames.GetPendingCount() != 0)
            {
                Log::Error("Vulkan Scene viewport output replacement deferred because an exact RenderGraph token is still incomplete");
                return false;
            }
            if (!EnsureOutputs(width, height)) return false;
            const std::shared_ptr<const SceneRasterFrame> prepared = Renderer::GetPreparedSceneRasterFrame();
            if (!prepared || prepared->SnapshotFrameIndex != snapshot.FrameIndex) return false;
            SceneRasterFrame frame = *prepared;
            if (!frame.HasValidView || frame.Instances.empty()) return false;
            Ref<std::vector<Scope<RHI::Buffer>>> constants = CreateRef<std::vector<Scope<RHI::Buffer>>>(); constants->reserve(frame.Instances.size());
            for (const SceneRasterInstance& instance : frame.Instances) { RHI::BufferDescription d; d.DebugName = "Vulkan Scene Viewport Instance Constants"; d.SizeBytes = kConstantBufferSize; d.Usage = WithUsage(RHI::BufferUsage::Constant, RHI::BufferUsage::CopyDest); Scope<RHI::Buffer> b = m_Device->CreateBuffer(d); Constants c {}; std::memcpy(c.ViewProjection, instance.ModelViewProjection.Values, sizeof(c.ViewProjection)); if (!b || !m_Device->UploadBuffer(*b, &c, sizeof(c))) return false; constants->push_back(std::move(b)); }
            RHI::ResourceState colorState = RHI::ResourceState::Unknown;
            RHI::ResourceState depthState = RHI::ResourceState::Unknown;
            if (!m_Device->QueryResourceState(m_Color.get(), colorState) || !m_Device->QueryResourceState(m_Depth.get(), depthState)) return false;
            RHI::ViewportClear clear; clear.Color[0] = clearColor.R; clear.Color[1] = clearColor.G; clear.Color[2] = clearColor.B; clear.Color[3] = clearColor.A;
            Scope<RenderGraph> graph = CreateScope<RenderGraph>();
            RHI::TextureDescription colorDescription = m_Color->GetDescription(); colorDescription.InitialState = colorState;
            RHI::TextureDescription depthDescription = m_Depth->GetDescription(); depthDescription.InitialState = depthState;
            const RenderGraph::ResourceHandle color = graph->AddTexture(colorDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle depth = graph->AddTexture(depthDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::PassHandle clearPass = graph->AddPass("Scene Viewport Graph Clear", RHI::QueueType::Graphics);
            graph->AddWrite(clearPass, color, RHI::ResourceState::RenderTarget); graph->AddWrite(clearPass, depth, RHI::ResourceState::DepthWrite);
            graph->SetPassCallback(clearPass, [clear](RenderGraph::ExecutionContext& context) { RHI::Texture* graphColor = context.GetTexture({ 0 }); RHI::Texture* graphDepth = context.GetTexture({ 1 }); return graphColor && graphDepth && context.GetCommandList().BindViewportOutputs(*graphColor, graphDepth) && context.GetCommandList().ClearViewportOutputs(clear); });
            graph->SetPassWorkerRecordingEligible(clearPass);
            const RenderGraph::PassHandle rasterPass = graph->AddPass("Scene Viewport Graph Raster", RHI::QueueType::Graphics);
            graph->AddWrite(rasterPass, color, RHI::ResourceState::RenderTarget); graph->AddWrite(rasterPass, depth, RHI::ResourceState::DepthWrite);
            graph->SetPassCallback(rasterPass, [this, width, height, &frame, constants](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphColor = context.GetTexture({ 0 }); RHI::Texture* graphDepth = context.GetTexture({ 1 }); RHI::CommandList& commands = context.GetCommandList();
                if (!graphColor || !graphDepth || !commands.BindViewportOutputs(*graphColor, graphDepth)) return false;
                commands.SetGraphicsPipeline(*m_Pipeline); commands.SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); commands.SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) }); commands.SetVertexBuffer(0, *m_VertexBuffer); commands.SetIndexBuffer(*m_IndexBuffer, RHI::IndexFormat::Uint16);
                for (const Scope<RHI::Buffer>& constant : *constants) { commands.SetGraphicsConstantBuffer(0, *constant); commands.DrawIndexed(static_cast<u32>(kIndices.size()), 1, 0, 0, 0); ++frame.IssuedDrawCount; }
                return true;
            });
            const RenderGraph::PassHandle handoffPass = graph->AddPass("Scene Viewport Graph Output Handoff", RHI::QueueType::Graphics);
            graph->AddRead(handoffPass, color, RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
            graph->SetPassCallback(handoffPass, [](RenderGraph::ExecutionContext& context) { return context.GetTexture({ 0 }) != nullptr; });
            graph->SetPassWorkerRecordingEligible(handoffPass);
            const RenderGraph::CompileResult compiled = graph->Compile();
            RenderGraph::ExecuteOptions executeOptions; executeOptions.RecordingMode = Application::Get().GetSpecification().CommandLineArgs.HasFlag("--frame-task-single-thread") ? FrameTaskExecutionMode::DeterministicSingleThread : FrameTaskExecutionMode::Parallel; executeOptions.EnableTimestampScopes = m_Device->GetCapabilities().GetFeature(RHI::DeviceFeature::Timestamps).IsUsable();
            const RenderGraph::ExecuteResult executed = graph->BindTexture(color, *m_Color) && graph->BindTexture(depth, *m_Depth) ? graph->Execute(*m_Device, compiled, executeOptions) : RenderGraph::ExecuteResult {};
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("RenderGraphRecordingV1 backend=Vulkan mode=", executeOptions.RecordingMode == FrameTaskExecutionMode::Parallel ? "worker" : "inline", " workerPasses=", executed.WorkerRecordedPassCount, " overlap=", executed.WorkerRecordingOverlapObserved ? "yes" : "no", " submitted=", executed.AcceptedPassCount, " result=", executed.Success ? "pass" : "fail");
            if (!executed.Completions.empty())
            {
                std::string retentionError;
                if (!m_SubmittedGraphFrames.Retain(snapshot.FrameIndex, std::move(graph), compiled, executed,
                    { constants }, &retentionError))
                {
                    Log::Error("Vulkan Scene viewport could not retain an accepted RenderGraph submission: ", retentionError);
                    m_Device->WaitIdle();
                    return false;
                }
            }
            if (!executed.Success) { Log::Error("Vulkan Scene viewport render graph failed: ", executed.Error); return false; }
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("ProductionRenderGraphRetirementV1 backend=Vulkan frame=", snapshot.FrameIndex, " passes=", executed.AcceptedPassCount, " cpuWaitBetween=no pending=", m_SubmittedGraphFrames.GetPendingCount(), " result=pass");
            const bool comparisonRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke");
            if (comparisonRequested)
            {
                RHI::TextureDescription referenceColorDescription = m_Color->GetDescription(); referenceColorDescription.DebugName = "Scene Viewport Bootstrap Reference Color";
                RHI::TextureDescription referenceDepthDescription = m_Depth->GetDescription(); referenceDepthDescription.DebugName = "Scene Viewport Bootstrap Reference Depth";
                Scope<RHI::Texture> referenceColor = m_Device->CreateTexture(referenceColorDescription);
                Scope<RHI::Texture> referenceDepth = m_Device->CreateTexture(referenceDepthDescription);
                RHI::TextureReadback graphReadback, referenceReadback;
                const bool referenceRendered = referenceColor && referenceDepth && RecordBootstrapReference(*referenceColor, *referenceDepth, width, height, clear, frame, *constants);
                const bool readBack = referenceRendered && ReadbackGraphOutput(*m_Color, graphReadback) && m_Device->ReadbackTexture(*referenceColor, referenceReadback);
                const bool equivalent = readBack && graphReadback.Extent.Width == referenceReadback.Extent.Width && graphReadback.Extent.Height == referenceReadback.Extent.Height
                    && graphReadback.RowPitchBytes == referenceReadback.RowPitchBytes && graphReadback.Data == referenceReadback.Data;
                Log::Info("SceneViewportRenderGraphV1 backend=Vulkan passes=3 labels=clear,raster,output-handoff execution=pass reference=direct comparator=exact-byte-", equivalent ? "pass" : "fail", " size=", width, "x", height, " bytes=", graphReadback.Data.size());
                if (!equivalent) return false;
            }
            Renderer::PublishSceneRasterFrame(std::move(frame));
            if (!comparisonRequested) Log::Trace("Scene viewport graph rendered without the smoke-only bootstrap comparator");
            return true;
        }

        bool ReadbackColor(RHI::TextureReadback& readback) const { return m_Device && m_Color && m_Device->ReadbackTexture(*m_Color, readback); }
        void Shutdown() { if (m_Device) m_Device->WaitIdle(); m_SubmittedGraphFrames.ReleaseAfterDeviceIdle(); m_Color.reset(); m_Depth.reset(); m_IndexBuffer.reset(); m_VertexBuffer.reset(); m_Pipeline.reset(); m_PixelShader.reset(); m_VertexShader.reset(); m_Device = nullptr; }
        RHI::Device* m_Device = nullptr; Scope<RHI::Shader> m_VertexShader, m_PixelShader; Scope<RHI::Pipeline> m_Pipeline; Scope<RHI::Buffer> m_VertexBuffer, m_IndexBuffer; Scope<RHI::Texture> m_Color, m_Depth; SubmittedRenderGraphFrameOwner m_SubmittedGraphFrames; u32 m_Width = 0, m_Height = 0; u64 m_OutputGeneration = 0;
    };

    NVRHIVulkanViewportSceneRenderer::NVRHIVulkanViewportSceneRenderer() = default;
    NVRHIVulkanViewportSceneRenderer::~NVRHIVulkanViewportSceneRenderer() { Shutdown(); }
    bool NVRHIVulkanViewportSceneRenderer::Initialize(RHI::Device* device) { m_Impl = CreateScope<Impl>(); if (m_Impl->Initialize(device)) return true; m_Impl.reset(); return false; }
    void NVRHIVulkanViewportSceneRenderer::Shutdown() { if (m_Impl) { m_Impl->Shutdown(); m_Impl.reset(); } }
    bool NVRHIVulkanViewportSceneRenderer::RenderCurrentSnapshot(u32 width, u32 height, const ClearColor& clearColor)
    {
        const std::shared_ptr<const SceneRenderSnapshot> snapshot = Renderer::GetSceneRenderSnapshot();
        const std::shared_ptr<const SceneRasterFrame> prepared = Renderer::GetPreparedSceneRasterFrame();
        if (!snapshot || !prepared || prepared->SnapshotFrameIndex != snapshot->FrameIndex) return false;
        return Render(*snapshot, width, height, clearColor);
    }
    bool NVRHIVulkanViewportSceneRenderer::Render(const SceneRenderSnapshot& snapshot, u32 width, u32 height, const ClearColor& clearColor) { return m_Impl && m_Impl->Render(snapshot, width, height, clearColor); }
    bool NVRHIVulkanViewportSceneRenderer::ReadbackColor(RHI::TextureReadback& readback) const { return m_Impl && m_Impl->ReadbackColor(readback); }
    u64 NVRHIVulkanViewportSceneRenderer::GetOutputGeneration() const { return m_Impl ? m_Impl->m_OutputGeneration : 0; }
    u32 NVRHIVulkanViewportSceneRenderer::GetOutputWidth() const { return m_Impl ? m_Impl->m_Width : 0; }
    u32 NVRHIVulkanViewportSceneRenderer::GetOutputHeight() const { return m_Impl ? m_Impl->m_Height : 0; }
    RHI::NVRHIVulkanTextureNativeHandles NVRHIVulkanViewportSceneRenderer::GetOutputNativeHandles() const
    {
        return m_Impl && m_Impl->m_Color ? RHI::GetNVRHIVulkanTextureNativeHandles(*m_Impl->m_Color) : RHI::NVRHIVulkanTextureNativeHandles {};
    }
#endif
}
