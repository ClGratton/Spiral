#include "Engine/Renderer/ToneMapPass.h"

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
        constexpr float kPaperWhiteScale = 1.0f;

        PortableShaderRequest MakeToneMapRequest(
            const ShaderSourceFile& source, RHI::ShaderStage stage, const char* entryPoint)
        {
            PortableShaderRequest request;
            request.SourceName = source.ResolvedPath.string();
            request.Source = source.Source;
            request.EntryPoint = entryPoint;
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
            request.ExpectedLayout = {
                { "ToneMapConstants", 'b', 0, 0, stage, "ConstantBuffer",
                    "struct{ExposureAndOutput:float32x4@0}", 1, 16, 0, 0 },
                { "HdrSceneSampler", 's', 0, 2, stage, "SamplerState", "sampler", 1, 0, 0, 0 },
                { "HdrScene", 't', 0, 2, stage, "Texture2D", "float32x4", 1, 0, 1, 4 }
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

        bool InitializeBuffer(RHI::Device& device, std::string_view name, RHI::BufferUsage usage,
            const void* bytes, u64 byteCount, u32 stride, Scope<RHI::Buffer>& output)
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

        std::array<float, 4> MakeToneMapConstants(
            const RendererColorPipelineSettings& settings)
        {
            return {
                static_cast<float>(settings.ManualExposureEV100),
                kPaperWhiteScale,
                static_cast<float>(settings.PostToneMapSaturation),
                static_cast<float>(settings.PostToneMapContrast)
            };
        }
    }

    bool ToneMapPass::Initialize(RHI::Device& device)
    {
        Shutdown();
        m_Device = &device;
        const ShaderSourceFile source = ShaderLibrary::LoadSource(
            "Engine/Shaders/ToneMap.hlsl", "Scene tone map");
        if (source.Status != ShaderSourceStatus::Loaded)
            return false;

        SlangShaderCompiler compiler(std::filesystem::path("output") / "cache" / "shaders");
        const PortableShaderRequest vertexRequest = MakeToneMapRequest(source, RHI::ShaderStage::Vertex, "VSMain");
        const PortableShaderRequest pixelRequest = MakeToneMapRequest(source, RHI::ShaderStage::Pixel, "PSMain");
        const PortableShaderPackage vertex = compiler.Compile(vertexRequest);
        const PortableShaderPackage pixel = compiler.Compile(pixelRequest);
        std::string error;
        if (!PortableShaderContract::ValidatePackage(vertexRequest, vertex, error)
            || !PortableShaderContract::ValidatePackage(pixelRequest, pixel, error))
        {
            Log::Error("Tone-map shader package validation failed: ", error);
            Shutdown();
            return false;
        }

        const bool d3d12 = device.GetCapabilities().ActiveBackend == RHI::Backend::NVRHID3D12;
        RHI::ShaderDescription vertexDescription;
        vertexDescription.DebugName = "Tone Map Vertex Shader";
        vertexDescription.SourceName = source.ResolvedPath.string();
        vertexDescription.EntryPoint = "main";
        vertexDescription.Stage = RHI::ShaderStage::Vertex;
        vertexDescription.BinaryFormat = d3d12 ? RHI::ShaderBinaryFormat::Dxil : RHI::ShaderBinaryFormat::Spirv;
        vertexDescription.Binary = d3d12 ? vertex.Dxil : vertex.Spirv;
        vertexDescription.Reflection = vertex.Reflection;
        RHI::ShaderDescription pixelDescription = vertexDescription;
        pixelDescription.DebugName = "Tone Map Pixel Shader";
        pixelDescription.Stage = RHI::ShaderStage::Pixel;
        pixelDescription.Binary = d3d12 ? pixel.Dxil : pixel.Spirv;
        pixelDescription.Reflection = pixel.Reflection;
        m_VertexShader = device.CreateShader(vertexDescription);
        m_PixelShader = device.CreateShader(pixelDescription);

        RHI::PipelineDescription pipeline;
        pipeline.DebugName = "Scene HDR Tone Map Pipeline";
        pipeline.VertexShader = m_VertexShader.get();
        pipeline.PixelShader = m_PixelShader.get();
        pipeline.VertexInputs = {
            { "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(FullscreenVertex, Position) },
            { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(FullscreenVertex, Color) },
            { "TEXCOORD", 0, RHI::Format::R32G32Float, 0, offsetof(FullscreenVertex, UV) }
        };
        pipeline.ConstantBufferBindings = {{ 0, 0, RHI::ShaderStage::AllGraphics }};
        pipeline.FixedSampledTexture = RHI::FixedSampledTextureBinding {};
        pipeline.RasterCullMode = RHI::CullMode::None;
        pipeline.ColorFormat = RHI::Format::R8G8B8A8Unorm;
        pipeline.DepthFormat = RHI::Format::Unknown;
        m_Pipeline = m_VertexShader && m_PixelShader ? device.CreatePipeline(pipeline) : nullptr;

        const bool buffers = m_Pipeline
            && InitializeBuffer(device, "Tone Map Fullscreen Vertices", RHI::BufferUsage::Vertex,
                kFullscreenVertices.data(), sizeof(kFullscreenVertices), sizeof(FullscreenVertex), m_VertexBuffer)
            && InitializeBuffer(device, "Tone Map Fullscreen Indices", RHI::BufferUsage::Index,
                kFullscreenIndices.data(), sizeof(kFullscreenIndices), sizeof(u32), m_IndexBuffer);
        if (!buffers)
        {
            Shutdown();
            return false;
        }
        Log::Info("SceneColorPipelineV1 hdr=RGBA16F manualExposureEV100=0 toneMap=Khronos-PBR-Neutral postToneMapGrade=saturation-contrast output=sRGB-encoded-RGBA8 result=ready");
        return true;
    }

    void ToneMapPass::Shutdown()
    {
        // Callers own device-idle ordering before shutdown when submitted graph
        // frames may still reference current or prior immutable constants.
        m_CurrentConstants.reset();
        m_ConstantGeneration = 0;
        m_ConstantAllocationCount = 0;
        m_ConstantReuseCount = 0;
        m_IndexBuffer.reset();
        m_VertexBuffer.reset();
        m_Pipeline.reset();
        m_PixelShader.reset();
        m_VertexShader.reset();
        m_Device = nullptr;
    }

    Ref<ToneMapPassConstants> ToneMapPass::AcquireConstants(
        const RendererColorPipelineSettings& settings) const
    {
        if (!m_Device || !IsValidRendererColorPipelineSettings(settings))
            return nullptr;
        if (m_CurrentConstants && m_CurrentConstants->Settings == settings)
        {
            ++m_ConstantReuseCount;
            return m_CurrentConstants;
        }

        const std::array<float, 4> constants = MakeToneMapConstants(settings);
        auto result = CreateRef<ToneMapPassConstants>();
        result->Settings = settings;
        result->Generation = m_ConstantGeneration + 1;
        if (!InitializeBuffer(*m_Device, "Tone Map Constants", RHI::BufferUsage::Constant,
            constants.data(), sizeof(constants), 256, result->Buffer))
        {
            return nullptr;
        }
        m_CurrentConstants = result;
        m_ConstantGeneration = result->Generation;
        ++m_ConstantAllocationCount;
        return m_CurrentConstants;
    }

    ToneMapPassConstantCacheDiagnostics ToneMapPass::GetConstantCacheDiagnostics() const
    {
        ToneMapPassConstantCacheDiagnostics diagnostics;
        diagnostics.HasCurrent = m_CurrentConstants != nullptr;
        if (m_CurrentConstants)
        {
            diagnostics.CurrentSettings = m_CurrentConstants->Settings;
            diagnostics.CurrentGeneration = m_CurrentConstants->Generation;
        }
        diagnostics.AllocationCount = m_ConstantAllocationCount;
        diagnostics.ReuseCount = m_ConstantReuseCount;
        return diagnostics;
    }

    bool ToneMapPass::Record(RHI::CommandList& commands, RHI::Texture& hdrScene,
        RHI::Texture& output, u32 width, u32 height,
        const ToneMapPassConstants& constants) const
    {
        if (!m_Pipeline || !m_VertexBuffer || !m_IndexBuffer || !constants.Buffer
            || !IsValidRendererColorPipelineSettings(constants.Settings)
            || width == 0 || height == 0 || !commands.BindViewportOutputs(output, nullptr))
            return false;
        commands.SetGraphicsPipeline(*m_Pipeline);
        if (!commands.BindGraphicsSampledTexture(hdrScene))
            return false;
        commands.SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f });
        commands.SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
        commands.SetVertexBuffer(0, *m_VertexBuffer);
        commands.SetIndexBuffer(*m_IndexBuffer, RHI::IndexFormat::Uint32);
        commands.SetGraphicsConstantBuffer(0, *constants.Buffer);
        commands.DrawIndexed(3, 1, 0, 0, 0);
        return true;
    }
}
