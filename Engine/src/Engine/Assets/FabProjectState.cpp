#include "Engine/Assets/FabProjectState.h"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>

namespace Engine
{
    namespace
    {
        std::string ShortId(std::string_view id)
        {
            return std::string(id.substr(0, 12));
        }

        std::vector<std::string> SplitSlash(std::string_view path)
        {
            std::vector<std::string> segments;
            size_t start = 0;
            while (start <= path.size())
            {
                const size_t separator = path.find('/', start);
                const size_t end = separator == std::string_view::npos ? path.size() : separator;
                segments.emplace_back(path.substr(start, end - start));
                if (separator == std::string_view::npos)
                    break;
                start = separator + 1;
            }
            return segments;
        }

        // True unless a component below `base` is a link. Missing components are
        // left to the caller's own existence checks.
        bool HasNoLinkComponents(const std::filesystem::path& base, std::string_view relative)
        {
            std::filesystem::path path = base;
            for (const std::string& segment : SplitSlash(relative))
            {
                path /= segment;
                std::error_code error;
                const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
                if (error || status.type() == std::filesystem::file_type::not_found)
                    return true;
                if (std::filesystem::is_symlink(status))
                    return false;
            }
            return true;
        }

        struct StreamTip
        {
            const FabImportReceipt* Receipt = nullptr;
            bool Ambiguous = false;
        };

        // Mirrors the receipt authority's stream-tip rule: the generation no
        // later source replacement of the same stream names as its predecessor.
        std::map<std::string, StreamTip> FindStreamTips(const FabReceiptCollection& receipts)
        {
            std::set<std::pair<std::string, std::string>> replaced;
            for (const FabImportReceipt& receipt : receipts.Receipts)
                if (receipt.Relation == FabGenerationRelation::SourceReplacement)
                    replaced.emplace(receipt.RelatedStreamId, receipt.RelatedGenerationId);

            std::map<std::string, StreamTip> tips;
            for (const FabImportReceipt& receipt : receipts.Receipts)
            {
                StreamTip& tip = tips[receipt.StreamId];
                if (replaced.contains({ receipt.StreamId, receipt.GenerationId }))
                    continue;
                if (tip.Receipt)
                    tip.Ambiguous = true;
                tip.Receipt = &receipt;
            }
            return tips;
        }
    }

    std::string GetFabProjectGenerationRoot(std::string_view generationId)
    {
        const bool valid = generationId.size() == 64 && std::all_of(generationId.begin(), generationId.end(),
            [](char character)
            {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            });
        return valid ? "fab/" + std::string(generationId) : std::string();
    }

    std::string GetFabProjectRegistrySourcePath(std::string_view streamId, std::string_view logicalPath)
    {
        return "fab:" + std::string(streamId) + "/" + std::string(logicalPath);
    }

    std::string GetFabProjectMaterialRelativePath(AssetHandle material)
    {
        return "materials/" + std::to_string(material) + ".spiralmat";
    }

    std::vector<ProjectCommitArtifact> GetFabGenerationArtifacts(const FabImportReceipt& receipt)
    {
        std::vector<ProjectCommitArtifact> artifacts;
        artifacts.reserve(receipt.Assets.size());
        for (const FabImportedAssetRecord& asset : receipt.Assets)
            artifacts.push_back({ asset.GenerationRelativeCookedPath, asset.ArtifactSha256 });
        return artifacts;
    }

    bool IsImmutableMaterialAsset(const AssetMetadata& metadata)
    {
        return metadata.Type == AssetType::Material
            && metadata.SourcePolicy == AssetSourcePolicy::ImmutablePackage;
    }

    std::filesystem::path GetFabProjectMaterialPath(
        AssetHandle material, std::string_view cookedRoot, const std::filesystem::path& cookedArtifactBase)
    {
        if (material == kInvalidAssetHandle || cookedRoot.empty() || !AssetRegistry::IsValidCookedRoot(cookedRoot)
            || cookedArtifactBase.empty() || !cookedArtifactBase.is_absolute())
            return {};
        return (cookedArtifactBase / std::filesystem::path(std::string(cookedRoot))
            / std::filesystem::path(GetFabProjectMaterialRelativePath(material))).lexically_normal();
    }

