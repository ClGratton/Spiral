#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanContext.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanDevice.h"
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
        bool RegisterViewportOutput(const RHI::NVRHIVulkanTextureNativeHandles& handles, u64 outputGeneration);
        void ReleaseViewportOutput();
        u64 GetViewportTextureId() const;
        void MarkViewportTextureQueued(u64 textureId);

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
}
