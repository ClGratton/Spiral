#include "Engine/Renderer/MeshGpuResourceCache.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <limits>

namespace Engine
{
    namespace
    {
        // Content identity of two decoded artifacts. Floats compare by bit
        // pattern so NaN payloads and signed zeros are distinguished exactly.
        bool SameArtifactContent(const MeshArtifact& left, const MeshArtifact& right)
        {
            if (&left == &right)
                return true;
            if (left.Asset != right.Asset
                || left.Primitives.size() != right.Primitives.size()
                || left.Vertices.size() != right.Vertices.size()
                || left.Indices != right.Indices
                || std::filesystem::path(left.SourcePath).lexically_normal().generic_string()
                    != std::filesystem::path(right.SourcePath).lexically_normal().generic_string())
                return false;
            for (size_t index = 0; index < left.Primitives.size(); ++index)
            {
                const MeshArtifactPrimitive& l = left.Primitives[index];
                const MeshArtifactPrimitive& r = right.Primitives[index];
                if (l.SourceMeshIndex != r.SourceMeshIndex || l.SourcePrimitiveIndex != r.SourcePrimitiveIndex
                    || l.VertexByteOffset != r.VertexByteOffset || l.VertexByteSize != r.VertexByteSize
                    || l.IndexByteOffset != r.IndexByteOffset || l.IndexByteSize != r.IndexByteSize)
                    return false;
            }
            for (size_t index = 0; index < left.Vertices.size(); ++index)
            {
                const MeshArtifactVertex& l = left.Vertices[index];
                const MeshArtifactVertex& r = right.Vertices[index];
                for (size_t component = 0; component < 3; ++component)
                    if (std::bit_cast<u32>(l.Position[component]) != std::bit_cast<u32>(r.Position[component])
                        || std::bit_cast<u32>(l.Normal[component]) != std::bit_cast<u32>(r.Normal[component])
                        || std::bit_cast<u32>(l.Color[component]) != std::bit_cast<u32>(r.Color[component])) return false;
                for (size_t component = 0; component < 2; ++component)
                    if (std::bit_cast<u32>(l.UV[component]) != std::bit_cast<u32>(r.UV[component])) return false;
            }
            return true;
        }

        bool ToU32(u64 value, u32& outValue)
        {
            if (value > std::numeric_limits<u32>::max())
                return false;
            outValue = static_cast<u32>(value);
            return true;
        }

        bool BuildPrimitiveRanges(const MeshArtifact& artifact, std::vector<MeshGpuPrimitiveRange>& outRanges, std::string& outError)
        {
            std::vector<MeshGpuPrimitiveRange> ranges;
            ranges.reserve(artifact.Primitives.size());
            for (const MeshArtifactPrimitive& primitive : artifact.Primitives)
            {
                if (primitive.VertexByteOffset % sizeof(MeshArtifactVertex) != 0 || primitive.VertexByteSize % sizeof(MeshArtifactVertex) != 0
                    || primitive.IndexByteOffset % sizeof(u32) != 0 || primitive.IndexByteSize % sizeof(u32) != 0)
                {
                    outError = "mesh artifact primitive ranges are not element aligned";
                    return false;
                }

                MeshGpuPrimitiveRange range;
                range.SourceMeshIndex = primitive.SourceMeshIndex;
                range.SourcePrimitiveIndex = primitive.SourcePrimitiveIndex;
                if (!ToU32(primitive.VertexByteOffset / sizeof(MeshArtifactVertex), range.FirstVertex)
                    || !ToU32(primitive.VertexByteSize / sizeof(MeshArtifactVertex), range.VertexCount)
                    || !ToU32(primitive.IndexByteOffset / sizeof(u32), range.FirstIndex)
                    || !ToU32(primitive.IndexByteSize / sizeof(u32), range.IndexCount))
                {
                    outError = "mesh artifact primitive range exceeds portable draw limits";
                    return false;
                }
                ranges.push_back(range);
            }
            outRanges = std::move(ranges);
            return true;
        }
    }

    struct MeshGpuResourceCache::Entry
    {
        const RHI::Device* Device = nullptr;
        // The entry retains the exact decoded artifact it was uploaded from.
        // Holding the reference keeps its address unique, so a pointer match
        // is an exact content match and needs no per-vertex comparison.
        Ref<const MeshArtifact> Source;
        Ref<const MeshGpuResourceBundle> Bundle;
        u64 LastAccess = 0;
    };

    MeshGpuResourceCache::MeshGpuResourceCache(size_t capacity)
        : m_Capacity(capacity)
    {
    }

    MeshGpuResourceCache::~MeshGpuResourceCache() = default;

    bool MeshGpuResourceCache::Acquire(RHI::Device& device, const MeshArtifact& artifact,
        Ref<const MeshGpuResourceBundle>& outBundle, std::string& outError)
    {
        if (m_Capacity == 0)
        {
            outError = "mesh GPU resource cache has zero capacity";
            return false;
        }
        // A content hit needs no copy and no revalidation: an equal artifact
        // was validated when its entry was created.
        for (Entry& entry : m_Entries)
        {
            if (entry.Device == &device && SameArtifactContent(*entry.Source, artifact))
            {
                entry.LastAccess = ++m_NextAccess;
                outBundle = entry.Bundle;
                return true;
            }
        }
        return AcquireMiss(device, CreateRef<const MeshArtifact>(artifact), outBundle, outError);
    }

