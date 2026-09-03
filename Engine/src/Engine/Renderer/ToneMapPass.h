#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/ColorPipelineSettings.h"
#include "Engine/RHI/Device.h"

namespace Engine
{
    struct ToneMapPassConstants
    {
        RendererColorPipelineSettings Settings;
        Scope<RHI::Buffer> Buffer;
        u64 Generation = 0;
    };

    struct ToneMapPassConstantCacheDiagnostics
    {
        bool HasCurrent = false;
        RendererColorPipelineSettings CurrentSettings;
        u64 CurrentGeneration = 0;
        u64 AllocationCount = 0;
        u64 ReuseCount = 0;
    };

    // Portable scene-linear HDR to display-encoded RGBA8 pass. The first
    // version intentionally owns a small fixed resource set and exposes no
    // backend handles.
    class ToneMapPass final
    {
    public:
        bool Initialize(RHI::Device& device);
        void Shutdown();

        Ref<ToneMapPassConstants> AcquireConstants(
            const RendererColorPipelineSettings& settings) const;
        ToneMapPassConstantCacheDiagnostics GetConstantCacheDiagnostics() const;
        bool Record(RHI::CommandList& commands, RHI::Texture& hdrScene,
            RHI::Texture& output, u32 width, u32 height,
            const ToneMapPassConstants& constants) const;
        bool IsInitialized() const { return m_Pipeline != nullptr; }

    private:
        RHI::Device* m_Device = nullptr;
        Scope<RHI::Shader> m_VertexShader;
        Scope<RHI::Shader> m_PixelShader;
        Scope<RHI::Pipeline> m_Pipeline;
        Scope<RHI::Buffer> m_VertexBuffer;
        Scope<RHI::Buffer> m_IndexBuffer;
        mutable Ref<ToneMapPassConstants> m_CurrentConstants;
        mutable u64 m_ConstantGeneration = 0;
        mutable u64 m_ConstantAllocationCount = 0;
        mutable u64 m_ConstantReuseCount = 0;
    };
}
