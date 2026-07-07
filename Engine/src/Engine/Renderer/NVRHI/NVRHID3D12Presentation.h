#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/NVRHI/NVRHID3D12Device.h"
#include "Engine/Renderer/Renderer.h"

#include <string_view>

struct ImDrawData;

namespace Engine
{
    class NVRHID3D12Presentation final
    {
    public:
        NVRHID3D12Presentation();
        ~NVRHID3D12Presentation();

        bool Initialize(void* nativeWindow, const RHI::NVRHID3D12NativeHandles& nativeHandles, u32 width, u32 height);
        void Shutdown();

        bool IsInitialized() const;
        void BeginImGuiFrame();
        void RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height);
        bool PrepareViewportTexture(u32 width, u32 height);
        u64 GetViewportTextureId() const;
        bool CaptureViewportToFile(std::string_view path);

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
}