    bool LoadImmutableMaterialAsset(const AssetRegistry& registry, AssetHandle material,
        MaterialAsset& outMaterial, std::string& outError)
    {
        const AssetMetadata* metadata = registry.GetAsset(material);
        if (!metadata || !IsImmutableMaterialAsset(*metadata))
        {
            outError = "the asset is not an immutable package material";
            return false;
        }
        const std::filesystem::path path
            = GetFabProjectMaterialPath(material, metadata->CookedRoot, registry.GetCookedArtifactBasePath());
        std::error_code filesystemError;
        if (path.empty() || !HasNoLinkComponents(registry.GetCookedArtifactBasePath(),
                metadata->CookedRoot + "/" + GetFabProjectMaterialRelativePath(material))
            || !std::filesystem::is_regular_file(path, filesystemError))
        {
            outError = "the immutable material artifact is missing, not a regular file, or reached through a link";
            return false;
        }

        MaterialAsset loaded;
        if (!MaterialAsset::LoadFromFile(path, loaded) || !IsValidMaterialAssetValues(loaded))
        {
            outError = "the immutable material artifact is malformed or has invalid values";
            return false;
        }
        outMaterial = std::move(loaded);
        outError.clear();
        return true;
    }

    bool ValidateFabProjectState(const AssetRegistry& registry, const FabReceiptCollection& receipts,
        const FabProjectValidationOptions& options, std::string& outError)
    {
        std::string receiptError;
        if (!ValidateFabReceiptCollection(receipts, receiptError))
        {
            outError = "the Fab receipt collection is invalid: " + receiptError;
            return false;
        }

        const std::map<std::string, StreamTip> tips = FindStreamTips(receipts);
        for (const auto& [streamId, tip] : tips)
        {
            if (!tip.Receipt || tip.Ambiguous)
            {
                outError = "stream " + ShortId(streamId) + " has no unique current generation";
                return false;
            }
        }

        // (generationId, handle) pairs that a registry entry claims.
        std::set<std::pair<std::string, AssetHandle>> claimed;
        for (const AssetMetadata& metadata : registry.GetAssets())
        {
            if (metadata.SourcePolicy != AssetSourcePolicy::ImmutablePackage)
                continue;
            const FabImportReceipt* owner = nullptr;
            const FabImportedAssetRecord* record = nullptr;
            size_t matches = 0;
            for (const auto& [streamId, tip] : tips)
            {
                (void)streamId;
                if (GetFabProjectGenerationRoot(tip.Receipt->GenerationId) != metadata.CookedRoot)
                    continue;
                for (const FabImportedAssetRecord& asset : tip.Receipt->Assets)
                {
                    if (asset.Handle == metadata.Handle)
                    {
                        owner = tip.Receipt;
                        record = &asset;
                        ++matches;
                    }
                }
            }
            if (matches != 1 || record->Type != metadata.Type
                || metadata.SourcePath != GetFabProjectRegistrySourcePath(owner->StreamId, record->LogicalPath))
            {
                outError = "immutable registry asset " + std::to_string(metadata.Handle)
                    + " is not claimed by exactly one current receipt record (orphan or stale)";
                return false;
            }
            claimed.emplace(owner->GenerationId, metadata.Handle);
        }

        const std::filesystem::path& base = registry.GetCookedArtifactBasePath();
        if (!tips.empty() && (base.empty() || !base.is_absolute()))
        {
            outError = "the registry has no absolute cooked artifact base";
            return false;
        }

        for (const auto& [streamId, tip] : tips)
        {
            const FabImportReceipt& receipt = *tip.Receipt;
            const std::string cookedRoot = GetFabProjectGenerationRoot(receipt.GenerationId);
            for (const FabImportedAssetRecord& asset : receipt.Assets)
            {
                const AssetMetadata* metadata = registry.GetAsset(asset.Handle);
                if (!metadata || metadata->SourcePolicy != AssetSourcePolicy::ImmutablePackage
                    || metadata->Type != asset.Type || metadata->CookedRoot != cookedRoot
                    || !claimed.contains({ receipt.GenerationId, asset.Handle }))
                {
                    outError = "receipt asset " + std::to_string(asset.Handle) + " of stream "
                        + ShortId(streamId) + " is not registered as its current generation";
                    return false;
                }
                if (asset.Type == AssetType::Material
                    && asset.GenerationRelativeCookedPath != GetFabProjectMaterialRelativePath(asset.Handle))
                {
                    outError = "material artifact " + std::to_string(asset.Handle)
                        + " is not at its generation-relative layout path";
                    return false;
                }
            }

            if (!HasNoLinkComponents(base, cookedRoot))
            {
                outError = "generation " + ShortId(receipt.GenerationId) + " is reached through a link";
                return false;
            }
            std::string verifyError;
            const bool hashContents = options.Level == FabProjectValidationLevel::FullHash
                && (options.FullHashGenerationIds.empty()
                    || std::find(options.FullHashGenerationIds.begin(), options.FullHashGenerationIds.end(),
                        receipt.GenerationId) != options.FullHashGenerationIds.end());
            if (!VerifyGenerationDirectory(base / std::filesystem::path(cookedRoot),
                    GetFabGenerationArtifacts(receipt), hashContents,
                    options.IsCancelled, verifyError))
            {
                outError = "generation " + ShortId(receipt.GenerationId) + " failed validation: " + verifyError;
                return false;
            }

            for (const FabImportedAssetRecord& asset : receipt.Assets)
            {
                if (asset.Type != AssetType::Material)
                    continue;
                MaterialAsset material;
                std::string materialError;
                if (!LoadImmutableMaterialAsset(registry, asset.Handle, material, materialError))
                {
                    outError = "generation " + ShortId(receipt.GenerationId) + ": " + materialError;
                    return false;
                }
            }
        }

        outError.clear();
        return true;
    }