    bool MeshGpuResourceCache::Acquire(RHI::Device& device, const Ref<const MeshArtifact>& artifact,
        Ref<const MeshGpuResourceBundle>& outBundle, std::string& outError)
    {
        if (m_Capacity == 0 || !artifact)
        {
            outError = m_Capacity == 0 ? "mesh GPU resource cache has zero capacity"
                : "mesh GPU resource cache requires a decoded artifact";
            return false;
        }
        for (Entry& entry : m_Entries)
        {
            // Pointer identity is the steady-state fast path for artifacts
            // shared by a resolver snapshot; content equality still lets an
            // independently decoded equal artifact reuse the upload.
            if (entry.Device == &device
                && (entry.Source == artifact || SameArtifactContent(*entry.Source, *artifact)))
            {
                // Adopt the newest equal reference (for example the next
                // snapshot generation's decode of unchanged content) so later
                // frames hit the pointer fast path instead of comparing again.
                entry.Source = artifact;
                entry.LastAccess = ++m_NextAccess;
                outBundle = entry.Bundle;
                return true;
            }
        }
        return AcquireMiss(device, artifact, outBundle, outError);
    }

    bool MeshGpuResourceCache::AcquireMiss(RHI::Device& device, const Ref<const MeshArtifact>& source,
        Ref<const MeshGpuResourceBundle>& outBundle, std::string& outError)
    {
        const MeshArtifact& artifact = *source;
        std::string validationError;
        if (!ValidateMeshArtifact(artifact, validationError))
        {
            outError = validationError;
            return false;
        }

        std::vector<MeshGpuPrimitiveRange> primitiveRanges;
        if (!BuildPrimitiveRanges(artifact, primitiveRanges, outError))
            return false;

        RHI::BufferDescription vertexDescription;
        vertexDescription.DebugName = "MeshArtifact Vertex Buffer";
        vertexDescription.SizeBytes = artifact.Vertices.size() * sizeof(MeshArtifactVertex);
        vertexDescription.StrideBytes = sizeof(MeshArtifactVertex);
        vertexDescription.Usage = static_cast<RHI::BufferUsage>(static_cast<u32>(RHI::BufferUsage::Vertex)
            | static_cast<u32>(RHI::BufferUsage::CopyDest));
        vertexDescription.CpuAccess = RHI::BufferCpuAccess::None;
        vertexDescription.InitialState = RHI::ResourceState::Common;
        Scope<RHI::Buffer> vertex = device.CreateBuffer(vertexDescription);
        if (!vertex)
        {
            outError = "mesh GPU resource cache could not create the vertex buffer";
            return false;
        }

        RHI::BufferDescription indexDescription;
        indexDescription.DebugName = "MeshArtifact Index Buffer";
        indexDescription.SizeBytes = artifact.Indices.size() * sizeof(u32);
        indexDescription.StrideBytes = sizeof(u32);
        indexDescription.Usage = static_cast<RHI::BufferUsage>(static_cast<u32>(RHI::BufferUsage::Index)
            | static_cast<u32>(RHI::BufferUsage::CopyDest));
        indexDescription.CpuAccess = RHI::BufferCpuAccess::None;
        indexDescription.InitialState = RHI::ResourceState::Common;
        Scope<RHI::Buffer> index = device.CreateBuffer(indexDescription);
        if (!index)
        {
            outError = "mesh GPU resource cache could not create the index buffer";
            return false;
        }

        if (!device.UploadBuffer(*vertex, artifact.Vertices.data(), vertexDescription.SizeBytes)
            || !device.UploadBuffer(*index, artifact.Indices.data(), indexDescription.SizeBytes))
        {
            outError = "mesh GPU resource cache could not upload immutable mesh bytes";
            return false;
        }

        Ref<MeshGpuResourceBundle> bundle = CreateRef<MeshGpuResourceBundle>();
        bundle->VertexBuffer = Ref<RHI::Buffer>(vertex.release());
        bundle->IndexBuffer = Ref<RHI::Buffer>(index.release());
        bundle->Primitives = std::move(primitiveRanges);
        bundle->Generation = ++m_NextGeneration;

        if (m_Entries.size() == m_Capacity)
        {
            const auto eviction = std::min_element(m_Entries.begin(), m_Entries.end(), [](const Entry& left, const Entry& right)
            {
                return left.LastAccess != right.LastAccess ? left.LastAccess < right.LastAccess
                    : left.Bundle->Generation < right.Bundle->Generation;
            });
            m_Entries.erase(eviction);
        }

        m_Entries.push_back({ &device, source, bundle, ++m_NextAccess });
        outBundle = std::move(bundle);
        return true;
    }

    void MeshGpuResourceCache::Clear()
    {
        m_Entries.clear();
    }

    size_t MeshGpuResourceCache::GetEntryCount() const
    {
        return m_Entries.size();
    }
}
