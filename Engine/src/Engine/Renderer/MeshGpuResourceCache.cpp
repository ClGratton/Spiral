#include "Engine/Renderer/MeshGpuResourceCache.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <limits>

namespace Engine
{
    namespace
    {
        struct ArtifactIdentity
        {
            AssetHandle Asset = kInvalidAssetHandle;
            std::string NormalizedSourcePath;
            std::vector<MeshArtifactPrimitive> Primitives;
            std::vector<MeshArtifactVertex> Vertices;
            std::vector<u32> Indices;

            bool operator==(const ArtifactIdentity& other) const
            {
                if (Asset != other.Asset || NormalizedSourcePath != other.NormalizedSourcePath
                    || Primitives.size() != other.Primitives.size() || Vertices.size() != other.Vertices.size()
                    || Indices != other.Indices)
                    return false;
                for (size_t index = 0; index < Primitives.size(); ++index)
                {
                    const MeshArtifactPrimitive& left = Primitives[index];
                    const MeshArtifactPrimitive& right = other.Primitives[index];
                    if (left.SourceMeshIndex != right.SourceMeshIndex || left.SourcePrimitiveIndex != right.SourcePrimitiveIndex
                        || left.VertexByteOffset != right.VertexByteOffset || left.VertexByteSize != right.VertexByteSize
                        || left.IndexByteOffset != right.IndexByteOffset || left.IndexByteSize != right.IndexByteSize)
                        return false;
                }
                for (size_t index = 0; index < Vertices.size(); ++index)
                {
                    const MeshArtifactVertex& left = Vertices[index];
                    const MeshArtifactVertex& right = other.Vertices[index];
                    for (size_t component = 0; component < 3; ++component)
                        if (std::bit_cast<u32>(left.Position[component]) != std::bit_cast<u32>(right.Position[component])
                            || std::bit_cast<u32>(left.Color[component]) != std::bit_cast<u32>(right.Color[component])) return false;
                    for (size_t component = 0; component < 2; ++component)
                        if (std::bit_cast<u32>(left.UV[component]) != std::bit_cast<u32>(right.UV[component])) return false;
                }
                return true;
            }
        };

        ArtifactIdentity MakeIdentity(const MeshArtifact& artifact)
        {
            ArtifactIdentity identity;
            identity.Asset = artifact.Asset;
            identity.NormalizedSourcePath = std::filesystem::path(artifact.SourcePath).lexically_normal().generic_string();
            identity.Primitives = artifact.Primitives;
            identity.Vertices = artifact.Vertices;
            identity.Indices = artifact.Indices;
            return identity;
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
        ArtifactIdentity Identity;
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
        std::string validationError;
        if (m_Capacity == 0 || !ValidateMeshArtifact(artifact, validationError))
        {
            outError = m_Capacity == 0 ? "mesh GPU resource cache has zero capacity" : validationError;
            return false;
        }

        ArtifactIdentity identity = MakeIdentity(artifact);
        for (Entry& entry : m_Entries)
        {
            if (entry.Device == &device && entry.Identity == identity)
            {
                entry.LastAccess = ++m_NextAccess;
                outBundle = entry.Bundle;
                return true;
            }
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

        m_Entries.push_back({ &device, std::move(identity), bundle, ++m_NextAccess });
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