    bool LoadFabProjectState(const std::filesystem::path& projectRoot,
        const std::filesystem::path& manifestRelativePath, const FabProjectValidationOptions& options,
        FabProjectState& outState, std::string& outError)
    {
        FabProjectState state;
        std::string manifestError;
        if (!LoadProjectManifest(projectRoot / manifestRelativePath, state.Manifest, manifestError))
        {
            outError = "could not load the project manifest: " + manifestError;
            return false;
        }

        const bool hasReceipts = !state.Manifest.FabReceiptsPath.empty();
        if (hasReceipts)
        {
            for (const std::string* path : { &state.Manifest.ScenePath, &state.Manifest.AssetRegistryPath,
                     &state.Manifest.FabReceiptsPath })
            {
                if (!IsPortableProjectRelativePath(*path) || !HasNoLinkComponents(projectRoot, *path))
                {
                    outError = "a project with Fab receipts must keep every committed file inside the project "
                               "root without links: " + *path;
                    return false;
                }
            }
        }

        if (!state.Registry.LoadFromFile(projectRoot / std::filesystem::path(state.Manifest.AssetRegistryPath)))
        {
            outError = "could not load the asset registry named by the project manifest";
            return false;
        }
        if (hasReceipts)
        {
            std::string receiptError;
            if (!LoadFabReceiptCollection(
                    projectRoot / std::filesystem::path(state.Manifest.FabReceiptsPath), state.Receipts, receiptError))
            {
                outError = "could not load the Fab receipt collection: " + receiptError;
                return false;
            }
        }

        if (!ValidateFabProjectState(state.Registry, state.Receipts, options, outError))
            return false;
        outState = std::move(state);
        outError.clear();
        return true;
    }

    bool FindFabProjectOrphans(const FabProjectState& state, const std::filesystem::path& projectRoot,
        const std::filesystem::path& manifestRelativePath, FabProjectOrphanReport& outReport,
        std::string& outError)
    {
        FabProjectOrphanReport report;
        std::set<std::string> referenced;
        for (const FabImportReceipt& receipt : state.Receipts.Receipts)
            referenced.insert(receipt.GenerationId);

        std::error_code error;
        const std::filesystem::path generationParent = state.Registry.GetCookedArtifactBasePath() / "fab";
        if (std::filesystem::is_directory(generationParent, error))
        {
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(generationParent, error))
                if (!referenced.contains(entry.path().filename().string()))
                    report.UnreferencedGenerations.push_back(entry.path().filename().string());
        }
        if (error)
        {
            outError = "could not enumerate the generation directory";
            return false;
        }

        const std::filesystem::path manifestPath = projectRoot / manifestRelativePath;
        const std::string prefix = "." + manifestPath.filename().string() + ".tmp.";
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(manifestPath.parent_path(), error))
            if (entry.path().filename().string().starts_with(prefix))
                report.TemporaryFiles.push_back(entry.path().filename().string());
        if (error)
        {
            outError = "could not enumerate the manifest directory";
            return false;
        }

        std::sort(report.UnreferencedGenerations.begin(), report.UnreferencedGenerations.end());
        std::sort(report.TemporaryFiles.begin(), report.TemporaryFiles.end());
        outReport = std::move(report);
        outError.clear();
        return true;
    }
}
