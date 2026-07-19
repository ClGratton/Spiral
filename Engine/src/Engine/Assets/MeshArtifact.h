#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Base.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
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

    // Immutable renderer-consumable view of the asset catalog. It owns a
    // registry copy so mutable editor state is never read during render work.
    class MeshArtifactResolver
    {
    public:
        explicit MeshArtifactResolver(const AssetRegistry& registry);

        bool Resolve(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError) const;

    private:
        std::shared_ptr<const AssetRegistry> m_Registry;
    };

    std::filesystem::path GetCookedMeshArtifactPath(AssetHandle asset);
    // The default Editor scene and the bounded backend raster fixtures share
    // this one versioned cooked payload. It is an engine-owned current
    // consumer of the normal MeshArtifact publication path, not a second
    // runtime mesh format or a renderer-side fallback.
    std::string_view GetDefaultSceneMeshSourcePath();
    bool CreateDefaultSceneMeshArtifact(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError);
    bool StoreDefaultSceneMeshArtifact(AssetHandle asset, std::string& outError);
    bool EnsureDefaultSceneMeshArtifact(AssetRegistry& registry, AssetHandle& outAsset, std::string& outError);
    bool ValidateMeshArtifact(const MeshArtifact& artifact, std::string& outError);
    bool StoreMeshArtifact(const std::filesystem::path& path, const MeshArtifact& artifact, std::string& outError);
    bool LoadMeshArtifact(const std::filesystem::path& path, MeshArtifact& outArtifact, std::string& outError);
    bool ResolveMeshArtifact(const AssetRegistry& registry, AssetHandle asset, MeshArtifact& outArtifact, std::string& outError);
}
