#pragma once

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/CommonImage.h"
#include "Engine/Assets/FabImportReceipt.h"
#include "Engine/Assets/LocalPackageSnapshot.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Assets/MeshArtifact.h"
#include "Engine/Assets/TextureArtifact.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    inline constexpr std::string_view kFabGltfImporterVersion = "SpiralFabGltfPrepareV1";
    inline constexpr std::string_view kFabGltfCookerVersion = "SpiralFabGltfCookV1";

    struct FabGltfLimits
    {
        u64 MaximumRootBytes = 1ull << 30;
        u64 MaximumTotalBufferBytes = 2ull << 30;
        u64 MaximumNodeCount = 65536;
        u64 MaximumPrimitiveInstances = 65536;
        u64 MaximumVertices = 16ull * 1024ull * 1024ull;
        u64 MaximumIndices = 48ull * 1024ull * 1024ull;
        u64 MaximumTotalDecodedImageBytes = 1ull << 30;
    };

    // Test-facing checkpoints. Prepare and Stage poll `IsCancelled` at every
    // checkpoint and additionally inside streaming reads and per-instance loops.
    enum class FabGltfStage
    {
        Started,
        RootRead,
        Parsed,
        BuffersLoaded,
        GeometryBaked,
        ImageDecoded,
        TextureCooked,
        MaterialBuilt,
        Completed,
        StagingDirectoriesCreated,
        ArtifactWritten,
        StagingCompleted
    };

    struct FabGltfPrepareOptions
    {
        FabGltfLimits Limits;
        CommonImageLimits ImageLimits;
        std::function<bool()> IsCancelled;
        // Receives the stage and a diagnostic label (never a host path).
        std::function<void(FabGltfStage, std::string_view)> TestHook;
    };

    struct FabGltfPreparedAsset
    {
        AssetType Type = AssetType::Unknown;
        // ComputeFabStableAssetHandle(streamId, type, role): derived from the
        // stream and the semantic role, never from package file names.
        AssetHandle Handle = kInvalidAssetHandle;
        std::string SemanticRole;                 // mesh.main, material.main, texture.base-color, ...
        std::string LogicalPath;                  // receipt path: the role with its first '.' as '/'
        std::string RegistrySourcePath;           // "fab:" + streamId + "/" + LogicalPath
        std::string Name;
        std::string GenerationRelativeCookedPath; // meshes/<h>.spiralmesh, textures/<h>.rgba-fallback.spiraltexture, ...
    };

    struct FabGltfPreparedPackage
    {
        // Identity, declaration, digests and versions. Assets are listed without
        // ArtifactSha256 and the relation is whatever the declaration carried;
        // Stage fills the hashes and BuildFabGltfCandidate assigns the relation.
        FabImportReceipt ReceiptDraft;
        // Deterministic order: mesh, material, then Textures[i] at Assets[2 + i]
        // (base color, ORM, normal, emissive, when present).
        std::vector<FabGltfPreparedAsset> Assets;
        MeshArtifact Mesh;
        std::vector<TextureArtifact> Textures;
        MaterialAsset Material;
        u32 VertexCount = 0;
        u32 TriangleCount = 0;
        u32 PrimitiveInstanceCount = 0;
    };

    // Pure: reads only `snapshot`, performs no filesystem, registry, Scene or
    // renderer access, and replaces `out` only on success. `declaration` carries
    // the user-confirmed receipt fields; StreamId, GenerationId, ExpandedTreeSha256,
    // the importer/cooker versions and Assets are computed here and must be empty
    // or equal in the declaration.
    bool PrepareFabGltfPackage(const LocalPackageSnapshot& snapshot, const FabImportReceipt& declaration,
        const FabGltfPrepareOptions& options, FabGltfPreparedPackage& out, std::string& error);

    // "fab/<generationId>", valid for AssetRegistry::IsValidCookedRoot; empty
    // for a malformed generation id.
    std::string GetFabCookedRoot(std::string_view generationId);
    // <cookedArtifactBase>/<cookedRoot>/materials/<handle>.spiralmat, the
    // rooted counterpart of the mesh and texture path functions. Empty for an
    // invalid handle/root or a relative base.
    std::filesystem::path GetFabGltfCookedMaterialPath(AssetHandle material,
        std::string_view cookedRoot, const std::filesystem::path& cookedArtifactBase);

    struct FabGltfStageOptions
    {
        std::function<bool()> IsCancelled;
        std::function<void(FabGltfStage, std::string_view)> TestHook;
    };

    struct FabGltfStagedGeneration
    {
        // Relative and identical before and after the caller renames the
        // directory from the staging base to the final base.
        std::string CookedRoot;
        // ArtifactSha256 filled from the re-read files; validated with
        // ValidateFabImportReceipt under a placeholder Initial relation.
        FabImportReceipt Receipt;
    };

    // Writes only beneath stagingBase/fab/<generationId>/{meshes,textures,materials},
    // which must not exist (create-once). Each file is re-read through the real
    // loader, compared with the prepared artifact and hashed. On any failure or
    // cancellation exactly the files and directories this call created are removed.
    bool StageFabGltfGeneration(const FabGltfPreparedPackage& prepared, const std::filesystem::path& stagingBase,
        const FabGltfStageOptions& options, FabGltfStagedGeneration& out, std::string& error);

    struct FabGltfCandidate
    {
        FabReceiptDecision Decision;
        // Copy of the base registry with this generation registered (new stream
        // or product update) or compare-and-swapped (source replacement); its
        // cooked artifact base is finalCookedBase. Unchanged on ExactReuse.
        AssetRegistry Registry;
        FabReceiptCollection Receipts;
        MaterialAsset Material;
        FabImportReceipt Receipt;
        // False on ExactReuse: the staged directory is redundant and is to be discarded.
        bool NeedsPublish = true;
    };

    // Pure copy-on-write construction: never mutates `baseRegistry` or
    // `priorReceipts`. `out` is replaced only on success; when classification
    // rejects the generation (invalid, corrupt prior state, conflict) only
    // `out.Decision` is set so the caller can read the kind, and `error`
    // carries the diagnostic.
    bool BuildFabGltfCandidate(const AssetRegistry& baseRegistry, const FabReceiptCollection& priorReceipts,
        const FabGltfPreparedPackage& prepared, const FabGltfStagedGeneration& staged,
        const std::filesystem::path& finalCookedBase, FabGltfCandidate& out, std::string& error);
}
