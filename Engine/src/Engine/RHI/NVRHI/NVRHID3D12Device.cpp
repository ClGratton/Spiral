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

    #include <iomanip>
    #include <sstream>
    #include <string>
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

                if (!CreateNVRHIDevice())
                    return false;

                QueryCapabilities();

                adapterInfo.Available = true;
                adapterInfo.HasNativeDevice = true;
                adapterInfo.AdapterName = m_AdapterName;
                adapterInfo.NativeBackendName = "D3D12";

                Log::Info("NVRHI D3D12 device created on adapter: ", m_AdapterName);
                Log::Info("D3D12 capabilities: ray tracing=", m_Capabilities.SupportsRayTracing ? "yes" : "no",
                    ", mesh shaders=", m_Capabilities.SupportsMeshShaders ? "yes" : "no",
                    ", cooperative vectors=", m_Capabilities.SupportsNeuralShaders ? "yes" : "no");

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
                (void)description;
                Log::Warn("NVRHI D3D12 pipeline creation is not implemented yet");
                return nullptr;
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
                (void)queueType;
                (void)debugName;
                Log::Warn("NVRHI D3D12 command list creation is not implemented yet");
                return nullptr;
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

            void QueryCapabilities()
            {
                m_Capabilities.ActiveBackend = Backend::NVRHID3D12;
                m_Capabilities.SupportsRayTracing =
                    m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::RayTracingAccelStruct)
                    && m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline);
                m_Capabilities.SupportsMeshShaders = m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::Meshlets);
                m_Capabilities.SupportsWorkGraphs = false;
                m_Capabilities.SupportsNeuralShaders = m_NVRHIDevice->queryFeatureSupport(nvrhi::Feature::CooperativeVectorInferencing);
                m_Capabilities.SupportsTimestamps = true;
            }

        private:
            DeviceDescription m_Description;
            DeviceCapabilities m_Capabilities;
            NVRHIMessageCallback m_MessageCallback;
            bool m_DebugLayerEnabled = false;
            std::string m_AdapterName = "Unknown Adapter";

            ComPtr<IDXGIFactory4> m_Factory;
            ComPtr<IDXGIAdapter1> m_Adapter;
            ComPtr<ID3D12Device> m_Device;
            ComPtr<ID3D12CommandQueue> m_GraphicsQueue;
            ComPtr<ID3D12CommandQueue> m_ComputeQueue;
            ComPtr<ID3D12CommandQueue> m_CopyQueue;
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
}
