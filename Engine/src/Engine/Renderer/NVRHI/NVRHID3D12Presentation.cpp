#include "Engine/Renderer/NVRHI/NVRHID3D12Presentation.h"

#include "Engine/Core/Log.h"
#include "Engine/Renderer/NVRHI/D3D12DebugMarkers.h"
#include "Engine/Renderer/NVRHI/NVRHID3D12ViewportSceneRenderer.h"

#if defined(GE_HAS_NVRHI_D3D12)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef GLFW_EXPOSE_NATIVE_WIN32
        #define GLFW_EXPOSE_NATIVE_WIN32
    #endif

    #include <Windows.h>
    #include <GLFW/glfw3.h>
    #include <GLFW/glfw3native.h>
    #include <dxgi1_6.h>
    #include <wrl/client.h>

    #include <directx/d3d12.h>
    #include <backends/imgui_impl_dx12.h>

    #include <array>
    #include <filesystem>
    #include <fstream>
    #include <limits>
    #include <sstream>
    #include <string>
    #include <vector>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_D3D12)
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr u32 kFrameCount = 2;
        constexpr u32 kSrvDescriptorCount = 256;
        constexpr u32 kInvalidDescriptorIndex = std::numeric_limits<u32>::max();
        constexpr DXGI_FORMAT kSwapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

        const wchar_t* GetBackBufferName(u32 index)
        {
            switch (index)
            {
                case 0: return L"Spiral Swapchain Back Buffer 0";
                case 1: return L"Spiral Swapchain Back Buffer 1";
                default: return L"Spiral Swapchain Back Buffer";
            }
        }

        const wchar_t* GetCommandAllocatorName(u32 index)
        {
            switch (index)
            {
                case 0: return L"Spiral Presentation Command Allocator 0";
                case 1: return L"Spiral Presentation Command Allocator 1";
                default: return L"Spiral Presentation Command Allocator";
            }
        }

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

        RHI::TextureUsage TextureUsageFlags(RHI::TextureUsage first, RHI::TextureUsage second, RHI::TextureUsage third = RHI::TextureUsage::None)
        {
            return static_cast<RHI::TextureUsage>(static_cast<u32>(first) | static_cast<u32>(second) | static_cast<u32>(third));
        }

        void WriteU16(std::ofstream& file, u16 value)
        {
            const char bytes[2] = {
                static_cast<char>(value & 0xFFu),
                static_cast<char>((value >> 8u) & 0xFFu)
            };
            file.write(bytes, sizeof(bytes));
        }

        void WriteU32(std::ofstream& file, u32 value)
        {
            const char bytes[4] = {
                static_cast<char>(value & 0xFFu),
                static_cast<char>((value >> 8u) & 0xFFu),
                static_cast<char>((value >> 16u) & 0xFFu),
                static_cast<char>((value >> 24u) & 0xFFu)
            };
            file.write(bytes, sizeof(bytes));
        }

        bool WriteBmp(const std::filesystem::path& path, const u8* rgbaPixels, u32 width, u32 height, u32 rowPitch)
        {
            if (!rgbaPixels || width == 0 || height == 0 || rowPitch < width * 4)
                return false;

            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path());

            std::ofstream file(path, std::ios::binary);
            if (!file)
                return false;

            constexpr u32 fileHeaderSize = 14;
            constexpr u32 infoHeaderSize = 40;
            constexpr u32 bytesPerPixel = 4;
            const u32 imageSize = width * height * bytesPerPixel;
            const u32 fileSize = fileHeaderSize + infoHeaderSize + imageSize;

            file.put('B');
            file.put('M');
            WriteU32(file, fileSize);
            WriteU16(file, 0);
            WriteU16(file, 0);
            WriteU32(file, fileHeaderSize + infoHeaderSize);

            WriteU32(file, infoHeaderSize);
            WriteU32(file, width);
            WriteU32(file, static_cast<u32>(-static_cast<int>(height)));
            WriteU16(file, 1);
            WriteU16(file, bytesPerPixel * 8);
            WriteU32(file, 0);
            WriteU32(file, imageSize);
            WriteU32(file, 2835);
            WriteU32(file, 2835);
            WriteU32(file, 0);
            WriteU32(file, 0);

            std::vector<u8> bgraRow(width * bytesPerPixel);
            for (u32 y = 0; y < height; ++y)
            {
                const u8* sourceRow = rgbaPixels + static_cast<size_t>(y) * rowPitch;
                for (u32 x = 0; x < width; ++x)
                {
                    bgraRow[x * 4 + 0] = sourceRow[x * 4 + 2];
                    bgraRow[x * 4 + 1] = sourceRow[x * 4 + 1];
                    bgraRow[x * 4 + 2] = sourceRow[x * 4 + 0];
                    bgraRow[x * 4 + 3] = sourceRow[x * 4 + 3];
                }
                file.write(reinterpret_cast<const char*>(bgraRow.data()), static_cast<std::streamsize>(bgraRow.size()));
            }

            return file.good();
        }

        struct FrameContext
        {
            ComPtr<ID3D12CommandAllocator> CommandAllocator;
            u64 FenceValue = 0;
        };
    }

    struct NVRHID3D12Presentation::Impl
    {
        bool Initialize(void* nativeWindow, RHI::Device* rhiDevice, const RHI::NVRHID3D12NativeHandles& nativeHandles, u32 width, u32 height)
        {
            m_Window = static_cast<GLFWwindow*>(nativeWindow);
            m_RHIDevice = rhiDevice;
            m_Factory = static_cast<IDXGIFactory4*>(nativeHandles.Factory);
            m_Device = static_cast<ID3D12Device*>(nativeHandles.Device);
            m_GraphicsQueue = static_cast<ID3D12CommandQueue*>(nativeHandles.GraphicsQueue);

            if (!m_Window || !m_RHIDevice || !m_Factory || !m_Device || !m_GraphicsQueue)
            {
                Log::Error("D3D12 presentation received incomplete native handles");
                return false;
            }

            if (!CreateDescriptorHeaps())
                return false;

            if (!CreateSwapchain(width, height))
                return false;

            if (!CreateCommandObjects())
                return false;

            if (!CreateFence())
                return false;

            if (!m_ViewportSceneRenderer.Initialize(m_Device, m_RHIDevice))
                return false;

            if (!InitializeImGui())
                return false;

            m_Initialized = true;
            Log::Info("D3D12 presentation initialized (", m_SwapchainWidth, "x", m_SwapchainHeight, ")");
            return true;
        }

        void Shutdown()
        {
            if (!m_Initialized)
                return;

            WaitIdle();
            ImGui_ImplDX12_Shutdown();
            ReleaseViewportTexture();
            m_ViewportSceneRenderer.Shutdown();
            ReleaseBackBuffers();

            if (m_FenceEvent)
            {
                CloseHandle(m_FenceEvent);
                m_FenceEvent = nullptr;
            }

            m_CommandList.Reset();
            for (FrameContext& frame : m_Frames)
                frame.CommandAllocator.Reset();

            m_Swapchain.Reset();
            m_RtvHeap.Reset();
            m_DsvHeap.Reset();
            m_SrvHeap.Reset();
            m_SrvAllocated.clear();
            m_RHIDevice = nullptr;
            m_Initialized = false;
        }

        bool IsInitialized() const
        {
            return m_Initialized;
        }

        void BeginImGuiFrame()
        {
            if (m_Initialized)
                ImGui_ImplDX12_NewFrame();
        }

        void RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height)
        {
            if (!m_Initialized || !drawData || width == 0 || height == 0)
                return;

            if (!ResizeSwapchainIfNeeded(width, height))
                return;

            m_LastClearColor = clearColor;
            m_FrameIndex = m_Swapchain->GetCurrentBackBufferIndex();
            FrameContext& frame = m_Frames[m_FrameIndex];
            WaitForFenceValue(frame.FenceValue);
            frame.FenceValue = 0;

            HRESULT result = frame.CommandAllocator->Reset();
            if (FAILED(result))
            {
                Log::Error("Could not reset D3D12 command allocator: ", HResultToString(result));
                return;
            }

            result = m_CommandList->Reset(frame.CommandAllocator.Get(), nullptr);
            if (FAILED(result))
            {
                Log::Error("Could not reset D3D12 command list: ", HResultToString(result));
                return;
            }

            {
                ScopedD3D12Marker frameMarker(m_CommandList.Get(), "Spiral Editor Frame");

                {
                    ScopedD3D12Marker viewportMarker(m_CommandList.Get(), "Editor Viewport Texture");
                    RenderViewportTexture(clearColor);
                }

                {
                    ScopedD3D12Marker backBufferMarker(m_CommandList.Get(), "Backbuffer Clear");
                    D3D12_RESOURCE_BARRIER presentToRender = TransitionBarrier(m_BackBuffers[m_FrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    m_CommandList->ResourceBarrier(1, &presentToRender);

                    const D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv = GetRtvCpuHandle(m_FrameIndex);
                    const float clear[4] = { 0.10f, 0.11f, 0.12f, 1.0f };
                    m_CommandList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
                    m_CommandList->ClearRenderTargetView(backBufferRtv, clear, 0, nullptr);
                }

                {
                    ScopedD3D12Marker imguiMarker(m_CommandList.Get(), "Editor ImGui");
                    ID3D12DescriptorHeap* heaps[] = { m_SrvHeap.Get() };
                    m_CommandList->SetDescriptorHeaps(1, heaps);
                    ImGui_ImplDX12_RenderDrawData(drawData, m_CommandList.Get());
                }

                D3D12_RESOURCE_BARRIER renderToPresent = TransitionBarrier(m_BackBuffers[m_FrameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                m_CommandList->ResourceBarrier(1, &renderToPresent);
            }

            result = m_CommandList->Close();
            if (FAILED(result))
            {
                Log::Error("Could not close D3D12 command list: ", HResultToString(result));
                return;
            }

            ID3D12CommandList* commandLists[] = { m_CommandList.Get() };
            m_GraphicsQueue->ExecuteCommandLists(1, commandLists);

            result = m_Swapchain->Present(1, 0);
            if (FAILED(result))
                Log::Error("D3D12 swapchain present failed: ", HResultToString(result));

            const u64 fenceValue = ++m_LastFenceValue;
            result = m_GraphicsQueue->Signal(m_Fence.Get(), fenceValue);
            if (FAILED(result))
                Log::Error("Could not signal D3D12 fence: ", HResultToString(result));
            else
                frame.FenceValue = fenceValue;
        }

        bool PrepareViewportTexture(u32 width, u32 height)
        {
            if (!m_Initialized || width == 0 || height == 0)
                return false;

            if (m_ViewportTexture && m_ViewportWidth == width && m_ViewportHeight == height)
                return true;

            WaitIdle();
            ReleaseViewportTexture();

            RHI::TextureDescription viewportTextureDesc;
            viewportTextureDesc.DebugName = "Editor Viewport Texture";
            viewportTextureDesc.Extent = { width, height };
            viewportTextureDesc.TextureFormat = RHI::Format::R8G8B8A8Unorm;
            viewportTextureDesc.Usage = TextureUsageFlags(RHI::TextureUsage::ShaderResource, RHI::TextureUsage::RenderTarget, RHI::TextureUsage::CopySource);
            viewportTextureDesc.InitialState = RHI::ResourceState::ShaderResource;
            m_ViewportTexture = m_RHIDevice->CreateTexture(viewportTextureDesc);
            if (!m_ViewportTexture)
            {
                Log::Error("Could not create viewport texture through RHI");
                return false;
            }

            const RHI::NVRHID3D12TextureNativeHandles viewportHandles = RHI::GetNVRHID3D12TextureNativeHandles(*m_ViewportTexture);
            m_ViewportTextureResource = static_cast<ID3D12Resource*>(viewportHandles.Resource);
            if (!m_ViewportTextureResource)
            {
                Log::Error("RHI viewport texture did not expose a D3D12 resource");
                ReleaseViewportTexture();
                return false;
            }

            m_ViewportState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            m_Device->CreateRenderTargetView(m_ViewportTextureResource, nullptr, GetRtvCpuHandle(kFrameCount));

            D3D12_CPU_DESCRIPTOR_HANDLE srvCpu {};
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpu {};
            if (!AllocateSrvDescriptor(&srvCpu, &srvGpu, &m_ViewportSrvIndex))
            {
                Log::Error("Could not allocate SRV descriptor for viewport texture");
                ReleaseViewportTexture();
                return false;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
            srvDesc.Format = kSwapchainFormat;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
            srvDesc.Texture2D.PlaneSlice = 0;
            srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            m_Device->CreateShaderResourceView(m_ViewportTextureResource, &srvDesc, srvCpu);

            m_ViewportTextureId = srvGpu.ptr;
            m_ViewportWidth = width;
            m_ViewportHeight = height;

            if (!CreateViewportDepthTexture(width, height))
            {
                ReleaseViewportTexture();
                return false;
            }

            return true;
        }

        u64 GetViewportTextureId() const
        {
            return m_ViewportTextureId;
        }

        bool CaptureViewportToFile(std::string_view path)
        {
            if (!m_Initialized || !m_ViewportTextureResource || m_ViewportWidth == 0 || m_ViewportHeight == 0)
            {
                Log::Warn("Viewport capture requested before the viewport texture exists");
                return false;
            }

            if (path.empty())
            {
                Log::Warn("Viewport capture requested with an empty output path");
                return false;
            }

            WaitIdle();

            D3D12_RESOURCE_DESC viewportDesc = m_ViewportTextureResource->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
            UINT rowCount = 0;
            UINT64 rowSizeBytes = 0;
            UINT64 readbackSizeBytes = 0;
            m_Device->GetCopyableFootprints(&viewportDesc, 0, 1, 0, &footprint, &rowCount, &rowSizeBytes, &readbackSizeBytes);

            ComPtr<ID3D12Resource> readbackBuffer;
            if (!CreateReadbackBuffer(readbackSizeBytes, L"Editor Viewport Capture Readback", readbackBuffer))
                return false;

            FrameContext& frame = m_Frames[0];
            HRESULT result = frame.CommandAllocator->Reset();
            if (FAILED(result))
            {
                Log::Error("Could not reset D3D12 command allocator for viewport capture: ", HResultToString(result));
                return false;
            }

            result = m_CommandList->Reset(frame.CommandAllocator.Get(), nullptr);
            if (FAILED(result))
            {
                Log::Error("Could not reset D3D12 command list for viewport capture: ", HResultToString(result));
                return false;
            }

            {
                ScopedD3D12Marker captureMarker(m_CommandList.Get(), "Viewport Capture Readback");

                {
                    ScopedD3D12Marker viewportMarker(m_CommandList.Get(), "Capture Viewport Texture Refresh");
                    m_ViewportSceneRenderer.Render(
                        m_CommandList.Get(),
                        m_ViewportTextureResource,
                        m_ViewportState,
                        GetRtvCpuHandle(kFrameCount),
                        m_ViewportDepthTextureResource,
                        GetDsvCpuHandle(),
                        m_ViewportWidth,
                        m_ViewportHeight,
                        m_LastClearColor);
                }

                ScopedD3D12Marker copyMarker(m_CommandList.Get(), "Copy Viewport To Readback Buffer");
                const D3D12_RESOURCE_STATES previousState = m_ViewportState;
                if (m_ViewportState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                {
                    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(m_ViewportTextureResource, m_ViewportState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    m_CommandList->ResourceBarrier(1, &barrier);
                    m_ViewportState = D3D12_RESOURCE_STATE_COPY_SOURCE;
                }

                D3D12_TEXTURE_COPY_LOCATION source {};
                source.pResource = m_ViewportTextureResource;
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION destination {};
                destination.pResource = readbackBuffer.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint = footprint;

                m_CommandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

                if (previousState != D3D12_RESOURCE_STATE_COPY_SOURCE)
                {
                    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(m_ViewportTextureResource, D3D12_RESOURCE_STATE_COPY_SOURCE, previousState);
                    m_CommandList->ResourceBarrier(1, &barrier);
                    m_ViewportState = previousState;
                }
            }

            result = m_CommandList->Close();
            if (FAILED(result))
            {
                Log::Error("Could not close D3D12 command list for viewport capture: ", HResultToString(result));
                return false;
            }

            ID3D12CommandList* commandLists[] = { m_CommandList.Get() };
            m_GraphicsQueue->ExecuteCommandLists(1, commandLists);

            const u64 fenceValue = ++m_LastFenceValue;
            result = m_GraphicsQueue->Signal(m_Fence.Get(), fenceValue);
            if (FAILED(result))
            {
                Log::Error("Could not signal D3D12 fence for viewport capture: ", HResultToString(result));
                return false;
            }
            WaitForFenceValue(fenceValue);

            D3D12_RANGE readRange { 0, static_cast<SIZE_T>(readbackSizeBytes) };
            void* mappedData = nullptr;
            result = readbackBuffer->Map(0, &readRange, &mappedData);
            if (FAILED(result))
            {
                Log::Error("Could not map viewport capture readback buffer: ", HResultToString(result));
                return false;
            }

            const std::filesystem::path outputPath { std::string(path) };
            const auto* pixelData = static_cast<const u8*>(mappedData) + footprint.Offset;
            const bool written = WriteBmp(outputPath, pixelData, m_ViewportWidth, m_ViewportHeight, footprint.Footprint.RowPitch);
            D3D12_RANGE writtenRange { 0, 0 };
            readbackBuffer->Unmap(0, &writtenRange);

            if (written)
                Log::Info("Viewport capture saved: ", outputPath.string());
            else
                Log::Error("Could not write viewport capture: ", outputPath.string());

            return written;
        }

    private:
        bool CreateDescriptorHeaps()
        {
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc {};
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDesc.NumDescriptors = kFrameCount + 1;
            rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

            HRESULT result = m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_RtvHeap));
            if (FAILED(result))
            {
                Log::Error("Could not create D3D12 RTV heap: ", HResultToString(result));
                return false;
            }

            m_RtvHeap->SetName(L"Spiral RTV Descriptor Heap");
            m_RtvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

            D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc {};
            dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsvHeapDesc.NumDescriptors = 1;
            dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

            result = m_Device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_DsvHeap));
            if (FAILED(result))
            {
                Log::Error("Could not create D3D12 DSV heap: ", HResultToString(result));
                return false;
            }

            m_DsvHeap->SetName(L"Spiral DSV Descriptor Heap");

            D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc {};
            srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvHeapDesc.NumDescriptors = kSrvDescriptorCount;
            srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            result = m_Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvHeap));
            if (FAILED(result))
            {
                Log::Error("Could not create D3D12 SRV heap: ", HResultToString(result));
                return false;
            }

            m_SrvHeap->SetName(L"Spiral Shader Visible SRV Descriptor Heap");
            m_SrvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            m_SrvAllocated.assign(kSrvDescriptorCount, false);
            return true;
        }

        bool CreateSwapchain(u32 width, u32 height)
        {
            HWND hwnd = glfwGetWin32Window(m_Window);
            if (!hwnd)
            {
                Log::Error("Could not query Win32 window handle for D3D12 swapchain");
                return false;
            }

            DXGI_SWAP_CHAIN_DESC1 swapchainDesc {};
            swapchainDesc.Width = width;
            swapchainDesc.Height = height;
            swapchainDesc.Format = kSwapchainFormat;
            swapchainDesc.Stereo = FALSE;
            swapchainDesc.SampleDesc.Count = 1;
            swapchainDesc.SampleDesc.Quality = 0;
            swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapchainDesc.BufferCount = kFrameCount;
            swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
            swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
            swapchainDesc.Flags = 0;

            ComPtr<IDXGISwapChain1> swapchain1;
            HRESULT result = m_Factory->CreateSwapChainForHwnd(m_GraphicsQueue, hwnd, &swapchainDesc, nullptr, nullptr, &swapchain1);
            if (FAILED(result))
            {
                Log::Error("Could not create D3D12 swapchain: ", HResultToString(result));
                return false;
            }

            m_Factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

            result = swapchain1.As(&m_Swapchain);
            if (FAILED(result))
            {
                Log::Error("Could not query IDXGISwapChain3: ", HResultToString(result));
                return false;
            }

            m_SwapchainWidth = width;
            m_SwapchainHeight = height;
            m_FrameIndex = m_Swapchain->GetCurrentBackBufferIndex();
            return CreateBackBufferViews();
        }

        bool CreateBackBufferViews()
        {
            for (u32 index = 0; index < kFrameCount; ++index)
            {
                HRESULT result = m_Swapchain->GetBuffer(index, IID_PPV_ARGS(&m_BackBuffers[index]));
                if (FAILED(result))
                {
                    Log::Error("Could not get D3D12 swapchain back buffer: ", HResultToString(result));
                    return false;
                }

                m_BackBuffers[index]->SetName(GetBackBufferName(index));
                m_Device->CreateRenderTargetView(m_BackBuffers[index].Get(), nullptr, GetRtvCpuHandle(index));
            }

            return true;
        }

        void ReleaseBackBuffers()
        {
            for (ComPtr<ID3D12Resource>& backBuffer : m_BackBuffers)
                backBuffer.Reset();
        }

        bool CreateCommandObjects()
        {
            for (u32 index = 0; index < kFrameCount; ++index)
            {
                FrameContext& frame = m_Frames[index];
                HRESULT result = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.CommandAllocator));
                if (FAILED(result))
                {
                    Log::Error("Could not create D3D12 command allocator: ", HResultToString(result));
                    return false;
                }

                frame.CommandAllocator->SetName(GetCommandAllocatorName(index));
            }

            HRESULT result = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Frames[0].CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_CommandList));
            if (FAILED(result))
            {
                Log::Error("Could not create D3D12 command list: ", HResultToString(result));
                return false;
            }

            m_CommandList->SetName(L"Spiral Presentation Command List");
            m_CommandList->Close();
            return true;
        }

        bool CreateReadbackBuffer(u64 sizeBytes, const wchar_t* name, ComPtr<ID3D12Resource>& resource)
        {
            D3D12_HEAP_PROPERTIES heapProperties {};
            heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
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
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&resource));

            if (FAILED(result))
            {
                Log::Error("Could not create D3D12 readback buffer: ", HResultToString(result));
                return false;
            }

            resource->SetName(name);
            return true;
        }

        bool CreateFence()
        {
            HRESULT result = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
            if (FAILED(result))
            {
                Log::Error("Could not create D3D12 presentation fence: ", HResultToString(result));
                return false;
            }

            m_Fence->SetName(L"Spiral Presentation Fence");

            m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!m_FenceEvent)
            {
                Log::Error("Could not create D3D12 fence event");
                return false;
            }

            return true;
        }

        bool InitializeImGui()
        {
            ImGui_ImplDX12_InitInfo initInfo;
            initInfo.Device = m_Device;
            initInfo.CommandQueue = m_GraphicsQueue;
            initInfo.NumFramesInFlight = static_cast<int>(kFrameCount);
            initInfo.RTVFormat = kSwapchainFormat;
            initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
            initInfo.UserData = this;
            initInfo.SrvDescriptorHeap = m_SrvHeap.Get();
            initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
            {
                auto* self = static_cast<Impl*>(info->UserData);
                if (!self->AllocateSrvDescriptor(outCpu, outGpu, nullptr))
                {
                    outCpu->ptr = 0;
                    outGpu->ptr = 0;
                }
            };
            initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
            {
                auto* self = static_cast<Impl*>(info->UserData);
                self->FreeSrvDescriptor(cpu, gpu);
            };

            if (!ImGui_ImplDX12_Init(&initInfo))
            {
                Log::Error("Could not initialize ImGui DX12 backend");
                return false;
            }

            return true;
        }

        bool ResizeSwapchainIfNeeded(u32 width, u32 height)
        {
            if (width == m_SwapchainWidth && height == m_SwapchainHeight)
                return true;

            WaitIdle();
            ReleaseBackBuffers();

            HRESULT result = m_Swapchain->ResizeBuffers(kFrameCount, width, height, kSwapchainFormat, 0);
            if (FAILED(result))
            {
                Log::Error("Could not resize D3D12 swapchain: ", HResultToString(result));
                return false;
            }

            m_SwapchainWidth = width;
            m_SwapchainHeight = height;
            m_FrameIndex = m_Swapchain->GetCurrentBackBufferIndex();
            return CreateBackBufferViews();
        }

        bool CreateViewportDepthTexture(u32 width, u32 height)
        {
            RHI::TextureDescription depthTextureDesc;
            depthTextureDesc.DebugName = "Editor Viewport Depth Texture";
            depthTextureDesc.Extent = { width, height };
            depthTextureDesc.TextureFormat = RHI::Format::D32Float;
            depthTextureDesc.Usage = RHI::TextureUsage::DepthStencil;
            depthTextureDesc.InitialState = RHI::ResourceState::DepthWrite;
            m_ViewportDepthTexture = m_RHIDevice->CreateTexture(depthTextureDesc);
            if (!m_ViewportDepthTexture)
            {
                Log::Error("Could not create viewport depth texture through RHI");
                return false;
            }

            const RHI::NVRHID3D12TextureNativeHandles depthHandles = RHI::GetNVRHID3D12TextureNativeHandles(*m_ViewportDepthTexture);
            m_ViewportDepthTextureResource = static_cast<ID3D12Resource*>(depthHandles.Resource);
            if (!m_ViewportDepthTextureResource)
            {
                Log::Error("RHI viewport depth texture did not expose a D3D12 resource");
                return false;
            }

            m_Device->CreateDepthStencilView(m_ViewportDepthTextureResource, nullptr, GetDsvCpuHandle());
            return true;
        }

        void RenderViewportTexture(const ClearColor& clearColor)
        {
            if (!m_ViewportTextureResource || !m_ViewportDepthTextureResource)
                return;

            m_ViewportSceneRenderer.Render(
                m_CommandList.Get(),
                m_ViewportTextureResource,
                m_ViewportState,
                GetRtvCpuHandle(kFrameCount),
                m_ViewportDepthTextureResource,
                GetDsvCpuHandle(),
                m_ViewportWidth,
                m_ViewportHeight,
                clearColor);
        }

        void ReleaseViewportTexture()
        {
            if (m_ViewportSrvIndex != kInvalidDescriptorIndex)
            {
                FreeSrvDescriptor(m_ViewportSrvIndex);
                m_ViewportSrvIndex = kInvalidDescriptorIndex;
            }

            m_ViewportTexture.reset();
            m_ViewportDepthTexture.reset();
            m_ViewportTextureResource = nullptr;
            m_ViewportDepthTextureResource = nullptr;
            m_ViewportTextureId = 0;
            m_ViewportWidth = 0;
            m_ViewportHeight = 0;
            m_ViewportState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        bool AllocateSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu, u32* outIndex)
        {
            for (u32 index = 0; index < kSrvDescriptorCount; ++index)
            {
                if (m_SrvAllocated[index])
                    continue;

                m_SrvAllocated[index] = true;
                if (outIndex)
                    *outIndex = index;

                *outCpu = GetSrvCpuHandle(index);
                *outGpu = GetSrvGpuHandle(index);
                return true;
            }

            return false;
        }

        void FreeSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
        {
            (void)gpu;
            if (!m_SrvHeap || cpu.ptr == 0)
                return;

            const size_t offset = static_cast<size_t>(cpu.ptr - m_SrvHeap->GetCPUDescriptorHandleForHeapStart().ptr);
            const u32 index = static_cast<u32>(offset / m_SrvDescriptorSize);
            FreeSrvDescriptor(index);
        }

        void FreeSrvDescriptor(u32 index)
        {
            if (index < m_SrvAllocated.size())
                m_SrvAllocated[index] = false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE GetRtvCpuHandle(u32 index) const
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * m_RtvDescriptorSize;
            return handle;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE GetDsvCpuHandle() const
        {
            return m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
        }

        D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCpuHandle(u32 index) const
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * m_SrvDescriptorSize;
            return handle;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle(u32 index) const
        {
            D3D12_GPU_DESCRIPTOR_HANDLE handle = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>(index) * m_SrvDescriptorSize;
            return handle;
        }

        void WaitForFenceValue(u64 fenceValue)
        {
            if (!fenceValue || !m_Fence)
                return;

            if (m_Fence->GetCompletedValue() >= fenceValue)
                return;

            m_Fence->SetEventOnCompletion(fenceValue, m_FenceEvent);
            WaitForSingleObject(m_FenceEvent, INFINITE);
        }

        void WaitIdle()
        {
            if (!m_GraphicsQueue || !m_Fence)
                return;

            const u64 fenceValue = ++m_LastFenceValue;
            if (SUCCEEDED(m_GraphicsQueue->Signal(m_Fence.Get(), fenceValue)))
                WaitForFenceValue(fenceValue);

            for (FrameContext& frame : m_Frames)
                frame.FenceValue = 0;
        }

    private:
        bool m_Initialized = false;
        GLFWwindow* m_Window = nullptr;
        RHI::Device* m_RHIDevice = nullptr;
        IDXGIFactory4* m_Factory = nullptr;
        ID3D12Device* m_Device = nullptr;
        ID3D12CommandQueue* m_GraphicsQueue = nullptr;

        ComPtr<IDXGISwapChain3> m_Swapchain;
        std::array<ComPtr<ID3D12Resource>, kFrameCount> m_BackBuffers;
        std::array<FrameContext, kFrameCount> m_Frames;
        ComPtr<ID3D12GraphicsCommandList> m_CommandList;
        ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
        ComPtr<ID3D12DescriptorHeap> m_DsvHeap;
        ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
        ComPtr<ID3D12Fence> m_Fence;
        NVRHID3D12ViewportSceneRenderer m_ViewportSceneRenderer;
        HANDLE m_FenceEvent = nullptr;

        std::vector<bool> m_SrvAllocated;
        u32 m_RtvDescriptorSize = 0;
        u32 m_SrvDescriptorSize = 0;
        u32 m_FrameIndex = 0;
        u32 m_SwapchainWidth = 0;
        u32 m_SwapchainHeight = 0;
        u64 m_LastFenceValue = 0;

        Scope<RHI::Texture> m_ViewportTexture;
        Scope<RHI::Texture> m_ViewportDepthTexture;
        ID3D12Resource* m_ViewportTextureResource = nullptr;
        ID3D12Resource* m_ViewportDepthTextureResource = nullptr;
        D3D12_RESOURCE_STATES m_ViewportState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        u32 m_ViewportWidth = 0;
        u32 m_ViewportHeight = 0;
        u32 m_ViewportSrvIndex = kInvalidDescriptorIndex;
        u64 m_ViewportTextureId = 0;
        ClearColor m_LastClearColor;
    };
