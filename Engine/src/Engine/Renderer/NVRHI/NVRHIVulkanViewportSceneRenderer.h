#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Device.h"
#include "Engine/Renderer/Renderer.h"
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
        void Shutdown();
        bool RenderCurrentSnapshot(u32 width, u32 height, const ClearColor& clearColor);
        bool Render(const SceneRenderSnapshot& snapshot, u32 width, u32 height, const ClearColor& clearColor);
        bool ReadbackColor(RHI::TextureReadback& readback) const;
        u64 GetOutputGeneration() const;

    private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
#endif
}
