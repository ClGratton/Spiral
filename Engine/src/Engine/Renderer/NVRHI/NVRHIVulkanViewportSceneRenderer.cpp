#include "Engine/Renderer/NVRHI/NVRHIVulkanViewportSceneRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/PortableShaderContract.h"
#include "Engine/Renderer/SceneRasterPreparation.h"
#include "Engine/Renderer/ShaderLibrary.h"
#include "Engine/Renderer/SlangShaderCompiler.h"

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
            // Render submits synchronously in this pre-handoff slice, so the old
            // renderer-owned outputs are GPU-retired before replacement.
            m_Color.reset(); m_Depth.reset();
            RHI::TextureDescription color; color.DebugName = "Vulkan Scene Viewport Color"; color.Extent = { width, height }; color.TextureFormat = RHI::Format::R8G8B8A8Unorm; color.Usage = static_cast<RHI::TextureUsage>(static_cast<u32>(RHI::TextureUsage::RenderTarget) | static_cast<u32>(RHI::TextureUsage::CopySource));
            RHI::TextureDescription depth = color; depth.DebugName = "Vulkan Scene Viewport Depth"; depth.TextureFormat = RHI::Format::D32Float; depth.Usage = RHI::TextureUsage::DepthStencil;
            m_Color = m_Device->CreateTexture(color); m_Depth = m_Device->CreateTexture(depth);
            if (!m_Color || !m_Depth) return false;
            m_Width = width; m_Height = height; ++m_OutputGeneration; return true;
        }

        bool Render(const SceneRenderSnapshot& snapshot, u32 width, u32 height, const ClearColor& clearColor)
        {
            if (!m_Device || width == 0 || height == 0 || !EnsureOutputs(width, height)) return false;
            SceneRasterFrame frame = PrepareSceneRasterFrame(snapshot);
            if (!frame.HasValidView || frame.Instances.empty()) return false;
            std::vector<Scope<RHI::Buffer>> constants; constants.reserve(frame.Instances.size());
            for (const SceneRasterInstance& instance : frame.Instances) { RHI::BufferDescription d; d.DebugName = "Vulkan Scene Viewport Instance Constants"; d.SizeBytes = kConstantBufferSize; d.Usage = WithUsage(RHI::BufferUsage::Constant, RHI::BufferUsage::CopyDest); Scope<RHI::Buffer> b = m_Device->CreateBuffer(d); Constants c {}; std::memcpy(c.ViewProjection, instance.ModelViewProjection.Values, sizeof(c.ViewProjection)); if (!b || !m_Device->UploadBuffer(*b, &c, sizeof(c))) return false; constants.push_back(std::move(b)); }
            Scope<RHI::CommandList> list = m_Device->CreateCommandList(RHI::QueueType::Graphics, "Vulkan Scene Viewport Snapshot Pass");
            RHI::ViewportClear clear; clear.Color[0] = clearColor.R; clear.Color[1] = clearColor.G; clear.Color[2] = clearColor.B; clear.Color[3] = clearColor.A;
            const bool opened = list && list->Begin() && list->BindViewportOutputs(*m_Color, m_Depth.get()) && list->TransitionTexture(*m_Color, RHI::ResourceState::RenderTarget) && list->TransitionTexture(*m_Depth, RHI::ResourceState::DepthWrite) && list->ClearViewportOutputs(clear);
            if (!opened) return false;
            list->BeginDebugMarker("Vulkan Scene Snapshot Raster"); list->SetGraphicsPipeline(*m_Pipeline); list->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f }); list->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) }); list->SetVertexBuffer(0, *m_VertexBuffer); list->SetIndexBuffer(*m_IndexBuffer, RHI::IndexFormat::Uint16);
            for (const Scope<RHI::Buffer>& constant : constants) { list->SetGraphicsConstantBuffer(0, *constant); list->DrawIndexed(static_cast<u32>(kIndices.size()), 1, 0, 0, 0); ++frame.IssuedDrawCount; }
            list->EndDebugMarker();
            const bool submitted = list->TransitionTexture(*m_Color, RHI::ResourceState::CopySource) && list->End() && m_Device->SubmitAndWait(*list);
            if (submitted) Renderer::PublishSceneRasterFrame(std::move(frame));
            return submitted;
        }

        bool ReadbackColor(RHI::TextureReadback& readback) const { return m_Device && m_Color && m_Device->ReadbackTexture(*m_Color, readback); }
        void Shutdown() { m_Color.reset(); m_Depth.reset(); m_IndexBuffer.reset(); m_VertexBuffer.reset(); m_Pipeline.reset(); m_PixelShader.reset(); m_VertexShader.reset(); m_Device = nullptr; }
        RHI::Device* m_Device = nullptr; Scope<RHI::Shader> m_VertexShader, m_PixelShader; Scope<RHI::Pipeline> m_Pipeline; Scope<RHI::Buffer> m_VertexBuffer, m_IndexBuffer; Scope<RHI::Texture> m_Color, m_Depth; u32 m_Width = 0, m_Height = 0; u64 m_OutputGeneration = 0;
    };

    NVRHIVulkanViewportSceneRenderer::NVRHIVulkanViewportSceneRenderer() = default;
    NVRHIVulkanViewportSceneRenderer::~NVRHIVulkanViewportSceneRenderer() { Shutdown(); }
    bool NVRHIVulkanViewportSceneRenderer::Initialize(RHI::Device* device) { m_Impl = CreateScope<Impl>(); if (m_Impl->Initialize(device)) return true; m_Impl.reset(); return false; }
    void NVRHIVulkanViewportSceneRenderer::Shutdown() { if (m_Impl) { m_Impl->Shutdown(); m_Impl.reset(); } }
    bool NVRHIVulkanViewportSceneRenderer::RenderCurrentSnapshot(u32 width, u32 height, const ClearColor& clearColor)
    {
        const std::shared_ptr<const SceneRenderSnapshot> snapshot = Renderer::GetSceneRenderSnapshot();
        return snapshot && Render(*snapshot, width, height, clearColor);
    }
    bool NVRHIVulkanViewportSceneRenderer::Render(const SceneRenderSnapshot& snapshot, u32 width, u32 height, const ClearColor& clearColor) { return m_Impl && m_Impl->Render(snapshot, width, height, clearColor); }
    bool NVRHIVulkanViewportSceneRenderer::ReadbackColor(RHI::TextureReadback& readback) const { return m_Impl && m_Impl->ReadbackColor(readback); }
    u64 NVRHIVulkanViewportSceneRenderer::GetOutputGeneration() const { return m_Impl ? m_Impl->m_OutputGeneration : 0; }
#endif
}
