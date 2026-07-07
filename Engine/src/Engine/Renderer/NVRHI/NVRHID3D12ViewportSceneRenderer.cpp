#include "Engine/Renderer/NVRHI/NVRHID3D12ViewportSceneRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Math/Math.h"
#include "Engine/Renderer/ShaderLibrary.h"

#if defined(GE_HAS_NVRHI_D3D12)
    #include <d3dcompiler.h>
    #include <wrl/client.h>

    #include <array>
    #include <cstddef>
    #include <cstring>
    #include <filesystem>
    #include <limits>
    #include <sstream>
    #include <string>
    #include <string_view>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_D3D12)
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
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

        std::string HResultToString(HRESULT result)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
            return stream.str();
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

        bool CompileD3DShader(const std::filesystem::path& path, const std::string& source, const char* entryPoint, const char* target, ComPtr<ID3DBlob>& outBlob)
        {
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(GE_DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

            ComPtr<ID3DBlob> errors;
            const std::string pathString = path.string();
            const HRESULT result = D3DCompile(source.data(), source.size(), pathString.c_str(), nullptr, nullptr, entryPoint, target, flags, 0, &outBlob, &errors);
            if (FAILED(result))
            {
                if (errors)
                    Log::Error("D3D shader compilation failed: ", static_cast<const char*>(errors->GetBufferPointer()));
                else
                    Log::Error("D3D shader compilation failed: ", HResultToString(result));
                return false;
            }

            return true;
        }
    }

    struct NVRHID3D12ViewportSceneRenderer::Impl
    {
        bool Initialize(ID3D12Device* device)
        {
            m_Device = device;
            if (!m_Device)
                return false;

            return CreatePipeline() && CreateMeshResources();
        }

        void Shutdown()
        {
            if (m_ConstantBuffer && m_ConstantBufferMapped)
            {
                m_ConstantBuffer->Unmap(0, nullptr);
                m_ConstantBufferMapped = nullptr;
            }

            m_ConstantBuffer.Reset();
            m_IndexBuffer.Reset();
            m_VertexBuffer.Reset();
            m_PipelineState.Reset();
            m_RootSignature.Reset();
            m_VertexBufferView = {};
            m_IndexBufferView = {};
            m_IndexCount = 0;
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

            if (m_PipelineState && m_RootSignature && m_ConstantBuffer && m_VertexBuffer && m_IndexBuffer)
            {
                PollShaderHotReload();
                UpdateConstants(width, height);

                D3D12_VIEWPORT viewport {};
                viewport.TopLeftX = 0.0f;
                viewport.TopLeftY = 0.0f;
                viewport.Width = static_cast<float>(width);
                viewport.Height = static_cast<float>(height);
                viewport.MinDepth = 0.0f;
                viewport.MaxDepth = 1.0f;

                D3D12_RECT scissor {};
                scissor.left = 0;
                scissor.top = 0;
                scissor.right = static_cast<LONG>(width);
                scissor.bottom = static_cast<LONG>(height);

                commandList->SetGraphicsRootSignature(m_RootSignature.Get());
                commandList->SetPipelineState(m_PipelineState.Get());
                commandList->SetGraphicsRootConstantBufferView(0, m_ConstantBuffer->GetGPUVirtualAddress());
                commandList->RSSetViewports(1, &viewport);
                commandList->RSSetScissorRects(1, &scissor);
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->IASetVertexBuffers(0, 1, &m_VertexBufferView);
                commandList->IASetIndexBuffer(&m_IndexBufferView);
                commandList->DrawIndexedInstanced(m_IndexCount, 1, 0, 0, 0);
            }

            D3D12_RESOURCE_BARRIER toShaderResource = TransitionBarrier(colorTexture, colorState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &toShaderResource);
            colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            return true;
        }

        bool CreatePipeline()
        {
            D3D12_ROOT_PARAMETER rootParameter {};
            rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParameter.Descriptor.ShaderRegister = 0;
            rootParameter.Descriptor.RegisterSpace = 0;
            rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

            D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc {};
            rootSignatureDesc.NumParameters = 1;
            rootSignatureDesc.pParameters = &rootParameter;
            rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

            ComPtr<ID3DBlob> signatureBlob;
            ComPtr<ID3DBlob> signatureErrors;
            HRESULT result = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &signatureErrors);
            if (FAILED(result))
            {
                if (signatureErrors)
                    Log::Error("Could not serialize viewport root signature: ", static_cast<const char*>(signatureErrors->GetBufferPointer()));
                else
                    Log::Error("Could not serialize viewport root signature: ", HResultToString(result));
                return false;
            }

            result = m_Device->CreateRootSignature(
                0,
                signatureBlob->GetBufferPointer(),
                signatureBlob->GetBufferSize(),
                IID_PPV_ARGS(&m_RootSignature));
            if (FAILED(result))
            {
                Log::Error("Could not create viewport root signature: ", HResultToString(result));
                return false;
            }

            m_RootSignature->SetName(L"Editor Viewport Root Signature");

            m_ShaderSource = ShaderLibrary::LoadSource(kViewportShaderPath, "Editor Viewport");
            if (m_ShaderSource.Status != ShaderSourceStatus::Loaded)
            {
                Log::Error("Could not load viewport shader: ", m_ShaderSource.ResolvedPath.string(), " (", ShaderLibrary::ToString(m_ShaderSource.Status), ")");
                return false;
            }

            ComPtr<ID3DBlob> vertexShader;
            ComPtr<ID3DBlob> pixelShader;
            if (!CompileD3DShader(m_ShaderSource.ResolvedPath, m_ShaderSource.Source, "VSMain", ShaderLibrary::DefaultTargetProfile(RHI::ShaderStage::Vertex), vertexShader))
                return false;
            if (!CompileD3DShader(m_ShaderSource.ResolvedPath, m_ShaderSource.Source, "PSMain", ShaderLibrary::DefaultTargetProfile(RHI::ShaderStage::Pixel), pixelShader))
                return false;

            Log::Info("Loaded viewport shader: ", m_ShaderSource.ResolvedPath.string(), " (revision ", m_ShaderSource.Revision, ")");

            D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlend {};
            renderTargetBlend.BlendEnable = FALSE;
            renderTargetBlend.LogicOpEnable = FALSE;
            renderTargetBlend.SrcBlend = D3D12_BLEND_ONE;
            renderTargetBlend.DestBlend = D3D12_BLEND_ZERO;
            renderTargetBlend.BlendOp = D3D12_BLEND_OP_ADD;
            renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
            renderTargetBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
            renderTargetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            renderTargetBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
            renderTargetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            D3D12_INPUT_ELEMENT_DESC inputElements[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ViewportVertex, Position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ViewportVertex, Color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc {};
            pipelineDesc.pRootSignature = m_RootSignature.Get();
            pipelineDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
            pipelineDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
            pipelineDesc.BlendState.AlphaToCoverageEnable = FALSE;
            pipelineDesc.BlendState.IndependentBlendEnable = FALSE;
            pipelineDesc.BlendState.RenderTarget[0] = renderTargetBlend;
            pipelineDesc.SampleMask = std::numeric_limits<unsigned int>::max();
            pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pipelineDesc.RasterizerState.FrontCounterClockwise = FALSE;
            pipelineDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
            pipelineDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
            pipelineDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
            pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
            pipelineDesc.RasterizerState.MultisampleEnable = FALSE;
            pipelineDesc.RasterizerState.AntialiasedLineEnable = FALSE;
            pipelineDesc.RasterizerState.ForcedSampleCount = 0;
            pipelineDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            pipelineDesc.DepthStencilState.DepthEnable = TRUE;
            pipelineDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pipelineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pipelineDesc.DepthStencilState.StencilEnable = FALSE;
            pipelineDesc.InputLayout = { inputElements, static_cast<UINT>(sizeof(inputElements) / sizeof(inputElements[0])) };
            pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipelineDesc.NumRenderTargets = 1;
            pipelineDesc.RTVFormats[0] = kColorFormat;
            pipelineDesc.DSVFormat = kDepthFormat;
            pipelineDesc.SampleDesc.Count = 1;
            pipelineDesc.SampleDesc.Quality = 0;

            result = m_Device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&m_PipelineState));
            if (FAILED(result))
            {
                Log::Error("Could not create viewport graphics pipeline: ", HResultToString(result));
                return false;
            }

            m_PipelineState->SetName(L"Editor Viewport Prototype Mesh Pipeline");
            return true;
        }

        bool CreateUploadBuffer(u64 sizeBytes, const wchar_t* name, ComPtr<ID3D12Resource>& resource)
        {
            D3D12_HEAP_PROPERTIES heapProperties {};
            heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
            heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProperties.CreationNodeMask = 1;
            heapProperties.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC bufferDesc {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = sizeBytes;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.SampleDesc.Quality = 0;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            const HRESULT result = m_Device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&resource));

            if (FAILED(result))
            {
                Log::Error("Could not create D3D12 upload buffer: ", HResultToString(result));
                return false;
            }

            resource->SetName(name);
            return true;
        }

        bool CreateMeshResources()
        {
            const u64 vertexBufferSize = sizeof(ViewportVertex) * kPrototypeMeshVertices.size();
            if (!CreateUploadBuffer(vertexBufferSize, L"Editor Viewport Prototype Vertex Buffer", m_VertexBuffer))
                return false;

            void* mappedVertexData = nullptr;
            D3D12_RANGE readRange { 0, 0 };
            HRESULT result = m_VertexBuffer->Map(0, &readRange, &mappedVertexData);
            if (FAILED(result))
            {
                Log::Error("Could not map viewport vertex buffer: ", HResultToString(result));
                return false;
            }
            std::memcpy(mappedVertexData, kPrototypeMeshVertices.data(), static_cast<size_t>(vertexBufferSize));
            m_VertexBuffer->Unmap(0, nullptr);

            m_VertexBufferView.BufferLocation = m_VertexBuffer->GetGPUVirtualAddress();
            m_VertexBufferView.SizeInBytes = static_cast<UINT>(vertexBufferSize);
            m_VertexBufferView.StrideInBytes = sizeof(ViewportVertex);

            const u64 indexBufferSize = sizeof(u16) * kPrototypeMeshIndices.size();
            if (!CreateUploadBuffer(indexBufferSize, L"Editor Viewport Prototype Index Buffer", m_IndexBuffer))
                return false;

            void* mappedIndexData = nullptr;
            result = m_IndexBuffer->Map(0, &readRange, &mappedIndexData);
            if (FAILED(result))
            {
                Log::Error("Could not map viewport index buffer: ", HResultToString(result));
                return false;
            }
            std::memcpy(mappedIndexData, kPrototypeMeshIndices.data(), static_cast<size_t>(indexBufferSize));
            m_IndexBuffer->Unmap(0, nullptr);

            m_IndexBufferView.BufferLocation = m_IndexBuffer->GetGPUVirtualAddress();
            m_IndexBufferView.SizeInBytes = static_cast<UINT>(indexBufferSize);
            m_IndexBufferView.Format = DXGI_FORMAT_R16_UINT;
            m_IndexCount = static_cast<u32>(kPrototypeMeshIndices.size());

            if (!CreateUploadBuffer(kViewportConstantBufferSize, L"Editor Viewport Constants", m_ConstantBuffer))
                return false;

            result = m_ConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_ConstantBufferMapped));
            if (FAILED(result))
            {
                Log::Error("Could not map viewport constant buffer: ", HResultToString(result));
                return false;
            }

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
        ComPtr<ID3D12RootSignature> m_RootSignature;
        ComPtr<ID3D12PipelineState> m_PipelineState;
        ComPtr<ID3D12Resource> m_VertexBuffer;
        ComPtr<ID3D12Resource> m_IndexBuffer;
        ComPtr<ID3D12Resource> m_ConstantBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_VertexBufferView {};
        D3D12_INDEX_BUFFER_VIEW m_IndexBufferView {};
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

    bool NVRHID3D12ViewportSceneRenderer::Initialize(ID3D12Device* device)
    {
        m_Impl = CreateScope<Impl>();
        if (m_Impl->Initialize(device))
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
