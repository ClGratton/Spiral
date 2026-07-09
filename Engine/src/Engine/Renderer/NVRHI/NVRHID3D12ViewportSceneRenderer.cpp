#include "Engine/Renderer/NVRHI/NVRHID3D12ViewportSceneRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Math/Math.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/Renderer/NVRHI/D3D12DebugMarkers.h"
#include "Engine/Renderer/ShaderLibrary.h"

#if defined(GE_HAS_NVRHI_D3D12)
    #include <array>
    #include <cstddef>
    #include <cstring>
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

        struct ViewportVertex
        {
            float Position[3];
            float Color[3];
        };

        struct ViewportConstants
        {
            float ViewProjection[16];
        };

        constexpr std::array<ViewportVertex, 24> kPrototypeMeshVertices = {
            ViewportVertex{{ -0.75f, -0.75f, -0.75f }, { 0.22f, 0.68f, 1.00f }},
            ViewportVertex{{ -0.75f,  0.75f, -0.75f }, { 0.22f, 0.68f, 1.00f }},
            ViewportVertex{{  0.75f,  0.75f, -0.75f }, { 0.22f, 0.68f, 1.00f }},
            ViewportVertex{{  0.75f, -0.75f, -0.75f }, { 0.22f, 0.68f, 1.00f }},
            ViewportVertex{{  0.75f, -0.75f,  0.75f }, { 0.95f, 0.72f, 0.28f }},
            ViewportVertex{{  0.75f,  0.75f,  0.75f }, { 0.95f, 0.72f, 0.28f }},
            ViewportVertex{{ -0.75f,  0.75f,  0.75f }, { 0.95f, 0.72f, 0.28f }},
            ViewportVertex{{ -0.75f, -0.75f,  0.75f }, { 0.95f, 0.72f, 0.28f }},
            ViewportVertex{{ -0.75f, -0.75f,  0.75f }, { 0.26f, 0.88f, 0.55f }},
            ViewportVertex{{ -0.75f,  0.75f,  0.75f }, { 0.26f, 0.88f, 0.55f }},
            ViewportVertex{{ -0.75f,  0.75f, -0.75f }, { 0.26f, 0.88f, 0.55f }},
            ViewportVertex{{ -0.75f, -0.75f, -0.75f }, { 0.26f, 0.88f, 0.55f }},
            ViewportVertex{{  0.75f, -0.75f, -0.75f }, { 0.88f, 0.35f, 0.37f }},
            ViewportVertex{{  0.75f,  0.75f, -0.75f }, { 0.88f, 0.35f, 0.37f }},
            ViewportVertex{{  0.75f,  0.75f,  0.75f }, { 0.88f, 0.35f, 0.37f }},
            ViewportVertex{{  0.75f, -0.75f,  0.75f }, { 0.88f, 0.35f, 0.37f }},
            ViewportVertex{{ -0.75f,  0.75f, -0.75f }, { 0.72f, 0.52f, 0.96f }},
            ViewportVertex{{ -0.75f,  0.75f,  0.75f }, { 0.72f, 0.52f, 0.96f }},
            ViewportVertex{{  0.75f,  0.75f,  0.75f }, { 0.72f, 0.52f, 0.96f }},
            ViewportVertex{{  0.75f,  0.75f, -0.75f }, { 0.72f, 0.52f, 0.96f }},
            ViewportVertex{{ -0.75f, -0.75f,  0.75f }, { 0.24f, 0.75f, 0.82f }},
            ViewportVertex{{ -0.75f, -0.75f, -0.75f }, { 0.24f, 0.75f, 0.82f }},
            ViewportVertex{{  0.75f, -0.75f, -0.75f }, { 0.24f, 0.75f, 0.82f }},
            ViewportVertex{{  0.75f, -0.75f,  0.75f }, { 0.24f, 0.75f, 0.82f }},
        };

        constexpr std::array<u16, 36> kPrototypeMeshIndices = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7,
            8, 9, 10, 8, 10, 11,
            12, 13, 14, 12, 14, 15,
            16, 17, 18, 16, 18, 19,
            20, 21, 22, 20, 22, 23,
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
        bool Initialize(ID3D12Device* device, RHI::Device* rhiDevice)
        {
            m_Device = device;
            m_RHIDevice = rhiDevice;
            if (!m_Device || !m_RHIDevice)
                return false;

            return CreatePipeline() && CreateMeshResources();
        }

        void Shutdown()
        {
            if (m_ConstantBuffer && m_ConstantBufferMapped)
            {
                m_ConstantBuffer->Unmap();
                m_ConstantBufferMapped = nullptr;
            }

            m_ConstantBuffer.reset();
            m_IndexBuffer.reset();
            m_VertexBuffer.reset();
            m_Pipeline.reset();
            m_PixelShader.reset();
            m_VertexShader.reset();
            m_IndexCount = 0;
            m_RHIDevice = nullptr;
            m_Device = nullptr;
        }

        bool Render(
            ID3D12GraphicsCommandList* commandList,
            ID3D12Resource* colorTexture,
            D3D12_RESOURCE_STATES& colorState,
            D3D12_CPU_DESCRIPTOR_HANDLE colorRtv,
            ID3D12Resource* depthTexture,
            D3D12_CPU_DESCRIPTOR_HANDLE depthDsv,
            u32 width,
            u32 height,
            const ClearColor& clearColor)
        {
            if (!commandList || !colorTexture || !depthTexture || width == 0 || height == 0)
                return false;

            ScopedD3D12Marker marker(commandList, "Viewport Prototype Mesh Pass");

            if (colorState != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                D3D12_RESOURCE_BARRIER toRenderTarget = TransitionBarrier(colorTexture, colorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
                commandList->ResourceBarrier(1, &toRenderTarget);
                colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }

            const float clear[4] = { clearColor.R, clearColor.G, clearColor.B, clearColor.A };
            commandList->OMSetRenderTargets(1, &colorRtv, FALSE, &depthDsv);
            commandList->ClearRenderTargetView(colorRtv, clear, 0, nullptr);
            commandList->ClearDepthStencilView(depthDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            if (m_Pipeline && m_ConstantBuffer && m_VertexBuffer && m_IndexBuffer)
            {
                PollShaderHotReload();
                UpdateConstants(width, height);

                Scope<RHI::CommandList> rhiCommandList = RHI::WrapNVRHID3D12CommandList(
                    RHI::QueueType::Graphics,
                    commandList,
                    "Editor Viewport Prototype Mesh Command Bridge");
                if (!rhiCommandList)
                    return false;

                rhiCommandList->SetGraphicsPipeline(*m_Pipeline);
                rhiCommandList->SetGraphicsConstantBuffer(0, *m_ConstantBuffer);
                rhiCommandList->SetViewport({ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f });
                rhiCommandList->SetScissorRect({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
                rhiCommandList->SetVertexBuffer(0, *m_VertexBuffer);
                rhiCommandList->SetIndexBuffer(*m_IndexBuffer, RHI::IndexFormat::Uint16);
                rhiCommandList->DrawIndexed(m_IndexCount, 1, 0, 0, 0);
            }

            D3D12_RESOURCE_BARRIER toShaderResource = TransitionBarrier(colorTexture, colorState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &toShaderResource);
            colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            return true;
        }

        bool CreatePipeline()
        {
            m_ShaderSource = ShaderLibrary::LoadSource(kViewportShaderPath, "Editor Viewport");
            if (m_ShaderSource.Status != ShaderSourceStatus::Loaded)
            {
                Log::Error("Could not load viewport shader: ", m_ShaderSource.ResolvedPath.string(), " (", ShaderLibrary::ToString(m_ShaderSource.Status), ")");
                return false;
            }

            if (!CreateRhiShader(RHI::ShaderStage::Vertex, "VSMain", "Editor Viewport Vertex Shader", m_VertexShader))
                return false;
            if (!CreateRhiShader(RHI::ShaderStage::Pixel, "PSMain", "Editor Viewport Pixel Shader", m_PixelShader))
                return false;

            Log::Info("Loaded viewport shader: ", m_ShaderSource.ResolvedPath.string(), " (revision ", m_ShaderSource.Revision, ")");

            RHI::PipelineDescription pipelineDesc;
            pipelineDesc.DebugName = "Editor Viewport Prototype Mesh Pipeline";
            pipelineDesc.Type = RHI::PipelineType::Graphics;
            pipelineDesc.VertexShader = m_VertexShader.get();
            pipelineDesc.PixelShader = m_PixelShader.get();
            pipelineDesc.VertexInputs = {
                { "POSITION", 0, RHI::Format::R32G32B32Float, 0, offsetof(ViewportVertex, Position) },
                { "COLOR", 0, RHI::Format::R32G32B32Float, 0, offsetof(ViewportVertex, Color) }
            };
            pipelineDesc.ConstantBufferBindings = {
                { 0, 0, RHI::ShaderStage::Vertex }
            };
            pipelineDesc.Topology = RHI::PrimitiveTopology::TriangleList;
            pipelineDesc.RasterCullMode = RHI::CullMode::None;
            pipelineDesc.ColorFormat = RHI::Format::R8G8B8A8Unorm;
            pipelineDesc.DepthFormat = RHI::Format::D32Float;
            pipelineDesc.DepthTestEnable = true;
            pipelineDesc.DepthWriteEnable = true;

            m_Pipeline = m_RHIDevice->CreatePipeline(pipelineDesc);
            if (!m_Pipeline)
                return false;

            return true;
        }

        bool CreateRhiShader(
            RHI::ShaderStage stage,
            const char* entryPoint,
            const char* debugName,
            Scope<RHI::Shader>& shader)
        {
            RHI::ShaderDescription description;
            description.DebugName = debugName;
            description.SourceName = m_ShaderSource.ResolvedPath.string();
            description.Source = m_ShaderSource.Source;
            description.EntryPoint = entryPoint;
            description.TargetProfile = ShaderLibrary::DefaultTargetProfile(stage);
            description.Stage = stage;

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

        bool WriteBuffer(RHI::Buffer& buffer, const void* sourceData, u64 sizeBytes)
        {
            if (!sourceData || sizeBytes == 0)
                return false;

            void* mappedData = buffer.Map();
            if (!mappedData)
                return false;

            std::memcpy(mappedData, sourceData, static_cast<size_t>(sizeBytes));
            buffer.Unmap();
            return true;
        }

        bool CreateMeshResources()
        {
            const u64 vertexBufferSize = sizeof(ViewportVertex) * kPrototypeMeshVertices.size();
            RHI::BufferDescription vertexBufferDesc;
            vertexBufferDesc.DebugName = "Editor Viewport Prototype Vertex Buffer";
            vertexBufferDesc.SizeBytes = vertexBufferSize;
            vertexBufferDesc.StrideBytes = sizeof(ViewportVertex);
            vertexBufferDesc.Usage = RHI::BufferUsage::Vertex;
            vertexBufferDesc.CpuAccess = RHI::BufferCpuAccess::Write;
            if (!CreateRhiBuffer(vertexBufferDesc, m_VertexBuffer))
                return false;

            if (!WriteBuffer(*m_VertexBuffer, kPrototypeMeshVertices.data(), vertexBufferSize))
                return false;

            const u64 indexBufferSize = sizeof(u16) * kPrototypeMeshIndices.size();
            RHI::BufferDescription indexBufferDesc;
            indexBufferDesc.DebugName = "Editor Viewport Prototype Index Buffer";
            indexBufferDesc.SizeBytes = indexBufferSize;
            indexBufferDesc.StrideBytes = sizeof(u16);
            indexBufferDesc.Usage = RHI::BufferUsage::Index;
            indexBufferDesc.CpuAccess = RHI::BufferCpuAccess::Write;
            if (!CreateRhiBuffer(indexBufferDesc, m_IndexBuffer))
                return false;

            if (!WriteBuffer(*m_IndexBuffer, kPrototypeMeshIndices.data(), indexBufferSize))
                return false;

            m_IndexCount = static_cast<u32>(kPrototypeMeshIndices.size());

            RHI::BufferDescription constantBufferDesc;
            constantBufferDesc.DebugName = "Editor Viewport Constants";
            constantBufferDesc.SizeBytes = kViewportConstantBufferSize;
            constantBufferDesc.StrideBytes = kViewportConstantBufferSize;
            constantBufferDesc.Usage = RHI::BufferUsage::Constant;
            constantBufferDesc.CpuAccess = RHI::BufferCpuAccess::Write;
            if (!CreateRhiBuffer(constantBufferDesc, m_ConstantBuffer))
                return false;

            m_ConstantBufferMapped = static_cast<std::byte*>(m_ConstantBuffer->Map());
            if (!m_ConstantBufferMapped)
                return false;

            return true;
        }

        void UpdateConstants(u32 width, u32 height)
        {
            if (!m_ConstantBufferMapped || width == 0 || height == 0)
                return;

            const float framePhase = static_cast<float>(m_FrameCounter++);
            const float yaw = 0.72f + framePhase * 0.006f;
            const float pitch = -0.34f;
            const Math::Mat4 model = Math::Multiply(Math::RotationY(yaw), Math::RotationX(pitch));
            const Math::Mat4 viewProjection = Math::Multiply(model, Renderer::GetCameraView().ViewProjection);

            ViewportConstants constants {};
            std::memcpy(constants.ViewProjection, viewProjection.Values, sizeof(constants.ViewProjection));
            std::memcpy(m_ConstantBufferMapped, &constants, sizeof(constants));
        }

        void PollShaderHotReload()
        {
            if (m_ShaderReloadLogged || !ShaderLibrary::HasSourceChanged(m_ShaderSource))
                return;

            if (ShaderLibrary::ReloadSourceIfChanged(m_ShaderSource))
            {
                Log::Warn("Shader source changed: ", m_ShaderSource.ResolvedPath.string(), ". Live D3D12 pipeline rebuild is queued for a later renderer-thread pass.");
                m_ShaderReloadLogged = true;
            }
        }

        ID3D12Device* m_Device = nullptr;
        RHI::Device* m_RHIDevice = nullptr;
        Scope<RHI::Pipeline> m_Pipeline;
        Scope<RHI::Shader> m_VertexShader;
        Scope<RHI::Shader> m_PixelShader;
        Scope<RHI::Buffer> m_VertexBuffer;
        Scope<RHI::Buffer> m_IndexBuffer;
        Scope<RHI::Buffer> m_ConstantBuffer;
        ShaderSourceFile m_ShaderSource;
        std::byte* m_ConstantBufferMapped = nullptr;
        u32 m_IndexCount = 0;
        u64 m_FrameCounter = 0;
        bool m_ShaderReloadLogged = false;
    };

    NVRHID3D12ViewportSceneRenderer::NVRHID3D12ViewportSceneRenderer() = default;

    NVRHID3D12ViewportSceneRenderer::~NVRHID3D12ViewportSceneRenderer()
    {
        Shutdown();
    }

    bool NVRHID3D12ViewportSceneRenderer::Initialize(ID3D12Device* device, RHI::Device* rhiDevice)
    {
        m_Impl = CreateScope<Impl>();
        if (m_Impl->Initialize(device, rhiDevice))
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
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* colorTexture,
        D3D12_RESOURCE_STATES& colorState,
        D3D12_CPU_DESCRIPTOR_HANDLE colorRtv,
        ID3D12Resource* depthTexture,
        D3D12_CPU_DESCRIPTOR_HANDLE depthDsv,
        u32 width,
        u32 height,
        const ClearColor& clearColor)
    {
        return m_Impl && m_Impl->Render(commandList, colorTexture, colorState, colorRtv, depthTexture, depthDsv, width, height, clearColor);
    }
#endif
}
