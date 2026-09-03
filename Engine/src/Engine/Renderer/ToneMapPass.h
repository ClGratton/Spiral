#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Device.h"

namespace Engine
{
    // Portable scene-linear HDR to display-encoded RGBA8 pass. The first
    // version intentionally owns a small fixed resource set and exposes no
    // backend handles.
    class ToneMapPass final
    {
    public:
        bool Initialize(RHI::Device& device);
        void Shutdown();

        bool Record(RHI::CommandList& commands, RHI::Texture& hdrScene,
            RHI::Texture& output, u32 width, u32 height) const;
        bool IsInitialized() const { return m_Pipeline != nullptr; }

    private:
        RHI::Device* m_Device = nullptr;
        Scope<RHI::Shader> m_VertexShader;
        Scope<RHI::Shader> m_PixelShader;
        Scope<RHI::Pipeline> m_Pipeline;
        Scope<RHI::Buffer> m_VertexBuffer;
        Scope<RHI::Buffer> m_IndexBuffer;
        Scope<RHI::Buffer> m_ConstantBuffer;
    };
}
