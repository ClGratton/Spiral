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
                (void)description;
                Log::Warn("NVRHI D3D12 buffer creation is not implemented yet");
                return nullptr;
            }

            Scope<Texture> CreateTexture(const TextureDescription& description) override
            {
                (void)description;
                Log::Warn("NVRHI D3D12 texture creation is not implemented yet");
                return nullptr;
            }

            Scope<Shader> CreateShader(const ShaderDescription& description) override
            {
                (void)description;
                Log::Warn("NVRHI D3D12 shader creation is not implemented yet");
                return nullptr;
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
}
