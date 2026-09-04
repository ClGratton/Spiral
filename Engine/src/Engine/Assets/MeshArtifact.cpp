#include "Engine/Assets/MeshArtifact.h"

#include "Engine/Assets/AssetRegistry.h"

#include <algorithm>
#include <array>
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
        constexpr u32 kMeshArtifactVersion = 2;
        constexpr u64 kLegacyMeshArtifactVertexStrideBytes = 32;
        constexpr u64 kMeshArtifactVertexStrideBytes = 44;
        constexpr u64 kMeshArtifactIndexStrideBytes = 4;
        constexpr u64 kMaxMeshArtifactVertices = 16ull * 1024ull * 1024ull;
        constexpr u64 kMaxMeshArtifactIndices = 48ull * 1024ull * 1024ull;
        constexpr double kMeshArtifactNormalLengthTolerance = 0.0001;

        bool IsFinite(float value)
        {
            return std::isfinite(value);
        }

        bool IsZeroNormal(const MeshArtifactVertex& vertex)
        {
            return vertex.Normal[0] == 0.0f && vertex.Normal[1] == 0.0f && vertex.Normal[2] == 0.0f;
        }

        bool Normalize(float normal[3])
        {
            const double lengthSquared = static_cast<double>(normal[0]) * normal[0]
                + static_cast<double>(normal[1]) * normal[1]
                + static_cast<double>(normal[2]) * normal[2];
            if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
                return false;
            const double inverseLength = 1.0 / std::sqrt(lengthSquared);
            for (size_t component = 0; component < 3; ++component)
                normal[component] = static_cast<float>(normal[component] * inverseLength);
            return IsFinite(normal[0]) && IsFinite(normal[1]) && IsFinite(normal[2]);
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

        constexpr std::string_view kDefaultSceneMeshSourcePath = "Engine/Generated/PrototypeCube.mesh";
    }

    std::filesystem::path GetCookedMeshArtifactPath(AssetHandle asset)
    {
        return std::filesystem::path("output") / "imports" / "gltf" / (std::to_string(asset) + ".spiralmesh");
    }

    std::string_view GetDefaultSceneMeshSourcePath()
    {
        return kDefaultSceneMeshSourcePath;
    }

    bool CreateDefaultSceneMeshArtifact(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError)
    {
        if (asset == kInvalidAssetHandle)
        {
            outError = "default scene mesh has an invalid asset handle";
            return false;
        }

        MeshArtifact candidate;
        candidate.Asset = asset;
        candidate.SourcePath = std::string(kDefaultSceneMeshSourcePath);
        const auto vertex = [](std::array<float, 3> position, std::array<float, 3> normal,
                               std::array<float, 3> color, std::array<float, 2> uv)
        {
            MeshArtifactVertex result;
            std::copy(position.begin(), position.end(), result.Position);
            std::copy(normal.begin(), normal.end(), result.Normal);
            std::copy(color.begin(), color.end(), result.Color);
            std::copy(uv.begin(), uv.end(), result.UV);
            return result;
        };
        candidate.Vertices = {
            vertex({-0.75f,-0.75f,-0.75f},{0,0,-1},{0.22f,0.68f,1.00f},{0,1}), vertex({-0.75f,0.75f,-0.75f},{0,0,-1},{0.22f,0.68f,1.00f},{0,0}), vertex({0.75f,0.75f,-0.75f},{0,0,-1},{0.22f,0.68f,1.00f},{1,0}), vertex({0.75f,-0.75f,-0.75f},{0,0,-1},{0.22f,0.68f,1.00f},{1,1}),
            vertex({0.75f,-0.75f,0.75f},{0,0,1},{0.95f,0.72f,0.28f},{0,1}), vertex({0.75f,0.75f,0.75f},{0,0,1},{0.95f,0.72f,0.28f},{0,0}), vertex({-0.75f,0.75f,0.75f},{0,0,1},{0.95f,0.72f,0.28f},{1,0}), vertex({-0.75f,-0.75f,0.75f},{0,0,1},{0.95f,0.72f,0.28f},{1,1}),
            vertex({-0.75f,-0.75f,0.75f},{-1,0,0},{0.26f,0.88f,0.55f},{0,1}), vertex({-0.75f,0.75f,0.75f},{-1,0,0},{0.26f,0.88f,0.55f},{0,0}), vertex({-0.75f,0.75f,-0.75f},{-1,0,0},{0.26f,0.88f,0.55f},{1,0}), vertex({-0.75f,-0.75f,-0.75f},{-1,0,0},{0.26f,0.88f,0.55f},{1,1}),
            vertex({0.75f,-0.75f,-0.75f},{1,0,0},{0.88f,0.35f,0.37f},{0,1}), vertex({0.75f,0.75f,-0.75f},{1,0,0},{0.88f,0.35f,0.37f},{0,0}), vertex({0.75f,0.75f,0.75f},{1,0,0},{0.88f,0.35f,0.37f},{1,0}), vertex({0.75f,-0.75f,0.75f},{1,0,0},{0.88f,0.35f,0.37f},{1,1}),
            vertex({-0.75f,0.75f,-0.75f},{0,1,0},{0.72f,0.52f,0.96f},{0,1}), vertex({-0.75f,0.75f,0.75f},{0,1,0},{0.72f,0.52f,0.96f},{0,0}), vertex({0.75f,0.75f,0.75f},{0,1,0},{0.72f,0.52f,0.96f},{1,0}), vertex({0.75f,0.75f,-0.75f},{0,1,0},{0.72f,0.52f,0.96f},{1,1}),
            vertex({-0.75f,-0.75f,0.75f},{0,-1,0},{0.24f,0.75f,0.82f},{0,1}), vertex({-0.75f,-0.75f,-0.75f},{0,-1,0},{0.24f,0.75f,0.82f},{0,0}), vertex({0.75f,-0.75f,-0.75f},{0,-1,0},{0.24f,0.75f,0.82f},{1,0}), vertex({0.75f,-0.75f,0.75f},{0,-1,0},{0.24f,0.75f,0.82f},{1,1})
        };
        candidate.Indices = {
            0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
            12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23
        };
        candidate.Primitives = {{ 0, 0, 0, sizeof(MeshArtifactVertex) * candidate.Vertices.size(), 0, sizeof(u32) * candidate.Indices.size() }};
        if (!ValidateMeshArtifact(candidate, outError))
            return false;

        outArtifact = std::move(candidate);
        outError.clear();
        return true;
    }

    bool EnsureMeshArtifactGeometricNormals(MeshArtifact& artifact, std::string& outError)
    {
        std::vector<MeshArtifactVertex> candidate = artifact.Vertices;
        for (const MeshArtifactPrimitive& primitive : artifact.Primitives)
        {
            if (primitive.VertexByteOffset % sizeof(MeshArtifactVertex) != 0
                || primitive.VertexByteSize % sizeof(MeshArtifactVertex) != 0
                || primitive.IndexByteOffset % sizeof(u32) != 0
                || primitive.IndexByteSize % sizeof(u32) != 0)
            {
                outError = "mesh normal generation requires current element-aligned primitive ranges";
                return false;
            }
            const size_t firstVertex = static_cast<size_t>(primitive.VertexByteOffset / sizeof(MeshArtifactVertex));
            const size_t vertexCount = static_cast<size_t>(primitive.VertexByteSize / sizeof(MeshArtifactVertex));
            const size_t firstIndex = static_cast<size_t>(primitive.IndexByteOffset / sizeof(u32));
            const size_t indexCount = static_cast<size_t>(primitive.IndexByteSize / sizeof(u32));
            if (firstVertex > candidate.size() || vertexCount > candidate.size() - firstVertex
                || firstIndex > artifact.Indices.size() || indexCount > artifact.Indices.size() - firstIndex
                || indexCount == 0 || indexCount % 3 != 0)
            {
                outError = "mesh normal generation received an invalid primitive range";
                return false;
            }
            bool anyZero = false;
            bool anyNonzero = false;
            for (size_t index = firstVertex; index < firstVertex + vertexCount; ++index)
            {
                const MeshArtifactVertex& vertex = candidate[index];
                const double lengthSquared = static_cast<double>(vertex.Normal[0]) * vertex.Normal[0]
                    + static_cast<double>(vertex.Normal[1]) * vertex.Normal[1]
                    + static_cast<double>(vertex.Normal[2]) * vertex.Normal[2];
                if (!std::isfinite(lengthSquared))
                {
                    outError = "mesh primitive has a non-finite authored geometric normal";
                    return false;
                }
                if (IsZeroNormal(vertex))
                    anyZero = true;
                else
                {
                    anyNonzero = true;
                    if (std::abs(lengthSquared - 1.0) > kMeshArtifactNormalLengthTolerance)
                    {
                        outError = "mesh primitive has a non-unit authored geometric normal";
                        return false;
                    }
                }
            }
            if (anyZero && anyNonzero)
            {
                outError = "mesh primitive mixes authored and missing geometric normals";
                return false;
            }
            if (!anyZero)
                continue;

            std::vector<std::array<double, 3>> accumulated(vertexCount);
            for (size_t index = firstIndex; index < firstIndex + indexCount; index += 3)
            {
                const u32 ia = artifact.Indices[index], ib = artifact.Indices[index + 1], ic = artifact.Indices[index + 2];
                if (ia < firstVertex || ib < firstVertex || ic < firstVertex
                    || ia >= firstVertex + vertexCount || ib >= firstVertex + vertexCount || ic >= firstVertex + vertexCount)
                {
                    outError = "mesh normal generation found an index outside its primitive";
                    return false;
                }
                const MeshArtifactVertex& a = candidate[ia];
                const MeshArtifactVertex& b = candidate[ib];
                const MeshArtifactVertex& c = candidate[ic];
                const double ab[3] { b.Position[0] - a.Position[0], b.Position[1] - a.Position[1], b.Position[2] - a.Position[2] };
                const double ac[3] { c.Position[0] - a.Position[0], c.Position[1] - a.Position[1], c.Position[2] - a.Position[2] };
                const std::array<double, 3> face {
                    ab[1] * ac[2] - ab[2] * ac[1],
                    ab[2] * ac[0] - ab[0] * ac[2],
                    ab[0] * ac[1] - ab[1] * ac[0]
                };
                const double lengthSquared = face[0] * face[0] + face[1] * face[1] + face[2] * face[2];
                if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
                {
                    outError = "mesh normal generation rejects degenerate triangles";
                    return false;
                }
                for (u32 vertexIndex : { ia, ib, ic })
                    for (size_t component = 0; component < 3; ++component)
                        accumulated[vertexIndex - firstVertex][component] += face[component];
            }
            for (size_t index = 0; index < vertexCount; ++index)
            {
                for (size_t component = 0; component < 3; ++component)
                    candidate[firstVertex + index].Normal[component] = static_cast<float>(accumulated[index][component]);
                if (!Normalize(candidate[firstVertex + index].Normal))
                {
                    outError = "mesh normal generation found an unreferenced or cancelling vertex normal";
                    return false;
                }
            }
        }
        artifact.Vertices = std::move(candidate);
        outError.clear();
        return true;
    }

    bool StoreDefaultSceneMeshArtifact(AssetHandle asset, std::string& outError)
    {
        MeshArtifact artifact;
        return CreateDefaultSceneMeshArtifact(asset, artifact, outError)
            && StoreMeshArtifact(GetCookedMeshArtifactPath(asset), artifact, outError);
    }

    bool EnsureDefaultSceneMeshArtifact(AssetRegistry& registry, AssetHandle& outAsset, std::string& outError)
    {
        const AssetHandle existing = registry.FindAssetByPath(AssetType::Mesh, kDefaultSceneMeshSourcePath);
        const bool newlyRegistered = existing == kInvalidAssetHandle;
        const AssetHandle candidate = newlyRegistered
            ? registry.RegisterAsset(AssetType::Mesh, std::string(kDefaultSceneMeshSourcePath), "Prototype Cube")
            : existing;
        if (candidate == kInvalidAssetHandle || !StoreDefaultSceneMeshArtifact(candidate, outError))
        {
            if (newlyRegistered && candidate != kInvalidAssetHandle)
                registry.RemoveAsset(candidate);
            if (candidate == kInvalidAssetHandle && outError.empty())
                outError = "could not register the default scene mesh";
            return false;
        }

        outAsset = candidate;
        outError.clear();
        return true;
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
            double normalLengthSquared = 0.0;
            for (float component : vertex.Normal)
            {
                if (!IsFinite(component))
                {
                    outError = "mesh artifact has a non-finite geometric normal";
                    return false;
                }
                normalLengthSquared += static_cast<double>(component) * component;
            }
            if (!std::isfinite(normalLengthSquared)
                || std::abs(normalLengthSquared - 1.0) > kMeshArtifactNormalLengthTolerance)
            {
                outError = "mesh artifact geometric normal is not normalized";
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
        output << "VertexLayout PositionNormalColorUV32F\n";
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
                << vertex.Normal[0] << ' ' << vertex.Normal[1] << ' ' << vertex.Normal[2] << ' '
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
        if (!ReadExpected(input, "SpiralMeshArtifact") || !(input >> version) || (version != 1 && version != kMeshArtifactVersion)
            || !ReadExpected(input, "Source") || !(input >> std::quoted(candidate.SourcePath))
            || !ReadExpected(input, "MeshAsset") || !(input >> candidate.Asset)
            || !ReadExpected(input, "VertexLayout") || !(input >> layout)
            || (version == 1 ? layout != "PositionColorUV32F" : layout != "PositionNormalColorUV32F")
            || !ReadExpected(input, "VertexStrideBytes") || !(input >> vertexCount)
            || vertexCount != (version == 1 ? kLegacyMeshArtifactVertexStrideBytes : kMeshArtifactVertexStrideBytes)
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
        if (version == 1)
        {
            for (MeshArtifactPrimitive& primitive : candidate.Primitives)
            {
                if (primitive.VertexByteOffset % kLegacyMeshArtifactVertexStrideBytes != 0
                    || primitive.VertexByteSize % kLegacyMeshArtifactVertexStrideBytes != 0)
                {
                    outError = "legacy cooked mesh artifact primitive range is malformed";
                    return false;
                }
                primitive.VertexByteOffset = primitive.VertexByteOffset / kLegacyMeshArtifactVertexStrideBytes * kMeshArtifactVertexStrideBytes;
                primitive.VertexByteSize = primitive.VertexByteSize / kLegacyMeshArtifactVertexStrideBytes * kMeshArtifactVertexStrideBytes;
            }
        }
        if (!ReadExpected(input, "Vertices"))
        {
            outError = "cooked mesh artifact is missing vertex data";
            return false;
        }

        candidate.Vertices.resize(static_cast<size_t>(vertexCount));
        for (MeshArtifactVertex& vertex : candidate.Vertices)
            if (!(input >> vertex.Position[0] >> vertex.Position[1] >> vertex.Position[2]))
            {
                outError = "cooked mesh artifact vertex data is malformed";
                return false;
            }
            else if (version == 2 && !(input >> vertex.Normal[0] >> vertex.Normal[1] >> vertex.Normal[2]))
            {
                outError = "cooked mesh artifact geometric normal data is malformed";
                return false;
            }
            else if (!(input >> vertex.Color[0] >> vertex.Color[1] >> vertex.Color[2]
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
        if (!ReadExpected(input, "End")
            || (version == 1 && !EnsureMeshArtifactGeometricNormals(candidate, outError))
            || !ValidateMeshArtifact(candidate, outError))
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

    MeshArtifactResolver::MeshArtifactResolver(const AssetRegistry& registry)
        : m_Registry(std::make_shared<const AssetRegistry>(registry))
    {
    }

    MeshArtifactResolver::MeshArtifactResolver(std::shared_ptr<const AssetRegistry> registry)
        : m_Registry(std::move(registry))
    {
    }

    bool MeshArtifactResolver::Resolve(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError) const
    {
        if (!m_Registry)
        {
            outError = "mesh artifact resolver has no published asset registry";
            return false;
        }

        return ResolveMeshArtifact(*m_Registry, asset, outArtifact, outError);
    }
}
