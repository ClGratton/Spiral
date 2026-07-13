#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Device.h"
#include "Engine/Renderer/Renderer.h"

#if defined(GE_HAS_NVRHI_D3D12)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <Windows.h>
    #include <directx/d3d12.h>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_D3D12)
    class NVRHID3D12ViewportSceneRenderer final
    {
    public:
        NVRHID3D12ViewportSceneRenderer();
        ~NVRHID3D12ViewportSceneRenderer();

        bool Initialize(ID3D12Device* device, RHI::Device* rhiDevice);
        void Shutdown();

        bool Render(
            ID3D12GraphicsCommandList* commandList,
            ID3D12Resource* colorTexture,
            D3D12_RESOURCE_STATES& colorState,
            D3D12_CPU_DESCRIPTOR_HANDLE colorRtv,
            ID3D12Resource* depthTexture,
            D3D12_CPU_DESCRIPTOR_HANDLE depthDsv,
            u32 width,
            u32 height,
            u32 frameSlot,
            const ClearColor& clearColor);

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
#endif
}
