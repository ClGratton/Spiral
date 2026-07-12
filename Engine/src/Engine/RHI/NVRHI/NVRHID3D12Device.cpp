#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"

#include "Engine/Core/Log.h"

#if defined(GE_HAS_NVRHI_D3D12)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <Windows.h>
    #include <d3dcompiler.h>
    #include <dxgi1_6.h>
    #include <wrl/client.h>

    #include <directx/d3d12.h>
    #include <directx/d3d12sdklayers.h>
    #include <nvrhi/d3d12.h>
    #include <nvrhi/validation.h>

    #if defined(DeviceCapabilities)
        #undef DeviceCapabilities
    #endif

    #include <cstring>
    #include <iomanip>
    #include <limits>
    #include <sstream>
    #include <string>
    #include <vector>
#endif

namespace Engine::RHI
{
#if defined(GE_HAS_NVRHI_D3D12)
    namespace
    {
        using Microsoft::WRL::ComPtr;

        D3D12_RESOURCE_STATES ConvertResourceState(ResourceState state)
        {
            switch (state)
            {
                case ResourceState::Common: return D3D12_RESOURCE_STATE_COMMON;
                case ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
                case ResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
                case ResourceState::ShaderResource: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                case ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                case ResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
                case ResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
                case ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
                case ResourceState::Unknown:
                default: return D3D12_RESOURCE_STATE_COMMON;
            }
        }

        bool HasBufferUsage(BufferUsage usage, BufferUsage flag)
        {
            return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
        }

        bool HasTextureUsage(TextureUsage usage, TextureUsage flag)
        {
            return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0;
        }

        DXGI_FORMAT ConvertFormat(Format format)
        {
            switch (format)
            {
                case Format::R8Unorm: return DXGI_FORMAT_R8_UNORM;
                case Format::R8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
                case Format::R8G8B8A8UnormSrgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                case Format::R11G11B10Float: return DXGI_FORMAT_R11G11B10_FLOAT;
                case Format::R16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case Format::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
                case Format::R32G32B32Float: return DXGI_FORMAT_R32G32B32_FLOAT;
                case Format::R32G32B32A32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
                case Format::R32Uint: return DXGI_FORMAT_R32_UINT;
                case Format::D24UnormS8Uint: return DXGI_FORMAT_D24_UNORM_S8_UINT;
                case Format::D32Float: return DXGI_FORMAT_D32_FLOAT;
                case Format::Unknown:
                default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        D3D12_RESOURCE_FLAGS ConvertTextureFlags(TextureUsage usage)
        {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
            if (HasTextureUsage(usage, TextureUsage::RenderTarget))
                flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (HasTextureUsage(usage, TextureUsage::DepthStencil))
                flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            if (HasTextureUsage(usage, TextureUsage::UnorderedAccess))
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            return flags;
        }

        D3D12_SHADER_VISIBILITY ConvertShaderVisibility(ShaderStage stage)
        {
            switch (stage)
            {
                case ShaderStage::Vertex: return D3D12_SHADER_VISIBILITY_VERTEX;
                case ShaderStage::Pixel: return D3D12_SHADER_VISIBILITY_PIXEL;
                case ShaderStage::AllGraphics:
                case ShaderStage::All: return D3D12_SHADER_VISIBILITY_ALL;
                default: return D3D12_SHADER_VISIBILITY_ALL;
            }
        }

        D3D12_CULL_MODE ConvertCullMode(CullMode cullMode)
        {
            switch (cullMode)
            {
                case CullMode::None: return D3D12_CULL_MODE_NONE;
                case CullMode::Front: return D3D12_CULL_MODE_FRONT;
                case CullMode::Back: return D3D12_CULL_MODE_BACK;
            }

            return D3D12_CULL_MODE_BACK;
        }

        D3D12_PRIMITIVE_TOPOLOGY_TYPE ConvertPrimitiveTopologyType(PrimitiveTopology topology)
        {
            switch (topology)
            {
                case PrimitiveTopology::TriangleList: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            }

            return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }

        D3D_PRIMITIVE_TOPOLOGY ConvertPrimitiveTopology(PrimitiveTopology topology)
        {
            switch (topology)
            {
                case PrimitiveTopology::TriangleList: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            }

            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }

        D3D12_INPUT_CLASSIFICATION ConvertVertexInputRate(VertexInputRate rate)
        {
            switch (rate)
            {
                case VertexInputRate::PerVertex: return D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                case VertexInputRate::PerInstance: return D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
            }

            return D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        }

        D3D12_COMMAND_LIST_TYPE ConvertCommandListType(QueueType queueType)
        {
            switch (queueType)
            {
                case QueueType::Graphics: return D3D12_COMMAND_LIST_TYPE_DIRECT;
                case QueueType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
                case QueueType::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
            }

            return D3D12_COMMAND_LIST_TYPE_DIRECT;
        }

        DXGI_FORMAT ConvertIndexFormat(IndexFormat format)
        {
            switch (format)
            {
                case IndexFormat::Uint16: return DXGI_FORMAT_R16_UINT;
                case IndexFormat::Uint32: return DXGI_FORMAT_R32_UINT;
            }

            return DXGI_FORMAT_R16_UINT;
        }

        const char* DefaultTargetProfile(ShaderStage stage)
        {
            switch (stage)
            {
                case ShaderStage::Vertex: return "vs_5_0";
                case ShaderStage::Pixel: return "ps_5_0";
                case ShaderStage::Compute: return "cs_5_0";
                default: return "";
            }
        }

        std::string HResultToString(HRESULT result)
        {
            std::ostringstream stream;
            stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
            return stream.str();
        }

        std::string WideToUtf8(const wchar_t* value)
        {
            if (!value || value[0] == L'\0')
                return {};

            const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
            if (requiredSize <= 0)
                return {};

            std::string result(static_cast<size_t>(requiredSize), '\0');
            WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), requiredSize, nullptr, nullptr);
            if (!result.empty() && result.back() == '\0')
                result.pop_back();

            return result;
        }

        std::wstring Utf8ToWide(const std::string& value)
        {
            if (value.empty())
                return {};

            const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
            if (requiredSize <= 0)
                return {};

            std::wstring result(static_cast<size_t>(requiredSize), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), requiredSize);
            if (!result.empty() && result.back() == L'\0')
                result.pop_back();

            return result;
        }

        class NVRHIMessageCallback final : public nvrhi::IMessageCallback
        {
        public:
            void message(nvrhi::MessageSeverity severity, const char* messageText) override
            {
                switch (severity)
                {
                    case nvrhi::MessageSeverity::Info:
                        Log::Info("[NVRHI] ", messageText);
                        break;
                    case nvrhi::MessageSeverity::Warning:
                        Log::Warn("[NVRHI] ", messageText);
                        break;
                    case nvrhi::MessageSeverity::Error:
                    case nvrhi::MessageSeverity::Fatal:
                        Log::Error("[NVRHI] ", messageText);
                        break;
                }
            }
        };

        class NVRHID3D12Buffer final : public Buffer
        {
        public:
            explicit NVRHID3D12Buffer(BufferDescription description)
                : m_Description(std::move(description))
            {
            }

