#include "Engine/Assets/MeshArtifact.h"

#include "Engine/Assets/AssetRegistry.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <string_view>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

namespace Engine
{
    namespace
    {
        constexpr u32 kMeshArtifactVersion = 1;
        constexpr u64 kMeshArtifactVertexStrideBytes = 32;
        constexpr u64 kMeshArtifactIndexStrideBytes = 4;
        constexpr u64 kMaxMeshArtifactVertices = 16ull * 1024ull * 1024ull;
        constexpr u64 kMaxMeshArtifactIndices = 48ull * 1024ull * 1024ull;

        bool IsFinite(float value)
        {
            return std::isfinite(value);
        }

        bool ReadExpected(std::istream& input, std::string_view expected)
        {
            std::string value;
            return static_cast<bool>(input >> value) && value == expected;
        }

        bool PublishAtomically(const std::filesystem::path& temporary, const std::filesystem::path& final, std::string& outError)
        {
#if defined(_WIN32)
            if (::ReplaceFileW(final.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
                return true;
            const DWORD replaceError = ::GetLastError();
            if (replaceError == ERROR_FILE_NOT_FOUND
                && ::MoveFileExW(temporary.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            outError = "could not atomically publish cooked mesh artifact (Windows error "
                + std::to_string(replaceError == ERROR_FILE_NOT_FOUND ? ::GetLastError() : replaceError) + ")";
#else
            if (std::rename(temporary.c_str(), final.c_str()) == 0)
                return true;
            outError = "could not atomically publish cooked mesh artifact";
#endif
            return false;
        }

        static_assert(sizeof(MeshArtifactVertex) == kMeshArtifactVertexStrideBytes);
    }

    std::filesystem::path GetCookedMeshArtifactPath(AssetHandle asset)
    {
        return std::filesystem::path("output") / "imports" / "gltf" / (std::to_string(asset) + ".spiralmesh");
    }

    bool ValidateMeshArtifact(const MeshArtifact& artifact, std::string& outError)
    {
        if (artifact.Asset == kInvalidAssetHandle)
        {
            outError = "mesh artifact has an invalid asset handle";
            return false;
        }
        if (artifact.SourcePath.empty())
        {
            outError = "mesh artifact has an empty source path";
            return false;
        }
        if (artifact.Vertices.empty() || artifact.Vertices.size() > kMaxMeshArtifactVertices)
        {
            outError = "mesh artifact has an invalid vertex count";
            return false;
        }
        if (artifact.Indices.empty() || artifact.Indices.size() > kMaxMeshArtifactIndices || artifact.Indices.size() % 3 != 0)
        {
            outError = "mesh artifact has an invalid triangle index count";
            return false;
        }
        if (artifact.Primitives.empty())
        {
            outError = "mesh artifact has no supported primitives";
            return false;
        }
        for (const MeshArtifactVertex& vertex : artifact.Vertices)
        {
            for (float component : vertex.Position)
                if (!IsFinite(component))
                {
                    outError = "mesh artifact has a non-finite position";
                    return false;
                }
            for (float component : vertex.Color)
                if (!IsFinite(component))
                {
                    outError = "mesh artifact has a non-finite color";
                    return false;
                }
            for (float component : vertex.UV)
                if (!IsFinite(component))
                {
                    outError = "mesh artifact has a non-finite UV";
                    return false;
                }
        }
        for (u32 index : artifact.Indices)
            if (index >= artifact.Vertices.size())
            {
                outError = "mesh artifact index is outside the vertex range";
                return false;
            }
        for (const MeshArtifactPrimitive& primitive : artifact.Primitives)
        {
            const u64 totalVertexBytes = artifact.Vertices.size() * kMeshArtifactVertexStrideBytes;
            const u64 totalIndexBytes = artifact.Indices.size() * kMeshArtifactIndexStrideBytes;
            if (primitive.VertexByteSize == 0 || primitive.IndexByteSize == 0
                || primitive.VertexByteOffset % kMeshArtifactVertexStrideBytes != 0
                || primitive.VertexByteSize % kMeshArtifactVertexStrideBytes != 0
                || primitive.IndexByteOffset % kMeshArtifactIndexStrideBytes != 0
                || primitive.IndexByteSize % kMeshArtifactIndexStrideBytes != 0
                || primitive.IndexByteSize / kMeshArtifactIndexStrideBytes % 3 != 0
                || primitive.VertexByteOffset > totalVertexBytes || primitive.VertexByteSize > totalVertexBytes - primitive.VertexByteOffset
                || primitive.IndexByteOffset > totalIndexBytes || primitive.IndexByteSize > totalIndexBytes - primitive.IndexByteOffset)
            {
                outError = "mesh artifact primitive range is invalid";
                return false;
            }
            const u64 firstVertex = primitive.VertexByteOffset / kMeshArtifactVertexStrideBytes;
            const u64 vertexCount = primitive.VertexByteSize / kMeshArtifactVertexStrideBytes;
            const u64 firstIndex = primitive.IndexByteOffset / kMeshArtifactIndexStrideBytes;
            const u64 indexCount = primitive.IndexByteSize / kMeshArtifactIndexStrideBytes;
            for (u64 index = firstIndex; index < firstIndex + indexCount; ++index)
                if (artifact.Indices[static_cast<size_t>(index)] < firstVertex
                    || artifact.Indices[static_cast<size_t>(index)] >= firstVertex + vertexCount)
                {
                    outError = "mesh artifact primitive index escapes its vertex range";
                    return false;
                }
        }

        outError.clear();
        return true;
    }

    bool StoreMeshArtifact(const std::filesystem::path& path, const MeshArtifact& artifact, std::string& outError)
    {
        if (!ValidateMeshArtifact(artifact, outError))
            return false;

        std::error_code error;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                outError = "could not create cooked mesh directory: " + error.message();
                return false;
            }
        }

        static std::atomic<u64> temporarySequence { 0 };
        const std::filesystem::path temporary = path.string() + ".tmp."
            + std::to_string(temporarySequence.fetch_add(1, std::memory_order_relaxed));
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if (!output)
        {
            outError = "could not open temporary cooked mesh artifact";
            return false;
        }

        output << std::setprecision(std::numeric_limits<float>::max_digits10);
        output << "SpiralMeshArtifact " << kMeshArtifactVersion << '\n';
        output << "Source " << std::quoted(artifact.SourcePath) << '\n';
        output << "MeshAsset " << artifact.Asset << '\n';
        output << "VertexLayout PositionColorUV32F\n";
        output << "VertexStrideBytes " << kMeshArtifactVertexStrideBytes << '\n';
        output << "VertexCount " << artifact.Vertices.size() << '\n';
        output << "IndexFormat UInt32\n";
        output << "IndexStrideBytes " << kMeshArtifactIndexStrideBytes << '\n';
        output << "IndexCount " << artifact.Indices.size() << '\n';
        output << "PrimitiveCount " << artifact.Primitives.size() << '\n';
        for (const MeshArtifactPrimitive& primitive : artifact.Primitives)
            output << "Primitive " << primitive.SourceMeshIndex << ' ' << primitive.SourcePrimitiveIndex << ' '
                << primitive.VertexByteOffset << ' ' << primitive.VertexByteSize << ' '
                << primitive.IndexByteOffset << ' ' << primitive.IndexByteSize << '\n';
        output << "Vertices\n";
        for (const MeshArtifactVertex& vertex : artifact.Vertices)
        {
            output << vertex.Position[0] << ' ' << vertex.Position[1] << ' ' << vertex.Position[2] << ' '
                << vertex.Color[0] << ' ' << vertex.Color[1] << ' ' << vertex.Color[2] << ' '
                << vertex.UV[0] << ' ' << vertex.UV[1] << '\n';
        }
        output << "Indices\n";
        for (u32 index : artifact.Indices)
            output << index << '\n';
        output << "End\n";
        output.close();
        if (!output)
        {
            std::filesystem::remove(temporary, error);
            outError = "could not write cooked mesh artifact";
            return false;
        }

        if (!PublishAtomically(temporary, path, outError))
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporary, cleanupError);
            return false;
        }

