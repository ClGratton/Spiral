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

        bool Initialize(void* nativeWindow, RHI::Device* rhiDevice, const RHI::NVRHID3D12NativeHandles& nativeHandles, u32 width, u32 height);
        void Shutdown();

        bool IsInitialized() const;
        void WaitForFrameLatency();
        bool ApplyPendingPresentationPolicy();
        void BeginImGuiFrame();
        void RenderImGuiDrawData(ImDrawData* drawData, const ClearColor& clearColor, u32 width, u32 height);
        bool PrepareViewportTexture(u32 width, u32 height);
        u64 GetViewportTextureId() const;
        // Dynamic UI textures (Renderer UI-texture service). Registration
        // allocates an SRV from the shared ImGui descriptor heap and returns its
        // GPU handle; the descriptor must outlive every submission that draws it.
        // Serials count ImGui draw submissions and are resolved against the
        // presentation fence without waiting.
        u64 RegisterUiTexture(RHI::Texture& texture);
        void UnregisterUiTexture(u64 imGuiId);
        u64 GetSubmittedPresentationSerial() const;
        u64 PollCompletedPresentationSerial();
        bool CaptureViewportToFile(std::string_view path);
        const RendererPresentationTiming& GetTiming() const;
        void SetPresentationPolicy(PresentationPolicy policy);
        const RendererPresentationPolicyDiagnostics& GetPresentationPolicyDiagnostics() const;

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
}
