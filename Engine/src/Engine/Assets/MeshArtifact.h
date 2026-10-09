#pragma once

#include "Engine/Assets/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"

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
        float Normal[3] {};
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

    // Object-space axis-aligned box of the geometry a mesh artifact draws.
    struct MeshArtifactBounds
    {
        float Min[3] {};
        float Max[3] {};
    };

    // Immutable renderer-consumable view of the asset catalog. It retains an
    // immutable registry snapshot so mutable editor state is never read during render work.
    class MeshArtifactResolver
    {
    public:
        explicit MeshArtifactResolver(const AssetRegistry& registry);
        explicit MeshArtifactResolver(std::shared_ptr<const AssetRegistry> registry);

        bool Resolve(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError) const;

    private:
        std::shared_ptr<const AssetRegistry> m_Registry;
    };

    std::filesystem::path GetCookedMeshArtifactPath(AssetHandle asset);
    // A nonempty cooked root selects one immutable generation. Empty retains
    // the legacy mutable import location used by existing cookers.
    std::filesystem::path GetCookedMeshArtifactPath(AssetHandle asset,
        std::string_view cookedRoot, const std::filesystem::path& cookedArtifactBase);
    // The default Editor scene and the bounded backend raster fixtures share
    // this one versioned cooked payload. It is an engine-owned current
    // consumer of the normal MeshArtifact publication path, not a second
    // runtime mesh format or a renderer-side fallback.
    std::string_view GetDefaultSceneMeshSourcePath();
    bool CreateDefaultSceneMeshArtifact(AssetHandle asset, MeshArtifact& outArtifact, std::string& outError);
    bool StoreDefaultSceneMeshArtifact(AssetHandle asset, std::string& outError);
    bool EnsureDefaultSceneMeshArtifact(AssetRegistry& registry, AssetHandle& outAsset, std::string& outError);
    // Fills an all-zero normal field per primitive from its triangle winding.
    // Authored finite unit normals are preserved within the admitted artifact
    // tolerance; mixed, non-finite, non-unit, or degenerate primitive data
    // fails without mutating the artifact.
    bool EnsureMeshArtifactGeometricNormals(MeshArtifact& artifact, std::string& outError);
    bool ValidateMeshArtifact(const MeshArtifact& artifact, std::string& outError);
    // Exact min/max over the vertices referenced by the primitives' index ranges,
    // i.e. the geometry that is drawn; unreferenced vertices do not widen the box.
    // Bounds are derived from the vertex data on demand, not stored: the cooked
    // payload version and every existing artifact stay valid. Callers that query
    // repeatedly key a cache on the asset handle plus AssetMetadata::CookedRoot,
    // which identifies one immutable generation (the empty legacy root is mutable
    // and must not be cached). An artifact that fails ValidateMeshArtifact fails
    // here and leaves outBounds untouched.
    bool ComputeMeshArtifactBounds(const MeshArtifact& artifact, MeshArtifactBounds& outBounds, std::string& outError);
    bool ResolveMeshArtifactBounds(const AssetRegistry& registry, AssetHandle asset,
        MeshArtifactBounds& outBounds, std::string& outError);
    // Float box of the object-space box after an affine transform in the
    // Math::Mat4 convention (row vectors, translation in Values[12..14], e.g.
    // TransformComponent::GetCameraRelativeTransform), evaluated in double and
    // rounded outward. A non-affine or non-finite transform, an inverted input
    // box, or a result outside float range fails and leaves outBounds untouched.
    bool TransformMeshArtifactBounds(const MeshArtifactBounds& bounds, const Math::Mat4& transform,
        MeshArtifactBounds& outBounds);
    bool StoreMeshArtifact(const std::filesystem::path& path, const MeshArtifact& artifact, std::string& outError);
    bool LoadMeshArtifact(const std::filesystem::path& path, MeshArtifact& outArtifact, std::string& outError);
    bool ResolveMeshArtifact(const AssetRegistry& registry, AssetHandle asset, MeshArtifact& outArtifact, std::string& outError);
}
