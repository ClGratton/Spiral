#include "Engine/Renderer/NVRHI/NVRHID3D12ViewportSceneRenderer.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Math/Math.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/RenderGraph/RenderGraph.h"
#include "Engine/Renderer/AsyncShaderPackageService.h"
#include "Engine/Renderer/MeshGpuResourceCache.h"
#include "Engine/Renderer/NVRHI/D3D12DebugMarkers.h"
#include "Engine/Renderer/NVRHI/D3D12ViewportShaderReloadCoordinator.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"

#if defined(GE_HAS_NVRHI_D3D12)
    #include <cstddef>
#include <filesystem>
#include <memory>
    #include <string>
    #include <string_view>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_D3D12)
    namespace
    {
        constexpr u32 kViewportConstantBufferSize = 256;
        constexpr std::string_view kViewportShaderPath = "Engine/Shaders/EditorViewport.hlsl";

        std::string_view ViewportShaderPath()
        {
            const std::string_view overridePath = Application::Get().GetSpecification().CommandLineArgs.GetOptionValue("--viewport-shader-path");
            return overridePath.empty() ? kViewportShaderPath : overridePath;
        }

        struct ViewportConstants
        {
            float ViewProjection[16];
        };

        struct ConstantBufferAllocation
        {
            Scope<RHI::Buffer> Buffer;
            std::byte* Mapped = nullptr;
        };

        struct ConstantBufferSet
        {
            ~ConstantBufferSet()
            {
                for (ConstantBufferAllocation& allocation : Allocations)
                    if (allocation.Buffer && allocation.Mapped)
                        allocation.Buffer->Unmap();
            }

            std::vector<ConstantBufferAllocation> Allocations;
        };

        struct SceneMeshDraw
        {
            Ref<const MeshGpuResourceBundle> Bundle;
            MeshGpuPrimitiveRange Primitive;
            size_t ConstantIndex = 0;
        };

        class ScopedCommandListDebugMarker final
        {
        public:
            ScopedCommandListDebugMarker(RHI::CommandList& commandList, std::string_view name)
                : m_CommandList(commandList)
            {
                m_CommandList.BeginDebugMarker(name);
            }

            ~ScopedCommandListDebugMarker()
            {
                m_CommandList.EndDebugMarker();
            }

            ScopedCommandListDebugMarker(const ScopedCommandListDebugMarker&) = delete;
            ScopedCommandListDebugMarker& operator=(const ScopedCommandListDebugMarker&) = delete;

        private:
            RHI::CommandList& m_CommandList;
        };

        D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            return barrier;
        }

    }

    struct NVRHID3D12ViewportSceneRenderer::Impl
    {
        bool RecordBootstrapReference(
            RHI::Texture& colorTexture,
            RHI::Texture& depthTexture,
            u32 width,
            u32 height,
            const RHI::ViewportClear& clear,
            const SceneRasterFrame& rasterFrame,
            const std::vector<ConstantBufferAllocation>* constantBuffers,
            const std::vector<SceneMeshDraw>& draws)
        {
            Scope<RHI::CommandList> commands = m_RHIDevice->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Bootstrap Reference");
            if (!commands || !commands->Begin()
                || !commands->TransitionTexture(colorTexture, RHI::ResourceState::RenderTarget)
                || !commands->TransitionTexture(depthTexture, RHI::ResourceState::DepthWrite)
                || !commands->BindViewportOutputs(colorTexture, &depthTexture)
                || !commands->ClearViewportOutputs(clear)) return false;
            commands->BeginDebugMarker("Scene Viewport Bootstrap Reference Raster");
            if (m_Pipeline && rasterFrame.HasValidView && !rasterFrame.Instances.empty())
            {
                commands->SetGraphicsPipeline(*m_Pipeline);
                commands->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f });
                commands->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
                for (const SceneMeshDraw& draw : draws)
                {
                    commands->SetVertexBuffer(0, *draw.Bundle->VertexBuffer);
                    commands->SetIndexBuffer(*draw.Bundle->IndexBuffer, RHI::IndexFormat::Uint32);
                    commands->SetGraphicsConstantBuffer(0, *(*constantBuffers)[draw.ConstantIndex].Buffer);
                    commands->DrawIndexed(draw.Primitive.IndexCount, 1, draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0);
                }
            }
            commands->EndDebugMarker();
            return commands->TransitionTexture(colorTexture, RHI::ResourceState::CopySource)
                && commands->End() && m_RHIDevice->SubmitAndWait(*commands);
        }

        bool ReadbackGraphOutput(RHI::Texture& colorTexture, RHI::TextureReadback& readback)
        {
            Scope<RHI::CommandList> commands = m_RHIDevice->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Graph Comparison Readback");
            if (!commands || !commands->Begin() || !commands->TransitionTexture(colorTexture, RHI::ResourceState::CopySource)
                || !commands->End() || !m_RHIDevice->SubmitAndWait(*commands) || !m_RHIDevice->ReadbackTexture(colorTexture, readback)) return false;
            commands = m_RHIDevice->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Graph Comparison Restore");
            return commands && commands->Begin() && commands->TransitionTexture(colorTexture, RHI::ResourceState::ShaderResource)
                && commands->End() && m_RHIDevice->SubmitAndWait(*commands);
        }

        bool Initialize(RHI::Device* rhiDevice)
        {
            m_RHIDevice = rhiDevice;
            if (!m_RHIDevice)
                return false;

            if (!RequestInitialPipeline())
                return false;
            return true;
        }

        void Shutdown()
        {
            m_ReloadCoordinator.Invalidate();
            if (m_RHIDevice)
                m_RHIDevice->WaitIdle();
            m_SubmittedGraphFrames.ReleaseAfterDeviceIdle();
            m_MeshResourceCache.Clear();
            m_FrameConstantBuffers.clear();
            m_Pipeline.reset();
            m_PixelShader.reset();
            m_VertexShader.reset();
            if (m_ShaderPackages)
                m_ShaderPackages->Shutdown();
            m_ShaderPackages.reset();
            m_RHIDevice = nullptr;
        }

        bool Render(
            RHI::Texture& colorTexture,
            RHI::Texture& depthTexture,
            u32 width,
            u32 height,
            u32 frameSlot,
            const ClearColor& clearColor)
        {
            if (width == 0 || height == 0)
                return false;

            const SubmittedRenderGraphFrameOwner::PollResult retirement = m_SubmittedGraphFrames.Poll(*m_RHIDevice);
            if (!retirement.Success)
            {
                Log::Error("D3D12 Scene viewport RenderGraph retirement failed: ", retirement.Error);
                return false;
            }
            for (const SubmittedRenderGraphFrameOwner::RetiredFrame& frame : retirement.Retired)
                if (!frame.TimestampScopes.empty() && !Renderer::PublishRenderGraphTimestampScopes(frame.TimestampScopes))
                    return false;
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke"))
                for (const SubmittedRenderGraphFrameOwner::RetiredFrame& frame : retirement.Retired)
                    if (!frame.TimestampScopes.empty())
                    {
                        const bool ready = std::all_of(frame.TimestampScopes.begin(), frame.TimestampScopes.end(), [](const RenderGraph::RawTimestampScope& scope)
                            { return scope.Start.Status == RHI::QueryResultStatus::Ready && scope.End.Status == RHI::QueryResultStatus::Ready; });
                        Log::Info("RenderGraphTimestampScopesV1 backend=D3D12 frame=", frame.FrameIndex, " scopes=", frame.TimestampScopes.size(),
                            " raw=", ready ? "ready" : "disjoint", " cpuWaitBetween=no result=", ready ? "pass" : "fail");
                    }
            if (!m_SubmittedGraphFrames.HasCapacity())
            {
                Log::Error("D3D12 Scene viewport RenderGraph retirement capacity exhausted without a CPU wait");
                return false;
            }

            PollShaderHotReload();
            // Source observation establishes the requested revision before a completed
            // worker result is allowed to reach the native pipeline boundary. This
            // prevents an older completion from briefly replacing newer source intent.
            PollShaderCompilation();

            RHI::ResourceState colorState = RHI::ResourceState::Unknown;
            RHI::ResourceState depthState = RHI::ResourceState::Unknown;
            if (!m_RHIDevice->QueryResourceState(&colorTexture, colorState)
                || !m_RHIDevice->QueryResourceState(&depthTexture, depthState))
                return false;

            RHI::ViewportClear clear;
            clear.Color[0] = clearColor.R;
            clear.Color[1] = clearColor.G;
            clear.Color[2] = clearColor.B;
            clear.Color[3] = clearColor.A;
            SceneRasterFrame rasterFrame;
            Ref<ConstantBufferSet> constantBufferSet;
            std::vector<ConstantBufferAllocation>* constantBuffers = nullptr;
            std::vector<SceneMeshDraw> draws;
            const std::shared_ptr<const SceneRenderSnapshot> snapshot = Renderer::GetSceneRenderSnapshot();
            const std::shared_ptr<const SceneRasterFrame> prepared = Renderer::GetPreparedSceneRasterFrame();
            if (snapshot)
            {
                if (!prepared || prepared->SnapshotFrameIndex != snapshot->FrameIndex)
                    return false;
                rasterFrame = *prepared;
            }
            if (!m_Pipeline)
            {
                rasterFrame.RasterAvailability = m_ShaderPipelineTerminalFailure
                    ? SceneRasterFrame::Availability::ShaderPipelineUnavailable
                    : SceneRasterFrame::Availability::ShaderPipelinePending;
                rasterFrame.Diagnostic = m_ShaderPipelineTerminalFailure
                    ? "scene raster is unavailable because the initial portable shader pipeline failed"
                    : "scene raster is pending initial portable shader pipeline publication";
            }

            bool renderSucceeded = true;
            if (m_Pipeline
                && rasterFrame.HasValidView
                && !rasterFrame.Instances.empty())
            {
                constantBufferSet = AcquireConstantBuffers(frameSlot, rasterFrame.Instances.size());
                if (!constantBufferSet)
                {
                    renderSucceeded = false;
                }
                else
                {
                    constantBuffers = &constantBufferSet->Allocations;
                    for (size_t index = 0; index < rasterFrame.Instances.size(); ++index)
                    {
                        ViewportConstants constants {};
                        std::memcpy(constants.ViewProjection, rasterFrame.Instances[index].ModelViewProjection.Values, sizeof(constants.ViewProjection));
                        std::memcpy((*constantBuffers)[index].Mapped, &constants, sizeof(constants));
                    }
                    std::string meshError;
                    for (size_t index = 0; index < rasterFrame.Instances.size(); ++index)
                    {
                        MeshArtifact artifact;
                        if (!Renderer::ResolvePublishedMeshArtifact(rasterFrame.Instances[index].MeshAsset, artifact, meshError))
                        {
                            Log::Error("D3D12 Scene viewport could not resolve snapshot mesh artifact: ", meshError);
                            renderSucceeded = false;
                            break;
                        }
                        Ref<const MeshGpuResourceBundle> bundle;
                        if (!m_MeshResourceCache.Acquire(*m_RHIDevice, artifact, bundle, meshError))
                        {
                            Log::Error("D3D12 Scene viewport could not acquire snapshot mesh GPU resources: ", meshError);
                            renderSucceeded = false;
                            break;
                        }
                        for (const MeshGpuPrimitiveRange& primitive : bundle->Primitives)
                            draws.push_back({ bundle, primitive, index });
                    }
                    if (draws.empty())
                    {
                        Log::Error("D3D12 Scene viewport resolved a snapshot mesh with no drawable primitives");
                        renderSucceeded = false;
                    }
                }
            }
            if (!renderSucceeded)
                return false;

            Scope<RenderGraph> graph = CreateScope<RenderGraph>();
            RHI::TextureDescription colorDescription = colorTexture.GetDescription();
            colorDescription.InitialState = colorState;
            RHI::TextureDescription depthDescription = depthTexture.GetDescription();
            depthDescription.InitialState = depthState;
            const RenderGraph::ResourceHandle color = graph->AddTexture(colorDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle depth = graph->AddTexture(depthDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::PassHandle clearPass = graph->AddPass("Scene Viewport Graph Clear", RHI::QueueType::Graphics);
            graph->AddWrite(clearPass, color, RHI::ResourceState::RenderTarget);
            graph->AddWrite(clearPass, depth, RHI::ResourceState::DepthWrite);
            graph->SetPassCallback(clearPass, [clear](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphColor = context.GetTexture({ 0 });
                RHI::Texture* graphDepth = context.GetTexture({ 1 });
                return graphColor && graphDepth && context.GetCommandList().BindViewportOutputs(*graphColor, graphDepth)
                    && context.GetCommandList().ClearViewportOutputs(clear);
            });
            graph->SetPassWorkerRecordingEligible(clearPass);
            const RenderGraph::PassHandle rasterPass = graph->AddPass("Scene Viewport Graph Raster", RHI::QueueType::Graphics);
            graph->AddWrite(rasterPass, color, RHI::ResourceState::RenderTarget);
            graph->AddWrite(rasterPass, depth, RHI::ResourceState::DepthWrite);
            const Ref<RHI::Pipeline> activePipeline = m_Pipeline;
            graph->SetPassCallback(rasterPass, [activePipeline, width, height, &rasterFrame, constantBuffers, draws](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphColor = context.GetTexture({ 0 });
                RHI::Texture* graphDepth = context.GetTexture({ 1 });
                RHI::CommandList& commands = context.GetCommandList();
                if (!graphColor || !graphDepth || !commands.BindViewportOutputs(*graphColor, graphDepth)) return false;
                if (!activePipeline || !rasterFrame.HasValidView || rasterFrame.Instances.empty()) return true;
                commands.SetGraphicsPipeline(*activePipeline);
                commands.SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f });
                commands.SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
                for (const SceneMeshDraw& draw : draws)
                {
                    commands.SetVertexBuffer(0, *draw.Bundle->VertexBuffer);
                    commands.SetIndexBuffer(*draw.Bundle->IndexBuffer, RHI::IndexFormat::Uint32);
                    commands.SetGraphicsConstantBuffer(0, *(*constantBuffers)[draw.ConstantIndex].Buffer);
                    commands.DrawIndexed(draw.Primitive.IndexCount, 1, draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0);
                    ++rasterFrame.IssuedDrawCount;
                }
                return true;
            });
            const RenderGraph::PassHandle handoffPass = graph->AddPass("Scene Viewport Graph Output Handoff", RHI::QueueType::Graphics);
            graph->AddRead(handoffPass, color, RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
            graph->SetPassCallback(handoffPass, [](RenderGraph::ExecutionContext& context) { return context.GetTexture({ 0 }) != nullptr; });
            graph->SetPassWorkerRecordingEligible(handoffPass);
            const RenderGraph::CompileResult compiled = graph->Compile();
            RenderGraph::ExecuteOptions executeOptions; executeOptions.RecordingMode = Application::Get().GetSpecification().CommandLineArgs.HasFlag("--frame-task-single-thread") ? FrameTaskExecutionMode::DeterministicSingleThread : FrameTaskExecutionMode::Parallel; executeOptions.EnableTimestampScopes = m_RHIDevice->GetCapabilities().GetFeature(RHI::DeviceFeature::Timestamps).IsUsable();
            const RenderGraph::ExecuteResult executed = graph->BindTexture(color, colorTexture) && graph->BindTexture(depth, depthTexture)
                ? graph->Execute(*m_RHIDevice, compiled, executeOptions) : RenderGraph::ExecuteResult {};
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("RenderGraphRecordingV1 backend=D3D12 mode=", executeOptions.RecordingMode == FrameTaskExecutionMode::Parallel ? "worker" : "inline", " workerPasses=", executed.WorkerRecordedPassCount, " overlap=", executed.WorkerRecordingOverlapObserved ? "yes" : "no", " submitted=", executed.AcceptedPassCount, " result=", executed.Success ? "pass" : "fail");
            if (!executed.Completions.empty())
            {
                std::vector<Ref<void>> payloads;
                if (constantBufferSet)
                    payloads.emplace_back(constantBufferSet);
                // The graph may be retired asynchronously. Keep the exact pipeline
                // selected while recording alive until its accepted GPU work retires.
                if (activePipeline)
                    payloads.emplace_back(activePipeline);
                for (const SceneMeshDraw& draw : draws)
                    payloads.emplace_back(std::const_pointer_cast<MeshGpuResourceBundle>(draw.Bundle));
                std::string retentionError;
                if (!m_SubmittedGraphFrames.Retain(Application::Get().GetFrameIndex(), std::move(graph), compiled,
                    executed, std::move(payloads), &retentionError))
                {
                    Log::Error("D3D12 Scene viewport could not retain an accepted RenderGraph submission: ", retentionError);
                    m_RHIDevice->WaitIdle();
                    return false;
                }
            }
            if (!executed.Success)
                return false;
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")
                && m_Pipeline && rasterFrame.HasValidView && !rasterFrame.Instances.empty() && !draws.empty())
                Log::Info("SceneMeshGpuIntegrationV1 backend=D3D12 snapshot=pass resolver=pass cache=pass indexFormat=UInt32 baseVertex=0 instances=", rasterFrame.Instances.size(),
                    " draws=", draws.size(), " constants=per-instance retained=gpu-retirement result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke"))
                Log::Info("ProductionRenderGraphRetirementV1 backend=D3D12 frame=", Application::Get().GetFrameIndex(),
                    " passes=", executed.AcceptedPassCount, " cpuWaitBetween=no pending=", m_SubmittedGraphFrames.GetPendingCount(), " result=pass");
            const bool comparisonRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke");
            if (comparisonRequested)
            {
                RHI::TextureDescription referenceColorDescription = colorTexture.GetDescription();
                referenceColorDescription.DebugName = "Scene Viewport Bootstrap Reference Color";
                RHI::TextureDescription referenceDepthDescription = depthTexture.GetDescription();
                referenceDepthDescription.DebugName = "Scene Viewport Bootstrap Reference Depth";
                Scope<RHI::Texture> referenceColor = m_RHIDevice->CreateTexture(referenceColorDescription);
                Scope<RHI::Texture> referenceDepth = m_RHIDevice->CreateTexture(referenceDepthDescription);
                RHI::TextureReadback graphReadback, referenceReadback;
                const bool referenceRendered = referenceColor && referenceDepth && RecordBootstrapReference(
                    *referenceColor, *referenceDepth, width, height, clear, rasterFrame, constantBuffers, draws);
                const bool readBack = referenceRendered && ReadbackGraphOutput(colorTexture, graphReadback)
                    && m_RHIDevice->ReadbackTexture(*referenceColor, referenceReadback);
                const bool equivalent = readBack && graphReadback.Extent.Width == referenceReadback.Extent.Width
                    && graphReadback.Extent.Height == referenceReadback.Extent.Height
                    && graphReadback.RowPitchBytes == referenceReadback.RowPitchBytes
                    && graphReadback.Data == referenceReadback.Data;
                Log::Info("SceneViewportRenderGraphV1 backend=D3D12 passes=3 labels=clear,raster,output-handoff execution=pass reference=direct comparator=exact-byte-",
                    equivalent ? "pass" : "fail", " size=", width, "x", height, " bytes=", graphReadback.Data.size());
                if (!equivalent) return false;
            }
            Renderer::PublishSceneRasterFrame(std::move(rasterFrame));
            if (!comparisonRequested)
                Log::Trace("Scene viewport graph rendered without the smoke-only bootstrap comparator");
            return true;
        }

        bool RequestInitialPipeline()
        {
            m_ShaderSource = ShaderLibrary::LoadSource(ViewportShaderPath(), "Editor Viewport");
            if (m_ShaderSource.Status != ShaderSourceStatus::Loaded)
            {
                Log::Error("Could not load viewport shader: ", m_ShaderSource.ResolvedPath.string(), " (", ShaderLibrary::ToString(m_ShaderSource.Status), ")");
                return false;
            }

            const std::filesystem::path cacheDirectory = std::filesystem::path("output") / "cache" / "shaders";
            const std::shared_ptr<SlangShaderCompiler> compiler = std::make_shared<SlangShaderCompiler>(cacheDirectory);
            const bool deterministicSmoke = Application::Get().GetSpecification().CommandLineArgs.HasFlag("--smoke-test");
            m_ShaderPackages = CreateScope<AsyncShaderPackageService>(
                [compiler](const PortableShaderRequest& request) { return compiler->Compile(request); },
                deterministicSmoke ? ShaderPackageExecutionMode::DeterministicInline : ShaderPackageExecutionMode::JobSystem);
            Log::Info("Portable shader execution mode: ", deterministicSmoke ? "deterministic-inline-smoke" : "job-system-fire-and-poll");
            RequestPipelineCompilation();
            return m_VertexRequest.IsValid() && m_PixelRequest.IsValid();
        }

        PortableShaderRequest MakeShaderRequest(RHI::ShaderStage stage, const char* entryPoint) const
        {
            PortableShaderRequest request;
            request.SourceName = m_ShaderSource.ResolvedPath.string();
            request.Source = m_ShaderSource.Source;
            request.EntryPoint = entryPoint;
            request.Stage = stage;
            request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv };
            request.CompilerIdentity = "Slang";
            request.CompilerVersion = "2026.13.1";
            request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
            request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
            request.ExpectedLayout = {
                { "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0}", 1, 64, 0, 0 }
            };
            if (stage == RHI::ShaderStage::Vertex)
            {
                request.ExpectedVertexInputs = {
                    { "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 },
                    { "Color", "COLOR", 0, 1, "float32x3", 12, 1, 3 },
                    { "UV", "TEXCOORD", 0, 2, "float32x2", 8, 1, 2 }
                };
            }
            return request;
        }

        static const char* CacheMode(const ShaderPackageRequestResult& result)
        {
            if (result.Status == ShaderPackageRequestStatus::CacheHit
                || (result.Package && result.Package->CacheSource == PortableShaderCacheSource::DiskCache))
            {
                return "cache-hit";
            }
            return result.Succeeded() ? "compiled" : "none";
        }

        static const char* CacheSource(const ShaderPackageRequestResult& result)
        {
            if (result.Status == ShaderPackageRequestStatus::CacheHit)
                return "service";
            if (result.Package && result.Package->CacheSource == PortableShaderCacheSource::DiskCache)
                return "disk";
            return result.Succeeded() ? "compiler" : "none";
        }

        static std::string ConventionEvidence(const PortableShaderPackage& package)
        {
            const PortableShaderConventions& conventions = package.Conventions;
            return "schema=" + std::to_string(conventions.Version)
                + "|matrix=" + (conventions.RowMajor ? "row-major" : "column-major")
                + "|d3dClipDepth=" + (conventions.ZeroToOneDepth ? "zero-to-one" : "unsupported")
                + "|spirvY=" + (conventions.VulkanYFlip ? "inverted" : "not-inverted")
                + "|frontFace=" + (conventions.ClockwiseFrontFace ? "clockwise" : "counter-clockwise")
                + "|binding=" + conventions.BindingPolicy;
        }

        static void LogTerminalResult(const char* stage, const ShaderPackageRequestResult& result)
        {
            const size_t bindingCount = result.Package ? result.Package->Reflection.size() : 0;
            const size_t vertexInputCount = result.Package ? result.Package->VertexInputs.size() : 0;
            const std::string conventions = result.Package
                ? ConventionEvidence(*result.Package)
                : "unavailable";
            if (result.Succeeded())
            {
                Log::Info("PortableShaderTerminalV1 status=", AsyncShaderPackageService::ToString(result.Status),
                    " request=", result.Diagnostic.RequestId, " stage=", stage,
                    " cacheMode=", CacheMode(result), " cacheSource=", CacheSource(result),
                    " compiler=Slang-2026.13.1 backend=Slang targets=DXIL+SPIR-V key=", result.Diagnostic.Key,
                    " bindings=", bindingCount, " vertexInputs=", vertexInputCount,
                    " conventions=", conventions, " legacySourceCompile=false");
            }
            else
            {
                Log::Error("PortableShaderTerminalV1 status=", AsyncShaderPackageService::ToString(result.Status),
                    " request=", result.Diagnostic.RequestId, " stage=", stage,
                    " cacheMode=none cacheSource=none compiler=Slang-2026.13.1 backend=Slang targets=DXIL+SPIR-V key=",
                    result.Diagnostic.Key, " bindings=", bindingCount, " vertexInputs=", vertexInputCount,
                    " conventions=", conventions, " legacySourceCompile=false message=", result.Diagnostic.Message);
            }
        }

        void RequestPipelineCompilation()
        {
            if (!m_ShaderPackages)
                return;
            const PortableShaderRequest vertexRequest = MakeShaderRequest(RHI::ShaderStage::Vertex, "VSMain");
            const PortableShaderRequest pixelRequest = MakeShaderRequest(RHI::ShaderStage::Pixel, "PSMain");
            m_VertexRequest = m_ShaderPackages->Request(vertexRequest);
            m_PixelRequest = m_ShaderPackages->Request(pixelRequest);
            m_LastProcessedVertexRequest = 0;
            m_LastProcessedPixelRequest = 0;
            m_ReloadTicket = m_ReloadCoordinator.Request(m_ShaderSource.Revision);
            if (!m_ReloadTicket.IsValid()) return;
            m_ShaderPipelineTerminalFailure = false;
            Log::Info("PortableShaderRequestV1 status=pending request=", m_VertexRequest.Id,
                " stage=vertex cacheMode=pending cacheSource=none compiler=Slang-2026.13.1 backend=Slang",
                " targets=DXIL+SPIR-V key=", m_VertexRequest.Key, " legacySourceCompile=false");
            Log::Info("PortableShaderRequestV1 status=pending request=", m_PixelRequest.Id,
                " stage=pixel cacheMode=pending cacheSource=none compiler=Slang-2026.13.1 backend=Slang",
                " targets=DXIL+SPIR-V key=", m_PixelRequest.Key, " legacySourceCompile=false");
            if (!m_Pipeline)
            {
                Log::Warn("D3D12 scene raster unavailable: status=shader-pipeline-pending, vertexRequest=",
                    m_VertexRequest.Id, ", pixelRequest=", m_PixelRequest.Id,
                    ", clear-only presentation is not a successful scene raster");
            }
        }

        void PollShaderCompilation()
        {
            if (!m_ShaderPackages || !m_VertexRequest.IsValid() || !m_PixelRequest.IsValid())
                return;
            if (m_LastProcessedVertexRequest == m_VertexRequest.Id
                && m_LastProcessedPixelRequest == m_PixelRequest.Id)
                return;

            const ShaderPackageRequestResult vertex = m_ShaderPackages->Poll(m_VertexRequest);
            const ShaderPackageRequestResult pixel = m_ShaderPackages->Poll(m_PixelRequest);
            if (!vertex.IsTerminal() || !pixel.IsTerminal())
                return;

            m_LastProcessedVertexRequest = m_VertexRequest.Id;
            m_LastProcessedPixelRequest = m_PixelRequest.Id;
            LogTerminalResult("vertex", vertex);
            LogTerminalResult("pixel", pixel);
            if (!m_ReloadCoordinator.IsCurrent(m_ReloadTicket))
            {
                Log::Info("D3D12LivePipelineRebuildV1 status=stale-rejected requestedRevision=", m_ReloadTicket.Revision,
                    " activeGeneration=", m_ReloadCoordinator.ActiveGeneration());
                return;
            }
            if (!vertex.Succeeded() || !pixel.Succeeded())
            {
                m_ReloadCoordinator.Publish(m_ReloadTicket, false);
                if (!m_Pipeline)
                    m_ShaderPipelineTerminalFailure = true;
                const ShaderPackageRequestDiagnostic& diagnostic = !vertex.Succeeded() ? vertex.Diagnostic : pixel.Diagnostic;
                Log::Error("Portable shader request: status=", AsyncShaderPackageService::ToString(diagnostic.Status),
                    ", request=", diagnostic.RequestId, ", source=", diagnostic.Source, ", entry=", diagnostic.EntryPoint,
                    ", targets=", diagnostic.Targets, ", backend=", diagnostic.Backend, ", key=", diagnostic.Key,
                    ", message=", diagnostic.Message, ". Last valid D3D12 viewport pipeline remains active.");
                return;
            }

            if (!BuildPipeline(*vertex.Package, *pixel.Package))
            {
                m_ReloadCoordinator.Publish(m_ReloadTicket, false);
                if (!m_Pipeline)
                    m_ShaderPipelineTerminalFailure = true;
                Log::Error("Portable shader packages were valid but D3D12 pipeline mutation failed; last valid viewport pipeline remains active");
                return;
            }
            m_ShaderPipelineTerminalFailure = false;
            m_ReloadCoordinator.Publish(m_ReloadTicket, true);
            Log::Info("D3D12LivePipelineRebuildV1 status=published requestedRevision=", m_ReloadTicket.Revision,
                " generation=", m_ReloadCoordinator.ActiveGeneration(), " boundary=frame-start retainedUntil=gpu-retirement");
            Log::Info("D3D12PortablePipelineV1 status=active vertexStatus=",
                AsyncShaderPackageService::ToString(vertex.Status), " vertexCacheMode=", CacheMode(vertex),
                " vertexCacheSource=", CacheSource(vertex), " vertexKey=", vertex.Package->Key,
                " vertexBindings=", vertex.Package->Reflection.size(), " vertexInputs=", vertex.Package->VertexInputs.size(),
                " pixelStatus=", AsyncShaderPackageService::ToString(pixel.Status), " pixelCacheMode=", CacheMode(pixel),
                " pixelCacheSource=", CacheSource(pixel), " pixelKey=", pixel.Package->Key,
                " pixelBindings=", pixel.Package->Reflection.size(), " pixelInputs=", pixel.Package->VertexInputs.size(),
                " compiler=Slang-2026.13.1 backend=D3D12+Slang targets=DXIL+SPIR-V conventions=",
                ConventionEvidence(*vertex.Package), " legacySourceCompile=false");
        }

        bool BuildPipeline(const PortableShaderPackage& vertexPackage, const PortableShaderPackage& pixelPackage)
        {
            Scope<RHI::Shader> vertexShader;
            Scope<RHI::Shader> pixelShader;
            if (!CreateRhiShader(RHI::ShaderStage::Vertex, "VSMain", "Editor Viewport Vertex Shader", vertexPackage.Dxil, vertexShader))
                return false;
            if (!CreateRhiShader(RHI::ShaderStage::Pixel, "PSMain", "Editor Viewport Pixel Shader", pixelPackage.Dxil, pixelShader))
                return false;

            RHI::PipelineDescription pipelineDesc;
            pipelineDesc.DebugName = "Editor Viewport Scene Mesh Pipeline";
            pipelineDesc.Type = RHI::PipelineType::Graphics;
            pipelineDesc.VertexShader = vertexShader.get();
            pipelineDesc.PixelShader = pixelShader.get();
            pipelineDesc.VertexInputs = {
                { "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Position) },
                { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Color) },
                { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(MeshArtifactVertex, UV) }
            };
            pipelineDesc.ConstantBufferBindings = {
                { 0, 0, RHI::ShaderStage::AllGraphics }
            };
            pipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            pipelineDesc.RasterCullMode = RHI::CullMode::None;
            pipelineDesc.ColorFormat = RHI::Format::R8G8B8A8Unorm;
            pipelineDesc.DepthFormat = RHI::Format::D32Float;
            pipelineDesc.DepthTestEnable = true;
            pipelineDesc.DepthWriteEnable = true;

            Scope<RHI::Pipeline> pipeline = m_RHIDevice->CreatePipeline(pipelineDesc);
            if (!pipeline)
                return false;
            // Construct every replacement before publication. Ref ownership lets an
            // accepted RenderGraph frame retain the exact generation it recorded.
            Ref<RHI::Shader> replacementVertex(vertexShader.release());
            Ref<RHI::Shader> replacementPixel(pixelShader.release());
            Ref<RHI::Pipeline> replacementPipeline(pipeline.release());
            m_VertexShader = std::move(replacementVertex);
            m_PixelShader = std::move(replacementPixel);
            m_Pipeline = std::move(replacementPipeline);
            return true;
        }

        bool CreateRhiShader(
            RHI::ShaderStage stage,
            const char* entryPoint,
            const char* debugName,
            const std::vector<u8>& dxil,
            Scope<RHI::Shader>& shader)
        {
            RHI::ShaderDescription description;
            description.DebugName = debugName;
            description.SourceName = m_ShaderSource.ResolvedPath.string();
            description.EntryPoint = entryPoint;
            description.Stage = stage;
            description.BinaryFormat = RHI::ShaderBinaryFormat::Dxil;
            description.Binary = dxil;

            shader = m_RHIDevice->CreateShader(description);
            if (!shader)
                return false;

            const RHI::NVRHID3D12ShaderNativeHandles handles = RHI::GetNVRHID3D12ShaderNativeHandles(*shader);
            if (!handles.Bytecode || handles.BytecodeSize == 0)
            {
                Log::Error("RHI shader did not expose D3D12 bytecode: ", debugName);
                shader.reset();
                return false;
            }

            return true;
        }

        bool CreateRhiBuffer(const RHI::BufferDescription& description, Scope<RHI::Buffer>& buffer)
        {
            if (!m_RHIDevice)
                return false;

            buffer = m_RHIDevice->CreateBuffer(description);
            if (!buffer)
                return false;

            const RHI::NVRHID3D12BufferNativeHandles handles = RHI::GetNVRHID3D12BufferNativeHandles(*buffer);
            if (!handles.Resource)
            {
                Log::Error("RHI buffer did not expose a D3D12 resource: ", description.DebugName);
                buffer.reset();
                return false;
            }

            return true;
        }

        Ref<ConstantBufferSet> AcquireConstantBuffers(u32 frameSlot, size_t requiredCount)
        {
            if (frameSlot >= m_FrameConstantBuffers.size())
                m_FrameConstantBuffers.resize(static_cast<size_t>(frameSlot) + 1);

            Ref<ConstantBufferSet>& set = m_FrameConstantBuffers[frameSlot];
            if (set && set.use_count() != 1)
                return nullptr;
            if (!set)
                set = CreateRef<ConstantBufferSet>();
            std::vector<ConstantBufferAllocation>& allocations = set->Allocations;
            while (allocations.size() < requiredCount)
            {
                RHI::BufferDescription description;
                description.DebugName = "Editor Viewport Scene Instance Constants";
                description.SizeBytes = kViewportConstantBufferSize;
                description.StrideBytes = kViewportConstantBufferSize;
                description.Usage = RHI::BufferUsage::Constant;
                description.CpuAccess = RHI::BufferCpuAccess::Write;

                ConstantBufferAllocation allocation;
                if (!CreateRhiBuffer(description, allocation.Buffer))
                    return nullptr;
                allocation.Mapped = static_cast<std::byte*>(allocation.Buffer->Map());
                if (!allocation.Mapped)
                    return nullptr;
                allocations.push_back(std::move(allocation));
            }

            return set;
        }

        void PollShaderHotReload()
        {
            if (!ShaderLibrary::HasSourceChanged(m_ShaderSource))
                return;

            if (ShaderLibrary::ReloadSourceIfChanged(m_ShaderSource))
            {
                Log::Info("Shader source changed: ", m_ShaderSource.ResolvedPath.string(), "; requesting asynchronous portable rebuild");
                RequestPipelineCompilation();
            }
        }

        RHI::Device* m_RHIDevice = nullptr;
        MeshGpuResourceCache m_MeshResourceCache { 32 };
        Ref<RHI::Pipeline> m_Pipeline;
        Ref<RHI::Shader> m_VertexShader;
        Ref<RHI::Shader> m_PixelShader;
        std::vector<Ref<ConstantBufferSet>> m_FrameConstantBuffers;
        SubmittedRenderGraphFrameOwner m_SubmittedGraphFrames;
        ShaderSourceFile m_ShaderSource;
        Scope<AsyncShaderPackageService> m_ShaderPackages;
        ShaderPackageRequestHandle m_VertexRequest;
        ShaderPackageRequestHandle m_PixelRequest;
        u64 m_LastProcessedVertexRequest = 0;
        u64 m_LastProcessedPixelRequest = 0;
        D3D12ViewportShaderReloadCoordinator m_ReloadCoordinator;
        D3D12ViewportShaderReloadCoordinator::Ticket m_ReloadTicket;
        bool m_ShaderPipelineTerminalFailure = false;
    };

    NVRHID3D12ViewportSceneRenderer::NVRHID3D12ViewportSceneRenderer() = default;

    NVRHID3D12ViewportSceneRenderer::~NVRHID3D12ViewportSceneRenderer()
    {
        Shutdown();
    }

    bool NVRHID3D12ViewportSceneRenderer::Initialize(RHI::Device* rhiDevice)
    {
        m_Impl = CreateScope<Impl>();
        if (m_Impl->Initialize(rhiDevice))
            return true;

        m_Impl.reset();
        return false;
    }

    void NVRHID3D12ViewportSceneRenderer::Shutdown()
    {
        if (m_Impl)
        {
            m_Impl->Shutdown();
            m_Impl.reset();
        }
    }

    bool NVRHID3D12ViewportSceneRenderer::Render(
        RHI::Texture& colorTexture,
        RHI::Texture& depthTexture,
        u32 width,
        u32 height,
        u32 frameSlot,
        const ClearColor& clearColor)
    {
        return m_Impl && m_Impl->Render(
            colorTexture,
            depthTexture,
            width,
            height,
            frameSlot,
            clearColor);
    }
#endif
}
