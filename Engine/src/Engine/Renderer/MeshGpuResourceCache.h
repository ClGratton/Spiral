#pragma once

#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Core/Base.h"
#include "Engine/RHI/Buffer.h"
#include "Engine/RHI/Device.h"

#include <vector>

namespace Engine
{
    struct MeshGpuPrimitiveRange
    {
        u32 SourceMeshIndex = 0;
        u32 SourcePrimitiveIndex = 0;
        u32 FirstVertex = 0;
        u32 VertexCount = 0;
        u32 FirstIndex = 0;
        u32 IndexCount = 0;
        // Artifact indices are stored in the combined vertex-buffer domain.
        // DrawIndexed therefore uses this explicit zero base vertex.
        int BaseVertex = 0;
    };

    // This bundle is intentionally Ref-owned so an accepted RenderGraph frame
    // can retain its exact mesh generation after cache eviction. Retiring that
    // payload with the frame is a later viewport-integration responsibility.
    struct MeshGpuResourceBundle
    {
        Ref<RHI::Buffer> VertexBuffer;
        Ref<RHI::Buffer> IndexBuffer;
        std::vector<MeshGpuPrimitiveRange> Primitives;
        u64 Generation = 0;
    };

    // One cache is scoped to the lifetimes of the RHI devices supplied to it.
    // Device addresses are exact live-instance identities; callers must clear
    // the cache before destroying a device and before reusing its storage.
    class MeshGpuResourceCache final
    {
    public:
        explicit MeshGpuResourceCache(size_t capacity);
        ~MeshGpuResourceCache();

        bool Acquire(RHI::Device& device, const MeshArtifact& artifact,
            Ref<const MeshGpuResourceBundle>& outBundle, std::string& outError);
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
