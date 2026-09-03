#include "Engine/Renderer/NVRHI/NVRHIVulkanViewportSceneRenderer.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Renderer/MeshGpuResourceCache.h"
#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"
#include "Engine/Renderer/TextureRuntimePublication.h"
#include "Engine/Renderer/ToneMapPass.h"
#include "Engine/RenderGraph/RenderGraph.h"

#if defined(GE_HAS_NVRHI_VULKAN)
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

        struct Constants
        {
            float ViewProjection[16];
            float BaseColorAndAlphaCutoff[4];
            float EmissiveAndStrength[4];
            float SurfaceFactors[4];
            float CallistoFactors[4];
            u32 TextureIndices0[4];
            u32 TextureIndices1[4];
            u32 TextureState[4];
        };

        static_assert(sizeof(Constants) <= kConstantBufferSize);

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

        Constants BuildConstants(
            const SceneRasterInstance& instance, const MaterialTextureBindingSet& bindings)
        {
            Constants constants {};
            std::memcpy(constants.ViewProjection,
                instance.ModelViewProjection.Values, sizeof(constants.ViewProjection));
            constants.BaseColorAndAlphaCutoff[0] = bindings.Material.BaseColor.X;
            constants.BaseColorAndAlphaCutoff[1] = bindings.Material.BaseColor.Y;
            constants.BaseColorAndAlphaCutoff[2] = bindings.Material.BaseColor.Z;
            constants.BaseColorAndAlphaCutoff[3] = bindings.Material.AlphaCutoff;
            constants.EmissiveAndStrength[0] = bindings.Material.EmissiveColor.X;
            constants.EmissiveAndStrength[1] = bindings.Material.EmissiveColor.Y;
            constants.EmissiveAndStrength[2] = bindings.Material.EmissiveColor.Z;
            constants.EmissiveAndStrength[3] = bindings.Material.EmissiveStrength;
            constants.SurfaceFactors[0] = bindings.Material.Metallic;
            constants.SurfaceFactors[1] = bindings.Material.Roughness;
            constants.SurfaceFactors[2] = bindings.Material.NormalScale;
            constants.SurfaceFactors[3] = bindings.Material.OcclusionStrength;
            constants.CallistoFactors[0] = bindings.Material.DiffuseFresnelIntensity;
            constants.CallistoFactors[1] = bindings.Material.RetroreflectionIntensity;
            constants.CallistoFactors[2] = bindings.Material.DiffuseFresnelFalloff;
            constants.CallistoFactors[3] = bindings.Material.RetroreflectionFalloff;
            for (size_t index = 0; index < 4; ++index)
                constants.TextureIndices0[index] = bindings.Handles[index].Index;
            constants.TextureIndices1[0] = bindings.Handles[4].Index;
            constants.TextureIndices1[1] = bindings.Handles[5].Index;
            constants.TextureState[0] = bindings.DeclaredMask;
            constants.TextureState[1] = bindings.ErrorMask;
            constants.TextureState[2] = static_cast<u32>(bindings.Material.AlphaMode);
            constants.TextureState[3] = static_cast<u32>(bindings.Material.ShadingModel);
            return constants;
        }

        struct SceneMeshDraw { Ref<const MeshGpuResourceBundle> Bundle; MeshGpuPrimitiveRange Primitive; size_t ConstantIndex = 0; };

    }

    struct NVRHIVulkanViewportSceneRenderer::Impl
    {
        bool RecordBootstrapReference(
            RHI::Texture& hdrTexture,
            RHI::Texture& colorTexture,
            RHI::Texture& depthTexture,
            u32 width,
            u32 height,
            const RHI::ViewportClear& clear,
            const SceneRasterFrame& frame,
            const std::vector<ConstantBufferAllocation>& constants,
            const std::vector<SceneMeshDraw>& draws,
            const ToneMapPassConstants& toneMapConstants)
        {
            Scope<RHI::CommandList> commands = m_Device->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Bootstrap Reference");
            if (!commands || !commands->Begin()
                || !commands->TransitionTexture(hdrTexture, RHI::ResourceState::RenderTarget)
                || !commands->TransitionTexture(depthTexture, RHI::ResourceState::DepthWrite)
                || !commands->BindViewportOutputs(hdrTexture, &depthTexture)
                || !commands->ClearViewportOutputs(clear)) return false;
            commands->BeginDebugMarker("Scene Viewport Bootstrap Reference Raster");
            if (m_Pipeline && frame.HasValidView && !frame.Instances.empty())
            {
                commands->SetGraphicsPipeline(*m_Pipeline);
                if (!m_TextureRuntime || !m_TextureRuntime->GetBindingTable()
                    || !commands->BindGraphicsSampledTextureTable(*m_TextureRuntime->GetBindingTable())) return false;
                commands->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); commands->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
                for (const SceneMeshDraw& draw : draws) { commands->SetVertexBuffer(0, *draw.Bundle->VertexBuffer); commands->SetIndexBuffer(*draw.Bundle->IndexBuffer, RHI::IndexFormat::Uint32); commands->SetGraphicsConstantBuffer(0, *constants[draw.ConstantIndex].Buffer); commands->DrawIndexed(draw.Primitive.IndexCount, 1, draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0); }
            }
            commands->EndDebugMarker();
            return commands->TransitionTexture(hdrTexture, RHI::ResourceState::ShaderResource)
                && commands->TransitionTexture(colorTexture, RHI::ResourceState::RenderTarget)
                && m_ToneMap.Record(*commands, hdrTexture, colorTexture, width, height, toneMapConstants)
                && commands->TransitionTexture(colorTexture, RHI::ResourceState::CopySource)
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
            m_TextureTableCapacity = RHI::SelectReadOnlyTextureTableCapacity(
                m_Device->GetCapabilities());
            if (m_TextureTableCapacity < 2)
                return false;

            ShaderSourceFile source = ShaderLibrary::LoadSource("Engine/Shaders/EditorViewport.hlsl", "Vulkan Scene viewport");
            if (source.Status != ShaderSourceStatus::Loaded)
                return false;
            auto makeRequest = [&source, this](RHI::ShaderStage stage, const char* entry) {
                PortableShaderRequest request;
                request.SourceName = source.ResolvedPath.string(); request.Source = source.Source; request.EntryPoint = entry; request.Stage = stage;
#ifdef _WIN32
                request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv }; request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
#else
                request.Targets = { PortableShaderTarget::Spirv };
#endif
                request.CompilerIdentity = "Slang"; request.CompilerVersion = "2026.13.1"; request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
                request.Defines = { "GE_READ_ONLY_TEXTURE_CAPACITY=" + std::to_string(m_TextureTableCapacity) };
                request.ExpectedLayout = {
                    { "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0,BaseColorAndAlphaCutoff:float32x4@64,EmissiveAndStrength:float32x4@80,SurfaceFactors:float32x4@96,CallistoFactors:float32x4@112,TextureIndices0:uint32x4@128,TextureIndices1:uint32x4@144,TextureState:uint32x4@160}", 1, 176, 0, 0 },
                    { "ReadOnlySamplers", 's', 0, 1, stage, "SamplerState", "sampler", m_TextureTableCapacity, 0, 0, 0 },
                    { "ReadOnlyTextures", 't', 0, 1, stage, "Texture2D", "float32x4", m_TextureTableCapacity, 0, 1, 4 }
                };
                if (stage == RHI::ShaderStage::Vertex) request.ExpectedVertexInputs = {{ "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 }, { "Color", "COLOR", 0, 1, "float32x3", 12, 1, 3 }, { "UV", "TEXCOORD", 0, 2, "float32x2", 8, 1, 2 }};
                return request;
            };
            SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
            PortableShaderPackage vertex = compiler.Compile(makeRequest(RHI::ShaderStage::Vertex, "VSMain"));
            PortableShaderPackage pixel = compiler.Compile(makeRequest(RHI::ShaderStage::Pixel, "PSMain"));
            std::string error;
            if (!PortableShaderContract::ValidatePackage(makeRequest(RHI::ShaderStage::Vertex, "VSMain"), vertex, error) || !PortableShaderContract::ValidatePackage(makeRequest(RHI::ShaderStage::Pixel, "PSMain"), pixel, error)) { Log::Error("Vulkan Scene viewport shader package validation failed: ", error); return false; }
            RHI::ShaderDescription vs; vs.DebugName = "Vulkan Scene Viewport VS"; vs.SourceName = source.ResolvedPath.string(); vs.EntryPoint = "main"; vs.Stage = RHI::ShaderStage::Vertex; vs.BinaryFormat = RHI::ShaderBinaryFormat::Spirv; vs.Binary = vertex.Spirv; vs.Reflection = vertex.Reflection;
            RHI::ShaderDescription ps = vs; ps.DebugName = "Vulkan Scene Viewport PS"; ps.Stage = RHI::ShaderStage::Pixel; ps.Binary = pixel.Spirv; ps.Reflection = pixel.Reflection;
            m_VertexShader = m_Device->CreateShader(vs); m_PixelShader = m_Device->CreateShader(ps);
            RHI::PipelineDescription pipeline; pipeline.DebugName = "Vulkan Scene Viewport Pipeline"; pipeline.VertexShader = m_VertexShader.get(); pipeline.PixelShader = m_PixelShader.get(); pipeline.VertexInputs = {{ "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Position) }, { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Color) }, { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(MeshArtifactVertex, UV) }}; pipeline.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }}; pipeline.SampledTextureTable = RHI::SampledTextureTableBinding { m_TextureTableCapacity }; pipeline.ColorFormat = RHI::Format::R16G16B16A16Float; pipeline.DepthFormat = RHI::Format::D32Float; pipeline.DepthTestEnable = true; pipeline.DepthWriteEnable = true; pipeline.RasterCullMode = RHI::CullMode::None;
            m_Pipeline = m_VertexShader && m_PixelShader ? m_Device->CreatePipeline(pipeline) : nullptr;
            m_TextureRuntime = m_Pipeline ? TextureRuntimePublication::Create(*m_Device,
                TextureTargetProfile::RGBAFallback, m_TextureTableCapacity - 1,
                m_TextureTableCapacity) : nullptr;
            return m_Pipeline != nullptr && m_TextureRuntime != nullptr && m_ToneMap.Initialize(*m_Device);
        }

        Ref<ConstantBufferSet> AcquireConstantBuffers(u64 frameIndex, size_t requiredCount)
        {
            const size_t frameSlot = static_cast<size_t>(frameIndex % SubmittedRenderGraphFrameOwner::Capacity);
            Ref<ConstantBufferSet>& set = m_FrameConstantBuffers[frameSlot];
            if (set && set.use_count() != 1)
                return nullptr;
            if (!set)
                set = CreateRef<ConstantBufferSet>();
            while (set->Allocations.size() < requiredCount)
            {
                RHI::BufferDescription description;
                description.DebugName = "Vulkan Scene Viewport Instance Constants";
                description.SizeBytes = kConstantBufferSize;
                description.StrideBytes = kConstantBufferSize;
                description.Usage = RHI::BufferUsage::Constant;
                description.CpuAccess = RHI::BufferCpuAccess::Write;
                ConstantBufferAllocation allocation;
                allocation.Buffer = m_Device->CreateBuffer(description);
                allocation.Mapped = allocation.Buffer
                    ? static_cast<std::byte*>(allocation.Buffer->Map()) : nullptr;
                if (!allocation.Buffer || !allocation.Mapped)
                    return nullptr;
                set->Allocations.push_back(std::move(allocation));
            }
            return set;
        }

        bool EnsureOutputs(u32 width, u32 height)
        {
            if (m_HdrColor && m_Color && m_Depth && m_Width == width && m_Height == height) return true;
            // Render rejects replacement while the submitted-frame owner still
            // retains an exact token for the current output generation.
            m_HdrColor.reset(); m_Color.reset(); m_Depth.reset();
            RHI::TextureDescription color; color.DebugName = "Vulkan Scene Viewport Color"; color.Extent = { width, height }; color.TextureFormat = RHI::Format::R8G8B8A8Unorm; color.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource) | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            RHI::TextureDescription hdrColor = color; hdrColor.DebugName = "Vulkan Scene Viewport Linear HDR"; hdrColor.TextureFormat = RHI::Format::R16G16B16A16Float; hdrColor.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            RHI::TextureDescription depth = color; depth.DebugName = "Vulkan Scene Viewport Depth"; depth.TextureFormat = RHI::Format::D32Float; depth.Usage = RHI::TextureUsage::DepthStencil;
            m_HdrColor = m_Device->CreateTexture(hdrColor); m_Color = m_Device->CreateTexture(color); m_Depth = m_Device->CreateTexture(depth);
            if (!m_HdrColor || !m_Color || !m_Depth) return false;
            m_Width = width; m_Height = height; ++m_OutputGeneration; return true;
        }

        bool Render(const SceneRenderSnapshot& snapshot, u32 width, u32 height, const ClearColor& clearColor)
        {
            if (!m_Device || width == 0 || height == 0) return false;
            const SubmittedRenderGraphFrameOwner::PollResult retirement = m_SubmittedGraphFrames.Poll(*m_Device);
            if (!retirement.Success) { Log::Error("Vulkan Scene viewport RenderGraph retirement failed: ", retirement.Error); return false; }
            for (const SubmittedRenderGraphFrameOwner::RetiredFrame& retired : retirement.Retired)
            {
                for (const RHI::CompletionToken& completion : retired.Completions)
                {
                    if (m_TextureRuntime && m_TextureRuntime->HasRetainedFrame(completion))
                    {
                        std::string textureRetirementError;
                        if (!m_TextureRuntime->Retire(completion, textureRetirementError))
                        { Log::Error("Vulkan Scene material texture retirement failed: ", textureRetirementError); return false; }
                    }
                }
                if (!retired.TimestampScopes.empty() && !Renderer::PublishRenderGraphTimestampScopes(retired.TimestampScopes)) return false;
            }
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
            std::string lightGridError;
            if (!BuildClusteredLightGrid(snapshot, 0, width, height, {}, frame.LightGrid, lightGridError))
            { Log::Error("Vulkan Scene viewport could not build clustered light grid: ", lightGridError); return false; }
            size_t directionalLightCount = 0;
            size_t localLightCount = 0;
            bool photometricPublicationValid = frame.LightGrid.Lights.size() == snapshot.Lights.size();
            for (size_t lightIndex = 0;
                photometricPublicationValid && lightIndex < snapshot.Lights.size(); ++lightIndex)
            {
                const SceneRenderLight& published = snapshot.Lights[lightIndex];
                const ClusteredLightRecord& clustered = frame.LightGrid.Lights[lightIndex];
                photometricPublicationValid = IsValidLightPhotometricValue(
                        published.Type, published.PhotometricUnit, published.PhotometricValue)
                    && clustered.SourceEntity == published.SourceEntity
                    && clustered.Type == published.Type
                    && clustered.PhotometricValue == published.PhotometricValue
                    && clustered.PhotometricUnit == published.PhotometricUnit;
                if (published.Type == LightType::Directional)
                    ++directionalLightCount;
                else
                    ++localLightCount;
            }
            if (!photometricPublicationValid)
            {
                Log::Error("Vulkan Scene viewport rejected an invalid photometric light publication");
                return false;
            }
            Ref<ConstantBufferSet> constantBufferSet = AcquireConstantBuffers(snapshot.FrameIndex, frame.Instances.size());
            if (!constantBufferSet) return false;
            std::vector<ConstantBufferAllocation>& constants = constantBufferSet->Allocations;
            std::vector<RHI::TextureBindingHandle> usedTextureHandles;
            for (size_t instanceIndex = 0; instanceIndex < frame.Instances.size(); ++instanceIndex)
            {
                const SceneRasterInstance& instance = frame.Instances[instanceIndex];
                MaterialTextureBindingSet materialBindings;
                std::string materialError;
                if (!m_TextureRuntime->ResolveMaterialTextures(instance.MaterialAsset, materialBindings, materialError))
                { Log::Error("Vulkan Scene viewport could not resolve snapshot material: ", materialError); return false; }
                if (!materialError.empty()) Log::Warn("Vulkan Scene material uses error resources: ", materialError);
                for (size_t slot = 0; slot < materialBindings.Handles.size(); ++slot)
                    if ((materialBindings.DeclaredMask & (1u << static_cast<u32>(slot))) != 0
                        && (materialBindings.ErrorMask & (1u << static_cast<u32>(slot))) == 0)
                        usedTextureHandles.push_back(materialBindings.Handles[slot]);
                const Constants instanceConstants = BuildConstants(instance, materialBindings);
                std::memcpy(constants[instanceIndex].Mapped, &instanceConstants, sizeof(instanceConstants));
            }
            std::vector<SceneMeshDraw> draws;
            std::string meshError;
            for (size_t index = 0; index < frame.Instances.size(); ++index)
            {
                MeshArtifact artifact;
                if (!Renderer::ResolvePublishedMeshArtifact(frame.Instances[index].MeshAsset, artifact, meshError)) { Log::Error("Vulkan Scene viewport could not resolve snapshot mesh artifact: ", meshError); return false; }
                Ref<const MeshGpuResourceBundle> bundle;
                if (!m_MeshResourceCache.Acquire(*m_Device, artifact, bundle, meshError)) { Log::Error("Vulkan Scene viewport could not acquire snapshot mesh GPU resources: ", meshError); return false; }
                for (const MeshGpuPrimitiveRange& primitive : bundle->Primitives) draws.push_back({ bundle, primitive, index });
            }
            if (draws.empty()) { Log::Error("Vulkan Scene viewport resolved a snapshot mesh with no drawable primitives"); return false; }
            const RendererColorPipelineSettings colorSettings = Renderer::GetColorPipelineSettings();
            Ref<ToneMapPassConstants> toneMapConstants = m_ToneMap.AcquireConstants(colorSettings);
            if (!toneMapConstants) { Log::Error("Vulkan Scene viewport could not allocate tone-map constants"); return false; }
            RHI::ResourceState hdrColorState = RHI::ResourceState::Unknown;
            RHI::ResourceState colorState = RHI::ResourceState::Unknown;
            RHI::ResourceState depthState = RHI::ResourceState::Unknown;
            if (!m_Device->QueryResourceState(m_HdrColor.get(), hdrColorState)
                || !m_Device->QueryResourceState(m_Color.get(), colorState)
                || !m_Device->QueryResourceState(m_Depth.get(), depthState)) return false;
            RHI::ViewportClear clear; clear.Color[0] = clearColor.R; clear.Color[1] = clearColor.G; clear.Color[2] = clearColor.B; clear.Color[3] = clearColor.A;
            Scope<RenderGraph> graph = CreateScope<RenderGraph>();
            RHI::TextureDescription hdrColorDescription = m_HdrColor->GetDescription(); hdrColorDescription.InitialState = hdrColorState;
            RHI::TextureDescription colorDescription = m_Color->GetDescription(); colorDescription.InitialState = colorState;
            RHI::TextureDescription depthDescription = m_Depth->GetDescription(); depthDescription.InitialState = depthState;
            const RenderGraph::ResourceHandle hdrColor = graph->AddTexture(hdrColorDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle color = graph->AddTexture(colorDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle depth = graph->AddTexture(depthDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::PassHandle clearPass = graph->AddPass("Scene Viewport Graph Clear", RHI::QueueType::Graphics);
            graph->AddWrite(clearPass, hdrColor, RHI::ResourceState::RenderTarget); graph->AddWrite(clearPass, depth, RHI::ResourceState::DepthWrite);
            graph->SetPassCallback(clearPass, [clear](RenderGraph::ExecutionContext& context) { RHI::Texture* graphColor = context.GetTexture({ 0 }); RHI::Texture* graphDepth = context.GetTexture({ 2 }); return graphColor && graphDepth && context.GetCommandList().BindViewportOutputs(*graphColor, graphDepth) && context.GetCommandList().ClearViewportOutputs(clear); });
            graph->SetPassWorkerRecordingEligible(clearPass);
            const RenderGraph::PassHandle rasterPass = graph->AddPass("Scene Viewport Graph Raster", RHI::QueueType::Graphics);
            graph->AddWrite(rasterPass, hdrColor, RHI::ResourceState::RenderTarget); graph->AddWrite(rasterPass, depth, RHI::ResourceState::DepthWrite);
            RHI::TextureBindingTable* textureTable = m_TextureRuntime->GetBindingTable();
            graph->SetPassCallback(rasterPass, [this, textureTable, width, height, &frame, constantBufferSet, draws](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphColor = context.GetTexture({ 0 }); RHI::Texture* graphDepth = context.GetTexture({ 2 }); RHI::CommandList& commands = context.GetCommandList();
                if (!graphColor || !graphDepth || !commands.BindViewportOutputs(*graphColor, graphDepth)) return false;
                commands.SetGraphicsPipeline(*m_Pipeline); if (!textureTable || !commands.BindGraphicsSampledTextureTable(*textureTable)) return false; commands.SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); commands.SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
                for (const SceneMeshDraw& draw : draws) { commands.SetVertexBuffer(0, *draw.Bundle->VertexBuffer); commands.SetIndexBuffer(*draw.Bundle->IndexBuffer, RHI::IndexFormat::Uint32); commands.SetGraphicsConstantBuffer(0, *constantBufferSet->Allocations[draw.ConstantIndex].Buffer); commands.DrawIndexed(draw.Primitive.IndexCount, 1, draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0); ++frame.IssuedDrawCount; }
                return true;
            });
            const RenderGraph::PassHandle toneMapPass = graph->AddPass("Scene Viewport Graph Tone Map", RHI::QueueType::Graphics);
            graph->AddRead(toneMapPass, hdrColor, RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
            graph->AddWrite(toneMapPass, color, RHI::ResourceState::RenderTarget);
            graph->SetPassCallback(toneMapPass, [this, width, height, toneMapConstants](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphHdr = context.GetTexture({ 0 });
                RHI::Texture* graphColor = context.GetTexture({ 1 });
                return graphHdr && graphColor && m_ToneMap.Record(
                    context.GetCommandList(), *graphHdr, *graphColor, width, height, *toneMapConstants);
            });
            const RenderGraph::PassHandle handoffPass = graph->AddPass("Scene Viewport Graph Output Handoff", RHI::QueueType::Graphics);
            graph->AddRead(handoffPass, color, RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
            graph->SetPassCallback(handoffPass, [](RenderGraph::ExecutionContext& context) { return context.GetTexture({ 1 }) != nullptr; });
            graph->SetPassWorkerRecordingEligible(handoffPass);
            const RenderGraph::CompileResult compiled = graph->Compile();
            const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
            RenderGraph::ExecuteOptions executeOptions;
            executeOptions.RecordingMode = args.HasFlag("--frame-task-single-thread")
                ? FrameTaskExecutionMode::DeterministicSingleThread : FrameTaskExecutionMode::Parallel;
            const bool timestampCaptureRequested = args.HasFlag("--renderer-gpu-timestamps")
                || args.HasFlag("--scene-viewport-render-graph-smoke") || args.HasFlag("--frame-pacing-benchmark");
            executeOptions.EnableTimestampScopes = timestampCaptureRequested
                && !args.HasFlag("--renderer-disable-gpu-timestamps")
                && m_Device->GetCapabilities().GetFeature(RHI::DeviceFeature::Timestamps).IsUsable();
            const RenderGraph::ExecuteResult executed = graph->BindTexture(hdrColor, *m_HdrColor)
                && graph->BindTexture(color, *m_Color) && graph->BindTexture(depth, *m_Depth)
                ? graph->Execute(*m_Device, compiled, executeOptions) : RenderGraph::ExecuteResult {};
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("RenderGraphRecordingV1 backend=Vulkan mode=", executeOptions.RecordingMode == FrameTaskExecutionMode::Parallel ? "worker" : "inline", " workerPasses=", executed.WorkerRecordedPassCount, " overlap=", executed.WorkerRecordingOverlapObserved ? "yes" : "no", " submitted=", executed.AcceptedPassCount, " result=", executed.Success ? "pass" : "fail");
            if (!executed.Completions.empty())
            {
                if (executed.Completions.size() > 1 && !usedTextureHandles.empty())
                {
                    std::string textureRetentionError;
                    if (!m_TextureRuntime->RetainAcceptedFrame(executed.Completions[1], usedTextureHandles, textureRetentionError))
                    { Log::Error("Vulkan Scene viewport could not retain material textures: ", textureRetentionError); m_Device->WaitIdle(); return false; }
                }
                std::string retentionError;
                std::vector<Ref<void>> payloads { constantBufferSet, toneMapConstants };
                for (const SceneMeshDraw& draw : draws) payloads.emplace_back(std::const_pointer_cast<MeshGpuResourceBundle>(draw.Bundle));
                if (!m_SubmittedGraphFrames.Retain(snapshot.FrameIndex, std::move(graph), compiled, executed,
                    std::move(payloads), &retentionError))
                {
                    Log::Error("Vulkan Scene viewport could not retain an accepted RenderGraph submission: ", retentionError);
                    m_Device->WaitIdle();
                    if (executed.Completions.size() > 1 && m_TextureRuntime
                        && m_TextureRuntime->HasRetainedFrame(executed.Completions[1]))
                    {
                        std::string ignoredTextureRetirementError;
                        m_TextureRuntime->Retire(executed.Completions[1], ignoredTextureRetirementError);
                    }
                    return false;
                }
            }
            if (!executed.Success) { Log::Error("Vulkan Scene viewport render graph failed: ", executed.Error); return false; }
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("SceneMeshGpuIntegrationV1 backend=Vulkan snapshot=pass resolver=pass cache=pass indexFormat=UInt32 baseVertex=0 instances=", frame.Instances.size(), " draws=", draws.size(), " constants=per-instance retained=gpu-retirement result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke") && !usedTextureHandles.empty()) Log::Info("SceneMaterialTextureIntegrationV1 backend=Vulkan material=immutable texture=sRGB-base-color sampler=declared table=bound mips=implicit fallbacks=semantic retained=exact-raster-token result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("ProductionRenderGraphRetirementV1 backend=Vulkan frame=", snapshot.FrameIndex, " passes=", executed.AcceptedPassCount, " cpuWaitBetween=no pending=", m_SubmittedGraphFrames.GetPendingCount(), " result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("ClusteredLightGridV1 backend=Vulkan tiles=", frame.LightGrid.TileCountX, "x", frame.LightGrid.TileCountY, " depthSlices=", frame.LightGrid.DepthSliceCount, " lights=", frame.LightGrid.Lights.size(), " global=", frame.LightGrid.GlobalLightIndices.size(), " localReferences=", frame.LightGrid.LocalLightIndices.size(), " overflow=", frame.LightGrid.OverflowedLocalLightReferences, " storage=bounded-csr result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("ScenePhotometricLightPublicationV1 backend=Vulkan directional=", directionalLightCount, " local=", localLightCount, " directionalUnit=", directionalLightCount > 0 ? "lux" : "none", " localUnit=", localLightCount > 0 ? "lm" : "none", " snapshot=typed grid=typed effectiveExposureEV100=", EffectiveExposureEV100(colorSettings), " exposureScale=", ManualExposureScale(colorSettings), " shaderConsumption=no result=pass");
            const bool comparisonRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke");
            if (comparisonRequested)
            {
                RHI::TextureDescription referenceColorDescription = m_Color->GetDescription(); referenceColorDescription.DebugName = "Scene Viewport Bootstrap Reference Color";
                RHI::TextureDescription referenceHdrDescription = m_HdrColor->GetDescription(); referenceHdrDescription.DebugName = "Scene Viewport Bootstrap Reference Linear HDR";
                RHI::TextureDescription referenceDepthDescription = m_Depth->GetDescription(); referenceDepthDescription.DebugName = "Scene Viewport Bootstrap Reference Depth";
                Scope<RHI::Texture> referenceHdr = m_Device->CreateTexture(referenceHdrDescription);
                Scope<RHI::Texture> referenceColor = m_Device->CreateTexture(referenceColorDescription);
                Scope<RHI::Texture> referenceDepth = m_Device->CreateTexture(referenceDepthDescription);
                RHI::TextureReadback graphReadback, referenceReadback;
                const bool referenceRendered = referenceHdr && referenceColor && referenceDepth && RecordBootstrapReference(*referenceHdr, *referenceColor, *referenceDepth, width, height, clear, frame, constants, draws, *toneMapConstants);
                const bool readBack = referenceRendered && ReadbackGraphOutput(*m_Color, graphReadback) && m_Device->ReadbackTexture(*referenceColor, referenceReadback);
                const bool equivalent = readBack && graphReadback.Extent.Width == referenceReadback.Extent.Width && graphReadback.Extent.Height == referenceReadback.Extent.Height
                    && graphReadback.RowPitchBytes == referenceReadback.RowPitchBytes && graphReadback.Data == referenceReadback.Data;
                Log::Info("SceneViewportRenderGraphV1 backend=Vulkan passes=4 labels=clear,raster,tone-map,output-handoff execution=pass reference=direct comparator=exact-byte-", equivalent ? "pass" : "fail", " size=", width, "x", height, " bytes=", graphReadback.Data.size());
                Log::Info("SceneColorPipelineV1 backend=Vulkan sceneLinear=RGBA16F manualExposureEV100=", colorSettings.ManualExposureEV100,
                    " exposureMode=", ToString(colorSettings.ExposureMode),
                    " effectiveExposureEV100=", EffectiveExposureEV100(colorSettings),
                    " exposureScale=", ManualExposureScale(colorSettings),
                    " toneMap=Khronos-PBR-Neutral postToneMapSaturation=", colorSettings.PostToneMapSaturation,
                    " postToneMapContrast=", colorSettings.PostToneMapContrast,
                    " output=sRGB-encoded-RGBA8 result=", equivalent ? "pass" : "fail");
                if (!equivalent) return false;
            }
            Renderer::PublishSceneRasterFrame(std::move(frame));
            if (!comparisonRequested && args.HasFlag("--renderer-frame-trace") && !args.HasFlag("--frame-pacing-benchmark"))
                Log::Trace("Scene viewport graph rendered without the smoke-only bootstrap comparator");
            return true;
        }

        bool ReadbackColor(RHI::TextureReadback& readback) const { return m_Device && m_Color && m_Device->ReadbackTexture(*m_Color, readback); }
        void Shutdown()
        {
            if (m_Device) m_Device->WaitIdle();
            if (m_TextureRuntime) m_TextureRuntime->ReleaseAfterDeviceIdle();
            m_TextureRuntime.reset();
            m_SubmittedGraphFrames.ReleaseAfterDeviceIdle();
            m_MeshResourceCache.Clear();
            m_FrameConstantBuffers = {};
            m_ToneMap.Shutdown();
            m_HdrColor.reset(); m_Color.reset(); m_Depth.reset(); m_Pipeline.reset();
            m_PixelShader.reset(); m_VertexShader.reset(); m_Device = nullptr;
        }
        RHI::Device* m_Device = nullptr; MeshGpuResourceCache m_MeshResourceCache { 32 };
        u32 m_TextureTableCapacity = 0;
        Scope<TextureRuntimePublication> m_TextureRuntime;
        ToneMapPass m_ToneMap;
        Scope<RHI::Shader> m_VertexShader, m_PixelShader; Scope<RHI::Pipeline> m_Pipeline;
        Scope<RHI::Texture> m_HdrColor, m_Color, m_Depth; SubmittedRenderGraphFrameOwner m_SubmittedGraphFrames;
        std::array<Ref<ConstantBufferSet>, SubmittedRenderGraphFrameOwner::Capacity> m_FrameConstantBuffers;
        u32 m_Width = 0, m_Height = 0; u64 m_OutputGeneration = 0;
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
    ToneMapPassConstantCacheDiagnostics NVRHIVulkanViewportSceneRenderer::GetToneMapConstantCacheDiagnostics() const
    {
        return m_Impl ? m_Impl->m_ToneMap.GetConstantCacheDiagnostics() : ToneMapPassConstantCacheDiagnostics {};
    }
    RHI::NVRHIVulkanTextureNativeHandles NVRHIVulkanViewportSceneRenderer::GetOutputNativeHandles() const
    {
        return m_Impl && m_Impl->m_Color ? RHI::GetNVRHIVulkanTextureNativeHandles(*m_Impl->m_Color) : RHI::NVRHIVulkanTextureNativeHandles {};
    }
#endif
}
