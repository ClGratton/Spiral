#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanContext.h"
#include "Engine/Renderer/Renderer.h"

struct ImDrawData;

namespace Engine
{
    class NVRHIVulkanPresentation final
    {
    public:
        NVRHIVulkanPresentation();
        ~NVRHIVulkanPresentation();

        bool Initialize(RHI::NVRHIVulkanContext* context, void* nativeWindow, u32 width, u32 height);
        void Shutdown();
        bool IsInitialized() const;
        void BeginImGuiFrame();
        void RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height);
        const RendererPresentationTiming& GetTiming() const;
        u64 GetSuccessfulPresentCount() const;

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
}
