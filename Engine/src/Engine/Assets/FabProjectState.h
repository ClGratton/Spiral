#pragma once

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/FabImportReceipt.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Assets/ProjectCommit.h"
#include "Engine/Assets/ProjectManifest.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    // The cooked generation layout is owned by the Fab cook (fab/<generationId>/
    // {meshes,textures,materials}, semantic registry source paths). These three
    // functions are the only place this module assumes it, so an integration
    // that changes the layout changes only them.
    std::string GetFabProjectGenerationRoot(std::string_view generationId);
    std::string GetFabProjectRegistrySourcePath(std::string_view streamId, std::string_view logicalPath);
    std::string GetFabProjectMaterialRelativePath(AssetHandle material);

    // The exhaustive artifact list of a receipt's generation, in the form
    // ProjectCommitGeneration::Artifacts expects.
    std::vector<ProjectCommitArtifact> GetFabGenerationArtifacts(const FabImportReceipt& receipt);

    // ----- immutable (Fab) materials -----

    // True for a material that lives in an immutable generation. Such a material
    // is loaded from its generation root, never from a physical project path, and
    // callers must not save or edit it in place.
    bool IsImmutableMaterialAsset(const AssetMetadata& metadata);
    // <cookedArtifactBase>/<cookedRoot>/materials/<handle>.spiralmat; empty for an
    // invalid handle or root or a relative base.
    std::filesystem::path GetFabProjectMaterialPath(
        AssetHandle material, std::string_view cookedRoot, const std::filesystem::path& cookedArtifactBase);
    // Loads and validates the material of an ImmutablePackage registry entry from
    // its generation. Fails for physical materials and for links beneath the
    // cooked base. outMaterial is replaced only on success.
    bool LoadImmutableMaterialAsset(const AssetRegistry& registry, AssetHandle material,
        MaterialAsset& outMaterial, std::string& outError);

    // ----- project-state validation -----

    enum class FabProjectValidationLevel
    {
        // Every open: receipts and registry agree, every artifact exists as a
        // regular file with no extras or links, Fab materials load. Receipts
        // record no sizes, so no size comparison is possible at this level.
        Structural,
        // Commit time and on demand: additionally SHA-256 of every artifact.
        FullHash
    };

    struct FabProjectValidationOptions
    {
        FabProjectValidationLevel Level = FabProjectValidationLevel::Structural;
        std::function<bool()> IsCancelled;
        // FullHash hashes every stream tip when this is empty. Otherwise only the named
        // generations are hashed and every other tip is validated structurally. A commit that
        // published one new generation names just that one: the other tips were already hashed
        // when they were committed or opened, and re-hashing all imported content after every
        // import grows the post-commit pause with the whole project instead of with the change.
        std::vector<std::string> FullHashGenerationIds;
    };

    // Invariants (current generation of each stream means the stream tip):
    //  - the receipt collection is valid;
    //  - every ImmutablePackage registry entry is claimed by exactly one receipt
    //    asset of a stream tip: same handle and type, CookedRoot equal to that
    //    generation's root, semantic SourcePath;
    //  - every asset of every stream tip is registered that way;
    //  - each tip generation directory holds exactly its receipt artifacts and
    //    each material artifact loads (Structural) / every hash matches (FullHash).
    // Superseded generations are retained by policy and are not verified.
    // The registry must have its cooked artifact base set (LoadFromFile does).
    bool ValidateFabProjectState(const AssetRegistry& registry, const FabReceiptCollection& receipts,
        const FabProjectValidationOptions& options, std::string& outError);

    struct FabProjectState
    {
        ProjectManifest Manifest;
        AssetRegistry Registry;
        FabReceiptCollection Receipts;
    };

    // Loads the manifest, its registry and (if named) receipt collection and
    // validates them. Every committed file must lie inside projectRoot without
    // link components when the manifest names receipts. outState is replaced only
    // on success.
    bool LoadFabProjectState(const std::filesystem::path& projectRoot,
        const std::filesystem::path& manifestRelativePath, const FabProjectValidationOptions& options,
        FabProjectState& outState, std::string& outError);

    // Read-only report for Tools > Validate Project. Nothing is deleted.
    struct FabProjectOrphanReport
    {
        // Generation directories under the cooked base's fab/ that no receipt names.
        std::vector<std::string> UnreferencedGenerations;
        // Atomic-write temporaries beside the manifest.
        std::vector<std::string> TemporaryFiles;
    };
    bool FindFabProjectOrphans(const FabProjectState& state, const std::filesystem::path& projectRoot,
        const std::filesystem::path& manifestRelativePath, FabProjectOrphanReport& outReport,
        std::string& outError);
}