            ~NVRHID3D12Buffer() override
            {
                if (m_MappedData)
                    Unmap();
            }

            bool Initialize(ID3D12Device* device)
            {
                if (!device || m_Description.SizeBytes == 0)
                    return false;

                D3D12_HEAP_PROPERTIES heapProperties {};
                heapProperties.Type = GetHeapType();
                heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                heapProperties.CreationNodeMask = 1;
                heapProperties.VisibleNodeMask = 1;

                D3D12_RESOURCE_DESC bufferDesc {};
                bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bufferDesc.Width = m_Description.SizeBytes;
                bufferDesc.Height = 1;
                bufferDesc.DepthOrArraySize = 1;
                bufferDesc.MipLevels = 1;
                bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
                bufferDesc.SampleDesc.Count = 1;
                bufferDesc.SampleDesc.Quality = 0;
                bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

                const HRESULT result = device->CreateCommittedResource(
                    &heapProperties,
                    D3D12_HEAP_FLAG_NONE,
                    &bufferDesc,
                    GetInitialState(),
                    nullptr,
                    IID_PPV_ARGS(&m_Resource));

                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 RHI buffer '", m_Description.DebugName, "': ", HResultToString(result));
                    return false;
                }

                const std::wstring name = Utf8ToWide(m_Description.DebugName);
                if (!name.empty())
                    m_Resource->SetName(name.c_str());

                m_CurrentState = GetInitialState();
                return true;
            }

            const BufferDescription& GetDescription() const override
            {
                return m_Description;
            }

            void* Map() override
            {
                if (!m_Resource || m_Description.CpuAccess == BufferCpuAccess::None)
                    return nullptr;

                if (m_MappedData)
                    return m_MappedData;

                D3D12_RANGE readRange {};
                if (m_Description.CpuAccess == BufferCpuAccess::Read)
                    readRange.End = static_cast<SIZE_T>(m_Description.SizeBytes);

                const HRESULT result = m_Resource->Map(0, &readRange, &m_MappedData);
                if (FAILED(result))
                {
                    Log::Error("Could not map D3D12 RHI buffer '", m_Description.DebugName, "': ", HResultToString(result));
                    return nullptr;
                }

                return m_MappedData;
            }

            void Unmap() override
            {
                if (!m_Resource || !m_MappedData)
                    return;

                D3D12_RANGE writtenRange {};
                if (m_Description.CpuAccess == BufferCpuAccess::Write)
                    writtenRange.End = static_cast<SIZE_T>(m_Description.SizeBytes);

                m_Resource->Unmap(0, &writtenRange);
                m_MappedData = nullptr;
            }

            ID3D12Resource* GetResource() const
            {
                return m_Resource.Get();
            }

            D3D12_RESOURCE_STATES GetCurrentState() const
            {
                return m_CurrentState;
            }

        private:
            D3D12_HEAP_TYPE GetHeapType() const
            {
                switch (m_Description.CpuAccess)
                {
                    case BufferCpuAccess::Write: return D3D12_HEAP_TYPE_UPLOAD;
                    case BufferCpuAccess::Read: return D3D12_HEAP_TYPE_READBACK;
                    case BufferCpuAccess::None:
                    default: return D3D12_HEAP_TYPE_DEFAULT;
                }
            }

            D3D12_RESOURCE_STATES GetInitialState() const
            {
                switch (m_Description.CpuAccess)
                {
                    case BufferCpuAccess::Write: return D3D12_RESOURCE_STATE_GENERIC_READ;
                    case BufferCpuAccess::Read: return D3D12_RESOURCE_STATE_COPY_DEST;
                    case BufferCpuAccess::None:
                    default: return ConvertResourceState(m_Description.InitialState);
                }
            }

