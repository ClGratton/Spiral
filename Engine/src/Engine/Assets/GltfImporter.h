#pragma once

#include "Engine/Assets/AssetFileSystem.h"
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
        // `resolver` maps the (normalized, possibly project-relative) source path to the file to
        // read. Unset, the legacy working-directory/executable-ancestor search is used; a project
        // rooted elsewhere must pass its own root-relative resolver.
        static GltfImportResult Import(const std::filesystem::path& sourcePath, AssetRegistry& registry,
            const AssetPathResolver& resolver = {});
    };
}
