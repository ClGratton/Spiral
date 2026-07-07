#pragma once

#include "Engine/Core/Base.h"

#if defined(GE_HAS_NVRHI_D3D12)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <Windows.h>
    #include <directx/d3d12.h>
    #include <pix.h>
#endif

namespace Engine
{
#if defined(GE_HAS_NVRHI_D3D12)
    class ScopedD3D12Marker final
    {
    public:
        ScopedD3D12Marker(ID3D12GraphicsCommandList* commandList, const char* name)
            : m_CommandList(commandList)
        {
            if (m_CommandList)
                PIXBeginEvent(m_CommandList, PIX_COLOR(60, 170, 220), "%s", name ? name : "Unnamed D3D12 Event");
        }

        ~ScopedD3D12Marker()
        {
            if (m_CommandList)
                PIXEndEvent(m_CommandList);
        }

        ScopedD3D12Marker(const ScopedD3D12Marker&) = delete;
        ScopedD3D12Marker& operator=(const ScopedD3D12Marker&) = delete;

    private:
        ID3D12GraphicsCommandList* m_CommandList = nullptr;
    };
#endif
}
