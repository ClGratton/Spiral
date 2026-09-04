#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Device.h"
#include "Engine/RHI/NVRHI/NVRHIVulkanDevice.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/Renderer/SceneLightPayload.h"
#include "Engine/Renderer/ToneMapPass.h"
#include "Engine/Scene/SceneRenderSnapshot.h"

namespace Engine
{
#if defined(GE_HAS_NVRHI_VULKAN)
    // Owns only renderer-offscreen outputs. Native WSI and ImGui exposure stay
    // in NVRHIVulkanPresentation.
    class NVRHIVulkanViewportSceneRenderer final
    {
    public:
        NVRHIVulkanViewportSceneRenderer();
        ~NVRHIVulkanViewportSceneRenderer();

        bool Initialize(RHI::Device* device);
        // Test-only pipeline selection. It retains the production Scene vertex,
        // resource, and constant interfaces while replacing only the pixel entry.
        bool InitializeSurfaceBasisProbe(RHI::Device* device);
        bool InitializeLightPayloadProbe(RHI::Device* device);
        void Shutdown();
        bool RenderCurrentSnapshot(u32 width, u32 height, const ClearColor& clearColor);
        bool Render(const SceneRenderSnapshot& snapshot, u32 width, u32 height, const ClearColor& clearColor);
        bool ReadbackColor(RHI::TextureReadback& readback) const;
        bool ReadbackHdr(RHI::TextureReadback& readback);
        u64 GetOutputGeneration() const;
        u32 GetOutputWidth() const;
        u32 GetOutputHeight() const;
        ToneMapPassConstantCacheDiagnostics GetToneMapConstantCacheDiagnostics() const;
        SceneLightPayloadPublicationDiagnostics GetLightPayloadPublicationDiagnostics() const;
        RHI::NVRHIVulkanTextureNativeHandles GetOutputNativeHandles() const;

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
#endif
}
