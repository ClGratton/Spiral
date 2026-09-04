#include "Engine/Renderer/NVRHI/NVRHID3D12ViewportSceneRenderer.h"

#include "Engine/Core/Application.h"
#include "Engine/Core/Log.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Math/Math.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/RenderGraph/RenderGraph.h"
#include "Engine/Renderer/AsyncShaderPackageService.h"
#include "Engine/Renderer/MeshGpuResourceCache.h"
#include "Engine/Renderer/SceneDebugOverlayPass.h"
#include "Engine/Renderer/SceneShadowMap.h"
#include "Engine/Renderer/SceneSurfaceConstants.h"
#include "Engine/Renderer/SceneLightPayload.h"
#include "Engine/Renderer/NVRHI/D3D12ViewportShaderReloadCoordinator.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"
#include "Engine/Renderer/SkyAtmospherePass.h"
#include "Engine/Renderer/TextureRuntimePublication.h"
#include "Engine/Renderer/ToneMapPass.h"

#if defined(GE_HAS_NVRHI_D3D12)
    #include <algorithm>
    #include <cmath>
    #include <cstddef>
    #include <filesystem>
    #include <limits>
    #include <memory>
    #include <optional>
    #include <string>
    #include <string_view>
    #include <vector>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_D3D12)
    namespace
    {
        constexpr u32 kViewportConstantBufferSize = 512;
        constexpr std::string_view kViewportShaderPath = "Engine/Shaders/EditorViewport.hlsl";

        std::string_view ViewportShaderPath()
        {
            const std::string_view overridePath = Application::Get().GetSpecification().CommandLineArgs.GetOptionValue("--viewport-shader-path");
            return overridePath.empty() ? kViewportShaderPath : overridePath;
        }

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
            const SceneRasterFrame& rasterFrame,
            const std::vector<ConstantBufferAllocation>* constantBuffers,
            const std::vector<SceneMeshDraw>& draws,
            const std::vector<SceneMeshDraw>& shadowDraws,
            const Ref<SceneLightPayloadSlot>& lightPayload,
            RHI::Texture& shadowDepth,
            const SkyAtmospherePassConstants& skyConstants,
            const ToneMapPassConstants& toneMapConstants,
            const Ref<SceneDebugOverlayPassConstants>& debugOverlayConstants)
        {
            Scope<RHI::CommandList> commands = m_RHIDevice->CreateCommandList(RHI::QueueType::Graphics, "Scene Viewport Bootstrap Reference");
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
                if (m_ShadowPipeline && rasterFrame.HasValidView && constantBuffers
                    && !shadowDraws.empty())
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
                        commands->SetIndexBuffer(*draw.Bundle->IndexBuffer,
                            RHI::IndexFormat::Uint32);
                        commands->SetGraphicsConstantBuffer(0,
                            *(*constantBuffers)[draw.ConstantIndex].Buffer);
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
                if (m_Pipeline && rasterFrame.HasValidView && !rasterFrame.Instances.empty())
                {
                    commands->SetGraphicsPipeline(*m_Pipeline);
                    if (!m_TextureRuntime || !m_TextureRuntime->GetBindingTable()
                        || !lightPayload || !lightPayload->Gpu
                        || !commands->BindGraphicsSampledTextureTable(*m_TextureRuntime->GetBindingTable())
                        || !commands->BindGraphicsSampledTexture(shadowDepth)
                        || !commands->BindGraphicsReadOnlyStructuredBuffer(*lightPayload->Gpu))
                        return false;
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
            m_TextureTableCapacity = RHI::SelectReadOnlyTextureTableCapacity(
                m_RHIDevice->GetCapabilities());
            if (m_TextureTableCapacity < 2)
                return false;

            if (!RequestInitialPipeline())
                return false;
            m_TextureRuntime = TextureRuntimePublication::Create(*m_RHIDevice,
                TextureTargetProfile::RGBAFallback, m_TextureTableCapacity - 1,
                m_TextureTableCapacity);
            RHI::TextureDescription shadowDepth;
            shadowDepth.DebugName = "D3D12 Scene Primary Directional Shadow Depth";
            shadowDepth.Extent = { kSceneShadowMapResolution, kSceneShadowMapResolution };
            shadowDepth.TextureFormat = RHI::Format::D32Float;
            shadowDepth.Usage = static_cast<RHI::TextureUsage>(
                static_cast<u32>(RHI::TextureUsage::DepthStencil)
                | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            m_ShadowDepth = m_TextureRuntime
                ? m_RHIDevice->CreateTexture(shadowDepth) : nullptr;
            return m_TextureRuntime != nullptr && m_ShadowDepth != nullptr
                && m_SkyAtmosphere.Initialize(*m_RHIDevice)
                && m_ToneMap.Initialize(*m_RHIDevice)
                && m_DebugOverlay.Initialize(*m_RHIDevice);
        }

        void Shutdown()
        {
            m_ReloadCoordinator.Invalidate();
            if (m_RHIDevice)
                m_RHIDevice->WaitIdle();
            if (m_TextureRuntime)
                m_TextureRuntime->ReleaseAfterDeviceIdle();
            m_TextureRuntime.reset();
            m_SubmittedGraphFrames.ReleaseAfterDeviceIdle();
            m_LightPayloadPublication.ClearAfterDeviceIdle();
            m_MeshResourceCache.Clear();
            m_FrameConstantBuffers.clear();
            m_SkyAtmosphere.Shutdown();
            m_ToneMap.Shutdown();
            m_DebugOverlay.Shutdown();
            m_HdrColor.reset();
            m_ToneMappedColor.reset();
            m_ShadowDepth.reset();
            m_ShadowPipeline.reset();
            m_Pipeline.reset();
            m_ShadowPixelShader.reset();
            m_ShadowVertexShader.reset();
            m_PixelShader.reset();
            m_VertexShader.reset();
            if (m_ShaderPackages)
                m_ShaderPackages->Shutdown();
            m_ShaderPackages.reset();
            m_RHIDevice = nullptr;
        }

        bool EnsureHdrOutput(u32 width, u32 height)
        {
            if (m_HdrColor && m_HdrWidth == width && m_HdrHeight == height)
                return true;
            if (m_SubmittedGraphFrames.GetPendingCount() != 0)
            {
                Log::Error("D3D12 Scene HDR output replacement deferred because an exact RenderGraph token is still incomplete");
                return false;
            }
            m_HdrColor.reset();
            m_ToneMappedColor.reset();
            RHI::TextureDescription description;
            description.DebugName = "D3D12 Scene Viewport Linear HDR";
            description.Extent = { width, height };
            description.TextureFormat = RHI::Format::R16G16B16A16Float;
            description.Usage = static_cast<RHI::TextureUsage>(
                static_cast<u32>(RHI::TextureUsage::RenderTarget)
                | static_cast<u32>(RHI::TextureUsage::ShaderResource));
            m_HdrColor = m_RHIDevice ? m_RHIDevice->CreateTexture(description) : nullptr;
            if (!m_HdrColor)
                return false;
            m_HdrWidth = width;
            m_HdrHeight = height;
            return true;
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
            RHI::TextureDescription description = m_HdrColor->GetDescription();
            description.DebugName = "D3D12 Scene Viewport Tone-Mapped Intermediate";
            description.TextureFormat = RHI::Format::R8G8B8A8Unorm;
            description.Usage = static_cast<RHI::TextureUsage>(
                static_cast<u32>(RHI::TextureUsage::RenderTarget)
                | static_cast<u32>(RHI::TextureUsage::ShaderResource)
                | static_cast<u32>(RHI::TextureUsage::CopySource));
            description.InitialState = RHI::ResourceState::Common;
            m_ToneMappedColor = m_RHIDevice->CreateTexture(description);
            return m_ToneMappedColor != nullptr;
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

            const ApplicationCommandLineArgs& args = Application::Get().GetSpecification().CommandLineArgs;
            const bool traceFrame = args.HasFlag("--renderer-frame-trace");
            auto stageStart = traceFrame ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
            const auto recordStage = [&](const char* name)
            {
                if (!traceFrame)
                    return;
                Renderer::RecordCpuPassTiming(name,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stageStart).count());
                stageStart = std::chrono::steady_clock::now();
            };

            const SubmittedRenderGraphFrameOwner::PollResult retirement = m_SubmittedGraphFrames.Poll(*m_RHIDevice);
            if (!retirement.Success)
            {
                Log::Error("D3D12 Scene viewport RenderGraph retirement failed: ", retirement.Error);
                return false;
            }
            for (const SubmittedRenderGraphFrameOwner::RetiredFrame& frame : retirement.Retired)
            {
                for (const RHI::CompletionToken& completion : frame.Completions)
                {
                    if (m_TextureRuntime && m_TextureRuntime->HasRetainedFrame(completion))
                    {
                        std::string textureRetirementError;
                        if (!m_TextureRuntime->Retire(completion, textureRetirementError))
                        {
                            Log::Error("D3D12 Scene material texture retirement failed: ", textureRetirementError);
                            return false;
                        }
                    }
                }
                if (!frame.TimestampScopes.empty() && !Renderer::PublishRenderGraphTimestampScopes(frame.TimestampScopes))
                    return false;
            }
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

            recordStage("D3D12 Viewport Retirement");

            PollShaderHotReload();
            // Source observation establishes the requested revision before a completed
            // worker result is allowed to reach the native pipeline boundary. This
            // prevents an older completion from briefly replacing newer source intent.
            PollShaderCompilation();
            recordStage("D3D12 Viewport Shader Poll");

            if (!EnsureHdrOutput(width, height))
                return false;

            RHI::ResourceState hdrColorState = RHI::ResourceState::Unknown;
            RHI::ResourceState colorState = RHI::ResourceState::Unknown;
            RHI::ResourceState depthState = RHI::ResourceState::Unknown;
            RHI::ResourceState shadowDepthState = RHI::ResourceState::Unknown;
            if (!m_RHIDevice->QueryResourceState(m_HdrColor.get(), hdrColorState)
                || !m_RHIDevice->QueryResourceState(&colorTexture, colorState)
                || !m_RHIDevice->QueryResourceState(&depthTexture, depthState)
                || !m_RHIDevice->QueryResourceState(m_ShadowDepth.get(), shadowDepthState))
                return false;

            const RendererColorPipelineSettings colorSettings =
                Renderer::GetColorPipelineSettings();
            SceneRasterFrame rasterFrame;
            Ref<ConstantBufferSet> constantBufferSet;
            std::vector<ConstantBufferAllocation>* constantBuffers = nullptr;
            std::vector<SceneMeshDraw> draws;
            std::vector<SceneMeshDraw> shadowDraws;
            std::vector<RHI::TextureBindingHandle> usedTextureHandles;
            Ref<SceneLightPayloadSlot> lightPayload;
            SceneDebugOverlayFrame debugOverlayFrame;
            Ref<SceneShadowMapFrame> shadowFrame = CreateRef<SceneShadowMapFrame>();
            shadowFrame->Resolution = kSceneShadowMapResolution;
            const std::shared_ptr<const SceneRenderSnapshot> snapshot = Renderer::GetSceneRenderSnapshot();
            const std::shared_ptr<const SceneRasterFrame> prepared = Renderer::GetPreparedSceneRasterFrame();
            if (snapshot)
            {
                if (!prepared || prepared->SnapshotFrameIndex != snapshot->FrameIndex
                    || !prepared->ArtifactResolvers)
                    return false;
                rasterFrame = *prepared;
                std::string lightGridError;
                if (rasterFrame.HasValidView && !BuildClusteredLightGrid(
                    *snapshot, 0, width, height, {}, rasterFrame.LightGrid, lightGridError))
                {
                    Log::Error("D3D12 Scene viewport could not build clustered light grid: ", lightGridError);
                    return false;
                }
                std::string lightPayloadError;
                if (rasterFrame.HasValidView && !m_LightPayloadPublication.Acquire(*m_RHIDevice,
                    *snapshot, 0, rasterFrame.LightGrid,
                    colorSettings,
                    m_LightPayloadPublication.GetLastAcceptedGeneration() + 1,
                    lightPayload, lightPayloadError))
                {
                    Log::Error("D3D12 Scene viewport could not publish scene light payload: ", lightPayloadError);
                    return false;
                }
            }
            ScenePreExposureState preExposure;
            if (lightPayload && lightPayload->Payload)
            {
                if (lightPayload->Payload->ColorSettings != colorSettings)
                    return false;
                preExposure = lightPayload->Payload->PreExposure;
            }
            else if (!TryResolveScenePreExposure(colorSettings, preExposure))
            {
                return false;
            }
            Math::Vec3 preExposedClear;
            if (!TryPreExposeSceneLinear(
                    { clearColor.R, clearColor.G, clearColor.B },
                    preExposure, preExposedClear)
                || !std::isfinite(clearColor.A))
                return false;
            RHI::ViewportClear clear;
            clear.Color[0] = preExposedClear.X;
            clear.Color[1] = preExposedClear.Y;
            clear.Color[2] = preExposedClear.Z;
            clear.Color[3] = std::clamp(clearColor.A, 0.0f, 1.0f);
            recordStage("D3D12 Viewport Clustered Light Grid");
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
            if (m_Pipeline && m_ShadowPipeline
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
                    std::vector<SceneSurfaceConstants> cpuConstants(
                        rasterFrame.Instances.size());
                    for (size_t index = 0; index < rasterFrame.Instances.size(); ++index)
                    {
                        MaterialTextureBindingSet materialBindings;
                        std::string materialError;
                        if (rasterFrame.Instances[index].MaterialId >= rasterFrame.MaterialRows.size())
                        {
                            renderSucceeded = false;
                            break;
                        }
                        const SceneMaterialRow& materialRow = rasterFrame.MaterialRows[rasterFrame.Instances[index].MaterialId];
                        if (materialRow.IsError)
                        {
                            materialBindings.Material = materialRow.Material;
                            materialBindings.Handles.fill(m_TextureRuntime->GetErrorHandle());
                            materialBindings.CatalogGeneration = materialRow.CatalogGeneration;
                        }
                        else if (!m_TextureRuntime->ResolveMaterialTextures(
                            *rasterFrame.ArtifactResolvers,
                            rasterFrame.Instances[index].MaterialAsset,
                            materialBindings, materialError))
                        {
                            Log::Error("D3D12 Scene viewport could not resolve snapshot material: ", materialError);
                            renderSucceeded = false;
                            break;
                        }
                        if (materialBindings.CatalogGeneration != rasterFrame.MaterialCatalogGeneration)
                        {
                            renderSucceeded = false;
                            break;
                        }
                        if (!materialError.empty())
                            Log::Warn("D3D12 Scene material uses error resources: ", materialError);
                        for (size_t slot = 0; slot < materialBindings.Handles.size(); ++slot)
                            if ((materialBindings.DeclaredMask & (1u << static_cast<u32>(slot))) != 0
                                && (materialBindings.ErrorMask & (1u << static_cast<u32>(slot))) == 0)
                                usedTextureHandles.push_back(materialBindings.Handles[slot]);
                        if (!TryBuildSceneSurfaceConstants(rasterFrame.Instances[index],
                            materialBindings, materialRow.IsError, cpuConstants[index]))
                        {
                            Log::Error("D3D12 Scene viewport rejected nonfinite material or surface constants");
                            renderSucceeded = false;
                            break;
                        }
                    }
                    if (!renderSucceeded)
                        return false;
                    std::vector<SceneObjectBounds> objectBounds;
                    objectBounds.reserve(rasterFrame.Instances.size());
                    std::string meshError;
                    for (size_t index = 0; index < rasterFrame.Instances.size(); ++index)
                    {
                        MeshArtifact artifact;
                        if (!Renderer::ResolvePublishedMeshArtifact(
                            *rasterFrame.ArtifactResolvers,
                            rasterFrame.Instances[index].MeshAsset, artifact,
                            meshError))
                        {
                            Log::Error("D3D12 Scene viewport could not resolve snapshot mesh artifact: ", meshError);
                            renderSucceeded = false;
                            break;
                        }
                        SceneObjectBounds bounds;
                        if (!TryCalculateObjectBounds(artifact, bounds))
                        {
                            Log::Error("D3D12 Scene viewport could not calculate finite mesh bounds");
                            renderSucceeded = false;
                            break;
                        }
                        objectBounds.push_back(bounds);
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
                    if (!renderSucceeded)
                        return false;
                    std::string debugOverlayError;
                    if (!TryPrepareSceneDebugOverlay(rasterFrame, objectBounds,
                        width, height, debugOverlayFrame, debugOverlayError))
                    {
                        Log::Error("D3D12 Scene viewport could not prepare debug overlay: ",
                            debugOverlayError);
                        return false;
                    }
                    std::string shadowError;
                    if (!TryPrepareSceneShadowMap(rasterFrame, objectBounds,
                        kSceneShadowMapResolution, *shadowFrame, shadowError))
                    {
                        Log::Error("D3D12 Scene viewport could not prepare its primary shadow map: ",
                            shadowError);
                        return false;
                    }
                    std::vector<SceneShadowCasterMode> casterModes(
                        rasterFrame.Instances.size(), SceneShadowCasterMode::Excluded);
                    for (const SceneShadowCaster& caster : shadowFrame->Casters)
                    {
                        if (caster.InstanceIndex >= casterModes.size())
                            return false;
                        casterModes[caster.InstanceIndex] = caster.Mode;
                    }
                    for (size_t index = 0; index < rasterFrame.Instances.size(); ++index)
                    {
                        if (!TryApplySceneShadowMapConstants(rasterFrame.Instances[index],
                            *shadowFrame, casterModes[index], cpuConstants[index]))
                        {
                            Log::Error("D3D12 Scene viewport rejected shadow constants");
                            return false;
                        }
                        if (!TryApplySceneSkyIrradianceConstants(
                            rasterFrame.SkyAtmosphere, cpuConstants[index]))
                        {
                            Log::Error("D3D12 Scene viewport rejected sky irradiance constants");
                            return false;
                        }
                        if (!TryApplySceneDebugVisualizationConstants(
                            rasterFrame.Instances[index],
                            rasterFrame.DebugVisualization,
                            cpuConstants[index]))
                        {
                            Log::Error("D3D12 Scene viewport rejected debug visualization constants");
                            return false;
                        }
                        std::memcpy((*constantBuffers)[index].Mapped, &cpuConstants[index],
                            sizeof(cpuConstants[index]));
                    }
                    shadowDraws.reserve(draws.size());
                    for (const SceneMeshDraw& draw : draws)
                        if (draw.ConstantIndex < casterModes.size()
                            && casterModes[draw.ConstantIndex]
                                != SceneShadowCasterMode::Excluded)
                            shadowDraws.push_back(draw);
                }
            }
            if (!renderSucceeded)
                return false;

            recordStage("D3D12 Viewport Scene Resolve");
            Ref<ToneMapPassConstants> toneMapConstants = m_ToneMap.AcquireConstants(colorSettings);
            if (!toneMapConstants)
                return false;
            std::string skyConstantsError;
            Ref<SkyAtmospherePassConstants> skyConstants =
                m_SkyAtmosphere.AcquireConstants(Application::Get().GetFrameIndex(),
                    rasterFrame.SkyAtmosphere, preExposure.Scale,
                    skyConstantsError);
            if (!skyConstants)
            {
                Log::Error("D3D12 Scene viewport could not acquire sky constants: ",
                    skyConstantsError);
                return false;
            }
            Ref<SceneDebugOverlayPassConstants> debugOverlayConstants;
            RHI::ResourceState toneMappedColorState = RHI::ResourceState::Unknown;
            std::string debugOverlayError;
            if (debugOverlayFrame.HasPostToneMapOverlay())
            {
                if (!EnsureDebugOverlayOutput(width, height)
                    || !m_RHIDevice->QueryResourceState(m_ToneMappedColor.get(),
                        toneMappedColorState))
                {
                    Log::Error("D3D12 Scene viewport could not allocate its debug overlay intermediate");
                    return false;
                }
                debugOverlayConstants = m_DebugOverlay.AcquireConstants(
                    Application::Get().GetFrameIndex(), debugOverlayFrame,
                    debugOverlayError);
                if (!debugOverlayConstants)
                {
                    Log::Error("D3D12 Scene viewport could not acquire debug overlay constants: ",
                        debugOverlayError);
                    return false;
                }
            }

            Scope<RenderGraph> graph = CreateScope<RenderGraph>();
            RHI::TextureDescription hdrColorDescription = m_HdrColor->GetDescription();
            hdrColorDescription.InitialState = hdrColorState;
            RHI::TextureDescription colorDescription = colorTexture.GetDescription();
            colorDescription.InitialState = colorState;
            RHI::TextureDescription depthDescription = depthTexture.GetDescription();
            depthDescription.InitialState = depthState;
            RHI::TextureDescription shadowDepthDescription = m_ShadowDepth->GetDescription();
            shadowDepthDescription.InitialState = shadowDepthState;
            const RenderGraph::ResourceHandle hdrColor = graph->AddTexture(hdrColorDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle color = graph->AddTexture(colorDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle depth = graph->AddTexture(depthDescription, RenderGraph::ResourceLifetimeKind::Imported);
            const RenderGraph::ResourceHandle shadowDepth = graph->AddTexture(
                shadowDepthDescription, RenderGraph::ResourceLifetimeKind::Imported);
            RenderGraph::ResourceHandle toneMappedColor;
            if (debugOverlayConstants)
            {
                RHI::TextureDescription toneMappedColorDescription =
                    m_ToneMappedColor->GetDescription();
                toneMappedColorDescription.InitialState = toneMappedColorState;
                toneMappedColor = graph->AddTexture(toneMappedColorDescription,
                    RenderGraph::ResourceLifetimeKind::Imported);
            }
            RenderGraph::ResourceHandle lightStaging;
            RenderGraph::ResourceHandle lightGpu;
            if (lightPayload)
            {
                RHI::ResourceState lightStagingState = RHI::ResourceState::Unknown;
                RHI::ResourceState lightGpuState = RHI::ResourceState::Unknown;
                if (!m_RHIDevice->QueryResourceState(lightPayload->Staging.get(), lightStagingState)
                    || !m_RHIDevice->QueryResourceState(lightPayload->Gpu.get(), lightGpuState))
                    return false;
                RHI::BufferDescription lightStagingDescription = lightPayload->Staging->GetDescription();
                lightStagingDescription.InitialState = lightStagingState;
                RHI::BufferDescription lightGpuDescription = lightPayload->Gpu->GetDescription();
                lightGpuDescription.InitialState = lightGpuState;
                lightStaging = graph->AddBuffer(lightStagingDescription,
                    RenderGraph::ResourceLifetimeKind::Imported);
                lightGpu = graph->AddBuffer(lightGpuDescription,
                    RenderGraph::ResourceLifetimeKind::Imported);
                const RenderGraph::PassHandle lightCopyPass = graph->AddPass(
                    "Scene Light Payload Copy", RHI::QueueType::Graphics);
                graph->AddRead(lightCopyPass, lightStaging, RHI::ResourceState::CopySource);
                graph->AddWrite(lightCopyPass, lightGpu, RHI::ResourceState::CopyDest);
                graph->SetPassCallback(lightCopyPass,
                    [lightStaging, lightGpu](RenderGraph::ExecutionContext& context)
                    {
                        RHI::Buffer* staging = context.GetBuffer(lightStaging);
                        RHI::Buffer* gpu = context.GetBuffer(lightGpu);
                        return staging && gpu && context.GetCommandList().CopyBuffer(
                            *gpu, 0, *staging, 0, gpu->GetDescription().SizeBytes);
                    });
            }
            const Ref<RHI::Pipeline> activePipeline = m_Pipeline;
            const Ref<RHI::Pipeline> activeShadowPipeline = m_ShadowPipeline;
            RHI::TextureBindingTable* textureTable = m_TextureRuntime->GetBindingTable();
            const RenderGraph::PassHandle shadowPass = graph->AddPass(
                "Scene Primary Directional Shadow Map", RHI::QueueType::Graphics);
            graph->AddWrite(shadowPass, shadowDepth, RHI::ResourceState::DepthWrite);
            graph->SetPassCallback(shadowPass,
                [activeShadowPipeline, textureTable, shadowDepth,
                    constantBuffers, shadowDraws](RenderGraph::ExecutionContext& context)
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
                if (!activeShadowPipeline || !constantBuffers || shadowDraws.empty())
                    return true;
                commands.SetGraphicsPipeline(*activeShadowPipeline);
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
                        *(*constantBuffers)[draw.ConstantIndex].Buffer);
                    commands.DrawIndexed(draw.Primitive.IndexCount, 1,
                        draw.Primitive.FirstIndex, draw.Primitive.BaseVertex, 0);
                }
                return true;
            });
            const RenderGraph::PassHandle clearPass = graph->AddPass("Scene Viewport Graph Clear", RHI::QueueType::Graphics);
            graph->AddWrite(clearPass, hdrColor, RHI::ResourceState::RenderTarget);
            graph->AddWrite(clearPass, depth, RHI::ResourceState::DepthWrite);
            graph->SetPassCallback(clearPass, [hdrColor, depth, clear](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphColor = context.GetTexture(hdrColor);
                RHI::Texture* graphDepth = context.GetTexture(depth);
                return graphColor && graphDepth && context.GetCommandList().BindViewportOutputs(*graphColor, graphDepth)
                    && context.GetCommandList().ClearViewportOutputs(clear);
            });
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
            graph->AddWrite(rasterPass, hdrColor, RHI::ResourceState::RenderTarget);
            graph->AddWrite(rasterPass, depth, RHI::ResourceState::DepthWrite);
            if (lightPayload)
                graph->AddRead(rasterPass, lightGpu, RHI::ResourceState::ShaderResource,
                    RHI::ShaderStage::Pixel);
            graph->AddRead(rasterPass, shadowDepth, RHI::ResourceState::ShaderResource,
                RHI::ShaderStage::Pixel);
            graph->SetPassCallback(rasterPass, [activePipeline, textureTable, lightPayload,
                hdrColor, depth, shadowDepth, lightGpu, width, height,
                &rasterFrame, constantBuffers, draws](RenderGraph::ExecutionContext& context)
            {
                RHI::Texture* graphColor = context.GetTexture(hdrColor);
                RHI::Texture* graphDepth = context.GetTexture(depth);
                RHI::Texture* graphShadow = context.GetTexture(shadowDepth);
                RHI::CommandList& commands = context.GetCommandList();
                if (!graphColor || !graphDepth || !commands.BindViewportOutputs(*graphColor, graphDepth)) return false;
                if (!activePipeline || !rasterFrame.HasValidView || rasterFrame.Instances.empty()) return true;
                RHI::Buffer* graphLightPayload = lightPayload
                    ? context.GetBuffer(lightGpu) : nullptr;
                commands.SetGraphicsPipeline(*activePipeline);
                if (!textureTable || !graphLightPayload || !graphShadow
                    || !commands.BindGraphicsSampledTextureTable(*textureTable)
                    || !commands.BindGraphicsSampledTexture(*graphShadow)
                    || !commands.BindGraphicsReadOnlyStructuredBuffer(*graphLightPayload)) return false;
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
            recordStage("D3D12 Viewport Graph Build And Compile");
            RenderGraph::ExecuteOptions executeOptions;
            executeOptions.RecordingMode = args.HasFlag("--frame-task-single-thread")
                ? FrameTaskExecutionMode::DeterministicSingleThread : FrameTaskExecutionMode::Parallel;
            const bool timestampCaptureRequested = args.HasFlag("--renderer-gpu-timestamps")
                || args.HasFlag("--scene-viewport-render-graph-smoke") || args.HasFlag("--frame-pacing-benchmark");
            executeOptions.EnableTimestampScopes = timestampCaptureRequested
                && !args.HasFlag("--renderer-disable-gpu-timestamps")
                && m_RHIDevice->GetCapabilities().GetFeature(RHI::DeviceFeature::Timestamps).IsUsable();
            const bool resourcesBound = graph->BindTexture(hdrColor, *m_HdrColor)
                && graph->BindTexture(color, colorTexture)
                && graph->BindTexture(depth, depthTexture)
                && graph->BindTexture(shadowDepth, *m_ShadowDepth)
                && (!debugOverlayConstants || graph->BindTexture(
                    toneMappedColor, *m_ToneMappedColor))
                && (!lightPayload || (graph->BindBuffer(lightStaging, *lightPayload->Staging)
                    && graph->BindBuffer(lightGpu, *lightPayload->Gpu)));
            const RenderGraph::ExecuteResult executed = resourcesBound
                ? graph->Execute(*m_RHIDevice, compiled, executeOptions)
                : RenderGraph::ExecuteResult {};
            recordStage("D3D12 Viewport Graph Execute");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")) Log::Info("RenderGraphRecordingV1 backend=D3D12 mode=", executeOptions.RecordingMode == FrameTaskExecutionMode::Parallel ? "worker" : "inline", " workerPasses=", executed.WorkerRecordedPassCount, " overlap=", executed.WorkerRecordingOverlapObserved ? "yes" : "no", " submitted=", executed.AcceptedPassCount, " result=", executed.Success ? "pass" : "fail");
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
                    {
                        Log::Error("D3D12 Scene viewport could not retain material textures: ", textureRetentionError);
                        m_RHIDevice->WaitIdle();
                        return false;
                    }
                }
                std::vector<Ref<void>> payloads;
                if (constantBufferSet)
                    payloads.emplace_back(constantBufferSet);
                payloads.emplace_back(skyConstants);
                payloads.emplace_back(toneMapConstants);
                if (debugOverlayConstants)
                    payloads.emplace_back(debugOverlayConstants);
                if (lightPayload)
                    payloads.emplace_back(lightPayload);
                payloads.emplace_back(shadowFrame);
                // The graph may be retired asynchronously. Keep the exact pipeline
                // selected while recording alive until its accepted GPU work retires.
                if (activePipeline)
                    payloads.emplace_back(activePipeline);
                if (activeShadowPipeline)
                    payloads.emplace_back(activeShadowPipeline);
                for (const SceneMeshDraw& draw : draws)
                    payloads.emplace_back(std::const_pointer_cast<MeshGpuResourceBundle>(draw.Bundle));
                std::string retentionError;
                if (!m_SubmittedGraphFrames.Retain(Application::Get().GetFrameIndex(), std::move(graph), compiled,
                    executed, std::move(payloads), &retentionError))
                {
                    Log::Error("D3D12 Scene viewport could not retain an accepted RenderGraph submission: ", retentionError);
                    m_RHIDevice->WaitIdle();
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
            if (!executed.Success)
                return false;
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")
                && m_Pipeline && rasterFrame.HasValidView && !rasterFrame.Instances.empty() && !draws.empty())
                Log::Info("SceneMeshGpuIntegrationV1 backend=D3D12 snapshot=pass resolver=pass cache=pass indexFormat=UInt32 baseVertex=0 instances=", rasterFrame.Instances.size(),
                    " draws=", draws.size(), " constants=per-instance retained=gpu-retirement result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke")
                && !usedTextureHandles.empty())
                Log::Info("SceneMaterialTextureIntegrationV1 backend=D3D12 material=immutable texture=sRGB-base-color sampler=declared table=bound mips=implicit fallbacks=semantic retained=exact-raster-token result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke"))
                Log::Info("ProductionRenderGraphRetirementV1 backend=D3D12 frame=", Application::Get().GetFrameIndex(),
                    " passes=", executed.AcceptedPassCount, " cpuWaitBetween=no pending=", m_SubmittedGraphFrames.GetPendingCount(), " result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke"))
                Log::Info("ClusteredLightGridV1 backend=D3D12 tiles=", rasterFrame.LightGrid.TileCountX,
                    "x", rasterFrame.LightGrid.TileCountY, " depthSlices=", rasterFrame.LightGrid.DepthSliceCount,
                    " lights=", rasterFrame.LightGrid.Lights.size(), " global=", rasterFrame.LightGrid.GlobalLightIndices.size(),
                    " localReferences=", rasterFrame.LightGrid.LocalLightIndices.size(),
                    " overflow=", rasterFrame.LightGrid.OverflowedLocalLightReferences,
                    " storage=bounded-csr result=pass");
            if (Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-viewport-render-graph-smoke"))
                Log::Info("SceneShadowMapV1 backend=D3D12 light=", shadowFrame->PrimaryLightEntity,
                    " resolution=", shadowFrame->Resolution,
                    " stabilization=texel-snapped filter=3x3-pcf casters=", shadowFrame->Casters.size(),
                    " opaque=", shadowFrame->OpaqueCasterCount,
                    " masked=", shadowFrame->AlphaTestedCasterCount,
                    " conservativeError=", shadowFrame->ConservativeErrorCasterCount,
                    " componentExcluded=", shadowFrame->ComponentExcludedCount,
                    " blendExcluded=", shadowFrame->BlendExcludedCount,
                    " receiverExclusions=deferred graphLabel=Scene-Primary-Directional-Shadow-Map result=pass");
            if (args.HasFlag("--scene-viewport-render-graph-smoke"))
                Log::Info("SceneSkyAtmosphereV1 backend=D3D12 model=Preetham1999 enabled=",
                    rasterFrame.SkyAtmosphere.Enabled ? "yes" : "no", " sunEntity=",
                    rasterFrame.SkyAtmosphere.SunEntity, " sunLightIndex=",
                    rasterFrame.SkyAtmosphere.SunLightIndex, " turbidity=",
                    rasterFrame.SkyAtmosphere.Turbidity, " groundAlbedo=",
                    rasterFrame.SkyAtmosphere.GroundAlbedo, " angularRadiusDegrees=",
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
                    Log::Error("SceneViewportRenderGraphV1 backend=D3D12 passes=",
                        compiled.Passes.size(), " labels=", actualPassNames,
                        " execution=fail reference=direct comparator=not-run accepted=",
                        executed.AcceptedPassCount, " expectedPasses=",
                        expectedPassNames.size());
                    return false;
                }
                RHI::TextureDescription referenceColorDescription = colorTexture.GetDescription();
                referenceColorDescription.DebugName = "Scene Viewport Bootstrap Reference Color";
                RHI::TextureDescription referenceHdrDescription = m_HdrColor->GetDescription();
                referenceHdrDescription.DebugName = "Scene Viewport Bootstrap Reference Linear HDR";
                RHI::TextureDescription referenceDepthDescription = depthTexture.GetDescription();
                referenceDepthDescription.DebugName = "Scene Viewport Bootstrap Reference Depth";
                RHI::TextureDescription referenceShadowDescription = m_ShadowDepth->GetDescription();
                referenceShadowDescription.DebugName = "Scene Viewport Bootstrap Reference Shadow Depth";
                Scope<RHI::Texture> referenceHdr = m_RHIDevice->CreateTexture(referenceHdrDescription);
                Scope<RHI::Texture> referenceColor = m_RHIDevice->CreateTexture(referenceColorDescription);
                Scope<RHI::Texture> referenceDepth = m_RHIDevice->CreateTexture(referenceDepthDescription);
                Scope<RHI::Texture> referenceShadow = m_RHIDevice->CreateTexture(
                    referenceShadowDescription);
                Scope<RHI::Texture> referenceToneMapped;
                if (debugOverlayConstants)
                {
                    RHI::TextureDescription referenceToneMappedDescription =
                        m_ToneMappedColor->GetDescription();
                    referenceToneMappedDescription.DebugName =
                        "Scene Viewport Bootstrap Reference Tone-Mapped Intermediate";
                    referenceToneMapped = m_RHIDevice->CreateTexture(
                        referenceToneMappedDescription);
                }
                RHI::TextureReadback graphReadback, referenceReadback;
                const bool referenceRendered = referenceHdr && referenceColor
                    && referenceDepth && referenceShadow
                    && (!debugOverlayConstants || referenceToneMapped)
                    && RecordBootstrapReference(
                        *referenceHdr, *referenceColor, referenceToneMapped.get(),
                        *referenceDepth, width, height,
                        clear, rasterFrame, constantBuffers, draws, shadowDraws,
                        lightPayload, *referenceShadow, *skyConstants,
                        *toneMapConstants, debugOverlayConstants);
                const bool readBack = referenceRendered && ReadbackGraphOutput(colorTexture, graphReadback)
                    && m_RHIDevice->ReadbackTexture(*referenceColor, referenceReadback);
                const bool equivalent = readBack && graphReadback.Extent.Width == referenceReadback.Extent.Width
                    && graphReadback.Extent.Height == referenceReadback.Extent.Height
                    && graphReadback.RowPitchBytes == referenceReadback.RowPitchBytes
                    && graphReadback.Data == referenceReadback.Data;
                Log::Info("SceneViewportRenderGraphV1 backend=D3D12 passes=",
                    compiled.Passes.size(),
                    " labels=light-payload-copy,primary-directional-shadow-map,clear,sky-atmosphere,raster,tone-map,",
                    debugOverlayConstants ? "debug-overlay," : "",
                    "output-handoff execution=pass reference=direct comparator=exact-byte-",
                    equivalent ? "pass" : "fail", " size=", width, "x", height,
                    " bytes=", graphReadback.Data.size());
                Log::Info("SceneColorPipelineV2 backend=D3D12 sceneLinear=pre-exposed-finite-RGBA16F exposurePlacement=before-storage toneMapExposure=none finiteClamp=65504 manualExposureEV100=", colorSettings.ManualExposureEV100,
                    " exposureMode=", ToString(colorSettings.ExposureMode),
                    " effectiveExposureEV100=", EffectiveExposureEV100(colorSettings),
                    " exposureScale=", ManualExposureScale(colorSettings),
                    " toneMap=Khronos-PBR-Neutral postToneMapSaturation=", colorSettings.PostToneMapSaturation,
                    " postToneMapContrast=", colorSettings.PostToneMapContrast,
                    " output=sRGB-encoded-RGBA8 result=", equivalent ? "pass" : "fail");
                if (!equivalent) return false;
            }
            std::string lightPayloadCommitError;
            if (lightPayload
                && !m_LightPayloadPublication.Commit(lightPayload, lightPayloadCommitError))
            {
                Log::Error("D3D12 Scene viewport could not commit its accepted light payload: ",
                    lightPayloadCommitError);
                return false;
            }
            Renderer::PublishSceneRasterFrame(std::move(rasterFrame));
            if (!comparisonRequested && args.HasFlag("--renderer-frame-trace") && !args.HasFlag("--frame-pacing-benchmark"))
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
            return m_VertexRequest.IsValid() && m_PixelRequest.IsValid()
                && m_ShadowVertexRequest.IsValid() && m_ShadowPixelRequest.IsValid();
        }

        PortableShaderRequest MakeShaderRequest(RHI::ShaderStage stage,
            const char* entryPoint, bool lightPayload, bool shadowReceiver) const
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
            request.Options = { "-O3" };
            request.Defines = {
                "GE_READ_ONLY_TEXTURE_CAPACITY=" + std::to_string(m_TextureTableCapacity),
                "GE_SCENE_LIGHT_PAYLOAD=" + std::to_string(lightPayload ? 1 : 0),
                "GE_SCENE_SHADOW_MAP=" + std::to_string(shadowReceiver ? 1 : 0)
            };
            request.ExpectedLayout = {
                { "ViewportConstants", 'b', 0, 0, stage, "ConstantBuffer", "struct{ViewProjection:float32x4x4:row-major@0,NormalTransform:float32x4x4:row-major@64,BaseColorAndAlphaCutoff:float32x4@128,EmissiveAndStrength:float32x4@144,SurfaceFactors:float32x4@160,CallistoFactors:float32x4@176,TextureIndices0:uint32x4@192,TextureIndices1:uint32x4@208,TextureState:uint32x4@224,MaterialState:uint32x4@240,ModelView:float32x4x4:row-major@256,NormalViewTransform:float32x4x4:row-major@320,ShadowViewProjection:float32x4x4:row-major@384,ShadowParameters:float32x4@448,ShadowState:uint32x4@464,SkyIrradianceUpper:float32x4@480,SkyIrradianceLower:float32x4@496}", 1, 512, 0, 0 },
                { "ReadOnlySamplers", 's', 0, 1, stage, "SamplerState", "sampler", m_TextureTableCapacity, 0, 0, 0 },
                { "ReadOnlyTextures", 't', 0, 1, stage, "Texture2D", "float32x4", m_TextureTableCapacity, 0, 1, 4 }
            };
            if (shadowReceiver)
            {
                request.ExpectedLayout.push_back({ "SceneShadowSampler", 's', 0, 2, RHI::ShaderStage::Pixel, "SamplerState", "sampler", 1, 0, 0, 0 });
                request.ExpectedLayout.push_back({ "SceneShadowDepth", 't', 0, 2, RHI::ShaderStage::Pixel, "Texture2D", "float32", 1, 0, 1, 1 });
            }
            if (lightPayload) request.ExpectedLayout.push_back({ "SceneLightPayload", 't', 0, 3, RHI::ShaderStage::Pixel, "StructuredBuffer", "uint32x4", 1, 0, 1, 4 });
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
            const PortableShaderRequest vertexRequest = MakeShaderRequest(
                RHI::ShaderStage::Vertex, "VSMain", false, false);
            const PortableShaderRequest pixelRequest = MakeShaderRequest(
                RHI::ShaderStage::Pixel, "PSMain", true, true);
            const PortableShaderRequest shadowVertexRequest = MakeShaderRequest(
                RHI::ShaderStage::Vertex, "VSShadowCaster", false, false);
            const PortableShaderRequest shadowPixelRequest = MakeShaderRequest(
                RHI::ShaderStage::Pixel, "PSShadowCaster", false, false);
            m_VertexRequest = m_ShaderPackages->Request(vertexRequest);
            m_PixelRequest = m_ShaderPackages->Request(pixelRequest);
            m_ShadowVertexRequest = m_ShaderPackages->Request(shadowVertexRequest);
            m_ShadowPixelRequest = m_ShaderPackages->Request(shadowPixelRequest);
            m_LastProcessedVertexRequest = 0;
            m_LastProcessedPixelRequest = 0;
            m_LastProcessedShadowVertexRequest = 0;
            m_LastProcessedShadowPixelRequest = 0;
            m_ReloadTicket = m_ReloadCoordinator.Request(m_ShaderSource.Revision);
            if (!m_ReloadTicket.IsValid()) return;
            m_ShaderPipelineTerminalFailure = false;
            Log::Info("PortableShaderRequestV1 status=pending request=", m_VertexRequest.Id,
                " stage=vertex cacheMode=pending cacheSource=none compiler=Slang-2026.13.1 backend=Slang",
                " targets=DXIL+SPIR-V key=", m_VertexRequest.Key, " legacySourceCompile=false");
            Log::Info("PortableShaderRequestV1 status=pending request=", m_PixelRequest.Id,
                " stage=pixel cacheMode=pending cacheSource=none compiler=Slang-2026.13.1 backend=Slang",
                " targets=DXIL+SPIR-V key=", m_PixelRequest.Key, " legacySourceCompile=false");
            Log::Info("PortableShaderRequestV1 status=pending request=", m_ShadowVertexRequest.Id,
                " stage=shadow-vertex cacheMode=pending cacheSource=none compiler=Slang-2026.13.1 backend=Slang",
                " targets=DXIL+SPIR-V key=", m_ShadowVertexRequest.Key, " legacySourceCompile=false");
            Log::Info("PortableShaderRequestV1 status=pending request=", m_ShadowPixelRequest.Id,
                " stage=shadow-pixel cacheMode=pending cacheSource=none compiler=Slang-2026.13.1 backend=Slang",
                " targets=DXIL+SPIR-V key=", m_ShadowPixelRequest.Key, " legacySourceCompile=false");
            if (!m_Pipeline)
            {
                Log::Warn("D3D12 scene raster unavailable: status=shader-pipeline-pending, vertexRequest=",
                    m_VertexRequest.Id, ", pixelRequest=", m_PixelRequest.Id,
                    ", shadowVertexRequest=", m_ShadowVertexRequest.Id,
                    ", shadowPixelRequest=", m_ShadowPixelRequest.Id,
                    ", clear-only presentation is not a successful scene raster");
            }
        }

        void PollShaderCompilation()
        {
            if (!m_ShaderPackages || !m_VertexRequest.IsValid() || !m_PixelRequest.IsValid()
                || !m_ShadowVertexRequest.IsValid() || !m_ShadowPixelRequest.IsValid())
                return;
            if (m_LastProcessedVertexRequest == m_VertexRequest.Id
                && m_LastProcessedPixelRequest == m_PixelRequest.Id
                && m_LastProcessedShadowVertexRequest == m_ShadowVertexRequest.Id
                && m_LastProcessedShadowPixelRequest == m_ShadowPixelRequest.Id)
                return;

            const ShaderPackageRequestResult vertex = m_ShaderPackages->Poll(m_VertexRequest);
            const ShaderPackageRequestResult pixel = m_ShaderPackages->Poll(m_PixelRequest);
            const ShaderPackageRequestResult shadowVertex = m_ShaderPackages->Poll(m_ShadowVertexRequest);
            const ShaderPackageRequestResult shadowPixel = m_ShaderPackages->Poll(m_ShadowPixelRequest);
            if (!vertex.IsTerminal() || !pixel.IsTerminal()
                || !shadowVertex.IsTerminal() || !shadowPixel.IsTerminal())
                return;

            m_LastProcessedVertexRequest = m_VertexRequest.Id;
            m_LastProcessedPixelRequest = m_PixelRequest.Id;
            m_LastProcessedShadowVertexRequest = m_ShadowVertexRequest.Id;
            m_LastProcessedShadowPixelRequest = m_ShadowPixelRequest.Id;
            LogTerminalResult("vertex", vertex);
            LogTerminalResult("pixel", pixel);
            LogTerminalResult("shadow-vertex", shadowVertex);
            LogTerminalResult("shadow-pixel", shadowPixel);
            if (!m_ReloadCoordinator.IsCurrent(m_ReloadTicket))
            {
                Log::Info("D3D12LivePipelineRebuildV1 status=stale-rejected requestedRevision=", m_ReloadTicket.Revision,
                    " activeGeneration=", m_ReloadCoordinator.ActiveGeneration());
                return;
            }
            if (!vertex.Succeeded() || !pixel.Succeeded()
                || !shadowVertex.Succeeded() || !shadowPixel.Succeeded())
            {
                m_ReloadCoordinator.Publish(m_ReloadTicket, false);
                if (!m_Pipeline)
                    m_ShaderPipelineTerminalFailure = true;
                const ShaderPackageRequestDiagnostic& diagnostic = !vertex.Succeeded()
                    ? vertex.Diagnostic : !pixel.Succeeded() ? pixel.Diagnostic
                    : !shadowVertex.Succeeded() ? shadowVertex.Diagnostic
                    : shadowPixel.Diagnostic;
                Log::Error("Portable shader request: status=", AsyncShaderPackageService::ToString(diagnostic.Status),
                    ", request=", diagnostic.RequestId, ", source=", diagnostic.Source, ", entry=", diagnostic.EntryPoint,
                    ", targets=", diagnostic.Targets, ", backend=", diagnostic.Backend, ", key=", diagnostic.Key,
                    ", message=", diagnostic.Message, ". Last valid D3D12 viewport pipeline remains active.");
                return;
            }

            if (!BuildPipeline(*vertex.Package, *pixel.Package,
                *shadowVertex.Package, *shadowPixel.Package))
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

        bool BuildPipeline(const PortableShaderPackage& vertexPackage,
            const PortableShaderPackage& pixelPackage,
            const PortableShaderPackage& shadowVertexPackage,
            const PortableShaderPackage& shadowPixelPackage)
        {
            Scope<RHI::Shader> vertexShader;
            Scope<RHI::Shader> pixelShader;
            Scope<RHI::Shader> shadowVertexShader;
            Scope<RHI::Shader> shadowPixelShader;
            if (!CreateRhiShader(RHI::ShaderStage::Vertex, "VSMain", "Editor Viewport Vertex Shader",
                vertexPackage.Dxil, vertexPackage.Reflection, vertexShader))
                return false;
            if (!CreateRhiShader(RHI::ShaderStage::Pixel, "PSMain", "Editor Viewport Pixel Shader",
                pixelPackage.Dxil, pixelPackage.Reflection, pixelShader))
                return false;
            if (!CreateRhiShader(RHI::ShaderStage::Vertex, "VSShadowCaster",
                "Editor Primary Directional Shadow Vertex Shader",
                shadowVertexPackage.Dxil, shadowVertexPackage.Reflection,
                shadowVertexShader))
                return false;
            if (!CreateRhiShader(RHI::ShaderStage::Pixel, "PSShadowCaster",
                "Editor Primary Directional Shadow Pixel Shader",
                shadowPixelPackage.Dxil, shadowPixelPackage.Reflection,
                shadowPixelShader))
                return false;

            RHI::PipelineDescription pipelineDesc;
            pipelineDesc.DebugName = "Editor Viewport Scene Mesh Pipeline";
            pipelineDesc.Type = RHI::PipelineType::Graphics;
            pipelineDesc.VertexShader = vertexShader.get();
            pipelineDesc.PixelShader = pixelShader.get();
            pipelineDesc.VertexInputs = {
                { "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Position) },
                { "NORMAL", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Normal) },
                { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(MeshArtifactVertex, Color) },
                { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(MeshArtifactVertex, UV) }
            };
            pipelineDesc.VertexStrideBytes = sizeof(MeshArtifactVertex);
            pipelineDesc.ConstantBufferBindings = {
                { 0, 0, RHI::ShaderStage::AllGraphics }
            };
            pipelineDesc.SampledTextureTable = RHI::SampledTextureTableBinding {
                m_TextureTableCapacity };
            pipelineDesc.FixedSampledTexture = RHI::FixedSampledTextureBinding {};
            pipelineDesc.FixedSampledTexture->PointSampling = true;
            pipelineDesc.FixedReadOnlyStructuredBuffer = RHI::FixedReadOnlyStructuredBufferBinding {};
            pipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            pipelineDesc.RasterCullMode = RHI::CullMode::None;
            pipelineDesc.ColorFormat = RHI::Format::R16G16B16A16Float;
            pipelineDesc.DepthFormat = RHI::Format::D32Float;
            pipelineDesc.DepthTestEnable = true;
            pipelineDesc.DepthWriteEnable = true;

            Scope<RHI::Pipeline> pipeline = m_RHIDevice->CreatePipeline(pipelineDesc);
            if (!pipeline)
                return false;
            RHI::PipelineDescription shadowPipelineDesc = pipelineDesc;
            shadowPipelineDesc.DebugName = "Editor Primary Directional Shadow Pipeline";
            shadowPipelineDesc.VertexShader = shadowVertexShader.get();
            shadowPipelineDesc.PixelShader = shadowPixelShader.get();
            shadowPipelineDesc.FixedSampledTexture.reset();
            shadowPipelineDesc.FixedReadOnlyStructuredBuffer.reset();
            shadowPipelineDesc.ColorFormat = RHI::Format::Unknown;
            Scope<RHI::Pipeline> shadowPipeline = m_RHIDevice->CreatePipeline(
                shadowPipelineDesc);
            if (!shadowPipeline)
                return false;
            // Construct every replacement before publication. Ref ownership lets an
            // accepted RenderGraph frame retain the exact generation it recorded.
            Ref<RHI::Shader> replacementVertex(vertexShader.release());
            Ref<RHI::Shader> replacementPixel(pixelShader.release());
            Ref<RHI::Shader> replacementShadowVertex(shadowVertexShader.release());
            Ref<RHI::Shader> replacementShadowPixel(shadowPixelShader.release());
            Ref<RHI::Pipeline> replacementPipeline(pipeline.release());
            Ref<RHI::Pipeline> replacementShadowPipeline(shadowPipeline.release());
            m_VertexShader = std::move(replacementVertex);
            m_PixelShader = std::move(replacementPixel);
            m_ShadowVertexShader = std::move(replacementShadowVertex);
            m_ShadowPixelShader = std::move(replacementShadowPixel);
            m_Pipeline = std::move(replacementPipeline);
            m_ShadowPipeline = std::move(replacementShadowPipeline);
            return true;
        }

        bool CreateRhiShader(
            RHI::ShaderStage stage,
            const char* entryPoint,
            const char* debugName,
            const std::vector<u8>& dxil,
            const std::vector<RHI::ShaderReflectionBinding>& reflection,
            Scope<RHI::Shader>& shader)
        {
            RHI::ShaderDescription description;
            description.DebugName = debugName;
            description.SourceName = m_ShaderSource.ResolvedPath.string();
            description.EntryPoint = entryPoint;
            description.Stage = stage;
            description.BinaryFormat = RHI::ShaderBinaryFormat::Dxil;
            description.Binary = dxil;
            description.Reflection = reflection;

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
        u32 m_TextureTableCapacity = 0;
        Scope<TextureRuntimePublication> m_TextureRuntime;
        SkyAtmospherePass m_SkyAtmosphere;
        ToneMapPass m_ToneMap;
        SceneDebugOverlayPass m_DebugOverlay;
        Scope<RHI::Texture> m_HdrColor;
        Scope<RHI::Texture> m_ToneMappedColor;
        Scope<RHI::Texture> m_ShadowDepth;
        u32 m_HdrWidth = 0;
        u32 m_HdrHeight = 0;
        Ref<RHI::Pipeline> m_Pipeline;
        Ref<RHI::Pipeline> m_ShadowPipeline;
        Ref<RHI::Shader> m_VertexShader;
        Ref<RHI::Shader> m_PixelShader;
        Ref<RHI::Shader> m_ShadowVertexShader;
        Ref<RHI::Shader> m_ShadowPixelShader;
        std::vector<Ref<ConstantBufferSet>> m_FrameConstantBuffers;
        SubmittedRenderGraphFrameOwner m_SubmittedGraphFrames;
        SceneLightPayloadPublication m_LightPayloadPublication;
        ShaderSourceFile m_ShaderSource;
        Scope<AsyncShaderPackageService> m_ShaderPackages;
        ShaderPackageRequestHandle m_VertexRequest;
        ShaderPackageRequestHandle m_PixelRequest;
        ShaderPackageRequestHandle m_ShadowVertexRequest;
        ShaderPackageRequestHandle m_ShadowPixelRequest;
        u64 m_LastProcessedVertexRequest = 0;
        u64 m_LastProcessedPixelRequest = 0;
        u64 m_LastProcessedShadowVertexRequest = 0;
        u64 m_LastProcessedShadowPixelRequest = 0;
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
