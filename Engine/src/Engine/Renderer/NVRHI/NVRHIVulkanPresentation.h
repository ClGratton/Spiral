#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanContext.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanDevice.h"
#include "Engine/Renderer/Renderer.h"

#include <vector>

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
        void SetPresentationPolicy(PresentationPolicy policy);
        const RendererPresentationPolicyDiagnostics& GetPresentationPolicyDiagnostics() const;
        u64 GetSuccessfulPresentCount() const;
        bool RegisterViewportOutput(const RHI::NVRHIVulkanTextureNativeHandles& handles, u64 outputGeneration);
        void ReleaseViewportOutput();
        u64 GetViewportTextureId() const;
        void MarkViewportTextureQueued(u64 textureId);

        // Dynamic UI textures (Renderer UI-texture service). Registration hands
        // the NVRHI image view to the ImGui Vulkan backend, which owns the one
        // shared sampler; the descriptor must outlive every submission that
        // draws it. Serials count ImGui draw submissions: the submitted serial
        // is the last one issued and the polled completed serial is the highest
        // N for which every submission up to N has finished (fences are polled,
        // never waited on).
        u64 RegisterUiTexture(RHI::Texture& texture);
        void UnregisterUiTexture(u64 imGuiId);
        u64 GetSubmittedPresentationSerial() const;
        u64 PollCompletedPresentationSerial();
        // Diagnostic: renders `drawData` through the ImGui Vulkan backend into a
        // private width x height target in a render pass compatible with the
        // presentation pass, waits, and returns tightly packed RGBA8 pixels.
        // Waits for device idle first, so it is a smoke/verification facility,
        // never part of a steady-state frame.
        bool CaptureDrawDataOffscreen(ImDrawData* drawData, u32 width, u32 height, std::vector<u8>& outRgba);

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
}
