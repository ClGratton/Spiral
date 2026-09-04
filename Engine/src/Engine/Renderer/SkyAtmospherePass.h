#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/SceneSkyAtmosphere.h"
#include "Engine/RHI/Device.h"

#include <array>

namespace Engine
{
    struct SkyAtmospherePassConstants
    {
        SceneSkyAtmosphereFrame Frame;
        SceneSkyAtmosphereGpuConstants GpuConstants;
        Ref<RHI::Buffer> Buffer;
        u64 FrameIndex = 0;
        u64 Generation = 0;
    };

    class SkyAtmospherePass final
    {
    public:
        static constexpr size_t ConstantSlotCount = 4;

        bool Initialize(RHI::Device& device);
        void Shutdown();
        Ref<SkyAtmospherePassConstants> AcquireConstants(u64 frameIndex,
            const SceneSkyAtmosphereFrame& frame, float preExposure,
            std::string& outError);
        bool Record(RHI::CommandList& commands, RHI::Texture& hdrScene,
            u32 width, u32 height,
            const SkyAtmospherePassConstants& constants) const;
        bool IsInitialized() const { return m_Pipeline != nullptr; }

    private:
        RHI::Device* m_Device = nullptr;
        Scope<RHI::Shader> m_VertexShader;
        Scope<RHI::Shader> m_PixelShader;
        Scope<RHI::Pipeline> m_Pipeline;
        Scope<RHI::Buffer> m_VertexBuffer;
        Scope<RHI::Buffer> m_IndexBuffer;
        std::array<Ref<SkyAtmospherePassConstants>, ConstantSlotCount>
            m_ConstantSlots;
        u64 m_ConstantGeneration = 0;
    };
}
