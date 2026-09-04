#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/SceneDebugVisualization.h"
#include "Engine/RHI/Device.h"

#include <array>
#include <string>

namespace Engine
{
    struct SceneDebugOverlayGpuConstants
    {
        float Segments[SceneDebugOverlayFrame::MaximumSegmentCount][4] {};
        float OverlayColorAndOpacity[4] {};
        float OverlayState[4] {};
    };

    static_assert(sizeof(SceneDebugOverlayGpuConstants) == 224);

    bool TryBuildSceneDebugOverlayGpuConstants(
        const SceneDebugOverlayFrame& frame,
        SceneDebugOverlayGpuConstants& outConstants,
        std::string& outError);

    struct SceneDebugOverlayPassConstants
    {
        SceneDebugOverlayFrame Frame;
        SceneDebugOverlayGpuConstants GpuConstants;
        Ref<RHI::Buffer> Buffer;
        u64 FrameIndex = 0;
        u64 Generation = 0;
    };

    class SceneDebugOverlayPass final
    {
    public:
        static constexpr size_t ConstantSlotCount = 4;

        bool Initialize(RHI::Device& device);
        void Shutdown();

        Ref<SceneDebugOverlayPassConstants> AcquireConstants(
            u64 frameIndex, const SceneDebugOverlayFrame& frame,
            std::string& outError);
        bool Record(RHI::CommandList& commands, RHI::Texture& input,
            RHI::Texture& output, u32 width, u32 height,
            const SceneDebugOverlayPassConstants& constants) const;
        bool IsInitialized() const { return m_Pipeline != nullptr; }

    private:
        RHI::Device* m_Device = nullptr;
        Scope<RHI::Shader> m_VertexShader;
        Scope<RHI::Shader> m_PixelShader;
        Scope<RHI::Pipeline> m_Pipeline;
        Scope<RHI::Buffer> m_VertexBuffer;
        Scope<RHI::Buffer> m_IndexBuffer;
        std::array<Ref<SceneDebugOverlayPassConstants>, ConstantSlotCount>
            m_ConstantSlots;
        u64 m_ConstantGeneration = 0;
    };
}
