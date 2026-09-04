#include "Engine/Renderer/NVRHI/NVRHIVulkanViewportSceneRenderer.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Renderer/MeshGpuResourceCache.h"
#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Renderer/SceneDebugOverlayPass.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/SceneShadowMap.h"
#include "Engine/Renderer/SceneSurfaceConstants.h"
#include "Engine/Renderer/SceneLightPayload.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"
#include "Engine/Renderer/SkyAtmospherePass.h"
#include "Engine/Renderer/TextureRuntimePublication.h"
#include "Engine/Renderer/ToneMapPass.h"
#include "Engine/RenderGraph/RenderGraph.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <algorithm>
    #include <cmath>
    #include <cstddef>
    #include <cstring>
    #include <filesystem>
    #include <limits>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <vector>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_VULKAN)
    namespace
    {
        constexpr u32 kConstantBufferSize = 512;

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

        struct SceneMeshDraw { Ref<const MeshGpuResourceBundle> Bundle; MeshGpuPrimitiveRange Primitive; size_t ConstantIndex = 0; };

        bool TryCalculateObjectBounds(const MeshArtifact& artifact,
            SceneObjectBounds& outBounds)
        {
            if (artifact.Vertices.empty())
                return false;
            SceneObjectBounds bounds {
                { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max() },
                { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max() }
            };
            for (const MeshArtifactVertex& vertex : artifact.Vertices)
            {
                for (size_t component = 0; component < 3; ++component)
                    if (!std::isfinite(vertex.Position[component]))
                        return false;
                bounds.Minimum.X = std::min(bounds.Minimum.X, vertex.Position[0]);
                bounds.Minimum.Y = std::min(bounds.Minimum.Y, vertex.Position[1]);
                bounds.Minimum.Z = std::min(bounds.Minimum.Z, vertex.Position[2]);
                bounds.Maximum.X = std::max(bounds.Maximum.X, vertex.Position[0]);
                bounds.Maximum.Y = std::max(bounds.Maximum.Y, vertex.Position[1]);
                bounds.Maximum.Z = std::max(bounds.Maximum.Z, vertex.Position[2]);
            }
            outBounds = bounds;
            return true;
        }

    }

    struct NVRHIVulkanViewportSceneRenderer::Impl
    {
        static_assert(SceneLightPayloadPublication::Capacity
            == SubmittedRenderGraphFrameOwner::Capacity);
        bool RecordBootstrapReference(
            RHI::Texture& hdrTexture,
            RHI::Texture& colorTexture,
            RHI::Texture* toneMappedTexture,
            RHI::Texture& depthTexture,
            u32 width,
            u32 height,
            const RHI::ViewportClear& clear,
            const SceneRasterFrame& frame,
            const std::vector<ConstantBufferAllocation>& constants,
            const std::vector<SceneMeshDraw>& draws,
            const std::vector<SceneMeshDraw>& shadowDraws,
            const Ref<SceneLightPayloadSlot>& lightPayload,
            RHI::Texture& shadowDepth,
            const SkyAtmospherePassConstants& skyConstants,
            const ToneMapPassConstants& toneMapConstants,
            const Ref<SceneDebugOverlayPassConstants>& debugOverlayConstants)
        {
            Scope<RHI::CommandList> commands = m_Device->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Bootstrap Reference");
            if (!commands || !commands->Begin()
                || !commands->TransitionTexture(shadowDepth, RHI::ResourceState::DepthWrite)
                || !commands->BindDepthOutput(shadowDepth)) return false;
            RHI::ViewportClear shadowClear;
            shadowClear.ClearColor = false;
            shadowClear.ClearDepth = true;
            shadowClear.Depth = 1.0f;
            if (!commands->ClearViewportOutputs(shadowClear)) return false;
            {
                RHI::ScopedDebugMarker marker(*commands,
                    "Scene Viewport Bootstrap Reference Shadow");
                if (m_ShadowPipeline && frame.HasValidView && !shadowDraws.empty())
                {
                    commands->SetGraphicsPipeline(*m_ShadowPipeline);
                    if (!m_TextureRuntime || !m_TextureRuntime->GetBindingTable()
                        || !commands->BindGraphicsSampledTextureTable(
                            *m_TextureRuntime->GetBindingTable())) return false;
                    commands->SetViewport({ 0.0f, 0.0f,
                        static_cast<float>(kSceneShadowMapResolution),
                        static_cast<float>(kSceneShadowMapResolution), 0.0f, 1.0f });
                    commands->SetScissorRect({ 0, 0,
                        static_cast<int>(kSceneShadowMapResolution),
                        static_cast<int>(kSceneShadowMapResolution) });
                    for (const SceneMeshDraw& draw : shadowDraws)
                    {
                        commands->SetVertexBuffer(0, *draw.Bundle->VertexBuffer);
                        commands->SetIndexBuffer(*draw.Bundle->IndexBuffer, RHI::IndexFormat::Uint32);
                        commands->SetGraphicsConstantBuffer(0,
                            *constants[draw.ConstantIndex].Buffer);
                        commands->DrawIndexed(draw.Primitive.IndexCount, 1,
                            draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0);
                    }
                }
            }
            if (!commands->TransitionTexture(shadowDepth, RHI::ResourceState::ShaderResource)
                || !commands->TransitionTexture(hdrTexture, RHI::ResourceState::RenderTarget)
                || !commands->TransitionTexture(depthTexture, RHI::ResourceState::DepthWrite)
                || !commands->BindViewportOutputs(hdrTexture, &depthTexture)
                || !commands->ClearViewportOutputs(clear)) return false;
            {
                RHI::ScopedDebugMarker marker(*commands,
                    "Scene Viewport Bootstrap Reference Sky Atmosphere");
                if (!m_SkyAtmosphere.Record(*commands, hdrTexture, width, height,
                    skyConstants)) return false;
            }
            {
                RHI::ScopedDebugMarker marker(*commands,
                    "Scene Viewport Bootstrap Reference Raster");
                if (!commands->BindViewportOutputs(hdrTexture, &depthTexture))
                    return false;
                if (m_Pipeline && frame.HasValidView && !frame.Instances.empty())
                {
                    commands->SetGraphicsPipeline(*m_Pipeline);
                    if (!m_TextureRuntime || !m_TextureRuntime->GetBindingTable()
                        || !lightPayload || !lightPayload->Gpu
                        || !commands->BindGraphicsSampledTextureTable(*m_TextureRuntime->GetBindingTable())
                        || !commands->BindGraphicsSampledTexture(shadowDepth)
                        || !commands->BindGraphicsReadOnlyStructuredBuffer(*lightPayload->Gpu)) return false;
                    commands->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); commands->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
                    for (const SceneMeshDraw& draw : draws) { commands->SetVertexBuffer(0, *draw.Bundle->VertexBuffer); commands->SetIndexBuffer(*draw.Bundle->IndexBuffer, RHI::IndexFormat::Uint32); commands->SetGraphicsConstantBuffer(0, *constants[draw.ConstantIndex].Buffer); commands->DrawIndexed(draw.Primitive.IndexCount, 1, draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0); }
                }
            }
            RHI::Texture* toneMapOutput = debugOverlayConstants
                ? toneMappedTexture : &colorTexture;
            {
                RHI::ScopedDebugMarker marker(*commands,
                    "Scene Viewport Bootstrap Reference Tone Map");
                if (!toneMapOutput
                    || !commands->TransitionTexture(hdrTexture,
                        RHI::ResourceState::ShaderResource)
                    || !commands->TransitionTexture(*toneMapOutput,
                        RHI::ResourceState::RenderTarget)
                    || !m_ToneMap.Record(*commands, hdrTexture, *toneMapOutput,
                        width, height, toneMapConstants))
                    return false;
            }
            if (debugOverlayConstants)
            {
                RHI::ScopedDebugMarker marker(*commands,
                    "Scene Viewport Bootstrap Reference Debug Overlay");
                if (!commands->TransitionTexture(*toneMapOutput,
                        RHI::ResourceState::ShaderResource)
                    || !commands->TransitionTexture(colorTexture,
                        RHI::ResourceState::RenderTarget)
                    || !m_DebugOverlay.Record(*commands, *toneMapOutput,
                        colorTexture, width, height, *debugOverlayConstants))
                    return false;
            }
            return commands->TransitionTexture(colorTexture,
                    RHI::ResourceState::CopySource)
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

        bool Initialize(RHI::Device* device, const char* pixelEntry)
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
            const std::string viewportConstantsShape =
                "struct{ViewProjection:float32x4x4:row-major@0,NormalTransform:float32x4x4:row-major@64,BaseColorAndAlphaCutoff:float32x4@128,EmissiveAndStrength:float32x4@144,SurfaceFactors:float32x4@160,CallistoFactors:float32x4@176,TextureIndices0:uint32x4@192,TextureIndices1:uint32x4@208,TextureState:uint32x4@224,MaterialState:uint32x4@240,ModelView:float32x4x4:row-major@256,NormalViewTransform:float32x4x4:row-major@320,ShadowViewProjection:float32x4x4:row-major@384,ShadowParameters:float32x4@448,ShadowState:uint32x4@464,SkyIrradianceUpper:float32x4@480,SkyIrradianceLower:float32x4@496}";
            auto makeRequest = [&source, &viewportConstantsShape, this](
                                   RHI::ShaderStage stage, const char* entry,
                                   bool lightPayload, bool shadowReceiver) {
                PortableShaderRequest request;
                request.SourceName = source.ResolvedPath.string(); request.Source = source.Source; request.EntryPoint = entry; request.Stage = stage;
#ifdef _WIN32
                request.Targets = { PortableShaderTarget::Dxil, PortableShaderTarget::Spirv }; request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
#else
                request.Targets = { PortableShaderTarget::Spirv };
#endif
                request.CompilerIdentity = "Slang"; request.CompilerVersion = "2026.13.1"; request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
                request.Options = { "-O3" };
                request.Defines = {
                    "GE_READ_ONLY_TEXTURE_CAPACITY=" + std::to_string(m_TextureTableCapacity),
                    "GE_SCENE_LIGHT_PAYLOAD=" + std::to_string(lightPayload ? 1 : 0),
                    "GE_SCENE_SHADOW_MAP=" + std::to_string(shadowReceiver ? 1 : 0)
                };
                request.ExpectedLayout = {
                    { "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", viewportConstantsShape, 1, 512, 0, 0 },
                    { "ReadOnlySamplers", 's', 0, 1, stage, "SamplerState", "sampler", m_TextureTableCapacity, 0, 0, 0 },
                    { "ReadOnlyTextures", 't', 0, 1, stage, "Texture2D", "float32x4", m_TextureTableCapacity, 0, 1, 4 }
                };
                if (shadowReceiver)
                {
                    request.ExpectedLayout.push_back({ "SceneShadowSampler", 's', 0, 2, RHI::ShaderStage::Pixel, "SamplerState", "sampler", 1, 0, 0, 0 });
                    request.ExpectedLayout.push_back({ "SceneShadowDepth", 't', 0, 2, RHI::ShaderStage::Pixel, "Texture2D", "float32", 1, 0, 1, 1 });
                }
                if (lightPayload) request.ExpectedLayout.push_back({ "SceneLightPayload", 't', 0, 3, RHI::ShaderStage::Pixel, "StructuredBuffer", "uint32x4", 1, 0, 1, 4 });
                if (stage == RHI::ShaderStage::Vertex) request.ExpectedVertexInputs = {{ "Position", "POSITION", 0, 0, "float32x3", 12, 1, 3 }, { "Normal", "NORMAL", 0, 1, "float32x3", 12, 1, 3 }, { "Color", "COLOR", 0, 2, "float32x3", 12, 1, 3 }, { "UV", "TEXCOORD", 0, 3, "float32x2", 8, 1, 2 }};
                return request;
            };
            SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
            const PortableShaderRequest vertexRequest = makeRequest(
                RHI::ShaderStage::Vertex, "VSMain", false, false);
            const PortableShaderRequest pixelRequest = makeRequest(
                RHI::ShaderStage::Pixel, pixelEntry, true, true);
            const PortableShaderRequest shadowVertexRequest = makeRequest(
                RHI::ShaderStage::Vertex, "VSShadowCaster", false, false);
            const PortableShaderRequest shadowPixelRequest = makeRequest(
                RHI::ShaderStage::Pixel, "PSShadowCaster", false, false);
            PortableShaderPackage vertex = compiler.Compile(vertexRequest);
            PortableShaderPackage pixel = compiler.Compile(pixelRequest);
            PortableShaderPackage shadowVertex = compiler.Compile(shadowVertexRequest);
            PortableShaderPackage shadowPixel = compiler.Compile(shadowPixelRequest);
            std::string error;
            if (!PortableShaderContract::ValidatePackage(vertexRequest, vertex, error)
                || !PortableShaderContract::ValidatePackage(pixelRequest, pixel, error)
                || !PortableShaderContract::ValidatePackage(shadowVertexRequest, shadowVertex, error)
                || !PortableShaderContract::ValidatePackage(shadowPixelRequest, shadowPixel, error))
            {
                const auto logDiagnostics = [](std::string_view label,
                                                const PortableShaderPackage& package)
                {
                    for (const PortableShaderDiagnostic& diagnostic : package.Diagnostics)
                        Log::Error("Vulkan Scene ", label, " shader diagnostic: ",
                            diagnostic.Message);
                };
                logDiagnostics("vertex", vertex);
                logDiagnostics("pixel", pixel);
                logDiagnostics("shadow-vertex", shadowVertex);
                logDiagnostics("shadow-pixel", shadowPixel);
                Log::Error("Vulkan Scene viewport shader package validation failed: ", error);
                return false;
            }
            RHI::ShaderDescription vs; vs.DebugName = "Vulkan Scene Viewport VS"; vs.SourceName = source.ResolvedPath.string(); vs.EntryPoint = "main"; vs.Stage = RHI::ShaderStage::Vertex; vs.BinaryFormat = RHI::ShaderBinaryFormat::Spirv; vs.Binary = vertex.Spirv; vs.Reflection = vertex.Reflection;
            RHI::ShaderDescription ps = vs; ps.DebugName = "Vulkan Scene Viewport PS"; ps.Stage = RHI::ShaderStage::Pixel; ps.Binary = pixel.Spirv; ps.Reflection = pixel.Reflection;
            RHI::ShaderDescription shadowVs = vs; shadowVs.DebugName = "Vulkan Scene Shadow Caster VS"; shadowVs.Binary = shadowVertex.Spirv; shadowVs.Reflection = shadowVertex.Reflection;
            RHI::ShaderDescription shadowPs = ps; shadowPs.DebugName = "Vulkan Scene Shadow Caster PS"; shadowPs.Binary = shadowPixel.Spirv; shadowPs.Reflection = shadowPixel.Reflection;
            m_VertexShader = m_Device->CreateShader(vs); m_PixelShader = m_Device->CreateShader(ps);
            m_ShadowVertexShader = m_Device->CreateShader(shadowVs); m_ShadowPixelShader = m_Device->CreateShader(shadowPs);
            const std::vector<RHI::VertexInputAttribute> vertexInputs = {{ "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Position) }, { "NORMAL", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Normal) }, { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Color) }, { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(MeshArtifactVertex, UV) }};
            RHI::PipelineDescription pipeline; pipeline.DebugName = "Vulkan Scene Viewport Pipeline"; pipeline.VertexShader = m_VertexShader.get(); pipeline.PixelShader = m_PixelShader.get(); pipeline.VertexInputs = vertexInputs; pipeline.VertexStrideBytes = sizeof(MeshArtifactVertex); pipeline.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }}; pipeline.SampledTextureTable = RHI::SampledTextureTableBinding { m_TextureTableCapacity }; pipeline.FixedSampledTexture = RHI::FixedSampledTextureBinding {}; pipeline.FixedSampledTexture->PointSampling = true; pipeline.FixedReadOnlyStructuredBuffer = RHI::FixedReadOnlyStructuredBufferBinding {}; pipeline.ColorFormat = RHI::Format::R16G16B16A16Float; pipeline.DepthFormat = RHI::Format::D32Float; pipeline.DepthTestEnable = true; pipeline.DepthWriteEnable = true; pipeline.RasterCullMode = RHI::CullMode::None;
            m_Pipeline = m_VertexShader && m_PixelShader ? m_Device->CreatePipeline(pipeline) : nullptr;
            RHI::PipelineDescription shadowPipeline; shadowPipeline.DebugName = "Vulkan Scene Primary Directional Shadow Pipeline"; shadowPipeline.VertexShader = m_ShadowVertexShader.get(); shadowPipeline.PixelShader = m_ShadowPixelShader.get(); shadowPipeline.VertexInputs = vertexInputs; shadowPipeline.VertexStrideBytes = sizeof(MeshArtifactVertex); shadowPipeline.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }}; shadowPipeline.SampledTextureTable = RHI::SampledTextureTableBinding { m_TextureTableCapacity }; shadowPipeline.ColorFormat = RHI::Format::Unknown; shadowPipeline.DepthFormat = RHI::Format::D32Float; shadowPipeline.DepthTestEnable = true; shadowPipeline.DepthWriteEnable = true; shadowPipeline.RasterCullMode = RHI::CullMode::None;
            m_ShadowPipeline = m_ShadowVertexShader && m_ShadowPixelShader
                ? m_Device->CreatePipeline(shadowPipeline) : nullptr;
            m_TextureRuntime = m_Pipeline && m_ShadowPipeline ? TextureRuntimePublication::Create(*m_Device,
                TextureTargetProfile::RGBAFallback, m_TextureTableCapacity - 1,
                m_TextureTableCapacity) : nullptr;
            RHI::TextureDescription shadowDepth;
            shadowDepth.DebugName = "Vulkan Scene Primary Directional Shadow Depth";
            shadowDepth.Extent = { kSceneShadowMapResolution, kSceneShadowMapResolution };
            shadowDepth.TextureFormat = RHI::Format::D32Float;
            shadowDepth.Usage = static_cast<RHI::TextureUsage>(
                static_cast<u32>(RHI::TextureUsage::DepthStencil)
                | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            m_ShadowDepth = m_TextureRuntime ? m_Device->CreateTexture(shadowDepth) : nullptr;
            return m_Pipeline != nullptr && m_ShadowPipeline != nullptr
                && m_TextureRuntime != nullptr && m_ShadowDepth != nullptr
                && m_SkyAtmosphere.Initialize(*m_Device)
                && m_ToneMap.Initialize(*m_Device)
                && m_DebugOverlay.Initialize(*m_Device);
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
            m_ToneMappedColor.reset();
            RHI::TextureDescription color; color.DebugName = "Vulkan Scene Viewport Color"; color.Extent = { width, height }; color.TextureFormat = RHI::Format::R8G8B8A8Unorm; color.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource) | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            RHI::TextureDescription hdrColor = color; hdrColor.DebugName = "Vulkan Scene Viewport Linear HDR"; hdrColor.TextureFormat = RHI::Format::R16G16B16A16Float; hdrColor.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::ShaderResource) | static_cast<u32>(RHI::TextureUsage::CopySource));
            RHI::TextureDescription depth = color; depth.DebugName = "Vulkan Scene Viewport Depth"; depth.TextureFormat = RHI::Format::D32Float; depth.Usage = RHI::TextureUsage::DepthStencil;
            m_HdrColor = m_Device->CreateTexture(hdrColor); m_Color = m_Device->CreateTexture(color); m_Depth = m_Device->CreateTexture(depth);
            if (!m_HdrColor || !m_Color || !m_Depth) return false;
            m_Width = width; m_Height = height; ++m_OutputGeneration; return true;
        }

        bool EnsureDebugOverlayOutput(u32 width, u32 height)
        {
            if (m_ToneMappedColor)
            {
                const RHI::TextureDescription& current =
                    m_ToneMappedColor->GetDescription();
                if (current.Extent.Width == width && current.Extent.Height == height)
                    return true;
                m_ToneMappedColor.reset();
            }
            RHI::TextureDescription description = m_Color->GetDescription();
            description.DebugName = "Vulkan Scene Viewport Tone-Mapped Intermediate";
            description.InitialState = RHI::ResourceState::Common;
            m_ToneMappedColor = m_Device->CreateTexture(description);
            return m_ToneMappedColor != nullptr;
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
            if (!frame.HasValidView || !frame.ArtifactResolvers)
                return false;
            const RendererColorPipelineSettings colorSettings =
                Renderer::GetColorPipelineSettings();
            std::string lightGridError;
            if (!BuildClusteredLightGrid(snapshot, 0, width, height, {}, frame.LightGrid, lightGridError))
            { Log::Error("Vulkan Scene viewport could not build clustered light grid: ", lightGridError); return false; }
            Ref<SceneLightPayloadSlot> lightPayload;
            std::string lightPayloadError;
            if (!m_LightPayloadPublication.Acquire(*m_Device, snapshot, 0, frame.LightGrid,
                colorSettings,
                m_LightPayloadPublication.GetLastAcceptedGeneration() + 1,
                lightPayload, lightPayloadError))
            { Log::Error("Vulkan Scene viewport could not publish scene light payload: ", lightPayloadError); return false; }
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
            std::vector<SceneSurfaceConstants> cpuConstants(frame.Instances.size());
            std::vector<RHI::TextureBindingHandle> usedTextureHandles;
            for (size_t instanceIndex = 0; instanceIndex < frame.Instances.size(); ++instanceIndex)
            {
                const SceneRasterInstance& instance = frame.Instances[instanceIndex];
                MaterialTextureBindingSet materialBindings;
                std::string materialError;
                if (instance.MaterialId >= frame.MaterialRows.size()) return false;
                const SceneMaterialRow& materialRow = frame.MaterialRows[instance.MaterialId];
                if (materialRow.IsError)
                {
                    materialBindings.Material = materialRow.Material;
                    materialBindings.Handles.fill(m_TextureRuntime->GetErrorHandle());
                    materialBindings.CatalogGeneration = materialRow.CatalogGeneration;
                }
                else if (!m_TextureRuntime->ResolveMaterialTextures(
                    *frame.ArtifactResolvers, instance.MaterialAsset,
                    materialBindings, materialError))
                { Log::Error("Vulkan Scene viewport could not resolve snapshot material: ", materialError); return false; }
                if (materialBindings.CatalogGeneration != frame.MaterialCatalogGeneration) return false;
                if (!materialError.empty()) Log::Warn("Vulkan Scene material uses error resources: ", materialError);
                for (size_t slot = 0; slot < materialBindings.Handles.size(); ++slot)
                    if ((materialBindings.DeclaredMask & (1u << static_cast<u32>(slot))) != 0
                        && (materialBindings.ErrorMask & (1u << static_cast<u32>(slot))) == 0)
                        usedTextureHandles.push_back(materialBindings.Handles[slot]);
                if (!TryBuildSceneSurfaceConstants(instance, materialBindings,
                    materialRow.IsError, cpuConstants[instanceIndex]))
                {
                    Log::Error("Vulkan Scene viewport rejected nonfinite material or surface constants");
                    return false;
                }
            }
            std::vector<SceneMeshDraw> draws;
            std::vector<SceneObjectBounds> objectBounds;
            objectBounds.reserve(frame.Instances.size());
            std::string meshError;
            for (size_t index = 0; index < frame.Instances.size(); ++index)
            {
                MeshArtifact artifact;
                if (!Renderer::ResolvePublishedMeshArtifact(
                    *frame.ArtifactResolvers, frame.Instances[index].MeshAsset,
                    artifact, meshError)) { Log::Error("Vulkan Scene viewport could not resolve snapshot mesh artifact: ", meshError); return false; }
                SceneObjectBounds bounds;
                if (!TryCalculateObjectBounds(artifact, bounds))
                { Log::Error("Vulkan Scene viewport could not calculate finite mesh bounds"); return false; }
                objectBounds.push_back(bounds);
                Ref<const MeshGpuResourceBundle> bundle;
                if (!m_MeshResourceCache.Acquire(*m_Device, artifact, bundle, meshError)) { Log::Error("Vulkan Scene viewport could not acquire snapshot mesh GPU resources: ", meshError); return false; }
                for (const MeshGpuPrimitiveRange& primitive : bundle->Primitives) draws.push_back({ bundle, primitive, index });
            }
            SceneDebugOverlayFrame debugOverlayFrame;
            std::string debugOverlayError;
            if (!TryPrepareSceneDebugOverlay(frame, objectBounds, width, height,
                debugOverlayFrame, debugOverlayError))
            {
                Log::Error("Vulkan Scene viewport could not prepare debug overlay: ",
                    debugOverlayError);
                return false;
            }
            Ref<SceneShadowMapFrame> shadowFrame = CreateRef<SceneShadowMapFrame>();
            std::string shadowError;
            if (!TryPrepareSceneShadowMap(frame, objectBounds,
                kSceneShadowMapResolution, *shadowFrame, shadowError))
            { Log::Error("Vulkan Scene viewport could not prepare its primary shadow map: ", shadowError); return false; }
            std::vector<SceneShadowCasterMode> casterModes(frame.Instances.size(),
                SceneShadowCasterMode::Excluded);
            for (const SceneShadowCaster& caster : shadowFrame->Casters)
            {
                if (caster.InstanceIndex >= casterModes.size())
                    return false;
                casterModes[caster.InstanceIndex] = caster.Mode;
            }
            for (size_t index = 0; index < frame.Instances.size(); ++index)
            {
                if (!TryApplySceneShadowMapConstants(frame.Instances[index],
                    *shadowFrame, casterModes[index], cpuConstants[index]))
                { Log::Error("Vulkan Scene viewport rejected shadow constants"); return false; }
                if (!TryApplySceneSkyIrradianceConstants(
                    frame.SkyAtmosphere, cpuConstants[index]))
                { Log::Error("Vulkan Scene viewport rejected sky irradiance constants"); return false; }
                if (!TryApplySceneDebugVisualizationConstants(
                    frame.Instances[index], frame.DebugVisualization,
                    cpuConstants[index]))
                { Log::Error("Vulkan Scene viewport rejected debug visualization constants"); return false; }
                std::memcpy(constants[index].Mapped, &cpuConstants[index],
                    sizeof(cpuConstants[index]));
            }
            std::vector<SceneMeshDraw> shadowDraws;
            shadowDraws.reserve(draws.size());
            for (const SceneMeshDraw& draw : draws)
                if (draw.ConstantIndex < casterModes.size()
                    && casterModes[draw.ConstantIndex] != SceneShadowCasterMode::Excluded)
                    shadowDraws.push_back(draw);
            Ref<ToneMapPassConstants> toneMapConstants = m_ToneMap.AcquireConstants(colorSettings);
            if (!toneMapConstants) { Log::Error("Vulkan Scene viewport could not allocate tone-map constants"); return false; }
            std::string skyConstantsError;
            Ref<SkyAtmospherePassConstants> skyConstants =
                m_SkyAtmosphere.AcquireConstants(snapshot.FrameIndex,
                    frame.SkyAtmosphere, lightPayload->Payload->PreExposure.Scale,
                    skyConstantsError);
            if (!skyConstants)
            { Log::Error("Vulkan Scene viewport could not acquire sky constants: ", skyConstantsError); return false; }
            Ref<SceneDebugOverlayPassConstants> debugOverlayConstants;
            if (debugOverlayFrame.HasPostToneMapOverlay())
            {
                if (!EnsureDebugOverlayOutput(width, height))
                {
                    Log::Error("Vulkan Scene viewport could not allocate its debug overlay intermediate");
                    return false;
                }
                debugOverlayConstants = m_DebugOverlay.AcquireConstants(
                    snapshot.FrameIndex, debugOverlayFrame, debugOverlayError);
                if (!debugOverlayConstants)
                {
                    Log::Error("Vulkan Scene viewport could not acquire debug overlay constants: ",
                        debugOverlayError);
                    return false;
                }
            }
            RHI::ResourceState hdrColorState = RHI::ResourceState::Unknown;
            RHI::ResourceState colorState = RHI::ResourceState::Unknown;
            RHI::ResourceState toneMappedColorState = RHI::ResourceState::Unknown;
            RHI::ResourceState depthState = RHI::ResourceState::Unknown;
            RHI::ResourceState shadowDepthState = RHI::ResourceState::Unknown;
            RHI::ResourceState lightStagingState = RHI::ResourceState::Unknown;
            RHI::ResourceState lightGpuState = RHI::ResourceState::Unknown;
            if (!m_Device->QueryResourceState(m_HdrColor.get(), hdrColorState)
                || !m_Device->QueryResourceState(m_Color.get(), colorState)
                || !m_Device->QueryResourceState(m_Depth.get(), depthState)
                || !m_Device->QueryResourceState(m_ShadowDepth.get(), shadowDepthState)
                || !m_Device->QueryResourceState(lightPayload->Staging.get(), lightStagingState)
                || !m_Device->QueryResourceState(lightPayload->Gpu.get(), lightGpuState)
                || (debugOverlayConstants && !m_Device->QueryResourceState(
                    m_ToneMappedColor.get(), toneMappedColorState))) return false;
            Math::Vec3 preExposedClear;
            if (!lightPayload->Payload
                || lightPayload->Payload->ColorSettings != colorSettings
                || !TryPreExposeSceneLinear({ clearColor.R, clearColor.G, clearColor.B },
                    lightPayload->Payload->PreExposure, preExposedClear)
                || !std::isfinite(clearColor.A))
            {
                Log::Error("Vulkan Scene viewport rejected a nonfinite clear or pre-exposure state");
                return false;
            }
            RHI::ViewportClear clear;
            clear.Color[0] = preExposedClear.X;
            clear.Color[1] = preExposedClear.Y;
            clear.Color[2] = preExposedClear.Z;
            clear.Color[3] = std::clamp(clearColor.A, 0.0f, 1.0f);
            Scope<RenderGraph> graph = CreateScope<RenderGraph>();
            RHI::TextureDescription hdrColorDescription = m_HdrColor->GetDescription(); hdrColorDescription.InitialState = hdrColorState;
            RHI::TextureDescription colorDescription = m_Color->GetDescription(); colorDescription.InitialState = colorState;
            RHI::TextureDescription depthDescription = m_Depth->GetDescription(); depthDescription.InitialState = depthState;
            RHI::TextureDescription shadowDepthDescription = m_ShadowDepth->GetDescription(); shadowDepthDescription.InitialState = shadowDepthState;
            const RenderGraph::ResourceHandle hdrColor = graph->AddTexture(hdrColorDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle color = graph->AddTexture(colorDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle depth = graph->AddTexture(depthDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle shadowDepth = graph->AddTexture(shadowDepthDescription, RenderGraph::ResourceLifetimeKind::Imported);
            RenderGraph::ResourceHandle toneMappedColor;
            if (debugOverlayConstants)
            {
                RHI::TextureDescription toneMappedColorDescription =
                    m_ToneMappedColor->GetDescription();
                toneMappedColorDescription.InitialState = toneMappedColorState;
                toneMappedColor = graph->AddTexture(toneMappedColorDescription,
                    RenderGraph::ResourceLifetimeKind::Imported);
            }
            RHI::BufferDescription lightStagingDescription = lightPayload->Staging->GetDescription(); lightStagingDescription.InitialState = lightStagingState;
            RHI::BufferDescription lightGpuDescription = lightPayload->Gpu->GetDescription(); lightGpuDescription.InitialState = lightGpuState;
            const RenderGraph::ResourceHandle lightStaging = graph->AddBuffer(lightStagingDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle lightGpu = graph->AddBuffer(lightGpuDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::PassHandle lightCopyPass = graph->AddPass("Scene Light Payload Copy", RHI::QueueType::Graphics);
            graph->AddRead(lightCopyPass, lightStaging, RHI::ResourceState::CopySource);
            graph->AddWrite(lightCopyPass, lightGpu, RHI::ResourceState::CopyDest);
            graph->SetPassCallback(lightCopyPass, [lightStaging, lightGpu](RenderGraph::ExecutionContext& context)
            {
                RHI::Buffer* staging = context.GetBuffer(lightStaging);
                RHI::Buffer* gpu = context.GetBuffer(lightGpu);
                return staging && gpu && context.GetCommandList().CopyBuffer(*gpu, 0, *staging, 0, gpu->GetDescription().SizeBytes);
            });
            RHI::TextureBindingTable* textureTable = m_TextureRuntime->GetBindingTable();
            const RenderGraph::PassHandle shadowPass = graph->AddPass(
                "Scene Primary Directional Shadow Map", RHI::QueueType::Graphics);
            graph->AddWrite(shadowPass, shadowDepth, RHI::ResourceState::DepthWrite);
            graph->SetPassCallback(shadowPass,
                [this, textureTable, shadowDepth, constantBufferSet, shadowDraws](
                    RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphShadow = context.GetTexture(shadowDepth);
                RHI::CommandList& commands = context.GetCommandList();
                RHI::ViewportClear shadowClear;
                shadowClear.ClearColor = false;
                shadowClear.ClearDepth = true;
                shadowClear.Depth = 1.0f;
                if (!graphShadow || !commands.BindDepthOutput(*graphShadow)
                    || !commands.ClearViewportOutputs(shadowClear))
                    return false;
                if (shadowDraws.empty())
                    return true;
                commands.SetGraphicsPipeline(*m_ShadowPipeline);
                if (!textureTable
                    || !commands.BindGraphicsSampledTextureTable(*textureTable))
                    return false;
                commands.SetViewport({ 0.0f, 0.0f,
                    static_cast<float>(kSceneShadowMapResolution),
                    static_cast<float>(kSceneShadowMapResolution), 0.0f, 1.0f });
                commands.SetScissorRect({ 0, 0,
                    static_cast<int>(kSceneShadowMapResolution),
                    static_cast<int>(kSceneShadowMapResolution) });
                for (const SceneMeshDraw& draw : shadowDraws)
                {
                    commands.SetVertexBuffer(0, *draw.Bundle->VertexBuffer);
                    commands.SetIndexBuffer(*draw.Bundle->IndexBuffer,
                        RHI::IndexFormat::Uint32);
                    commands.SetGraphicsConstantBuffer(0,
                        *constantBufferSet->Allocations[draw.ConstantIndex].Buffer);
                    commands.DrawIndexed(draw.Primitive.IndexCount, 1,
                        draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0);
                }
                return true;
            });
            const RenderGraph::PassHandle clearPass = graph->AddPass("Scene Viewport Graph Clear", RHI::QueueType::Graphics);
            graph->AddWrite(clearPass, hdrColor, RHI::ResourceState::RenderTarget); graph->AddWrite(clearPass, depth, RHI::ResourceState::DepthWrite);
            graph->SetPassCallback(clearPass, [hdrColor, depth, clear](RenderGraph::ExecutionContext& context) { RHI::Texture* graphColor = context.GetTexture(hdrColor); RHI::Texture* graphDepth = context.GetTexture(depth); return graphColor && graphDepth && context.GetCommandList().BindViewportOutputs(*graphColor, graphDepth) && context.GetCommandList().ClearViewportOutputs(clear); });
            graph->SetPassWorkerRecordingEligible(clearPass);
            const RenderGraph::PassHandle skyPass = graph->AddPass(
                "Scene Sky Atmosphere", RHI::QueueType::Graphics);
            graph->AddWrite(skyPass, hdrColor, RHI::ResourceState::RenderTarget);
            graph->SetPassCallback(skyPass,
                [this, hdrColor, width, height, skyConstants](
                    RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphHdr = context.GetTexture(hdrColor);
                return graphHdr && m_SkyAtmosphere.Record(context.GetCommandList(),
                    *graphHdr, width, height, *skyConstants);
            });
            const RenderGraph::PassHandle rasterPass = graph->AddPass("Scene Viewport Graph Raster", RHI::QueueType::Graphics);
            graph->AddWrite(rasterPass, hdrColor, RHI::ResourceState::RenderTarget); graph->AddWrite(rasterPass, depth, RHI::ResourceState::DepthWrite);
            graph->AddRead(rasterPass, lightGpu, RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
            graph->AddRead(rasterPass, shadowDepth, RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
            graph->SetPassCallback(rasterPass, [this, textureTable, hdrColor, depth, lightGpu, shadowDepth, width, height, &frame, constantBufferSet, draws](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphColor = context.GetTexture(hdrColor); RHI::Texture* graphDepth = context.GetTexture(depth); RHI::Texture* graphShadow = context.GetTexture(shadowDepth); RHI::CommandList& commands = context.GetCommandList();
                RHI::Buffer* graphLightPayload = context.GetBuffer(lightGpu);
                if (!graphColor || !graphDepth || !commands.BindViewportOutputs(*graphColor, graphDepth)) return false;
                commands.SetGraphicsPipeline(*m_Pipeline); if (!textureTable || !graphLightPayload || !graphShadow || !commands.BindGraphicsSampledTextureTable(*textureTable) || !commands.BindGraphicsSampledTexture(*graphShadow) || !commands.BindGraphicsReadOnlyStructuredBuffer(*graphLightPayload)) return false; commands.SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); commands.SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
                for (const SceneMeshDraw& draw : draws) { commands.SetVertexBuffer(0, *draw.Bundle->VertexBuffer); commands.SetIndexBuffer(*draw.Bundle->IndexBuffer, RHI::IndexFormat::Uint32); commands.SetGraphicsConstantBuffer(0, *constantBufferSet->Allocations[draw.ConstantIndex].Buffer); commands.DrawIndexed(draw.Primitive.IndexCount, 1, draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0); ++frame.IssuedDrawCount; }
                return true;
            });
            const RenderGraph::PassHandle toneMapPass = graph->AddPass("Scene Viewport Graph Tone Map", RHI::QueueType::Graphics);
            graph->AddRead(toneMapPass, hdrColor, RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
            const RenderGraph::ResourceHandle toneMapOutput = debugOverlayConstants
                ? toneMappedColor : color;
            graph->AddWrite(toneMapPass, toneMapOutput, RHI::ResourceState::RenderTarget);
            graph->SetPassCallback(toneMapPass, [this, hdrColor, toneMapOutput,
                width, height, toneMapConstants](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphHdr = context.GetTexture(hdrColor);
                RHI::Texture* graphColor = context.GetTexture(toneMapOutput);
                return graphHdr && graphColor && m_ToneMap.Record(
                    context.GetCommandList(), *graphHdr, *graphColor, width, height, *toneMapConstants);
            });
            if (debugOverlayConstants)
            {
                const RenderGraph::PassHandle debugOverlayPass = graph->AddPass(
                    "Scene Debug Overlay", RHI::QueueType::Graphics);
                graph->AddRead(debugOverlayPass, toneMappedColor,
                    RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
                graph->AddWrite(debugOverlayPass, color,
                    RHI::ResourceState::RenderTarget);
                graph->SetPassCallback(debugOverlayPass,
                    [this, toneMappedColor, color, width, height,
                        debugOverlayConstants](RenderGraph::ExecutionContext& context)
                {
                    RHI::Texture* graphInput = context.GetTexture(toneMappedColor);
                    RHI::Texture* graphOutput = context.GetTexture(color);
                    return graphInput && graphOutput && m_DebugOverlay.Record(
                        context.GetCommandList(), *graphInput, *graphOutput,
                        width, height, *debugOverlayConstants);
                });
            }
            const RenderGraph::PassHandle handoffPass = graph->AddPass("Scene Viewport Graph Output Handoff", RHI::QueueType::Graphics);
            graph->AddRead(handoffPass, color, RHI::ResourceState::ShaderResource, RHI::ShaderStage::Pixel);
            graph->SetPassCallback(handoffPass, [color](RenderGraph::ExecutionContext& context) { return context.GetTexture(color) != nullptr; });
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
                && graph->BindTexture(shadowDepth, *m_ShadowDepth)
                && (!debugOverlayConstants || graph->BindTexture(
                    toneMappedColor, *m_ToneMappedColor))
                && graph->BindBuffer(lightStaging, *lightPayload->Staging) && graph->BindBuffer(lightGpu, *lightPayload->Gpu)
                ? graph->Execute(*m_Device, compiled, executeOptions) : RenderGraph::ExecuteResult {};
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("RenderGraphRecordingV1 backend=Vulkan mode=", executeOptions.RecordingMode == FrameTaskExecutionMode::Parallel ? "worker" : "inline", " workerPasses=", executed.WorkerRecordedPassCount, " overlap=", executed.WorkerRecordingOverlapObserved ? "yes" : "no", " submitted=", executed.AcceptedPassCount, " result=", executed.Success ? "pass" : "fail");
            std::optional<RHI::CompletionToken> materialTextureToken;
            const auto compiledRaster = std::find_if(compiled.Passes.begin(), compiled.Passes.end(),
                [rasterPass](const RenderGraph::CompiledPass& pass)
                {
                    return pass.Pass.Index == rasterPass.Index;
                });
            if (compiledRaster != compiled.Passes.end())
            {
                const size_t rasterIndex = static_cast<size_t>(
                    std::distance(compiled.Passes.begin(), compiledRaster));
                if (rasterIndex < executed.Completions.size())
                    materialTextureToken = executed.Completions[rasterIndex];
            }
            if (!executed.Completions.empty())
            {
                if (materialTextureToken && !usedTextureHandles.empty())
                {
                    std::string textureRetentionError;
                    if (!m_TextureRuntime->RetainAcceptedFrame(
                        *materialTextureToken, usedTextureHandles, textureRetentionError))
                    { Log::Error("Vulkan Scene viewport could not retain material textures: ", textureRetentionError); m_Device->WaitIdle(); return false; }
                }
                std::string retentionError;
                std::vector<Ref<void>> payloads { constantBufferSet, skyConstants,
                    toneMapConstants, lightPayload, shadowFrame };
                if (debugOverlayConstants)
                    payloads.emplace_back(debugOverlayConstants);
                for (const SceneMeshDraw& draw : draws) payloads.emplace_back(std::const_pointer_cast<MeshGpuResourceBundle>(draw.Bundle));
                if (!m_SubmittedGraphFrames.Retain(snapshot.FrameIndex, std::move(graph), compiled, executed,
                    std::move(payloads), &retentionError))
                {
                    Log::Error("Vulkan Scene viewport could not retain an accepted RenderGraph submission: ", retentionError);
                    m_Device->WaitIdle();
                    if (materialTextureToken && m_TextureRuntime
                        && m_TextureRuntime->HasRetainedFrame(*materialTextureToken))
                    {
                        std::string ignoredTextureRetirementError;
                        m_TextureRuntime->Retire(
                            *materialTextureToken, ignoredTextureRetirementError);
                    }
                    return false;
                }
            }
            if (!executed.Success) { Log::Error("Vulkan Scene viewport render graph failed: ", executed.Error); return false; }
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("SceneMeshGpuIntegrationV1 backend=Vulkan snapshot=pass resolver=pass cache=pass indexFormat=UInt32 baseVertex=0 instances=", frame.Instances.size(), " draws=", draws.size(), " constants=per-instance retained=gpu-retirement result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke") && !usedTextureHandles.empty()) Log::Info("SceneMaterialTextureIntegrationV1 backend=Vulkan material=immutable texture=sRGB-base-color sampler=declared table=bound mips=implicit fallbacks=semantic retained=exact-raster-token result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("ProductionRenderGraphRetirementV1 backend=Vulkan frame=", snapshot.FrameIndex, " passes=", executed.AcceptedPassCount, " cpuWaitBetween=no pending=", m_SubmittedGraphFrames.GetPendingCount(), " result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("ClusteredLightGridV1 backend=Vulkan tiles=", frame.LightGrid.TileCountX, "x", frame.LightGrid.TileCountY, " depthSlices=", frame.LightGrid.DepthSliceCount, " lights=", frame.LightGrid.Lights.size(), " global=", frame.LightGrid.GlobalLightIndices.size(), " localReferences=", frame.LightGrid.LocalLightIndices.size(), " overflow=", frame.LightGrid.OverflowedLocalLightReferences, " storage=bounded-csr result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("ScenePhotometricLightPublicationV2 backend=Vulkan directional=", directionalLightCount, " local=", localLightCount, " directionalUnit=", directionalLightCount > 0 ? "lux" : "none", " localUnit=", localLightCount > 0 ? "lm" : "none", " snapshot=typed grid=typed effectiveExposureEV100=", EffectiveExposureEV100(colorSettings), " exposureScale=", ManualExposureScale(colorSettings), " shaderConsumption=production-PSMain result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("SceneShadowMapV1 backend=Vulkan light=", shadowFrame->PrimaryLightEntity, " resolution=", shadowFrame->Resolution, " stabilization=texel-snapped filter=3x3-pcf casters=", shadowFrame->Casters.size(), " opaque=", shadowFrame->OpaqueCasterCount, " masked=", shadowFrame->AlphaTestedCasterCount, " conservativeError=", shadowFrame->ConservativeErrorCasterCount, " componentExcluded=", shadowFrame->ComponentExcludedCount, " blendExcluded=", shadowFrame->BlendExcludedCount, " receiverExclusions=deferred graphLabel=Scene-Primary-Directional-Shadow-Map result=pass");
            if (args.HasFlag("--scene-viewport-render-graph-smoke")
                || args.HasFlag("--scene-sky-atmosphere-smoke"))
                Log::Info("SceneSkyAtmosphereV1 backend=Vulkan model=Preetham1999 enabled=",
                    frame.SkyAtmosphere.Enabled ? "yes" : "no", " sunEntity=",
                    frame.SkyAtmosphere.SunEntity, " sunLightIndex=",
                    frame.SkyAtmosphere.SunLightIndex, " turbidity=",
                    frame.SkyAtmosphere.Turbidity, " groundAlbedo=",
                    frame.SkyAtmosphere.GroundAlbedo, " angularRadiusDegrees=",
                    kBasicSunAngularRadiusDegrees,
                    " skyDome=procedural-fullscreen diffuseIrradiance=first-order-zonal graphLabel=Scene-Sky-Atmosphere result=pass");
            const bool comparisonRequested = Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke");
            if (comparisonRequested)
            {
                std::vector<std::string_view> expectedPassNames {
                    "Scene Light Payload Copy",
                    "Scene Primary Directional Shadow Map",
                    "Scene Viewport Graph Clear",
                    "Scene Sky Atmosphere",
                    "Scene Viewport Graph Raster",
                    "Scene Viewport Graph Tone Map"
                };
                if (debugOverlayConstants)
                    expectedPassNames.emplace_back("Scene Debug Overlay");
                expectedPassNames.emplace_back(
                    "Scene Viewport Graph Output Handoff");
                bool topologyMatches = compiled.Success && executed.Success
                    && compiled.Passes.size() == expectedPassNames.size()
                    && executed.AcceptedPassCount
                        == static_cast<u32>(expectedPassNames.size());
                for (size_t index = 0;
                    topologyMatches && index < expectedPassNames.size(); ++index)
                {
                    topologyMatches = compiled.Passes[index].DebugName
                        == expectedPassNames[index];
                }
                if (!topologyMatches)
                {
                    std::string actualPassNames;
                    for (const RenderGraph::CompiledPass& pass : compiled.Passes)
                    {
                        if (!actualPassNames.empty())
                            actualPassNames += ",";
                        actualPassNames += pass.DebugName;
                    }
                    Log::Error("SceneViewportRenderGraphV1 backend=Vulkan passes=",
                        compiled.Passes.size(), " labels=", actualPassNames,
                        " execution=fail reference=direct comparator=not-run accepted=",
                        executed.AcceptedPassCount, " expectedPasses=",
                        expectedPassNames.size());
                    return false;
                }
                RHI::TextureDescription referenceColorDescription = m_Color->GetDescription(); referenceColorDescription.DebugName = "Scene Viewport Bootstrap Reference Color";
                RHI::TextureDescription referenceHdrDescription = m_HdrColor->GetDescription(); referenceHdrDescription.DebugName = "Scene Viewport Bootstrap Reference Linear HDR";
                RHI::TextureDescription referenceDepthDescription = m_Depth->GetDescription(); referenceDepthDescription.DebugName = "Scene Viewport Bootstrap Reference Depth";
                RHI::TextureDescription referenceShadowDescription = m_ShadowDepth->GetDescription(); referenceShadowDescription.DebugName = "Scene Viewport Bootstrap Reference Shadow Depth";
                Scope<RHI::Texture> referenceHdr = m_Device->CreateTexture(referenceHdrDescription);
                Scope<RHI::Texture> referenceColor = m_Device->CreateTexture(referenceColorDescription);
                Scope<RHI::Texture> referenceDepth = m_Device->CreateTexture(referenceDepthDescription);
                Scope<RHI::Texture> referenceShadow = m_Device->CreateTexture(referenceShadowDescription);
                Scope<RHI::Texture> referenceToneMapped;
                if (debugOverlayConstants)
                {
                    RHI::TextureDescription referenceToneMappedDescription =
                        m_ToneMappedColor->GetDescription();
                    referenceToneMappedDescription.DebugName =
                        "Scene Viewport Bootstrap Reference Tone-Mapped Intermediate";
                    referenceToneMapped = m_Device->CreateTexture(
                        referenceToneMappedDescription);
                }
                RHI::TextureReadback graphReadback, referenceReadback;
                const bool referenceRendered = referenceHdr && referenceColor && referenceDepth
                    && referenceShadow && (!debugOverlayConstants || referenceToneMapped)
                    && RecordBootstrapReference(*referenceHdr,
                        *referenceColor, referenceToneMapped.get(), *referenceDepth,
                        width, height, clear,
                        frame, constants, draws, shadowDraws, lightPayload,
                        *referenceShadow, *skyConstants, *toneMapConstants,
                        debugOverlayConstants);
                const bool readBack = referenceRendered && ReadbackGraphOutput(*m_Color, graphReadback) && m_Device->ReadbackTexture(*referenceColor, referenceReadback);
                const bool equivalent = readBack && graphReadback.Extent.Width == referenceReadback.Extent.Width && graphReadback.Extent.Height == referenceReadback.Extent.Height
                    && graphReadback.RowPitchBytes == referenceReadback.RowPitchBytes && graphReadback.Data == referenceReadback.Data;
                Log::Info("SceneViewportRenderGraphV1 backend=Vulkan passes=",
                    compiled.Passes.size(),
                    " labels=light-payload-copy,primary-directional-shadow-map,clear,sky-atmosphere,raster,tone-map,",
                    debugOverlayConstants ? "debug-overlay," : "",
                    "output-handoff execution=pass reference=direct comparator=exact-byte-",
                    equivalent ? "pass" : "fail", " size=", width, "x", height,
                    " bytes=", graphReadback.Data.size());
                Log::Info("SceneColorPipelineV2 backend=Vulkan sceneLinear=pre-exposed-finite-RGBA16F exposurePlacement=before-storage toneMapExposure=none finiteClamp=65504 manualExposureEV100=", colorSettings.ManualExposureEV100,
                    " exposureMode=", ToString(colorSettings.ExposureMode),
                    " effectiveExposureEV100=", EffectiveExposureEV100(colorSettings),
                    " exposureScale=", ManualExposureScale(colorSettings),
                    " toneMap=Khronos-PBR-Neutral postToneMapSaturation=", colorSettings.PostToneMapSaturation,
                    " postToneMapContrast=", colorSettings.PostToneMapContrast,
                    " output=sRGB-encoded-RGBA8 result=", equivalent ? "pass" : "fail");
                if (!equivalent) return false;
            }
            std::string lightPayloadCommitError;
            if (!m_LightPayloadPublication.Commit(lightPayload, lightPayloadCommitError))
            {
                Log::Error("Vulkan Scene viewport could not commit its accepted light payload: ",
                    lightPayloadCommitError);
                return false;
            }
            Renderer::PublishSceneRasterFrame(std::move(frame));
            if (!comparisonRequested && args.HasFlag("--renderer-frame-trace") && !args.HasFlag("--frame-pacing-benchmark"))
                Log::Trace("Scene viewport graph rendered without the smoke-only bootstrap comparator");
            return true;
        }

        bool ReadbackColor(RHI::TextureReadback& readback) const { return m_Device && m_Color && m_Device->ReadbackTexture(*m_Color, readback); }
        bool ReadbackHdr(RHI::TextureReadback& readback)
        {
            return m_Device && m_HdrColor && ReadbackGraphOutput(*m_HdrColor, readback);
        }
        void Shutdown()
        {
            if (m_Device) m_Device->WaitIdle();
            if (m_TextureRuntime) m_TextureRuntime->ReleaseAfterDeviceIdle();
            m_TextureRuntime.reset();
            m_SubmittedGraphFrames.ReleaseAfterDeviceIdle();
            m_LightPayloadPublication.ClearAfterDeviceIdle();
            m_MeshResourceCache.Clear();
            m_FrameConstantBuffers = {};
            m_SkyAtmosphere.Shutdown();
            m_ToneMap.Shutdown();
            m_DebugOverlay.Shutdown();
            m_HdrColor.reset(); m_Color.reset(); m_ToneMappedColor.reset();
            m_Depth.reset(); m_ShadowDepth.reset();
            m_ShadowPipeline.reset(); m_Pipeline.reset();
            m_ShadowPixelShader.reset(); m_ShadowVertexShader.reset();
            m_PixelShader.reset(); m_VertexShader.reset(); m_Device = nullptr;
        }
        RHI::Device* m_Device = nullptr; MeshGpuResourceCache m_MeshResourceCache { 32 };
        u32 m_TextureTableCapacity = 0;
        Scope<TextureRuntimePublication> m_TextureRuntime;
        SkyAtmospherePass m_SkyAtmosphere;
        ToneMapPass m_ToneMap;
        SceneDebugOverlayPass m_DebugOverlay;
        Scope<RHI::Shader> m_VertexShader, m_PixelShader;
        Scope<RHI::Shader> m_ShadowVertexShader, m_ShadowPixelShader;
        Scope<RHI::Pipeline> m_Pipeline, m_ShadowPipeline;
        Scope<RHI::Texture> m_HdrColor, m_Color, m_ToneMappedColor, m_Depth,
            m_ShadowDepth;
        SubmittedRenderGraphFrameOwner m_SubmittedGraphFrames;
        SceneLightPayloadPublication m_LightPayloadPublication;
        std::array<Ref<ConstantBufferSet>, SubmittedRenderGraphFrameOwner::Capacity> m_FrameConstantBuffers;
        u32 m_Width = 0, m_Height = 0; u64 m_OutputGeneration = 0;
    };

    NVRHIVulkanViewportSceneRenderer::NVRHIVulkanViewportSceneRenderer() = default;
    NVRHIVulkanViewportSceneRenderer::~NVRHIVulkanViewportSceneRenderer() { Shutdown(); }
    bool NVRHIVulkanViewportSceneRenderer::Initialize(RHI::Device* device) { m_Impl = CreateScope<Impl>(); if (m_Impl->Initialize(device, "PSMain")) return true; m_Impl.reset(); return false; }
    bool NVRHIVulkanViewportSceneRenderer::InitializeSurfaceBasisProbe(RHI::Device* device) { m_Impl = CreateScope<Impl>(); if (m_Impl->Initialize(device, "PSSurfaceBasisMaterialProbe")) return true; m_Impl.reset(); return false; }
    bool NVRHIVulkanViewportSceneRenderer::InitializeLightPayloadProbe(RHI::Device* device) { m_Impl = CreateScope<Impl>(); if (m_Impl->Initialize(device, "PSLightPayloadProbe")) return true; m_Impl.reset(); return false; }
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
    bool NVRHIVulkanViewportSceneRenderer::ReadbackHdr(RHI::TextureReadback& readback) { return m_Impl && m_Impl->ReadbackHdr(readback); }
    u64 NVRHIVulkanViewportSceneRenderer::GetOutputGeneration() const { return m_Impl ? m_Impl->m_OutputGeneration : 0; }
    u32 NVRHIVulkanViewportSceneRenderer::GetOutputWidth() const { return m_Impl ? m_Impl->m_Width : 0; }
    u32 NVRHIVulkanViewportSceneRenderer::GetOutputHeight() const { return m_Impl ? m_Impl->m_Height : 0; }
    ToneMapPassConstantCacheDiagnostics NVRHIVulkanViewportSceneRenderer::GetToneMapConstantCacheDiagnostics() const
    {
        return m_Impl ? m_Impl->m_ToneMap.GetConstantCacheDiagnostics() : ToneMapPassConstantCacheDiagnostics {};
    }
    SceneLightPayloadPublicationDiagnostics NVRHIVulkanViewportSceneRenderer::GetLightPayloadPublicationDiagnostics() const
    {
        return m_Impl ? m_Impl->m_LightPayloadPublication.GetDiagnostics()
            : SceneLightPayloadPublicationDiagnostics {};
    }
    RHI::NVRHIVulkanTextureNativeHandles NVRHIVulkanViewportSceneRenderer::GetOutputNativeHandles() const
    {
        return m_Impl && m_Impl->m_Color ? RHI::GetNVRHIVulkanTextureNativeHandles(*m_Impl->m_Color) : RHI::NVRHIVulkanTextureNativeHandles {};
    }
#endif
}
