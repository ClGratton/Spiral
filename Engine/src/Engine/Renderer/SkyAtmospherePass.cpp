#include "Engine/Renderer/SkyAtmospherePass.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"

#include <array>
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

        PortableShaderRequest MakeSkyRequest(const ShaderSourceFile& source,
            RHI::ShaderStage stage, const char* entryPoint)
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
                { "SkyAtmosphereConstants", 'b', 0, 0, stage,
                    "ConstantBuffer",
                    "struct{ProjectionAndExposure:float32x4@0,SunDirectionAndCosRadius:float32x4@16,ViewUpAndEnabled:float32x4@32,ZenithxyYAndSunTheta:float32x4@48,LuminancePerezABCD:float32x4@64,ChromaticityXPerezABCD:float32x4@80,ChromaticityYPerezABCD:float32x4@96,PerezE:float32x4@112,SunRadiance:float32x4@128,GroundRadiance:float32x4@144}",
                    1, 160, 0, 0 }
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

    bool SkyAtmospherePass::Initialize(RHI::Device& device)
    {
        Shutdown();
        m_Device = &device;
        const ShaderSourceFile source = ShaderLibrary::LoadSource(
            "Engine/Shaders/SkyAtmosphere.hlsl", "Scene sky atmosphere");
        if (source.Status != ShaderSourceStatus::Loaded)
            return false;

        SlangShaderCompiler compiler(std::filesystem::path("output")
            / "cache" / "shaders");
        const PortableShaderRequest vertexRequest = MakeSkyRequest(
            source, RHI::ShaderStage::Vertex, "VSMain");
        const PortableShaderRequest pixelRequest = MakeSkyRequest(
            source, RHI::ShaderStage::Pixel, "PSMain");
        const PortableShaderPackage vertex = compiler.Compile(vertexRequest);
        const PortableShaderPackage pixel = compiler.Compile(pixelRequest);
        std::string error;
        if (!PortableShaderContract::ValidatePackage(vertexRequest, vertex, error)
            || !PortableShaderContract::ValidatePackage(pixelRequest, pixel, error))
        {
            for (const PortableShaderDiagnostic& diagnostic : vertex.Diagnostics)
                Log::Error("Sky atmosphere vertex shader diagnostic: ",
                    diagnostic.Message);
            for (const PortableShaderDiagnostic& diagnostic : pixel.Diagnostics)
                Log::Error("Sky atmosphere pixel shader diagnostic: ",
                    diagnostic.Message);
            Log::Error("Sky atmosphere shader package validation failed: ", error);
            Shutdown();
            return false;
        }

        const bool d3d12 = device.GetCapabilities().ActiveBackend
            == RHI::Backend::NVRHID3D12;
        RHI::ShaderDescription vertexDescription;
        vertexDescription.DebugName = "Scene Sky Atmosphere Vertex Shader";
        vertexDescription.SourceName = source.ResolvedPath.string();
        vertexDescription.EntryPoint = "main";
        vertexDescription.Stage = RHI::ShaderStage::Vertex;
        vertexDescription.BinaryFormat = d3d12
            ? RHI::ShaderBinaryFormat::Dxil : RHI::ShaderBinaryFormat::Spirv;
        vertexDescription.Binary = d3d12 ? vertex.Dxil : vertex.Spirv;
        vertexDescription.Reflection = vertex.Reflection;
        RHI::ShaderDescription pixelDescription = vertexDescription;
        pixelDescription.DebugName = "Scene Sky Atmosphere Pixel Shader";
        pixelDescription.Stage = RHI::ShaderStage::Pixel;
        pixelDescription.Binary = d3d12 ? pixel.Dxil : pixel.Spirv;
        pixelDescription.Reflection = pixel.Reflection;
        m_VertexShader = device.CreateShader(vertexDescription);
        m_PixelShader = device.CreateShader(pixelDescription);

        RHI::PipelineDescription pipeline;
        pipeline.DebugName = "Scene Sky Atmosphere Pipeline";
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
        pipeline.RasterCullMode = RHI::CullMode::None;
        pipeline.ColorFormat = RHI::Format::R16G16B16A16Float;
        pipeline.DepthFormat = RHI::Format::Unknown;
        m_Pipeline = m_VertexShader && m_PixelShader
            ? device.CreatePipeline(pipeline) : nullptr;

        const bool buffers = m_Pipeline
            && InitializeBuffer(device, "Sky Atmosphere Fullscreen Vertices",
                RHI::BufferUsage::Vertex, kFullscreenVertices.data(),
                sizeof(kFullscreenVertices), sizeof(FullscreenVertex),
                m_VertexBuffer)
            && InitializeBuffer(device, "Sky Atmosphere Fullscreen Indices",
                RHI::BufferUsage::Index, kFullscreenIndices.data(),
                sizeof(kFullscreenIndices), sizeof(u32), m_IndexBuffer);
        if (!buffers)
        {
            Shutdown();
            return false;
        }
        return true;
    }

    void SkyAtmospherePass::Shutdown()
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

    Ref<SkyAtmospherePassConstants> SkyAtmospherePass::AcquireConstants(
        u64 frameIndex, const SceneSkyAtmosphereFrame& frame,
        float preExposure, std::string& outError)
    {
        SceneSkyAtmosphereGpuConstants gpuConstants;
        if (!m_Device || !TryBuildSceneSkyAtmosphereGpuConstants(
            frame, preExposure, gpuConstants, outError))
            return nullptr;
        Ref<SkyAtmospherePassConstants>& slot = m_ConstantSlots[
            static_cast<size_t>(frameIndex % ConstantSlotCount)];
        if (slot && slot.use_count() != 1)
        {
            outError = "sky atmosphere constant slot is still retained by GPU work";
            return nullptr;
        }
        if (!slot)
        {
            auto candidate = CreateRef<SkyAtmospherePassConstants>();
            Scope<RHI::Buffer> buffer;
            if (!InitializeBuffer(*m_Device, "Sky Atmosphere Constants",
                RHI::BufferUsage::Constant, &gpuConstants,
                sizeof(gpuConstants), 256, buffer))
            {
                outError = "sky atmosphere could not allocate its constant buffer";
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
                outError = "sky atmosphere could not map its reusable constant buffer";
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

    bool SkyAtmospherePass::Record(RHI::CommandList& commands,
        RHI::Texture& hdrScene, u32 width, u32 height,
        const SkyAtmospherePassConstants& constants) const
    {
        if (!m_Pipeline || !m_VertexBuffer || !m_IndexBuffer
            || !constants.Buffer || !IsValidSceneSkyAtmosphereFrame(constants.Frame)
            || width == 0 || height == 0)
            return false;
        if (!constants.Frame.Enabled)
            return true;
        if (!commands.BindViewportOutputs(hdrScene, nullptr))
            return false;
        commands.SetGraphicsPipeline(*m_Pipeline);
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