        outError.clear();
        return true;
    }

    bool LoadMeshArtifact(const std::filesystem::path& path, MeshArtifact& outArtifact, std::string& outError)
    {
        std::ifstream input(path);
        if (!input)
        {
            outError = "could not open cooked mesh artifact";
            return false;
        }

        MeshArtifact candidate;
        u32 version = 0;
        u64 vertexCount = 0;
        u64 indexCount = 0;
        u64 primitiveCount = 0;
        std::string layout;
        std::string indexFormat;
        if (!ReadExpected(input, "SpiralMeshArtifact") || !(input >> version) || version != kMeshArtifactVersion
            || !ReadExpected(input, "Source") || !(input >> std::quoted(candidate.SourcePath))
            || !ReadExpected(input, "MeshAsset") || !(input >> candidate.Asset)
            || !ReadExpected(input, "VertexLayout") || !(input >> layout) || layout != "PositionColorUV32F"
            || !ReadExpected(input, "VertexStrideBytes") || !(input >> vertexCount) || vertexCount != kMeshArtifactVertexStrideBytes
            || !ReadExpected(input, "VertexCount") || !(input >> vertexCount) || vertexCount == 0 || vertexCount > kMaxMeshArtifactVertices
            || !ReadExpected(input, "IndexFormat") || !(input >> indexFormat) || indexFormat != "UInt32"
            || !ReadExpected(input, "IndexStrideBytes") || !(input >> indexCount) || indexCount != kMeshArtifactIndexStrideBytes
            || !ReadExpected(input, "IndexCount") || !(input >> indexCount) || indexCount == 0 || indexCount > kMaxMeshArtifactIndices || indexCount % 3 != 0
            || !ReadExpected(input, "PrimitiveCount") || !(input >> primitiveCount) || primitiveCount == 0 || primitiveCount > indexCount / 3)
        {
            outError = "cooked mesh artifact header is malformed or unsupported";
            return false;
        }

        candidate.Primitives.resize(static_cast<size_t>(primitiveCount));
        for (MeshArtifactPrimitive& primitive : candidate.Primitives)
            if (!ReadExpected(input, "Primitive") || !(input >> primitive.SourceMeshIndex >> primitive.SourcePrimitiveIndex
                >> primitive.VertexByteOffset >> primitive.VertexByteSize >> primitive.IndexByteOffset >> primitive.IndexByteSize))
            {
                outError = "cooked mesh artifact primitive range is malformed";
                return false;
            }
        if (!ReadExpected(input, "Vertices"))
        {
            outError = "cooked mesh artifact is missing vertex data";
            return false;
        }

        candidate.Vertices.resize(static_cast<size_t>(vertexCount));
        for (MeshArtifactVertex& vertex : candidate.Vertices)
            if (!(input >> vertex.Position[0] >> vertex.Position[1] >> vertex.Position[2]
                >> vertex.Color[0] >> vertex.Color[1] >> vertex.Color[2]
                >> vertex.UV[0] >> vertex.UV[1]))
            {
                outError = "cooked mesh artifact vertex data is malformed";
                return false;
            }

        if (!ReadExpected(input, "Indices"))
        {
            outError = "cooked mesh artifact is missing index data";
            return false;
        }
        candidate.Indices.resize(static_cast<size_t>(indexCount));
        for (u32& index : candidate.Indices)
            if (!(input >> index))
            {
                outError = "cooked mesh artifact index data is malformed";
                return false;
            }
        if (!ReadExpected(input, "End") || !ValidateMeshArtifact(candidate, outError))
            return false;

        std::string trailing;
        if (input >> trailing)
        {
            outError = "cooked mesh artifact has trailing data";
            return false;
        }

        outArtifact = std::move(candidate);
        outError.clear();
        return true;
    }

    bool ResolveMeshArtifact(const AssetRegistry& registry, AssetHandle asset, MeshArtifact& outArtifact, std::string& outError)
    {
        const AssetMetadata* metadata = registry.GetAsset(asset);
        if (!metadata || metadata->Type != AssetType::Mesh)
        {
            outError = "mesh asset handle is missing or has the wrong type";
            return false;
        }

        MeshArtifact candidate;
        if (!LoadMeshArtifact(GetCookedMeshArtifactPath(asset), candidate, outError))
            return false;
        if (candidate.Asset != asset || candidate.SourcePath != metadata->SourcePath)
        {
            outError = "cooked mesh artifact provenance does not match the registry";
            return false;
        }

        outArtifact = std::move(candidate);
        return true;
    }
}
