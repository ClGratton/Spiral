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
        // Inverse view depth at both endpoints, two segments per float4.
        float SegmentDepths[SceneDebugOverlayFrame::MaximumSegmentCount / 2][4] {};
        float OverlayColorAndOpacity[4] {};
        float OverlayState[4] {};
        float OccludedState[4] {};
        float DepthState[4] {};
    };

    static_assert(sizeof(SceneDebugOverlayGpuConstants) == 352);

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
        // Blends the overlay over the tone-mapped color already in output. The
        // Scene depth is sampled (it must be in ShaderResource state) rather than
        // bound as an attachment, so the pass declares a depth read and no write.
        bool Record(RHI::CommandList& commands, RHI::Texture& sceneDepth,
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