#else
    struct NVRHID3D12Presentation::Impl
    {
    };
#endif

    NVRHID3D12Presentation::NVRHID3D12Presentation()
        : m_Impl(CreateScope<Impl>())
    {
    }

    NVRHID3D12Presentation::~NVRHID3D12Presentation()
    {
        Shutdown();
    }

    bool NVRHID3D12Presentation::Initialize(void* nativeWindow, RHI::Device* rhiDevice, const RHI::NVRHID3D12NativeHandles& nativeHandles, u32 width, u32 height)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        return m_Impl->Initialize(nativeWindow, rhiDevice, nativeHandles, width, height);
#else
        (void)nativeWindow;
        (void)rhiDevice;
        (void)nativeHandles;
        (void)width;
        (void)height;
        return false;
#endif
    }

    void NVRHID3D12Presentation::Shutdown()
    {
#if defined(GE_HAS_NVRHI_D3D12)
        m_Impl->Shutdown();
#endif
    }

    bool NVRHID3D12Presentation::IsInitialized() const
    {
#if defined(GE_HAS_NVRHI_D3D12)
        return m_Impl->IsInitialized();
#else
        return false;
#endif
    }

    void NVRHID3D12Presentation::BeginImGuiFrame()
    {
#if defined(GE_HAS_NVRHI_D3D12)
        m_Impl->BeginImGuiFrame();
#endif
    }

    void NVRHID3D12Presentation::RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        m_Impl->RenderImGuiDrawData(drawData, clearColor, width, height);
#else
        (void)drawData;
        (void)clearColor;
        (void)width;
        (void)height;
#endif
    }

    bool NVRHID3D12Presentation::PrepareViewportTexture(u32 width, u32 height)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        return m_Impl->PrepareViewportTexture(width, height);
#else
        (void)width;
        (void)height;
        return false;
#endif
    }

    u64 NVRHID3D12Presentation::GetViewportTextureId() const
    {
#if defined(GE_HAS_NVRHI_D3D12)
        return m_Impl->GetViewportTextureId();
#else
        return 0;
#endif
    }

    bool NVRHID3D12Presentation::CaptureViewportToFile(std::string_view path)
    {
#if defined(GE_HAS_NVRHI_D3D12)
        return m_Impl->CaptureViewportToFile(path);
#else
        (void)path;
        return false;
#endif
    }
}
