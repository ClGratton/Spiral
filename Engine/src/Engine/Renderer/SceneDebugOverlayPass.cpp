#include "Engine/Renderer/SceneDebugOverlayPass.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>

namespace Engine
{
    namespace
    {
        struct FullscreenVertex
        {
            float Position[3];
            float Color[3];
            float UV[2];
        };

        static_assert(sizeof(FullscreenVertex) == 32);

        constexpr std::array<FullscreenVertex, 3> kFullscreenVertices {{
            {{ -1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 1.0f }},
            {{ -1.0f,  3.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, -1.0f }},
            {{  3.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, { 2.0f, 1.0f }}
        }};
        constexpr std::array<u32, 3> kFullscreenIndices { 0, 1, 2 };

        PortableShaderRequest MakeDebugOverlayRequest(
            const ShaderSourceFile& source, RHI::ShaderStage stage,
            const char* entryPoint)
        {
            PortableShaderRequest request;
            request.SourceName = source.ResolvedPath.string();
            request.Source = source.Source;
            request.EntryPoint = entryPoint;
            request.Stage = stage;
#ifdef _WIN32
            request.Targets = { PortableShaderTarget::Dxil,
                PortableShaderTarget::Spirv };
            request.DownstreamCompilerPackageHash = GE_DXC_PACKAGE_SHA256;
#else
            request.Targets = { PortableShaderTarget::Spirv };
#endif
            request.CompilerIdentity = "Slang";
            request.CompilerVersion = "2026.13.1";
            request.CompilerPackageHash = GE_SLANG_PACKAGE_SHA256;
            request.Options = { "-O3" };
            request.ExpectedLayout = {
                { "SceneDebugOverlayConstants", 'b', 0, 0, stage,
                    "ConstantBuffer",
                    "struct{Segment0:float32x4@0,Segment1:float32x4@16,Segment2:float32x4@32,Segment3:float32x4@48,Segment4:float32x4@64,Segment5:float32x4@80,Segment6:float32x4@96,Segment7:float32x4@112,Segment8:float32x4@128,Segment9:float32x4@144,Segment10:float32x4@160,Segment11:float32x4@176,SegmentDepth0:float32x4@192,SegmentDepth1:float32x4@208,SegmentDepth2:float32x4@224,SegmentDepth3:float32x4@240,SegmentDepth4:float32x4@256,SegmentDepth5:float32x4@272,OverlayColorAndOpacity:float32x4@288,OverlayState:float32x4@304,OccludedState:float32x4@320,DepthState:float32x4@336}",
                    1, 352, 0, 0 },
                { "SceneDepthSampler", 's', 0, 2, stage,
                    "SamplerState", "sampler", 1, 0, 0, 0 },
                { "SceneDepth", 't', 0, 2, stage,
                    "Texture2D", "float32", 1, 0, 1, 1 }
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

        bool InitializeBuffer(RHI::Device& device, std::string_view name,
            RHI::BufferUsage usage, const void* bytes, u64 byteCount,
            u32 stride, Scope<RHI::Buffer>& output)
        {
            RHI::BufferDescription description;
            description.DebugName = std::string(name);
            description.SizeBytes = byteCount;
            description.StrideBytes = stride;
            description.Usage = usage;
            description.CpuAccess = RHI::BufferCpuAccess::Write;
            output = device.CreateBuffer(description);
            void* mapped = output ? output->Map() : nullptr;
            if (!mapped)
                return false;
            std::memcpy(mapped, bytes, static_cast<size_t>(byteCount));
            output->Unmap();
            return true;
        }
    }

    bool TryBuildSceneDebugOverlayGpuConstants(
        const SceneDebugOverlayFrame& frame,
        SceneDebugOverlayGpuConstants& outConstants,
        std::string& outError)
    {
        outError.clear();
        if (!IsValidSceneDebugVisualizationSettings(frame.Settings)
            || !frame.HasPostToneMapOverlay()
            || frame.SegmentCount > frame.Segments.size()
            || frame.ViewportWidth == 0 || frame.ViewportHeight == 0)
        {
            outError = "debug overlay frame is invalid or empty";
            return false;
        }
        SceneDebugOverlayGpuConstants candidate;
        for (size_t segment = 0; segment < frame.SegmentCount; ++segment)
        {
            for (size_t component = 0; component < 4; ++component)
            {
                const float value = frame.Segments[segment].Values[component];
                if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                {
                    outError = "debug overlay segment is outside normalized viewport bounds";
                    return false;
                }
                candidate.Segments[segment][component] = value;
            }
        }
        using Style = SceneDebugOverlayStyle;
        if (!std::isfinite(frame.DepthScale) || !std::isfinite(frame.DepthOffset)
            || frame.DepthScale <= 1.0f || frame.DepthOffset >= 0.0f)
        {
            outError = "debug overlay depth mapping is not a valid perspective projection";
            return false;
        }
        for (size_t segment = 0; segment < frame.SegmentCount; ++segment)
        {
            for (size_t endpoint = 0; endpoint < 2; ++endpoint)
            {
                const float depth = frame.Segments[segment].Depth[endpoint];
                if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f)
                {
                    outError = "debug overlay segment depth is outside the zero-to-one range";
                    return false;
                }
                candidate.SegmentDepths[segment / 2][(segment % 2) * 2 + endpoint]
                    = SceneDebugInverseViewDepth(depth, frame.DepthScale,
                        frame.DepthOffset);
            }
        }
        candidate.OverlayColorAndOpacity[0] = 69.0f / 255.0f;
        candidate.OverlayColorAndOpacity[1] = 133.0f / 255.0f;
        candidate.OverlayColorAndOpacity[2] = 179.0f / 255.0f;
        candidate.OverlayColorAndOpacity[3] = Style::VisibleOpacity;
        candidate.OverlayState[0] = static_cast<float>(frame.SegmentCount);
        candidate.OverlayState[1] = static_cast<float>(frame.ViewportWidth);
        candidate.OverlayState[2] = static_cast<float>(frame.ViewportHeight);
        candidate.OverlayState[3] = Style::VisibleThicknessPixels;
        candidate.OccludedState[0]
            = frame.Settings.ShowOccludedSelectionBounds ? 1.0f : 0.0f;
        candidate.OccludedState[1] = Style::OccludedOpacity;
        candidate.OccludedState[2] = Style::OccludedThicknessPixels;
        candidate.OccludedState[3] = Style::DepthQuantizationSteps
            * Style::DepthQuantizationStep / std::abs(frame.DepthOffset);
        candidate.DepthState[0] = frame.DepthScale;
        candidate.DepthState[1] = frame.DepthOffset;
        candidate.DepthState[2] = Style::RelativeDepthTolerance;
        candidate.DepthState[3] = Style::SlopeFootprintPixels;
        outConstants = candidate;
        return true;
    }

    bool SceneDebugOverlayPass::Initialize(RHI::Device& device)
    {
        Shutdown();
        m_Device = &device;
        const ShaderSourceFile source = ShaderLibrary::LoadSource(
            "Engine/Shaders/SceneDebugOverlay.hlsl", "Scene debug overlay");
        if (source.Status != ShaderSourceStatus::Loaded)
            return false;
        SlangShaderCompiler compiler(std::filesystem::path("output")
            / "cache" / "shaders");
        const PortableShaderRequest vertexRequest = MakeDebugOverlayRequest(
            source, RHI::ShaderStage::Vertex, "VSMain");
        const PortableShaderRequest pixelRequest = MakeDebugOverlayRequest(
            source, RHI::ShaderStage::Pixel, "PSMain");
        const PortableShaderPackage vertex = compiler.Compile(vertexRequest);
        const PortableShaderPackage pixel = compiler.Compile(pixelRequest);
        std::string error;
        if (!PortableShaderContract::ValidatePackage(vertexRequest, vertex, error)
            || !PortableShaderContract::ValidatePackage(pixelRequest, pixel, error))
        {
            for (const PortableShaderDiagnostic& diagnostic : vertex.Diagnostics)
                Log::Error("Scene debug overlay vertex shader diagnostic: ",
                    diagnostic.Message);
            for (const PortableShaderDiagnostic& diagnostic : pixel.Diagnostics)
                Log::Error("Scene debug overlay pixel shader diagnostic: ",
                    diagnostic.Message);
            Log::Error("Scene debug overlay shader package validation failed: ", error);
            Shutdown();
            return false;
        }

        const bool d3d12 = device.GetCapabilities().ActiveBackend
            == RHI::Backend::NVRHID3D12;
        RHI::ShaderDescription vertexDescription;
        vertexDescription.DebugName = "Scene Debug Overlay Vertex Shader";
        vertexDescription.SourceName = source.ResolvedPath.string();
        vertexDescription.EntryPoint = "main";
        vertexDescription.Stage = RHI::ShaderStage::Vertex;
        vertexDescription.BinaryFormat = d3d12
            ? RHI::ShaderBinaryFormat::Dxil : RHI::ShaderBinaryFormat::Spirv;
        vertexDescription.Binary = d3d12 ? vertex.Dxil : vertex.Spirv;
        vertexDescription.Reflection = vertex.Reflection;
        RHI::ShaderDescription pixelDescription = vertexDescription;
        pixelDescription.DebugName = "Scene Debug Overlay Pixel Shader";
        pixelDescription.Stage = RHI::ShaderStage::Pixel;
        pixelDescription.Binary = d3d12 ? pixel.Dxil : pixel.Spirv;
        pixelDescription.Reflection = pixel.Reflection;
        m_VertexShader = device.CreateShader(vertexDescription);
        m_PixelShader = device.CreateShader(pixelDescription);

        RHI::PipelineDescription pipeline;
        pipeline.DebugName = "Scene Debug Overlay Pipeline";
        pipeline.VertexShader = m_VertexShader.get();
        pipeline.PixelShader = m_PixelShader.get();
        pipeline.VertexInputs = {
            { "POSITION", 0, RHI::Format::R32G32B32Float, 0,
                offsetof(FullscreenVertex, Position) },
            { "COLOR", 0, RHI::Format::R32G32B32Float, 0,
                offsetof(FullscreenVertex, Color) },
            { "TEXCOORD", 0, RHI::Format::R32G32Float, 0,
                offsetof(FullscreenVertex, UV) }
        };
        pipeline.VertexStrideBytes = sizeof(FullscreenVertex);
        pipeline.ConstantBufferBindings = {{ 0, 0,
            RHI::ShaderStage::AllGraphics }};
        pipeline.FixedSampledTexture = RHI::FixedSampledTextureBinding {};
        pipeline.FixedSampledTexture->PointSampling = true;
        pipeline.RasterCullMode = RHI::CullMode::None;
        pipeline.ColorFormat = RHI::Format::R8G8B8A8Unorm;
        pipeline.DepthFormat = RHI::Format::Unknown;
        pipeline.AlphaBlendEnable = true;
        m_Pipeline = m_VertexShader && m_PixelShader
            ? device.CreatePipeline(pipeline) : nullptr;

        const bool buffers = m_Pipeline
            && InitializeBuffer(device, "Debug Overlay Fullscreen Vertices",
                RHI::BufferUsage::Vertex, kFullscreenVertices.data(),
                sizeof(kFullscreenVertices), sizeof(FullscreenVertex),
                m_VertexBuffer)
            && InitializeBuffer(device, "Debug Overlay Fullscreen Indices",
                RHI::BufferUsage::Index, kFullscreenIndices.data(),
                sizeof(kFullscreenIndices), sizeof(u32), m_IndexBuffer);
        if (!buffers)
        {
            Shutdown();
            return false;
        }
        return true;
    }

    void SceneDebugOverlayPass::Shutdown()
    {
        m_ConstantSlots = {};
        m_ConstantGeneration = 0;
        m_IndexBuffer.reset();
        m_VertexBuffer.reset();
        m_Pipeline.reset();
        m_PixelShader.reset();
        m_VertexShader.reset();
        m_Device = nullptr;
    }

    Ref<SceneDebugOverlayPassConstants> SceneDebugOverlayPass::AcquireConstants(
        u64 frameIndex, const SceneDebugOverlayFrame& frame,
        std::string& outError)
    {
        SceneDebugOverlayGpuConstants gpuConstants;
        if (!m_Device || !TryBuildSceneDebugOverlayGpuConstants(
            frame, gpuConstants, outError))
            return nullptr;
        Ref<SceneDebugOverlayPassConstants>& slot = m_ConstantSlots[
            static_cast<size_t>(frameIndex % ConstantSlotCount)];
        if (slot && slot.use_count() != 1)
        {
            outError = "debug overlay constant slot is still retained by GPU work";
            return nullptr;
        }
        if (!slot)
        {
            auto candidate = CreateRef<SceneDebugOverlayPassConstants>();
            Scope<RHI::Buffer> buffer;
            if (!InitializeBuffer(*m_Device, "Scene Debug Overlay Constants",
                RHI::BufferUsage::Constant, &gpuConstants,
                sizeof(gpuConstants), 256, buffer))
            {
                outError = "debug overlay could not allocate its constant buffer";
                return nullptr;
            }
            candidate->Buffer = Ref<RHI::Buffer>(std::move(buffer));
            slot = std::move(candidate);
        }
        else
        {
            void* mapped = slot->Buffer ? slot->Buffer->Map() : nullptr;
            if (!mapped)
            {
                outError = "debug overlay could not map its reusable constant buffer";
                return nullptr;
            }
            std::memcpy(mapped, &gpuConstants, sizeof(gpuConstants));
            slot->Buffer->Unmap();
        }
        slot->Frame = frame;
        slot->GpuConstants = gpuConstants;
        slot->FrameIndex = frameIndex;
        slot->Generation = ++m_ConstantGeneration;
        outError.clear();
        return slot;
    }

    bool SceneDebugOverlayPass::Record(RHI::CommandList& commands,
        RHI::Texture& sceneDepth, RHI::Texture& output, u32 width, u32 height,
        const SceneDebugOverlayPassConstants& constants) const
    {
        if (!m_Pipeline || !m_VertexBuffer || !m_IndexBuffer
            || !constants.Buffer || !constants.Frame.HasPostToneMapOverlay()
            || constants.Frame.ViewportWidth != width
            || constants.Frame.ViewportHeight != height
            || width == 0 || height == 0 || &sceneDepth == &output
            || !commands.BindViewportOutputs(output, nullptr))
            return false;
        commands.SetGraphicsPipeline(*m_Pipeline);
        if (!commands.BindGraphicsSampledTexture(sceneDepth))
            return false;
        commands.SetViewport({ 0.0f, 0.0f, static_cast<float>(width),
            static_cast<float>(height), 0.0f, 1.0f });
        commands.SetScissorRect({ 0, 0, static_cast<int>(width),
            static_cast<int>(height) });
        commands.SetVertexBuffer(0, *m_VertexBuffer);
        commands.SetIndexBuffer(*m_IndexBuffer, RHI::IndexFormat::Uint32);
        commands.SetGraphicsConstantBuffer(0, *constants.Buffer);
        commands.DrawIndexed(3, 1, 0, 0, 0);
        return true;
    }
}