        private:
            BufferDescription m_Description;
            ComPtr<ID3D12Resource> m_Resource;
            void* m_MappedData = nullptr;
            D3D12_RESOURCE_STATES m_CurrentState = D3D12_RESOURCE_STATE_COMMON;
        };

        class NVRHID3D12Texture final : public Texture
        {
        public:
            explicit NVRHID3D12Texture(TextureDescription description)
                : m_Description(std::move(description))
            {
            }

            bool Initialize(ID3D12Device* device)
            {
                if (!device || m_Description.Extent.Width == 0 || m_Description.Extent.Height == 0)
                    return false;

                const DXGI_FORMAT format = ConvertFormat(m_Description.TextureFormat);
                if (format == DXGI_FORMAT_UNKNOWN)
                {
                    Log::Error("Could not create D3D12 RHI texture '", m_Description.DebugName, "': unsupported format");
                    return false;
                }

                D3D12_HEAP_PROPERTIES heapProperties {};
                heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
                heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                heapProperties.CreationNodeMask = 1;
                heapProperties.VisibleNodeMask = 1;

                D3D12_RESOURCE_DESC textureDesc {};
                textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                textureDesc.Width = m_Description.Extent.Width;
                textureDesc.Height = m_Description.Extent.Height;
                textureDesc.DepthOrArraySize = static_cast<UINT16>(m_Description.ArrayLayers);
                textureDesc.MipLevels = static_cast<UINT16>(m_Description.MipLevels);
                textureDesc.Format = format;
                textureDesc.SampleDesc.Count = m_Description.SampleCount == 0 ? 1 : m_Description.SampleCount;
                textureDesc.SampleDesc.Quality = 0;
                textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                textureDesc.Flags = ConvertTextureFlags(m_Description.Usage);

                D3D12_CLEAR_VALUE clearValue {};
                D3D12_CLEAR_VALUE* clearValuePtr = nullptr;
                if (HasTextureUsage(m_Description.Usage, TextureUsage::DepthStencil))
                {
                    clearValue.Format = format;
                    clearValue.DepthStencil.Depth = 1.0f;
                    clearValue.DepthStencil.Stencil = 0;
                    clearValuePtr = &clearValue;
                }
                else if (HasTextureUsage(m_Description.Usage, TextureUsage::RenderTarget))
                {
                    clearValue.Format = format;
                    clearValue.Color[0] = 0.0f;
                    clearValue.Color[1] = 0.0f;
                    clearValue.Color[2] = 0.0f;
                    clearValue.Color[3] = 1.0f;
                    clearValuePtr = &clearValue;
                }

                const HRESULT result = device->CreateCommittedResource(
                    &heapProperties,
                    D3D12_HEAP_FLAG_NONE,
                    &textureDesc,
                    ConvertResourceState(m_Description.InitialState),
                    clearValuePtr,
                    IID_PPV_ARGS(&m_Resource));

                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 RHI texture '", m_Description.DebugName, "': ", HResultToString(result));
                    return false;
                }

                const std::wstring name = Utf8ToWide(m_Description.DebugName);
                if (!name.empty())
                    m_Resource->SetName(name.c_str());

                return true;
            }

            const TextureDescription& GetDescription() const override
            {
                return m_Description;
            }

            ID3D12Resource* GetResource() const
            {
                return m_Resource.Get();
            }

        private:
            TextureDescription m_Description;
            ComPtr<ID3D12Resource> m_Resource;
        };

        class NVRHID3D12Shader final : public Shader
        {
        public:
            explicit NVRHID3D12Shader(ShaderDescription description)
                : m_Description(std::move(description))
            {
            }

            bool Initialize()
            {
                if (m_Description.Stage == ShaderStage::None || m_Description.Source.empty() || m_Description.EntryPoint.empty())
                    return false;

                const std::string targetProfile = m_Description.TargetProfile.empty()
                    ? DefaultTargetProfile(m_Description.Stage)
                    : m_Description.TargetProfile;
                if (targetProfile.empty())
                {
                    Log::Error("Could not compile D3D12 RHI shader '", m_Description.DebugName, "': unsupported shader stage");
                    return false;
                }

                UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(GE_DEBUG)
                flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

                ComPtr<ID3DBlob> errors;
                const char* sourceName = m_Description.SourceName.empty() ? m_Description.DebugName.c_str() : m_Description.SourceName.c_str();
                const HRESULT result = D3DCompile(
                    m_Description.Source.data(),
                    m_Description.Source.size(),
                    sourceName,
                    nullptr,
                    nullptr,
                    m_Description.EntryPoint.c_str(),
                    targetProfile.c_str(),
                    flags,
                    0,
                    &m_Bytecode,
                    &errors);

                if (FAILED(result))
                {
                    if (errors)
                        Log::Error("D3D12 RHI shader compilation failed for '", m_Description.DebugName, "': ", static_cast<const char*>(errors->GetBufferPointer()));
                    else
                        Log::Error("D3D12 RHI shader compilation failed for '", m_Description.DebugName, "': ", HResultToString(result));
                    return false;
                }

                return true;
            }

            const ShaderDescription& GetDescription() const override
            {
                return m_Description;
            }

            const void* GetBytecode() const
            {
                return m_Bytecode ? m_Bytecode->GetBufferPointer() : nullptr;
            }

            u64 GetBytecodeSize() const
            {
                return m_Bytecode ? static_cast<u64>(m_Bytecode->GetBufferSize()) : 0;
            }

        private:
            ShaderDescription m_Description;
            ComPtr<ID3DBlob> m_Bytecode;
        };

        class NVRHID3D12Pipeline final : public Pipeline
        {
        public:
            explicit NVRHID3D12Pipeline(PipelineDescription description)
                : m_Description(std::move(description))
            {
            }

            bool Initialize(ID3D12Device* device)
            {
                if (!device || m_Description.Type != PipelineType::Graphics || !m_Description.VertexShader || !m_Description.PixelShader)
                    return false;

                if (!CreateRootSignature(device))
                    return false;

                return CreateGraphicsPipelineState(device);
            }

            const PipelineDescription& GetDescription() const override
            {
                return m_Description;
            }

            ID3D12RootSignature* GetRootSignature() const
            {
                return m_RootSignature.Get();
            }

            ID3D12PipelineState* GetPipelineState() const
            {
                return m_PipelineState.Get();
            }

        private:
            bool CreateRootSignature(ID3D12Device* device)
            {
                std::vector<D3D12_ROOT_PARAMETER> rootParameters;
                rootParameters.reserve(m_Description.ConstantBufferBindings.size());
                for (const RootConstantBufferBinding& binding : m_Description.ConstantBufferBindings)
                {
                    D3D12_ROOT_PARAMETER rootParameter {};
                    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                    rootParameter.Descriptor.ShaderRegister = binding.ShaderRegister;
                    rootParameter.Descriptor.RegisterSpace = binding.RegisterSpace;
                    rootParameter.ShaderVisibility = ConvertShaderVisibility(binding.Visibility);
                    rootParameters.push_back(rootParameter);
                }

                D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc {};
                rootSignatureDesc.NumParameters = static_cast<UINT>(rootParameters.size());
                rootSignatureDesc.pParameters = rootParameters.empty() ? nullptr : rootParameters.data();
                rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                    | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                    | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                    | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

                ComPtr<ID3DBlob> signatureBlob;
                ComPtr<ID3DBlob> signatureErrors;
                const HRESULT serializeResult = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &signatureErrors);
                if (FAILED(serializeResult))
                {
                    if (signatureErrors)
                        Log::Error("Could not serialize D3D12 RHI root signature '", m_Description.DebugName, "': ", static_cast<const char*>(signatureErrors->GetBufferPointer()));
                    else
                        Log::Error("Could not serialize D3D12 RHI root signature '", m_Description.DebugName, "': ", HResultToString(serializeResult));
                    return false;
                }

                const HRESULT createResult = device->CreateRootSignature(
                    0,
                    signatureBlob->GetBufferPointer(),
                    signatureBlob->GetBufferSize(),
                    IID_PPV_ARGS(&m_RootSignature));
                if (FAILED(createResult))
                {
                    Log::Error("Could not create D3D12 RHI root signature '", m_Description.DebugName, "': ", HResultToString(createResult));
                    return false;
                }

                const std::wstring name = Utf8ToWide(m_Description.DebugName + " Root Signature");
                if (!name.empty())
                    m_RootSignature->SetName(name.c_str());

                return true;
            }

            bool CreateGraphicsPipelineState(ID3D12Device* device)
            {
                const NVRHID3D12ShaderNativeHandles vertexShader = GetNVRHID3D12ShaderNativeHandles(*m_Description.VertexShader);
                const NVRHID3D12ShaderNativeHandles pixelShader = GetNVRHID3D12ShaderNativeHandles(*m_Description.PixelShader);
                if (!vertexShader.Bytecode || vertexShader.BytecodeSize == 0 || !pixelShader.Bytecode || pixelShader.BytecodeSize == 0)
                {
                    Log::Error("Could not create D3D12 RHI pipeline '", m_Description.DebugName, "': shader bytecode is missing");
                    return false;
                }

                std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
                inputElements.reserve(m_Description.VertexInputs.size());
                for (const VertexInputAttribute& input : m_Description.VertexInputs)
                {
                    const DXGI_FORMAT format = ConvertFormat(input.AttributeFormat);
                    if (format == DXGI_FORMAT_UNKNOWN)
                    {
                        Log::Error("Could not create D3D12 RHI pipeline '", m_Description.DebugName, "': unsupported vertex input format");
                        return false;
                    }

                    D3D12_INPUT_ELEMENT_DESC inputElement {};
                    inputElement.SemanticName = input.SemanticName.c_str();
                    inputElement.SemanticIndex = input.SemanticIndex;
                    inputElement.Format = format;
                    inputElement.InputSlot = input.InputSlot;
                    inputElement.AlignedByteOffset = input.OffsetBytes;
                    inputElement.InputSlotClass = ConvertVertexInputRate(input.InputRate);
                    inputElement.InstanceDataStepRate = input.InstanceStepRate;
                    inputElements.push_back(inputElement);
                }

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

                D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc {};
                pipelineDesc.pRootSignature = m_RootSignature.Get();
                pipelineDesc.VS = { vertexShader.Bytecode, static_cast<SIZE_T>(vertexShader.BytecodeSize) };
                pipelineDesc.PS = { pixelShader.Bytecode, static_cast<SIZE_T>(pixelShader.BytecodeSize) };
                pipelineDesc.BlendState.AlphaToCoverageEnable = FALSE;
                pipelineDesc.BlendState.IndependentBlendEnable = FALSE;
                pipelineDesc.BlendState.RenderTarget[0] = renderTargetBlend;
                pipelineDesc.SampleMask = std::numeric_limits<unsigned int>::max();
                pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pipelineDesc.RasterizerState.CullMode = ConvertCullMode(m_Description.RasterCullMode);
                pipelineDesc.RasterizerState.FrontCounterClockwise = FALSE;
                pipelineDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
                pipelineDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
                pipelineDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
                pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
                pipelineDesc.RasterizerState.MultisampleEnable = FALSE;
                pipelineDesc.RasterizerState.AntialiasedLineEnable = FALSE;
                pipelineDesc.RasterizerState.ForcedSampleCount = 0;
                pipelineDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
                pipelineDesc.DepthStencilState.DepthEnable = m_Description.DepthTestEnable ? TRUE : FALSE;
                pipelineDesc.DepthStencilState.DepthWriteMask = m_Description.DepthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
                pipelineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                pipelineDesc.DepthStencilState.StencilEnable = FALSE;
                pipelineDesc.InputLayout = { inputElements.empty() ? nullptr : inputElements.data(), static_cast<UINT>(inputElements.size()) };
                pipelineDesc.PrimitiveTopologyType = ConvertPrimitiveTopologyType(m_Description.Topology);
                pipelineDesc.NumRenderTargets = 1;
                pipelineDesc.RTVFormats[0] = ConvertFormat(m_Description.ColorFormat);
                pipelineDesc.DSVFormat = ConvertFormat(m_Description.DepthFormat);
                pipelineDesc.SampleDesc.Count = 1;
                pipelineDesc.SampleDesc.Quality = 0;

                const HRESULT result = device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&m_PipelineState));
                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 RHI graphics pipeline '", m_Description.DebugName, "': ", HResultToString(result));
                    return false;
                }

                const std::wstring name = Utf8ToWide(m_Description.DebugName);
                if (!name.empty())
                    m_PipelineState->SetName(name.c_str());

                return true;
            }

        private:
            PipelineDescription m_Description;
            ComPtr<ID3D12RootSignature> m_RootSignature;
            ComPtr<ID3D12PipelineState> m_PipelineState;
        };

        class NVRHID3D12CommandList final : public CommandList
        {
        public:
            enum class State
            {
                Initial,
                Recording,
                Closed,
                Submitted,
                Error
            };

            NVRHID3D12CommandList(QueueType queueType, ID3D12GraphicsCommandList* commandList, std::string debugName)
                : m_QueueType(queueType)
                , m_CommandList(commandList)
                , m_DebugName(std::move(debugName))
            {
            }

            NVRHID3D12CommandList(
                QueueType queueType,
                ComPtr<ID3D12CommandAllocator> commandAllocator,
                ComPtr<ID3D12GraphicsCommandList> commandList,
                std::string debugName)
                : m_QueueType(queueType)
                , m_CommandAllocator(std::move(commandAllocator))
                , m_OwnedCommandList(std::move(commandList))
                , m_CommandList(m_OwnedCommandList.Get())
                , m_DebugName(std::move(debugName))
            {
            }

            QueueType GetQueueType() const override
            {
                return m_QueueType;
            }

            bool Begin() override
            {
                if (!m_CommandAllocator || !m_OwnedCommandList || m_State == State::Recording)
                    return false;

                HRESULT result = m_CommandAllocator->Reset();
                if (FAILED(result))
                {
                    Log::Error("Could not reset D3D12 RHI command allocator '", m_DebugName, "': ", HResultToString(result));
                    m_State = State::Error;
                    return false;
                }

                result = m_OwnedCommandList->Reset(m_CommandAllocator.Get(), nullptr);
                if (FAILED(result))
                {
                    Log::Error("Could not begin D3D12 RHI command list '", m_DebugName, "': ", HResultToString(result));
                    m_State = State::Error;
                    return false;
                }

                m_State = State::Recording;
                return true;
            }

            bool End() override
            {
                if (!m_OwnedCommandList || m_State != State::Recording)
                    return false;

                const HRESULT result = m_OwnedCommandList->Close();
                if (FAILED(result))
                {
                    Log::Error("Could not close D3D12 RHI command list '", m_DebugName, "': ", HResultToString(result));
                    m_State = State::Error;
                    return false;
                }

                m_State = State::Closed;
                return true;
            }

            bool IsReadyToSubmit() const
            {
                return m_OwnedCommandList && m_State == State::Closed;
            }

            void MarkSubmitted()
            {
                m_State = State::Submitted;
            }

            ID3D12CommandList* GetNativeCommandList() const
            {
                return m_CommandList;
            }

            void BeginDebugMarker(std::string_view name) override
            {
                (void)name;
            }

            void EndDebugMarker() override
            {
            }

            void SetGraphicsPipeline(Pipeline& pipeline) override
            {
                if (!m_CommandList)
                    return;

                auto* nativePipeline = dynamic_cast<NVRHID3D12Pipeline*>(&pipeline);
                if (!nativePipeline || !nativePipeline->GetRootSignature() || !nativePipeline->GetPipelineState())
                {
                    Log::Error("D3D12 RHI command list could not bind graphics pipeline: ", m_DebugName);
                    return;
                }

                m_CommandList->SetGraphicsRootSignature(nativePipeline->GetRootSignature());
                m_CommandList->SetPipelineState(nativePipeline->GetPipelineState());
                m_CommandList->IASetPrimitiveTopology(ConvertPrimitiveTopology(pipeline.GetDescription().Topology));
            }

            void SetGraphicsConstantBuffer(u32 rootParameterIndex, Buffer& buffer) override
            {
                if (!m_CommandList)
                    return;

                ID3D12Resource* resource = GetD3D12Resource(buffer);
                if (!resource)
                    return;

                m_CommandList->SetGraphicsRootConstantBufferView(rootParameterIndex, resource->GetGPUVirtualAddress());
            }

            void SetViewport(const Viewport& viewport) override
            {
                if (!m_CommandList)
                    return;

                D3D12_VIEWPORT d3dViewport {};
                d3dViewport.TopLeftX = viewport.X;
                d3dViewport.TopLeftY = viewport.Y;
                d3dViewport.Width = viewport.Width;
                d3dViewport.Height = viewport.Height;
                d3dViewport.MinDepth = viewport.MinDepth;
                d3dViewport.MaxDepth = viewport.MaxDepth;
                m_CommandList->RSSetViewports(1, &d3dViewport);
            }

            void SetScissorRect(const ScissorRect& rect) override
            {
                if (!m_CommandList)
                    return;

                D3D12_RECT d3dRect {};
                d3dRect.left = static_cast<LONG>(rect.Left);
                d3dRect.top = static_cast<LONG>(rect.Top);
                d3dRect.right = static_cast<LONG>(rect.Right);
                d3dRect.bottom = static_cast<LONG>(rect.Bottom);
                m_CommandList->RSSetScissorRects(1, &d3dRect);
            }

            void SetVertexBuffer(u32 slot, Buffer& buffer) override
            {
                if (!m_CommandList)
                    return;

                ID3D12Resource* resource = GetD3D12Resource(buffer);
                if (!resource)
                    return;

                const BufferDescription& description = buffer.GetDescription();
                D3D12_VERTEX_BUFFER_VIEW view {};
                view.BufferLocation = resource->GetGPUVirtualAddress();
                view.SizeInBytes = static_cast<UINT>(description.SizeBytes);
                view.StrideInBytes = description.StrideBytes;
                m_CommandList->IASetVertexBuffers(slot, 1, &view);
            }

            void SetIndexBuffer(Buffer& buffer, IndexFormat format) override
            {
                if (!m_CommandList)
                    return;

                ID3D12Resource* resource = GetD3D12Resource(buffer);
                if (!resource)
                    return;

                const BufferDescription& description = buffer.GetDescription();
                D3D12_INDEX_BUFFER_VIEW view {};
                view.BufferLocation = resource->GetGPUVirtualAddress();
                view.SizeInBytes = static_cast<UINT>(description.SizeBytes);
                view.Format = ConvertIndexFormat(format);
                m_CommandList->IASetIndexBuffer(&view);
            }

            bool CopyBuffer(Buffer& destination, u64 destinationOffset, Buffer& source, u64 sourceOffset, u64 sizeBytes) override
            {
                if (!m_CommandList || m_State != State::Recording || sizeBytes == 0)
                    return false;

                auto* nativeDestination = dynamic_cast<NVRHID3D12Buffer*>(&destination);
                auto* nativeSource = dynamic_cast<NVRHID3D12Buffer*>(&source);
                if (!nativeDestination || !nativeSource || !HasBufferUsage(destination.GetDescription().Usage, BufferUsage::CopyDest))
                {
                    Log::Error("D3D12 RHI buffer copy requires D3D12 buffers and CopyDest destination usage: ", m_DebugName);
                    return false;
                }

                const BufferDescription& destinationDescription = destination.GetDescription();
                const BufferDescription& sourceDescription = source.GetDescription();
                if (destinationOffset > destinationDescription.SizeBytes
                    || sourceOffset > sourceDescription.SizeBytes
                    || sizeBytes > destinationDescription.SizeBytes - destinationOffset
                    || sizeBytes > sourceDescription.SizeBytes - sourceOffset)
                {
                    Log::Error("D3D12 RHI buffer copy range is outside the source or destination buffer: ", m_DebugName);
                    return false;
                }

                ID3D12Resource* destinationResource = nativeDestination->GetResource();
                ID3D12Resource* sourceResource = nativeSource->GetResource();
                if (!destinationResource || !sourceResource)
                    return false;

                const D3D12_RESOURCE_STATES previousState = nativeDestination->GetCurrentState();
                if (previousState != D3D12_RESOURCE_STATE_COPY_DEST)
                {
                    D3D12_RESOURCE_BARRIER barrier {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = destinationResource;
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    barrier.Transition.StateBefore = previousState;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    m_CommandList->ResourceBarrier(1, &barrier);
                }

                m_CommandList->CopyBufferRegion(
                    destinationResource,
                    destinationOffset,
                    sourceResource,
                    sourceOffset,
                    sizeBytes);

                if (previousState != D3D12_RESOURCE_STATE_COPY_DEST)
                {
                    D3D12_RESOURCE_BARRIER barrier {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = destinationResource;
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    barrier.Transition.StateAfter = previousState;
                    m_CommandList->ResourceBarrier(1, &barrier);
                }

                return true;
            }

            void DrawIndexed(u32 indexCount, u32 instanceCount, u32 startIndex, int baseVertex, u32 startInstance) override
            {
                if (m_CommandList)
                    m_CommandList->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
            }

            void ResetQueryPool(QueryPool& queryPool, u32 firstQuery, u32 queryCount) override
            {
                (void)queryPool;
                (void)firstQuery;
                (void)queryCount;
            }

            void WriteTimestamp(QueryPool& queryPool, u32 queryIndex) override
            {
                (void)queryPool;
                (void)queryIndex;
            }

            void ResolveQueryPool(QueryPool& queryPool, u32 firstQuery, u32 queryCount) override
            {
                (void)queryPool;
                (void)firstQuery;
                (void)queryCount;
            }

        private:
            ID3D12Resource* GetD3D12Resource(Buffer& buffer) const
            {
                const NVRHID3D12BufferNativeHandles handles = GetNVRHID3D12BufferNativeHandles(buffer);
                ID3D12Resource* resource = static_cast<ID3D12Resource*>(handles.Resource);
                if (!resource)
                    Log::Error("D3D12 RHI command list could not bind buffer: ", buffer.GetDescription().DebugName);
                return resource;
            }

            QueueType m_QueueType = QueueType::Graphics;
            ComPtr<ID3D12CommandAllocator> m_CommandAllocator;
            ComPtr<ID3D12GraphicsCommandList> m_OwnedCommandList;
            ID3D12GraphicsCommandList* m_CommandList = nullptr;
            std::string m_DebugName;
            State m_State = State::Initial;
        };

        class NVRHIQueryPoolStub final : public QueryPool
        {
        public:
            explicit NVRHIQueryPoolStub(QueryPoolDescription description)
                : m_Description(std::move(description))
            {
            }

            const QueryPoolDescription& GetDescription() const override
            {
                return m_Description;
            }

            QueryResult ReadResult(u32 queryIndex) const override
            {
                (void)queryIndex;
                return {};
            }

        private:
            QueryPoolDescription m_Description;
        };

        class NVRHID3D12Device final : public Device
        {
        public:
            explicit NVRHID3D12Device(DeviceDescription description)
                : m_Description(std::move(description))
            {
            }

            ~NVRHID3D12Device() override
            {
                WaitIdle();
                if (m_SubmissionFenceEvent)
                    CloseHandle(m_SubmissionFenceEvent);
            }

            bool Initialize(NVRHIAdapterInfo& adapterInfo)
            {
                EnableDebugLayer();

                if (!CreateFactory())
                    return false;

                if (!ChooseAdapter())
                    return false;

                if (!CreateNativeDevice())
                    return false;

                if (!CreateQueues())
                    return false;

                if (!CreateSubmissionFence())
                    return false;

                if (!CreateNVRHIDevice())
                    return false;

                QueryCapabilities();

                adapterInfo.Available = true;
                adapterInfo.HasNativeDevice = true;
                adapterInfo.AdapterName = m_AdapterName;
                adapterInfo.NativeBackendName = "D3D12";

                Log::Info("NVRHI D3D12 device created on adapter: ", m_AdapterName,
                    " (type=", ToString(m_Capabilities.Identity.Type),
                    ", vendor=", m_Capabilities.Identity.VendorId,
                    ", device=", m_Capabilities.Identity.DeviceId,
                    ", qualification=", ToString(m_Capabilities.Qualification), ")");
                for (u32 featureIndex = 0; featureIndex < static_cast<u32>(DeviceFeature::Count); ++featureIndex)
                {
                    const DeviceFeature feature = static_cast<DeviceFeature>(featureIndex);
                    const CapabilityState& state = m_Capabilities.GetFeature(feature);
                    Log::Info("D3D12 capability state: ", ToString(feature),
                        " advertised=", state.Advertised ? "yes" : "no",
                        ", enabled=", state.Enabled ? "yes" : "no",
                        ", implemented=", state.Implemented ? "yes" : "no",
                        ", exercised=", state.Exercised ? "yes" : "no",
                        state.Detail.empty() ? "" : ", detail=", state.Detail);
                }
                for (const std::string& fallback : m_Capabilities.Fallbacks)
                    Log::Info("D3D12 capability fallback: ", fallback);

                return true;
            }

            const DeviceDescription& GetDescription() const override { return m_Description; }
            const DeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }

            Scope<Buffer> CreateBuffer(const BufferDescription& description) override
            {
                Scope<NVRHID3D12Buffer> buffer = CreateScope<NVRHID3D12Buffer>(description);
                if (!buffer->Initialize(m_Device.Get()))
                    return nullptr;

                return buffer;
            }

            Scope<Texture> CreateTexture(const TextureDescription& description) override
            {
                Scope<NVRHID3D12Texture> texture = CreateScope<NVRHID3D12Texture>(description);
                if (!texture->Initialize(m_Device.Get()))
                    return nullptr;

                return texture;
            }

            Scope<Shader> CreateShader(const ShaderDescription& description) override
            {
                Scope<NVRHID3D12Shader> shader = CreateScope<NVRHID3D12Shader>(description);
                if (!shader->Initialize())
                    return nullptr;

                return shader;
            }

            Scope<Pipeline> CreatePipeline(const PipelineDescription& description) override
            {
                Scope<NVRHID3D12Pipeline> pipeline = CreateScope<NVRHID3D12Pipeline>(description);
                if (!pipeline->Initialize(m_Device.Get()))
                    return nullptr;

                return pipeline;
            }

            Scope<QueryPool> CreateQueryPool(const QueryPoolDescription& description) override
            {
                if (description.Count == 0)
                {
                    Log::Warn("NVRHI D3D12 query pool requested with zero query slots");
                    return nullptr;
                }

                Log::Warn("NVRHI D3D12 query heap resolve is not implemented yet; creating CPU-visible query pool stub: ", description.DebugName);
                return CreateScope<NVRHIQueryPoolStub>(description);
            }

            Scope<CommandList> CreateCommandList(QueueType queueType, std::string_view debugName) override
            {
                const D3D12_COMMAND_LIST_TYPE commandListType = ConvertCommandListType(queueType);
                ComPtr<ID3D12CommandAllocator> allocator;
                HRESULT result = m_Device->CreateCommandAllocator(commandListType, IID_PPV_ARGS(&allocator));
                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 RHI command allocator '", debugName, "': ", HResultToString(result));
                    return nullptr;
                }

                ComPtr<ID3D12GraphicsCommandList> commandList;
                result = m_Device->CreateCommandList(0, commandListType, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 RHI command list '", debugName, "': ", HResultToString(result));
                    return nullptr;
                }

                const std::wstring name = Utf8ToWide(std::string(debugName));
                if (!name.empty())
                {
                    allocator->SetName((name + L" Allocator").c_str());
                    commandList->SetName(name.c_str());
                }

                result = commandList->Close();
                if (FAILED(result))
                {
                    Log::Error("Could not initialize D3D12 RHI command list '", debugName, "': ", HResultToString(result));
                    return nullptr;
                }

                return CreateScope<NVRHID3D12CommandList>(queueType, std::move(allocator), std::move(commandList), std::string(debugName));
            }

            bool UploadBuffer(Buffer& destination, const void* sourceData, u64 sizeBytes, u64 destinationOffset) override
            {
                if (!sourceData || sizeBytes == 0 || destination.GetDescription().CpuAccess != BufferCpuAccess::None
                    || !HasBufferUsage(destination.GetDescription().Usage, BufferUsage::CopyDest))
                    return false;

                if (destinationOffset > destination.GetDescription().SizeBytes
                    || sizeBytes > destination.GetDescription().SizeBytes - destinationOffset)
                    return false;

                BufferDescription uploadDescription;
                uploadDescription.DebugName = destination.GetDescription().DebugName + " Upload Staging";
                uploadDescription.SizeBytes = sizeBytes;
                uploadDescription.Usage = BufferUsage::CopySource;
                uploadDescription.CpuAccess = BufferCpuAccess::Write;
                Scope<Buffer> uploadBuffer = CreateBuffer(uploadDescription);
                if (!uploadBuffer)
                    return false;

                void* mappedData = uploadBuffer->Map();
                if (!mappedData)
                    return false;
                std::memcpy(mappedData, sourceData, static_cast<size_t>(sizeBytes));
                uploadBuffer->Unmap();

                Scope<CommandList> commandList = CreateCommandList(QueueType::Copy, uploadDescription.DebugName);
                if (!commandList || !commandList->Begin()
                    || !commandList->CopyBuffer(destination, destinationOffset, *uploadBuffer, 0, sizeBytes)
                    || !commandList->End())
                    return false;

                return SubmitAndWait(*commandList);
            }

            bool SubmitAndWait(CommandList& commandList) override
            {
                auto* nativeCommandList = dynamic_cast<NVRHID3D12CommandList*>(&commandList);
                if (!nativeCommandList || !nativeCommandList->IsReadyToSubmit())
                {
                    Log::Error("D3D12 RHI command list submission requires a closed, device-owned command list");
                    return false;
                }

                ID3D12CommandQueue* queue = GetQueue(nativeCommandList->GetQueueType());
                ID3D12CommandList* nativeLists[] = { nativeCommandList->GetNativeCommandList() };
                if (!queue || !nativeLists[0])
                {
                    Log::Error("D3D12 RHI command list submission received incomplete native objects");
                    return false;
                }

                queue->ExecuteCommandLists(1, nativeLists);
                nativeCommandList->MarkSubmitted();
                return WaitForQueueSubmission(queue);
            }

            void WaitIdle() override
            {
                if (m_NVRHIDevice.Get() && !m_NVRHIDevice->waitForIdle())
                    Log::Warn("NVRHI D3D12 waitForIdle reported a device problem");
            }

            NVRHID3D12NativeHandles GetNativeHandles() const
            {
                NVRHID3D12NativeHandles handles;
                handles.Factory = m_Factory.Get();
                handles.Device = m_Device.Get();
                handles.GraphicsQueue = m_GraphicsQueue.Get();
                handles.ComputeQueue = m_ComputeQueue.Get();
                handles.CopyQueue = m_CopyQueue.Get();
                handles.NVRHIDevice = m_NVRHIDevice.Get();
                return handles;
            }

        private:
            void EnableDebugLayer()
            {
                if (!m_Description.EnableValidation)
                    return;

                ComPtr<ID3D12Debug> debug;
                const HRESULT result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
                if (FAILED(result))
                {
                    Log::Warn("D3D12 debug layer unavailable: ", HResultToString(result));
                    return;
                }

                debug->EnableDebugLayer();
                m_DebugLayerEnabled = true;
            }

            bool CreateFactory()
            {
                UINT flags = 0;
                if (m_DebugLayerEnabled)
                    flags |= DXGI_CREATE_FACTORY_DEBUG;

                HRESULT result = CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_Factory));
                if (FAILED(result) && flags != 0)
                {
                    Log::Warn("DXGI debug factory unavailable, retrying without debug flag: ", HResultToString(result));
                    result = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_Factory));
                }

                if (FAILED(result))
                {
                    Log::Error("Could not create DXGI factory: ", HResultToString(result));
                    return false;
                }

                return true;
            }

            bool ChooseAdapter()
            {
                ComPtr<IDXGIFactory6> factory6;
                if (SUCCEEDED(m_Factory.As(&factory6)))
                {
                    for (UINT index = 0;; ++index)
                    {
                        ComPtr<IDXGIAdapter1> adapter;
                        if (factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
                            break;

                        if (AcceptAdapter(adapter.Get(), false))
                            return true;
                    }
                }

                for (UINT index = 0;; ++index)
                {
                    ComPtr<IDXGIAdapter1> adapter;
                    if (m_Factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
                        break;

                    if (AcceptAdapter(adapter.Get(), false))
                        return true;
                }

                ComPtr<IDXGIAdapter> warpAdapter;
                const HRESULT warpResult = m_Factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
                if (SUCCEEDED(warpResult))
                {
                    ComPtr<IDXGIAdapter1> warpAdapter1;
                    if (SUCCEEDED(warpAdapter.As(&warpAdapter1)) && AcceptAdapter(warpAdapter1.Get(), true))
                    {
                        Log::Warn("No hardware D3D12 adapter accepted; using WARP software adapter");
                        return true;
                    }
                }

                Log::Error("No hardware or WARP adapter supports the engine D3D12 minimum feature level");
                return false;
            }

            bool AcceptAdapter(IDXGIAdapter1* adapter, bool allowSoftware)
            {
                DXGI_ADAPTER_DESC1 description {};
                adapter->GetDesc1(&description);

                if (!allowSoftware && (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                    return false;

                if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr)))
                    return false;

                m_Adapter = adapter;
                m_AdapterName = WideToUtf8(description.Description);
                m_AdapterVendorId = description.VendorId;
                m_AdapterDeviceId = description.DeviceId;
                m_AdapterDedicatedVideoMemoryBytes = static_cast<u64>(description.DedicatedVideoMemory);
                m_AdapterFlags = description.Flags;

                LARGE_INTEGER driverVersion {};
                if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion)))
                {
                    m_AdapterDriverVersion = std::to_string(HIWORD(driverVersion.HighPart)) + "."
                        + std::to_string(LOWORD(driverVersion.HighPart)) + "."
                        + std::to_string(HIWORD(driverVersion.LowPart)) + "."
                        + std::to_string(LOWORD(driverVersion.LowPart));
                }
                return true;
            }

            bool CreateNativeDevice()
            {
                const HRESULT result = D3D12CreateDevice(m_Adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_Device));
                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 device: ", HResultToString(result));
                    return false;
                }

                m_Device->SetName(L"Spiral D3D12 Device");
                return true;
            }

            bool CreateQueue(D3D12_COMMAND_LIST_TYPE type, const wchar_t* name, ID3D12CommandQueue** queue)
            {
                D3D12_COMMAND_QUEUE_DESC description {};
                description.Type = type;
                description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
                description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
                description.NodeMask = 0;

                const HRESULT result = m_Device->CreateCommandQueue(&description, IID_PPV_ARGS(queue));
                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 command queue: ", HResultToString(result));
                    return false;
                }

                (*queue)->SetName(name);
                return true;
            }

            bool CreateQueues()
            {
                return CreateQueue(D3D12_COMMAND_LIST_TYPE_DIRECT, L"Spiral Graphics Queue", &m_GraphicsQueue)
                    && CreateQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE, L"Spiral Compute Queue", &m_ComputeQueue)
                    && CreateQueue(D3D12_COMMAND_LIST_TYPE_COPY, L"Spiral Copy Queue", &m_CopyQueue);
            }

            ID3D12CommandQueue* GetQueue(QueueType queueType) const
            {
                switch (queueType)
                {
                    case QueueType::Graphics: return m_GraphicsQueue.Get();
                    case QueueType::Compute: return m_ComputeQueue.Get();
                    case QueueType::Copy: return m_CopyQueue.Get();
                }

                return nullptr;
            }

            bool CreateNVRHIDevice()
            {
                nvrhi::d3d12::DeviceDesc description {};
                description.errorCB = &m_MessageCallback;
                description.pDevice = m_Device.Get();
                description.pGraphicsCommandQueue = m_GraphicsQueue.Get();
                description.pComputeCommandQueue = m_ComputeQueue.Get();
                description.pCopyCommandQueue = m_CopyQueue.Get();
                description.enableHeapDirectlyIndexed = true;
                description.enableEnhancedBarriers = true;

                nvrhi::d3d12::DeviceHandle nativeDevice = nvrhi::d3d12::createDevice(description);
                if (!nativeDevice.Get())
                {
                    Log::Error("Could not create NVRHI D3D12 device");
                    return false;
                }

                m_NVRHIDevice = nativeDevice;
                if (m_Description.EnableValidation)
                {
                    nvrhi::DeviceHandle validationDevice = nvrhi::validation::createValidationLayer(m_NVRHIDevice.Get());
                    if (validationDevice.Get())
                        m_NVRHIDevice = validationDevice;
                }

                return true;
            }

            bool CreateSubmissionFence()
            {
                const HRESULT result = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_SubmissionFence));
                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 RHI submission fence: ", HResultToString(result));
                    return false;
                }

                m_SubmissionFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (!m_SubmissionFenceEvent)
                {
                    Log::Error("Could not create D3D12 RHI submission-fence event");
                    return false;
                }

                return true;
            }

            bool WaitForQueueSubmission(ID3D12CommandQueue* queue)
            {
                if (!queue || !m_SubmissionFence || !m_SubmissionFenceEvent)
                    return false;

                const u64 fenceValue = m_NextSubmissionFenceValue++;
                const HRESULT signalResult = queue->Signal(m_SubmissionFence.Get(), fenceValue);
                if (FAILED(signalResult))
                {
                    Log::Error("Could not signal D3D12 RHI submission fence: ", HResultToString(signalResult));
                    return false;
                }

                if (m_SubmissionFence->GetCompletedValue() >= fenceValue)
                    return true;

                const HRESULT eventResult = m_SubmissionFence->SetEventOnCompletion(fenceValue, m_SubmissionFenceEvent);
                if (FAILED(eventResult))
                {
                    Log::Error("Could not wait for D3D12 RHI submission fence: ", HResultToString(eventResult));
                    return false;
                }

                if (WaitForSingleObject(m_SubmissionFenceEvent, INFINITE) != WAIT_OBJECT_0)
                {
                    Log::Error("D3D12 RHI submission fence wait did not complete");
                    return false;
                }

                return true;
            }

            void QueryCapabilities()
            {
                m_Capabilities.ActiveBackend = Backend::NVRHID3D12;
                m_Capabilities.ProfileName = "Phase 3 D3D12 Bootstrap V1";
                m_Capabilities.Qualification = QualificationLevel::Bootstrap;
                m_Capabilities.Identity.Name = m_AdapterName;
                m_Capabilities.Identity.DriverVersion = m_AdapterDriverVersion;
                m_Capabilities.Identity.VendorId = m_AdapterVendorId;
                m_Capabilities.Identity.DeviceId = m_AdapterDeviceId;
                m_Capabilities.Identity.DedicatedVideoMemoryBytes = m_AdapterDedicatedVideoMemoryBytes;

                if ((m_AdapterFlags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
                {
                    m_Capabilities.Identity.Type = AdapterType::Software;
                    m_Capabilities.Fallbacks.emplace_back(
                        "No hardware adapter met the D3D12 bootstrap minimum; selected the WARP software adapter");
                }
                else if ((m_AdapterFlags & DXGI_ADAPTER_FLAG_REMOTE) != 0)
                    m_Capabilities.Identity.Type = AdapterType::Virtual;
                else
                {
                    D3D12_FEATURE_DATA_ARCHITECTURE architecture {};
                    architecture.NodeIndex = 0;
                    m_Capabilities.Identity.Type = SUCCEEDED(m_Device->CheckFeatureSupport(
                        D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture))) && architecture.UMA
                        ? AdapterType::Integrated
                        : AdapterType::Discrete;
                }

                m_Capabilities.Queues.Graphics = m_GraphicsQueue.Get() != nullptr;
                m_Capabilities.Queues.Compute = m_ComputeQueue.Get() != nullptr;
                m_Capabilities.Queues.Copy = m_CopyQueue.Get() != nullptr;
                m_Capabilities.Queues.Present = m_GraphicsQueue.Get() != nullptr;
                m_Capabilities.Queues.DedicatedCompute = m_ComputeQueue.Get() != nullptr;
                m_Capabilities.Queues.DedicatedCopy = m_CopyQueue.Get() != nullptr;

                const bool rayTracingAdvertised =
                    m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct)
                    && m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline);
                const bool meshShadersAdvertised = m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::Meshlets);
                const bool neuralShadersAdvertised =
                    m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::CooperativeVectorInferencing);

                m_Capabilities.GetFeature(DeviceFeature::RayTracing) = MakeCapabilityState(
                    rayTracingAdvertised, false, false, false,
                    "Reported by NVRHI; no engine ray-tracing path is enabled or implemented");
                m_Capabilities.GetFeature(DeviceFeature::MeshShaders) = MakeCapabilityState(
                    meshShadersAdvertised, false, false, false,
                    "Reported by NVRHI; the renderer retains the future indexed-indirect baseline");
                m_Capabilities.GetFeature(DeviceFeature::WorkGraphs) = MakeCapabilityState(
                    false, false, false, false,
                    "Not queried or enabled by the Phase 3 D3D12 bootstrap profile");
                m_Capabilities.GetFeature(DeviceFeature::NeuralShaders) = MakeCapabilityState(
                    neuralShadersAdvertised, false, false, false,
                    "Reported by NVRHI; no engine neural-shader path is enabled or implemented");
                m_Capabilities.GetFeature(DeviceFeature::Timestamps) = MakeCapabilityState(
                    true, false, false, false,
                    "Native D3D12 timestamp queries are advertised, but RHI recording and resolve remain a stub");
                m_Capabilities.GetFeature(DeviceFeature::TimelineSynchronization) = MakeCapabilityState(
                    true, true, true, false,
                    "D3D12 fence synchronization backs synchronous RHI queue submission; runtime exercise is reported separately");
                m_Capabilities.GetFeature(DeviceFeature::BufferDeviceAddress) = MakeCapabilityState(
                    false, false, false, false,
                    "Not required or queried by the Phase 3 D3D12 bootstrap profile");
            }

        private:
            DeviceDescription m_Description;
            DeviceCapabilities m_Capabilities;
            NVRHIMessageCallback m_MessageCallback;
            bool m_DebugLayerEnabled = false;
            std::string m_AdapterName = "Unknown Adapter";
            std::string m_AdapterDriverVersion;
            u32 m_AdapterVendorId = 0;
            u32 m_AdapterDeviceId = 0;
            u64 m_AdapterDedicatedVideoMemoryBytes = 0;
            u32 m_AdapterFlags = 0;

            ComPtr<IDXGIFactory4> m_Factory;
            ComPtr<IDXGIAdapter1> m_Adapter;
            ComPtr<ID3D12Device> m_Device;
            ComPtr<ID3D12CommandQueue> m_GraphicsQueue;
            ComPtr<ID3D12CommandQueue> m_ComputeQueue;
            ComPtr<ID3D12CommandQueue> m_CopyQueue;
            ComPtr<ID3D12Fence> m_SubmissionFence;
            HANDLE m_SubmissionFenceEvent = nullptr;
            u64 m_NextSubmissionFenceValue = 1;
            nvrhi::DeviceHandle m_NVRHIDevice;
        };
    }
