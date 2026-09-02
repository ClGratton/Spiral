#pragma once

#include "Engine/Assets/TextureArtifact.h"
#include "Engine/Core/Base.h"
#include "Engine/Renderer/TextureArtifactUploadPlan.h"

#include <vector>

namespace Engine
{
    // The bundle retains the exact selected upload plan so an external owner
    // keeps both immutable payload and GPU resource generations after eviction.
    struct TextureGpuResourceBundle
    {
        Ref<RHI::Texture> Texture;
        TextureArtifactUploadPlan UploadPlan;
        bool UsedExplicitRgbaFallback = false;
        std::string Diagnostic;
        u64 Generation = 0;
    };

    // Device addresses identify exact live instances. Callers must clear the
    // cache and release every external bundle before destroying that device.
    class TextureGpuResourceCache final
    {
    public:
        explicit TextureGpuResourceCache(size_t capacity);
        ~TextureGpuResourceCache();

        bool Acquire(RHI::Device& device, const TextureArtifact& preferred,
            const TextureArtifact* rgbaFallback,
            Ref<const TextureGpuResourceBundle>& outBundle, std::string& outError);
        void Clear();
        size_t GetEntryCount() const;

    private:
        struct Entry;

        size_t m_Capacity = 0;
        u64 m_NextAccess = 0;
        u64 m_NextGeneration = 0;
        std::vector<Entry> m_Entries;
    };
}
