#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/ClusteredLightGrid.h"
#include "Engine/Renderer/ColorPipelineSettings.h"
#include "Engine/RHI/Device.h"

#include <array>
#include <string>
#include <type_traits>
#include <vector>

namespace Engine
{
    // Fixed 16-byte elements for the one admitted t0,space3 structured SRV.
    using SceneLightPayloadWord = std::array<u32, 4>;
    static_assert(sizeof(SceneLightPayloadWord)
        == RHI::kFixedReadOnlyStructuredBufferStrideBytes);
    static_assert(std::is_trivially_copyable_v<SceneLightPayloadWord>);

    struct SceneLightPayload
    {
        // V3 keeps the exact authored record in words 0-5 and appends a
        // finite renderer-prepared RGB coefficient plus reciprocal range in
        // word 6. Word 5.w carries the spot cone's reciprocal cosine span.
        static constexpr u32 Version = 3;
        static constexpr u32 HeaderWordCount = 6;
        static constexpr u32 LightRecordWordCount = 7;
        static constexpr u32 MaximumWordCount = 4u * 1024u * 1024u;
        static constexpr u32 MaximumLocalLightsPerCluster = 64;

        u64 Generation = 0;
        RendererColorPipelineSettings ColorSettings;
        ScenePreExposureState PreExposure;
        std::vector<SceneLightPayloadWord> Words;
    };

    // Constructs the ABI without modifying outPayload until every source/grid
    // invariant and every checked size calculation has succeeded.
    bool BuildSceneLightPayload(const SceneRenderSnapshot& snapshot,
        size_t viewIndex, const ClusteredLightGrid& grid,
        const RendererColorPipelineSettings& colorSettings, u64 generation,
        SceneLightPayload& outPayload, std::string& outError);

    struct SceneLightPayloadSlot
    {
        Ref<const SceneLightPayload> Payload;
        Ref<RHI::Buffer> Staging;
        Ref<RHI::Buffer> Gpu;
    };

    struct SceneLightPayloadPublicationDiagnostics
    {
        u64 AllocationCount = 0;
        u64 ReuseCount = 0;
        u64 CapacityRejectionCount = 0;
        u64 CommitCount = 0;
    };

    class SceneLightPayloadPublication final
    {
    public:
        static constexpr size_t Capacity = 4;
        bool Acquire(RHI::Device& device, const SceneRenderSnapshot& snapshot,
            size_t viewIndex, const ClusteredLightGrid& grid,
            const RendererColorPipelineSettings& colorSettings, u64 generation,
            Ref<SceneLightPayloadSlot>& outSlot, std::string& outError);
        bool Commit(const Ref<SceneLightPayloadSlot>& slot, std::string& outError);
        [[nodiscard]] Ref<const SceneLightPayload> GetLastAcceptedPayload() const
        {
            return m_LastAcceptedSlot ? m_LastAcceptedSlot->Payload : nullptr;
        }
        [[nodiscard]] u64 GetLastAcceptedGeneration() const
        {
            return m_LastAcceptedSlot && m_LastAcceptedSlot->Payload
                ? m_LastAcceptedSlot->Payload->Generation : 0;
        }
        [[nodiscard]] const SceneLightPayloadPublicationDiagnostics& GetDiagnostics() const
        {
            return m_Diagnostics;
        }
        void ClearAfterDeviceIdle();
    private:
        std::array<Ref<SceneLightPayloadSlot>, Capacity> m_Slots;
        // Keeps the last accepted GPU output unavailable for reuse until a
        // newer complete graph publication commits.
        Ref<SceneLightPayloadSlot> m_LastAcceptedSlot;
        RHI::Device* m_Device = nullptr;
        SceneLightPayloadPublicationDiagnostics m_Diagnostics;
    };
}