#endif

    Scope<Device> CreateNVRHID3D12Device(DeviceDescription description, NVRHIAdapterInfo& adapterInfo, NVRHID3D12NativeHandles* nativeHandles)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        Scope<NVRHID3D12Device> device = CreateScope<NVRHID3D12Device>(std::move(description));
        if (!device->Initialize(adapterInfo))
            return nullptr;

        if (nativeHandles)
            *nativeHandles = device->GetNativeHandles();

        return device;
#else
        (void)description;
        (void)adapterInfo;
        (void)nativeHandles;
        return nullptr;
#endif
    }

    NVRHID3D12BufferNativeHandles GetNVRHID3D12BufferNativeHandles(Buffer& buffer)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        auto* nativeBuffer = dynamic_cast<NVRHID3D12Buffer*>(&buffer);
        if (!nativeBuffer)
            return {};

        NVRHID3D12BufferNativeHandles handles;
        handles.Resource = nativeBuffer->GetResource();
        return handles;
#else
        (void)buffer;
        return {};
#endif
    }

    NVRHID3D12TextureNativeHandles GetNVRHID3D12TextureNativeHandles(Texture& texture)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        auto* nativeTexture = dynamic_cast<NVRHID3D12Texture*>(&texture);
        if (!nativeTexture)
            return {};

        NVRHID3D12TextureNativeHandles handles;
        handles.Resource = nativeTexture->GetResource();
        return handles;
#else
        (void)texture;
        return {};
#endif
    }

    NVRHID3D12ShaderNativeHandles GetNVRHID3D12ShaderNativeHandles(Shader& shader)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        auto* nativeShader = dynamic_cast<NVRHID3D12Shader*>(&shader);
        if (!nativeShader)
            return {};

        NVRHID3D12ShaderNativeHandles handles;
        handles.Bytecode = nativeShader->GetBytecode();
        handles.BytecodeSize = nativeShader->GetBytecodeSize();
        return handles;
#else
        (void)shader;
        return {};
#endif
    }

    Scope<CommandList> WrapNVRHID3D12CommandList(QueueType queueType, void* nativeCommandList, std::string_view debugName)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        auto* d3dCommandList = static_cast<ID3D12GraphicsCommandList*>(nativeCommandList);
        if (!d3dCommandList)
            return nullptr;

        return CreateScope<NVRHID3D12CommandList>(queueType, d3dCommandList, std::string(debugName));
#else
        (void)queueType;
        (void)nativeCommandList;
        (void)debugName;
        return nullptr;
#endif
    }
}
