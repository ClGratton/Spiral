#pragma once

#include "Engine/Assets/AssetHandle.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine
{
    class AssetRegistry;

    struct GltfMeshImportInfo
    {
        std::string Name;
        std::size_t PrimitiveCount = 0;
        std::size_t TrianglePrimitiveCount = 0;
        std::size_t UnsupportedPrimitiveCount = 0;
        std::size_t VertexCount = 0;
        std::size_t TriangleCount = 0;
    };

    struct GltfImportResult
    {
        bool Succeeded = false;
        AssetHandle MeshAsset = kInvalidAssetHandle;
        std::string SourcePath;
        std::filesystem::path CookedPath;
        std::vector<GltfMeshImportInfo> Meshes;
        std::string Error;
    };

    class GltfImporter
    {
    public:
        static GltfImportResult Import(const std::filesystem::path& sourcePath, AssetRegistry& registry);
    };
}
