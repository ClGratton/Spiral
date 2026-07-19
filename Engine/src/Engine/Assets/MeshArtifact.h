#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Base.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Engine
{
    class AssetRegistry;

    struct MeshArtifactVertex
    {
        float Position[3] {};
        float Color[3] { 1.0f, 1.0f, 1.0f };
        float UV[2] {};
    };

    struct MeshArtifactPrimitive
    {
        u32 SourceMeshIndex = 0;
        u32 SourcePrimitiveIndex = 0;
        u64 VertexByteOffset = 0;
        u64 VertexByteSize = 0;
        u64 IndexByteOffset = 0;
        u64 IndexByteSize = 0;
    };

    struct MeshArtifact
    {
        AssetHandle Asset = kInvalidAssetHandle;
        std::string SourcePath;
        std::vector<MeshArtifactPrimitive> Primitives;
        std::vector<MeshArtifactVertex> Vertices;
        std::vector<u32> Indices;
    };

    std::filesystem::path GetCookedMeshArtifactPath(AssetHandle asset);
    bool ValidateMeshArtifact(const MeshArtifact& artifact, std::string& outError);
    bool StoreMeshArtifact(const std::filesystem::path& path, const MeshArtifact& artifact, std::string& outError);
    bool LoadMeshArtifact(const std::filesystem::path& path, MeshArtifact& outArtifact, std::string& outError);
    bool ResolveMeshArtifact(const AssetRegistry& registry, AssetHandle asset, MeshArtifact& outArtifact, std::string& outError);
}
