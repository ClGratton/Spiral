#include "FabProjectCommitTests.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/FabImportReceipt.h"
#include "Engine/Assets/FabProjectState.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Assets/ProjectCommit.h"
#include "Engine/Assets/ProjectManifest.h"
#include "Engine/Core/Sha256.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
    #include <fcntl.h>
    #include <sys/file.h>
    #include <unistd.h>
#endif

namespace
{
    using namespace Engine;
    namespace fs = std::filesystem;

    constexpr const char* kManifestName = "Project.spiralproject";

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    bool WriteAll(const fs::path& path, std::string_view bytes)
    {
        std::error_code error;
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    }

    std::string Sha256OfFile(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Sha256Builder hash;
        std::vector<char> buffer(1u << 20);
        while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || input.gcount() > 0)
            hash.Update(std::span<const u8>(reinterpret_cast<const u8*>(buffer.data()), static_cast<size_t>(input.gcount())));
        return hash.FinalizeHex();
    }

    // Writes `bytes` of deterministic pseudo-random data without holding it in memory.
    bool WriteFilled(const fs::path& path, size_t bytes, u64 seed)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        std::vector<u64> chunk(1u << 17);
        u64 state = seed * 0x9E3779B97F4A7C15ull + 1;
        size_t remaining = bytes;
        while (remaining > 0 && output)
        {
            for (u64& word : chunk)
            {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                word = state;
            }
            const size_t count = std::min(remaining, chunk.size() * sizeof(u64));
            output.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(count));
            remaining -= count;
        }
        return static_cast<bool>(output);
    }

    bool FlipByte(const fs::path& path, size_t offset)
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file)
            return false;
        char value = 0;
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(&value, 1);
        if (!file)
            return false;
        value = static_cast<char>(value ^ 0x01);
        file.seekp(static_cast<std::streamoff>(offset));
        file.write(&value, 1);
        return static_cast<bool>(file);
    }

    // Path -> "D" | "L:<target>" | "F:<size>:<sha256>", so "unchanged" is a byte-level statement.
    using Snapshot = std::map<std::string, std::string>;

    Snapshot TakeSnapshot(const fs::path& root, std::string_view skipPrefix = {})
    {
        Snapshot snapshot;
        std::error_code error;
        if (!fs::exists(root, error))
            return snapshot;
        for (fs::recursive_directory_iterator iterator(root, error), end; !error && iterator != end; iterator.increment(error))
        {
            const std::string relative = fs::relative(iterator->path(), root, error).generic_string();
            if (!skipPrefix.empty() && (relative == skipPrefix || relative.starts_with(std::string(skipPrefix) + "/")))
                continue;
            const fs::file_status status = iterator->symlink_status(error);
            if (fs::is_symlink(status))
                snapshot[relative] = "L:" + fs::read_symlink(iterator->path(), error).string();
            else if (fs::is_directory(status))
                snapshot[relative] = "D";
            else
                snapshot[relative] = "F:" + std::to_string(fs::file_size(iterator->path(), error)) + ":"
                    + Sha256OfFile(iterator->path());
        }
        return snapshot;
    }

    class Workspace
    {
    public:
        Workspace()
        {
            static std::atomic<u64> sequence { 0 };
            std::error_code error;
            const u64 tick = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            m_Base = fs::temp_directory_path(error) / ("spiral-fab-project-commit-test-" + std::to_string(tick) + "-"
                + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            m_Ready = !error && fs::create_directories(m_Base / "project", error) && !error
                && fs::create_directories(m_Base / "staging", error) && !error;
        }

        ~Workspace()
        {
            if (!m_Ready)
                return;
            std::error_code error;
            for (const fs::directory_entry& entry :
                fs::recursive_directory_iterator(m_Base, fs::directory_options::skip_permission_denied, error))
                if (entry.is_directory(error) && !entry.is_symlink(error))
                    fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add, error);
            fs::remove_all(m_Base, error);
        }

        Workspace(const Workspace&) = delete;
        Workspace& operator=(const Workspace&) = delete;

        bool IsReady() const { return m_Ready; }
        fs::path Base() const { return m_Base; }
        fs::path Project() const { return m_Base / "project"; }
        fs::path Staging() const { return m_Base / "staging"; }
        fs::path Manifest() const { return Project() / kManifestName; }

    private:
        fs::path m_Base;
        bool m_Ready = false;
    };

    struct BuiltGeneration
    {
        FabImportReceipt Receipt;
        fs::path StagedDirectory;
        std::string RelativeRoot;   // project-relative final location
        std::string CookedRoot;     // registry CookedRoot
        std::vector<AssetMetadata> RegistryEntries;
        ProjectCommitGeneration Generation;
        AssetHandle MeshHandle = kInvalidAssetHandle;
        AssetHandle MaterialHandle = kInvalidAssetHandle;
        AssetHandle TextureHandle = kInvalidAssetHandle;
        std::string ReplacesGeneration;
    };

    std::string HashHex(std::string_view text)
    {
        return Sha256Builder::HashString(text);
    }

    // Synthetic Fab-shaped generation: one mesh, one material, one texture. Mesh and texture bytes are
    // opaque (the validator never parses them); the material is a real, loadable material file.
    bool BuildGeneration(const Workspace& workspace, std::string_view label, std::string_view variant,
        size_t meshBytes, BuiltGeneration& out, std::string_view stagingTag = "s", std::string_view replaces = {},
        std::string_view versionLabel = "1.0-glb", const BuiltGeneration* productUpdateOf = nullptr)
    {
        const std::string labelHash = HashHex(label);
        const std::string uuid = labelHash.substr(0, 8) + "-" + labelHash.substr(8, 4) + "-" + labelHash.substr(12, 4)
            + "-" + labelHash.substr(16, 4) + "-" + labelHash.substr(20, 12);

        BuiltGeneration built;
        FabImportReceipt& receipt = built.Receipt;
        receipt.ProductIdentity = "https://www.fab.com/listings/" + uuid;
        receipt.ProductName = "Synthetic " + std::string(label);
        receipt.Publisher = "Test Publisher";
        receipt.VersionOrDownloadLabel = std::string(versionLabel);
        receipt.PackageFormat = FabPackageFormat::Glb;
        receipt.LicenseFamily = FabLicenseFamily::FabStandard;
        receipt.LicenseTier = FabLicenseTier::Personal;
        receipt.MetadataConfirmedByUser = true;
        receipt.NoAI = FabMetadataFlag::No;
        receipt.GeneratedWithAI = FabMetadataFlag::No;
        receipt.SourceDigestKind = FabDigestKind::Sha256;
        receipt.SourceSha256 = HashHex("source:" + std::string(label) + ":" + std::string(variant));
        receipt.ExpandedTreeSha256 = HashHex("tree:" + std::string(label) + ":" + std::string(variant));
        receipt.ImporterVersion = "SpiralFabTestImporter/1";
        receipt.CookerVersion = "SpiralFabTestCooker/1";
        receipt.DiagnosticAcquiredAtUtc = "2026-10-09T10:20:30Z";
        receipt.RawSourcePolicy = FabRawSourcePolicy::ExcludedFromProject;
        receipt.StreamId = ComputeFabStreamId(receipt.ProductIdentity, receipt.VersionOrDownloadLabel, receipt.PackageFormat);
        receipt.GenerationId = ComputeFabGenerationId(receipt.StreamId, receipt.SourceSha256, receipt.ExpandedTreeSha256);
        if (productUpdateOf)
        {
            receipt.Relation = FabGenerationRelation::ProductUpdate;
            receipt.RelatedStreamId = productUpdateOf->Receipt.StreamId;
            receipt.RelatedGenerationId = productUpdateOf->Receipt.GenerationId;
        }
        else if (replaces.empty())
            receipt.Relation = FabGenerationRelation::Initial;
        else
        {
            receipt.Relation = FabGenerationRelation::SourceReplacement;
            receipt.RelatedStreamId = receipt.StreamId;
            receipt.RelatedGenerationId = std::string(replaces);
            built.ReplacesGeneration = std::string(replaces);
        }

        built.CookedRoot = GetFabProjectGenerationRoot(receipt.GenerationId);
        built.RelativeRoot = "Assets/" + built.CookedRoot;
        built.StagedDirectory = workspace.Staging() / std::string(stagingTag) / receipt.GenerationId;
        std::error_code error;
        fs::remove_all(built.StagedDirectory, error);
        fs::create_directories(built.StagedDirectory, error);

        struct Spec
        {
            AssetType Type;
            const char* Role;
            const char* Logical;
        };
        const Spec specs[] = {
            { AssetType::Mesh, "mesh.main", "mesh/main" },
            { AssetType::Material, "material.main", "material/main" },
            { AssetType::Texture, "texture.base-color", "texture/base-color" },
        };
        for (const Spec& spec : specs)
        {
            FabImportedAssetRecord record;
            record.Type = spec.Type;
            record.SemanticRole = spec.Role;
            record.LogicalPath = spec.Logical;
            record.Handle = ComputeFabStableAssetHandle(receipt.StreamId, spec.Type, spec.Role);
            const std::string handle = std::to_string(record.Handle);
            if (spec.Type == AssetType::Mesh)
            {
                record.GenerationRelativeCookedPath = "meshes/" + handle + ".spiralmesh";
                built.MeshHandle = record.Handle;
            }
            else if (spec.Type == AssetType::Material)
            {
                record.GenerationRelativeCookedPath = GetFabProjectMaterialRelativePath(record.Handle);
                built.MaterialHandle = record.Handle;
            }
            else
            {
                record.GenerationRelativeCookedPath = "textures/" + handle + ".rgba-fallback.spiraltexture";
                built.TextureHandle = record.Handle;
            }
            receipt.Assets.push_back(std::move(record));
        }

        for (FabImportedAssetRecord& record : receipt.Assets)
        {
            const fs::path file = built.StagedDirectory / fs::path(record.GenerationRelativeCookedPath);
            bool written = false;
            if (record.Type == AssetType::Mesh)
                written = WriteFilled(file, meshBytes, record.Handle ^ std::hash<std::string_view> {}(variant));
            else if (record.Type == AssetType::Texture)
                written = WriteFilled(file, 1000, record.Handle ^ std::hash<std::string_view> {}(variant));
            else
            {
                MaterialAsset material;
                material.Name = "Fab Material " + std::string(label);
                material.BaseColor = { 0.5f, 0.25f, 0.125f };
                material.Metallic = 0.75f;
                material.Roughness = 0.5f;
                material.Textures.BaseColor = built.TextureHandle;
                written = material.SaveToFile(file);
            }
            if (!written)
                return false;
            record.ArtifactSha256 = Sha256OfFile(file);
            built.Generation.Artifacts.push_back({ record.GenerationRelativeCookedPath, record.ArtifactSha256 });

            AssetMetadata metadata;
            metadata.Handle = record.Handle;
            metadata.Type = record.Type;
            metadata.SourcePath = GetFabProjectRegistrySourcePath(receipt.StreamId, record.LogicalPath);
            metadata.Name = record.SemanticRole;
            metadata.SourcePolicy = AssetSourcePolicy::ImmutablePackage;
            metadata.CookedRoot = built.CookedRoot;
            built.RegistryEntries.push_back(std::move(metadata));
        }
        built.Generation.StagedDirectory = built.StagedDirectory;
        built.Generation.RelativeRoot = built.RelativeRoot;
        std::string receiptError;
        if (!ValidateFabImportReceipt(receipt, receiptError))
        {
            std::cerr << "fixture receipt invalid: " << receiptError << '\n';
            return false;
        }
        out = std::move(built);
        return true;
    }

    bool CreateBaseProject(const Workspace& workspace)
    {
        Scene scene("Base Scene");
        scene.CreateEntity("Cube");
        AssetRegistry registry;
        registry.RegisterAsset(AssetType::Mesh, "Engine/Generated/PrototypeCube.mesh", "Cube");
        return scene.SaveToFile(workspace.Project() / "Scenes" / "Main.spiral")
            && registry.SaveToFile(workspace.Project() / "Assets" / "assets.spiralassets")
            && WriteAll(workspace.Manifest(),
                "SpiralProject 6\nScene \"Scenes/Main.spiral\"\nAssetRegistry \"Assets/assets.spiralassets\"\n"
                "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
                "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1\n"
                "ExposureMode ManualEV100\nCameraApertureFNumber 1\nCameraShutterSeconds 1\nCameraISO 100\n");
    }

    struct Candidate
    {
        ProjectCommitRequest Request;
        std::string ManifestBytes;
        std::vector<std::string> RevisionPaths;  // scene, registry, receipts
    };

    // Builds the next revision on top of whatever manifest is on disk now.
    bool BuildCandidate(const Workspace& workspace, const BuiltGeneration& generation, std::string_view token,
        Candidate& out, bool applyGeneration = true)
    {
        const std::string baseBytes = ReadAll(workspace.Manifest());
        ProjectManifest manifest;
        std::string error;
        if (!DeserializeProjectManifest(baseBytes, manifest, error))
            return false;
        AssetRegistry registry;
        if (!registry.LoadFromFile(workspace.Project() / fs::path(manifest.AssetRegistryPath)))
            return false;
        FabReceiptCollection receipts;
        if (!manifest.FabReceiptsPath.empty()
            && !LoadFabReceiptCollection(workspace.Project() / fs::path(manifest.FabReceiptsPath), receipts, error))
            return false;
        const std::string sceneBytes = ReadAll(workspace.Project() / fs::path(manifest.ScenePath));

        if (applyGeneration)
        {
            for (const AssetMetadata& metadata : generation.RegistryEntries)
            {
                if (generation.ReplacesGeneration.empty())
                {
                    if (!registry.RegisterAsset(metadata))
                        return false;
                }
                else if (!registry.CompareAndSwapAssetGeneration(metadata.Handle,
                             { AssetSourcePolicy::ImmutablePackage, GetFabProjectGenerationRoot(generation.ReplacesGeneration) },
                             { AssetSourcePolicy::ImmutablePackage, metadata.CookedRoot }))
                    return false;
            }
            FabReceiptDecision decision;
            if (!AddFabImportReceipt(receipts, generation.Receipt, decision, error))
            {
                std::cerr << "fixture receipt rejected: " << error << '\n';
                return false;
            }
        }

        const u64 revision = manifest.ProjectRevision + 1;
        const std::string suffix = ".r" + std::to_string(revision) + "-" + std::string(token);
        Candidate candidate;
        candidate.RevisionPaths = { "Scenes/Main" + suffix + ".spiral", "Assets/assets" + suffix + ".spiralassets",
            "Assets/Fab/receipts" + suffix + ".spiralfab" };

        const fs::path scratch = workspace.Staging() / ("registry-" + std::string(token));
        if (!registry.SaveToFile(scratch))
            return false;
        const std::string registryBytes = ReadAll(scratch);
        std::error_code removeError;
        fs::remove(scratch, removeError);
        std::string receiptBytes;
        if (!SerializeFabReceiptCollection(receipts, receiptBytes, error))
            return false;

        ProjectManifest next = manifest;
        next.ScenePath = candidate.RevisionPaths[0];
        next.AssetRegistryPath = candidate.RevisionPaths[1];
        next.FabReceiptsPath = candidate.RevisionPaths[2];
        next.ProjectRevision = revision;
        if (!SerializeProjectManifest(next, candidate.ManifestBytes, error))
            return false;

        candidate.Request.ProjectRoot = workspace.Project();
        candidate.Request.ManifestRelativePath = kManifestName;
        candidate.Request.ManifestBytes = candidate.ManifestBytes;
        candidate.Request.ExpectedManifestSha256 = Sha256Builder::HashString(baseBytes);
        candidate.Request.Generation = generation.Generation;
        candidate.Request.RevisionFiles = { { candidate.RevisionPaths[0], sceneBytes },
            { candidate.RevisionPaths[1], registryBytes }, { candidate.RevisionPaths[2], receiptBytes } };
        out = std::move(candidate);
        return true;
    }

    bool ProjectLoads(const Workspace& workspace, FabProjectValidationLevel level, std::string& error)
    {
        FabProjectState state;
        FabProjectValidationOptions options;
        options.Level = level;
        return LoadFabProjectState(workspace.Project(), kManifestName, options, state, error);
    }

    bool GenerationIsAbsentOrComplete(const Workspace& workspace, const BuiltGeneration& generation)
    {
        const fs::path directory = workspace.Project() / fs::path(generation.RelativeRoot);
        std::error_code error;
        if (!fs::exists(directory, error))
            return true;
        std::string verifyError;
        return VerifyGenerationDirectory(directory, generation.Generation.Artifacts, true, {}, verifyError);
    }

    struct Checker
    {
        bool Passed = true;
        const char* Prefix = "";

        void operator()(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << Prefix << ": " << message << '\n';
                Passed = false;
            }
        }
    };
}

namespace SpiralTests
{
    bool TestProjectCommitPublishesRevisionsAtomically()
    {
        Checker check { true, "Project commit publication test failed" };
        Workspace workspace;
        check(workspace.IsReady() && CreateBaseProject(workspace), "workspace and base project are created");
        if (!check.Passed)
            return false;

        if (!IsProjectCommitSupported())
        {
            BuiltGeneration generation;
            Candidate candidate;
            const Snapshot before = TakeSnapshot(workspace.Project());
            check(BuildGeneration(workspace, "alpha", "a", 4096, generation) && BuildCandidate(workspace, generation, "t1", candidate),
                "unsupported-platform candidate builds");
            const ProjectCommitResult result = CommitProjectRevision(candidate.Request);
            check(result.Outcome == ProjectCommitOutcome::NotCommitted && result.Error == ProjectCommitError::Unsupported
                    && !result.Message.empty() && TakeSnapshot(workspace.Project()) == before,
                "an unsupported platform fails closed with an explicit status and changes nothing");
            return check.Passed;
        }

        // ----- first revision: generation + three revision files + pointer -----
        BuiltGeneration alpha;
        Candidate first;
        check(BuildGeneration(workspace, "alpha", "a", 4096, alpha) && BuildCandidate(workspace, alpha, "t1", first),
            "first candidate builds");
        const Snapshot baseSnapshot = TakeSnapshot(workspace.Project());
        std::string manifestOnDiskDuringReload;
        first.Request.ValidateCommitted = [&](std::string& message)
        {
            manifestOnDiskDuringReload = ReadAll(workspace.Manifest());
            return ProjectLoads(workspace, FabProjectValidationLevel::Structural, message);
        };
        const ProjectCommitResult committed = CommitProjectRevision(first.Request);
        check(committed.Outcome == ProjectCommitOutcome::Committed
                || committed.Outcome == ProjectCommitOutcome::CommittedDurabilityUnconfirmed,
            "the first revision commits");
        check(committed.Error == ProjectCommitError::None && committed.Generation == ProjectCommitGenerationDisposition::Published,
            "the generation was published by this commit");
        check(committed.UndoBarrier.ClearUndoRedoHistory && committed.UndoBarrier.PreviousProjectRevision == 0
                && committed.UndoBarrier.NewProjectRevision == 1,
            "the undo barrier data carries the revision transition");
        check(committed.CommittedManifestSha256 == Sha256Builder::HashString(first.ManifestBytes)
                && ReadAll(workspace.Manifest()) == first.ManifestBytes,
            "the manifest on disk is exactly the candidate and its hash is reported");
        check(manifestOnDiskDuringReload == first.ManifestBytes && committed.Outcome != ProjectCommitOutcome::CommittedRecoveryRequired,
            "the structural reload validator ran against the committed manifest and passed");
        check(!fs::exists(alpha.StagedDirectory), "the staged directory was consumed by the no-replace rename");
        for (const std::string& path : first.RevisionPaths)
            check(ReadAll(workspace.Project() / path) == [&] {
                for (const ProjectCommitFile& file : first.Request.RevisionFiles)
                    if (file.RelativePath == path)
                        return file.Bytes;
                return std::string("missing");
            }(), "revision file written exactly: " + path);
        for (const auto& [path, state] : baseSnapshot)
            if (state.starts_with("F:") && path != kManifestName)
                check(TakeSnapshot(workspace.Project()).at(path) == state, "pre-existing file untouched: " + path);

        std::string error;
        check(ProjectLoads(workspace, FabProjectValidationLevel::Structural, error), "structural reload validation passes: " + error);
        check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error), "full-hash reload validation passes: " + error);
        FabProjectState state;
        check(LoadFabProjectState(workspace.Project(), kManifestName, {}, state, error)
                && state.Manifest.ProjectRevision == 1 && state.Receipts.Receipts.size() == 1
                && state.Registry.GetAssets().size() == 4,
            "the committed state has revision 1, one receipt and four registry assets");
        MaterialAsset material;
        check(LoadImmutableMaterialAsset(state.Registry, alpha.MaterialHandle, material, error)
                && material.Name == "Fab Material alpha" && material.Metallic == 0.75f,
            "the committed immutable material loads from its generation");
        FabProjectOrphanReport orphans;
        check(FindFabProjectOrphans(state, workspace.Project(), kManifestName, orphans, error)
                && orphans.UnreferencedGenerations.empty() && orphans.TemporaryFiles.empty(),
            "a clean commit reports no orphans");

        // ----- the old base can no longer commit -----
        check(CommitProjectRevision(first.Request).Error == ProjectCommitError::BaseChanged,
            "replaying a committed request is refused as a changed base");

        // ----- second revision: another product; the first generation is retained -----
        BuiltGeneration beta;
        Candidate second;
        check(BuildGeneration(workspace, "beta", "a", 2048, beta) && BuildCandidate(workspace, beta, "t2", second),
            "second candidate builds");
        const ProjectCommitResult secondResult = CommitProjectRevision(second.Request);
        check(IsCommitted(secondResult.Outcome) && secondResult.UndoBarrier.PreviousProjectRevision == 1
                && secondResult.UndoBarrier.NewProjectRevision == 2,
            "the second revision commits as revision 2");
        check(fs::exists(workspace.Project() / fs::path(alpha.RelativeRoot)), "the first generation is retained");
        check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error), "two-stream project validates: " + error);

        // ----- third revision: same-stream source replacement; the old generation is retained, unreferenced -----
        BuiltGeneration alpha2;
        Candidate third;
        check(BuildGeneration(workspace, "alpha", "b", 4096, alpha2, "s", alpha.Receipt.GenerationId)
                && BuildCandidate(workspace, alpha2, "t3", third),
            "source-replacement candidate builds");
        check(alpha2.MeshHandle == alpha.MeshHandle && alpha2.Receipt.GenerationId != alpha.Receipt.GenerationId,
            "a replacement keeps stable handles under a new generation");
        const ProjectCommitResult thirdResult = CommitProjectRevision(third.Request);
        check(IsCommitted(thirdResult.Outcome), "the replacement commits");
        check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error), "replaced project validates: " + error);
        FabProjectState replaced;
        check(LoadFabProjectState(workspace.Project(), kManifestName, {}, replaced, error)
                && replaced.Registry.GetAsset(alpha.MeshHandle)
                && replaced.Registry.GetAsset(alpha.MeshHandle)->CookedRoot == alpha2.CookedRoot,
            "the registry now resolves the stable handle to the replacement generation");
        check(fs::exists(workspace.Project() / fs::path(alpha.RelativeRoot)), "the superseded generation is retained");

        // Superseded generations are retained but not verified (documented boundary of the validator).
        const fs::path supersededMesh = workspace.Project() / fs::path(alpha.RelativeRoot) / fs::path(alpha.Receipt.Assets[0].GenerationRelativeCookedPath);
        check(FlipByte(supersededMesh, 10) && ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error),
            "tampering with a superseded generation is outside the current-state validation");

        // ----- a product update is a new stream beside the old one; both tips stay registered -----
        BuiltGeneration alphaV2;
        Candidate updateCandidate;
        check(BuildGeneration(workspace, "alpha", "v2", 1024, alphaV2, "s", {}, "2.0-glb", &alpha2)
                && alphaV2.Receipt.StreamId != alpha.Receipt.StreamId && alphaV2.MeshHandle != alpha.MeshHandle
                && BuildCandidate(workspace, alphaV2, "t2b", updateCandidate),
            "product-update candidate builds as a new stream");
        check(IsCommitted(CommitProjectRevision(updateCandidate.Request).Outcome)
                && ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error),
            "a product update commits and the project validates: " + error);
        FabProjectState updated;
        check(LoadFabProjectState(workspace.Project(), kManifestName, {}, updated, error)
                && updated.Registry.GetAsset(alpha.MeshHandle) && updated.Registry.GetAsset(alphaV2.MeshHandle)
                && updated.Receipts.Receipts.size() == 4,
            "both product versions remain registered; four receipts (alpha, beta, alpha replacement, alpha v2)");
        return check.Passed;
    }

    bool TestProjectCommitInjectedFailuresPreserveOldProject()
    {
        Checker check { true, "Project commit failure-injection test failed" };
        if (!IsProjectCommitSupported())
            return true;

        // Learn the hook table once from an uninjected commit.
        std::vector<ProjectCommitHook> hooks;
        {
            Workspace workspace;
            BuiltGeneration generation;
            Candidate candidate;
            check(workspace.IsReady() && CreateBaseProject(workspace) && BuildGeneration(workspace, "alpha", "a", 4096, generation)
                    && BuildCandidate(workspace, generation, "t1", candidate),
                "baseline fixture builds");
            if (!check.Passed)
                return false;
            candidate.Request.TestHook = [&hooks](ProjectCommitHook point, std::string_view)
            {
                hooks.push_back(point);
                return ProjectCommitHookAction::Continue;
            };
            check(IsCommitted(CommitProjectRevision(candidate.Request).Outcome), "the observed commit succeeds");
        }
        const std::vector<ProjectCommitHook> expectedTable = {
            ProjectCommitHook::Begin, ProjectCommitHook::Locked, ProjectCommitHook::BaseVerified,
            ProjectCommitHook::GenerationVerified, ProjectCommitHook::GenerationPublished,
            ProjectCommitHook::BeforeRevisionFile, ProjectCommitHook::RevisionFileWritten,
            ProjectCommitHook::BeforeRevisionFile, ProjectCommitHook::RevisionFileWritten,
            ProjectCommitHook::BeforeRevisionFile, ProjectCommitHook::RevisionFileWritten,
            ProjectCommitHook::BeforePointer, ProjectCommitHook::AfterPointer };
        check(hooks == expectedTable, "the hook table is exactly the documented sequence");

        struct Injection
        {
            ProjectCommitHookAction Action;
            const char* Name;
        };
        const Injection injections[] = { { ProjectCommitHookAction::Fail, "fail" }, { ProjectCommitHookAction::Cancel, "cancel" },
            { ProjectCommitHookAction::SimulateCrash, "crash" } };

        for (size_t ordinalToInject = 0; ordinalToInject < expectedTable.size(); ++ordinalToInject)
        {
            for (const Injection& injection : injections)
            {
                const std::string label = std::string(injection.Name) + "@" + std::to_string(ordinalToInject);
                Workspace workspace;
                BuiltGeneration generation;
                Candidate candidate;
                if (!workspace.IsReady() || !CreateBaseProject(workspace) || !BuildGeneration(workspace, "alpha", "a", 4096, generation)
                    || !BuildCandidate(workspace, generation, "t1", candidate))
                {
                    check(false, "fixture builds for " + label);
                    continue;
                }
                const std::string oldManifest = ReadAll(workspace.Manifest());
                const Snapshot projectBefore = TakeSnapshot(workspace.Project(), "Assets/fab");
                const Snapshot stagingBefore = TakeSnapshot(workspace.Staging());
                size_t ordinal = 0;
                candidate.Request.TestHook = [&](ProjectCommitHook, std::string_view)
                {
                    return ordinal++ == ordinalToInject ? injection.Action : ProjectCommitHookAction::Continue;
                };
                const ProjectCommitResult result = CommitProjectRevision(candidate.Request);
                const bool afterPointer = expectedTable[ordinalToInject] == ProjectCommitHook::AfterPointer;
                std::string error;

                if (afterPointer)
                {
                    check(IsCommitted(result.Outcome), label + ": cancel and failure after the pointer are ignored, the commit stands");
                    check(ReadAll(workspace.Manifest()) == candidate.ManifestBytes
                            && ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error),
                        label + ": the committed project loads and fully validates: " + error);
                    continue;
                }

                const ProjectCommitOutcome expectedOutcome = injection.Action == ProjectCommitHookAction::Cancel
                    ? ProjectCommitOutcome::Cancelled : ProjectCommitOutcome::NotCommitted;
                check(result.Outcome == expectedOutcome, label + ": outcome");
                check(ReadAll(workspace.Manifest()) == oldManifest, label + ": the old manifest is byte-identical");
                check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error),
                    label + ": the old project loads and fully validates: " + error);
                check(GenerationIsAbsentOrComplete(workspace, generation), label + ": no partially visible generation");

                const bool published = fs::exists(workspace.Project() / fs::path(generation.RelativeRoot));
                const bool publishedByHookPoint = ordinalToInject >= 4;
                check(published == publishedByHookPoint, label + ": the generation exists exactly after the publish point");
                if (!published)
                    check(TakeSnapshot(workspace.Staging()) == stagingBefore, label + ": the staged directory is untouched before publish");
                else
                    check(!fs::exists(generation.StagedDirectory), label + ": a published generation consumed its staged directory");

                if (injection.Action != ProjectCommitHookAction::SimulateCrash)
                    check(TakeSnapshot(workspace.Project(), "Assets/fab") == projectBefore,
                        label + ": every transaction-created revision file and directory was removed");
                else
                {
                    // A crash leaves unreferenced garbage; the old manifest still names only old files.
                    ProjectManifest old;
                    check(DeserializeProjectManifest(oldManifest, old, error) && old.FabReceiptsPath.empty(),
                        label + ": the surviving manifest names no receipts");
                }

                // Retry after the failure: new file names, same generation. It must commit, adopting a
                // generation that was already published, and the result must fully validate.
                Candidate retry;
                check(BuildCandidate(workspace, generation, "retry", retry), label + ": retry candidate builds");
                const ProjectCommitResult retried = CommitProjectRevision(retry.Request);
                check(IsCommitted(retried.Outcome) && retried.Error == ProjectCommitError::None, label + ": the retry commits");
                check(retried.Generation == (published ? ProjectCommitGenerationDisposition::AdoptedExisting
                                                       : ProjectCommitGenerationDisposition::Published),
                    label + ": the retry adopts an existing generation or publishes a missing one");
                check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error),
                    label + ": the retried project fully validates: " + error);
            }
        }

        // ----- cancellation requested through IsCancelled at every poll -----
        size_t polls = 0;
        {
            Workspace workspace;
            BuiltGeneration generation;
            Candidate candidate;
            check(workspace.IsReady() && CreateBaseProject(workspace) && BuildGeneration(workspace, "alpha", "a", 4096, generation)
                    && BuildCandidate(workspace, generation, "t1", candidate),
                "poll-counting fixture builds");
            candidate.Request.IsCancelled = [&polls]() { ++polls; return false; };
            check(IsCommitted(CommitProjectRevision(candidate.Request).Outcome), "the poll-counting commit succeeds");
        }
        check(polls >= 5, "cancellation is polled at the generation, each revision file and the pointer");
        for (size_t cancelAt = 1; cancelAt <= polls + 1; ++cancelAt)
        {
            const std::string label = "cancel-poll@" + std::to_string(cancelAt);
            Workspace workspace;
            BuiltGeneration generation;
            Candidate candidate;
            if (!workspace.IsReady() || !CreateBaseProject(workspace) || !BuildGeneration(workspace, "alpha", "a", 4096, generation)
                || !BuildCandidate(workspace, generation, "t1", candidate))
            {
                check(false, label + ": fixture builds");
                continue;
            }
            const std::string oldManifest = ReadAll(workspace.Manifest());
            const Snapshot projectBefore = TakeSnapshot(workspace.Project(), "Assets/fab");
            size_t seen = 0;
            candidate.Request.IsCancelled = [&seen, cancelAt]() { return ++seen == cancelAt; };
            const ProjectCommitResult result = CommitProjectRevision(candidate.Request);
            std::string error;
            if (cancelAt <= polls)
            {
                check(result.Outcome == ProjectCommitOutcome::Cancelled, label + ": reports Cancelled");
                check(ReadAll(workspace.Manifest()) == oldManifest && TakeSnapshot(workspace.Project(), "Assets/fab") == projectBefore
                        && GenerationIsAbsentOrComplete(workspace, generation) && ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error),
                    label + ": the old project is intact and loads");
            }
            else
                check(IsCommitted(result.Outcome), label + ": cancelling after the last poll cannot stop a commit that already passed it");
        }

        // ----- post-commit reload failure is recovery-required, never a rollback -----
        {
            Workspace workspace;
            BuiltGeneration generation;
            Candidate candidate;
            check(workspace.IsReady() && CreateBaseProject(workspace) && BuildGeneration(workspace, "alpha", "a", 4096, generation)
                    && BuildCandidate(workspace, generation, "t1", candidate),
                "reload-failure fixture builds");
            candidate.Request.ValidateCommitted = [](std::string& message)
            {
                message = "injected reload failure";
                return false;
            };
            const ProjectCommitResult result = CommitProjectRevision(candidate.Request);
            check(result.Outcome == ProjectCommitOutcome::CommittedRecoveryRequired && IsCommitted(result.Outcome)
                    && result.Error == ProjectCommitError::ReloadValidationFailed
                    && result.Message.find("injected reload failure") != std::string::npos,
                "a failed post-commit validation reports recovery-required");
            check(ReadAll(workspace.Manifest()) == candidate.ManifestBytes, "the disk stays committed");
        }
        return check.Passed;
    }

    bool TestProjectCommitGenerationPublicationIsCreateOnce()
    {
        Checker check { true, "Project commit generation test failed" };
        if (!IsProjectCommitSupported())
            return true;

        // Every case starts from a fresh project plus a candidate for the same generation, then
        // prepares the final location differently and asserts nothing there was ever replaced.
        struct Case
        {
            const char* Name;
            bool KeepStaged;
            ProjectCommitError ExpectedError;  // None means the commit must succeed
            ProjectCommitGenerationDisposition ExpectedDisposition;
            void (*Prepare)(const Workspace&, const BuiltGeneration&);
        };
        const Case cases[] = {
            { "existing identical directory is adopted", true, ProjectCommitError::None, ProjectCommitGenerationDisposition::AdoptedExisting,
                [](const Workspace& w, const BuiltGeneration& g)
                {
                    std::error_code e;
                    fs::create_directories((w.Project() / fs::path(g.RelativeRoot)).parent_path(), e);
                    fs::copy(g.StagedDirectory, w.Project() / fs::path(g.RelativeRoot), fs::copy_options::recursive, e);
                } },
            { "existing identical directory is adopted without a staged copy", false, ProjectCommitError::None,
                ProjectCommitGenerationDisposition::AdoptedExisting,
                [](const Workspace& w, const BuiltGeneration& g)
                {
                    std::error_code e;
                    fs::create_directories((w.Project() / fs::path(g.RelativeRoot)).parent_path(), e);
                    fs::copy(g.StagedDirectory, w.Project() / fs::path(g.RelativeRoot), fs::copy_options::recursive, e);
                } },
            { "existing directory with a flipped artifact byte", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace& w, const BuiltGeneration& g)
                {
                    std::error_code e;
                    fs::create_directories((w.Project() / fs::path(g.RelativeRoot)).parent_path(), e);
                    fs::copy(g.StagedDirectory, w.Project() / fs::path(g.RelativeRoot), fs::copy_options::recursive, e);
                    FlipByte(w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[0].GenerationRelativeCookedPath), 5);
                } },
            { "existing directory with an extra file", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace& w, const BuiltGeneration& g)
                {
                    std::error_code e;
                    fs::create_directories((w.Project() / fs::path(g.RelativeRoot)).parent_path(), e);
                    fs::copy(g.StagedDirectory, w.Project() / fs::path(g.RelativeRoot), fs::copy_options::recursive, e);
                    WriteAll(w.Project() / fs::path(g.RelativeRoot) / "extra.bin", "x");
                } },
            { "existing directory missing an artifact", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace& w, const BuiltGeneration& g)
                {
                    std::error_code e;
                    fs::create_directories((w.Project() / fs::path(g.RelativeRoot)).parent_path(), e);
                    fs::copy(g.StagedDirectory, w.Project() / fs::path(g.RelativeRoot), fs::copy_options::recursive, e);
                    fs::remove(w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[2].GenerationRelativeCookedPath), e);
                } },
            { "existing EMPTY directory is not replaced", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace& w, const BuiltGeneration& g)
                {
                    std::error_code e;
                    fs::create_directories(w.Project() / fs::path(g.RelativeRoot), e);
                } },
            { "a regular file at the generation location", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace& w, const BuiltGeneration& g) { WriteAll(w.Project() / fs::path(g.RelativeRoot), "file"); } },
            { "a symlink to a valid generation is not adopted", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace& w, const BuiltGeneration& g)
                {
                    std::error_code e;
                    fs::create_directories((w.Project() / fs::path(g.RelativeRoot)).parent_path(), e);
                    fs::copy(g.StagedDirectory, w.Base() / "elsewhere", fs::copy_options::recursive, e);
                    fs::create_directory_symlink(w.Base() / "elsewhere", w.Project() / fs::path(g.RelativeRoot), e);
                } },
            { "a tampered staged artifact is never published", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace&, const BuiltGeneration& g)
                {
                    FlipByte(g.StagedDirectory / fs::path(g.Receipt.Assets[0].GenerationRelativeCookedPath), 7);
                } },
            { "a staged directory with an extra file is never published", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace&, const BuiltGeneration& g) { WriteAll(g.StagedDirectory / "extra.bin", "x"); } },
            { "a staged directory missing an artifact is never published", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace&, const BuiltGeneration& g)
                {
                    std::error_code e;
                    fs::remove(g.StagedDirectory / fs::path(g.Receipt.Assets[1].GenerationRelativeCookedPath), e);
                } },
            { "a staged symlinked artifact is never published", true, ProjectCommitError::GenerationMismatch, ProjectCommitGenerationDisposition::None,
                [](const Workspace& w, const BuiltGeneration& g)
                {
                    std::error_code e;
                    const fs::path artifact = g.StagedDirectory / fs::path(g.Receipt.Assets[0].GenerationRelativeCookedPath);
                    fs::rename(artifact, w.Base() / "moved.bin", e);
                    fs::create_symlink(w.Base() / "moved.bin", artifact, e);
                } },
            { "neither an existing nor a staged generation", false, ProjectCommitError::GenerationMissing, ProjectCommitGenerationDisposition::None,
                [](const Workspace&, const BuiltGeneration&) {} },
        };

        for (const Case& testCase : cases)
        {
            const std::string label = testCase.Name;
            Workspace workspace;
            BuiltGeneration generation;
            Candidate candidate;
            if (!workspace.IsReady() || !CreateBaseProject(workspace) || !BuildGeneration(workspace, "alpha", "a", 4096, generation)
                || !BuildCandidate(workspace, generation, "t1", candidate))
            {
                check(false, label + ": fixture builds");
                continue;
            }
            testCase.Prepare(workspace, generation);
            if (!testCase.KeepStaged)
                candidate.Request.Generation->StagedDirectory.clear();
            if (std::string_view(testCase.Name) == "neither an existing nor a staged generation")
            {
                std::error_code e;
                fs::remove_all(generation.StagedDirectory, e);
            }
            const std::string oldManifest = ReadAll(workspace.Manifest());
            const Snapshot projectBefore = TakeSnapshot(workspace.Project());
            const Snapshot stagingBefore = TakeSnapshot(workspace.Staging());
            const Snapshot elsewhereBefore = TakeSnapshot(workspace.Base() / "elsewhere");
            const ProjectCommitResult result = CommitProjectRevision(candidate.Request);
            std::string error;

            if (testCase.ExpectedError == ProjectCommitError::None)
            {
                check(IsCommitted(result.Outcome) && result.Generation == testCase.ExpectedDisposition,
                    label + ": commits with the expected disposition");
                check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error), label + ": the result fully validates: " + error);
                if (testCase.KeepStaged)
                    check(TakeSnapshot(workspace.Staging()) == stagingBefore,
                        label + ": an adopted generation leaves the caller's staged copy untouched");
                continue;
            }

            check(result.Outcome == ProjectCommitOutcome::NotCommitted && result.Error == testCase.ExpectedError,
                label + ": fails closed with the expected error (" + result.Message + ")");
            check(ReadAll(workspace.Manifest()) == oldManifest, label + ": the old manifest is byte-identical");
            check(TakeSnapshot(workspace.Project()) == projectBefore,
                label + ": nothing under the project changed, including the pre-existing generation location");
            check(TakeSnapshot(workspace.Staging()) == stagingBefore, label + ": the staged directory is untouched");
            check(TakeSnapshot(workspace.Base() / "elsewhere") == elsewhereBefore, label + ": nothing outside the project changed");
            check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error), label + ": the old project still validates: " + error);
        }

        // ----- a competing publisher wins between verification and the rename -----
        enum class Competitor { Identical, Different, EmptyDirectory };
        for (const Competitor competitor : { Competitor::Identical, Competitor::Different, Competitor::EmptyDirectory })
        {
            const bool identical = competitor == Competitor::Identical;
            const std::string label = competitor == Competitor::Identical ? "competing identical publisher"
                : competitor == Competitor::Different ? "competing different publisher" : "competing empty directory";
            Workspace workspace;
            BuiltGeneration generation;
            Candidate candidate;
            check(workspace.IsReady() && CreateBaseProject(workspace) && BuildGeneration(workspace, "alpha", "a", 4096, generation)
                    && BuildCandidate(workspace, generation, "t1", candidate),
                label + ": fixture builds");
            const fs::path target = workspace.Project() / fs::path(generation.RelativeRoot);
            candidate.Request.TestHook = [&](ProjectCommitHook point, std::string_view detail)
            {
                if (point == ProjectCommitHook::GenerationVerified && detail == "staged")
                {
                    std::error_code e;
                    fs::create_directories(target.parent_path(), e);
                    if (competitor == Competitor::EmptyDirectory)
                        fs::create_directory(target, e);
                    else
                        fs::copy(generation.StagedDirectory, target, fs::copy_options::recursive, e);
                    if (competitor == Competitor::Different)
                        FlipByte(target / fs::path(generation.Receipt.Assets[0].GenerationRelativeCookedPath), 3);
                }
                return ProjectCommitHookAction::Continue;
            };
            const Snapshot stagingBefore = TakeSnapshot(workspace.Staging());
            const ProjectCommitResult result = CommitProjectRevision(candidate.Request);
            std::string error;
            if (identical)
                check(IsCommitted(result.Outcome) && result.Generation == ProjectCommitGenerationDisposition::AdoptedExisting
                        && ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error),
                    label + ": losing the NOREPLACE race to an identical directory adopts it");
            else
            {
                const Snapshot foreign = TakeSnapshot(target);
                check(result.Outcome == ProjectCommitOutcome::NotCommitted && result.Error == ProjectCommitError::GenerationMismatch
                        && TakeSnapshot(target) == foreign,
                    label + ": a directory that appeared first is neither replaced nor adopted");
                check(TakeSnapshot(workspace.Staging()) == stagingBefore, label + ": the staged copy is untouched");
                check(ReadAll(workspace.Manifest()).starts_with("SpiralProject 6"), label + ": the old manifest remains");
            }
        }

        // ----- idempotent re-run: the same generation staged again, committed on top of a newer revision -----
        {
            Workspace workspace;
            BuiltGeneration generation;
            Candidate first;
            check(workspace.IsReady() && CreateBaseProject(workspace) && BuildGeneration(workspace, "alpha", "a", 4096, generation)
                    && BuildCandidate(workspace, generation, "t1", first) && IsCommitted(CommitProjectRevision(first.Request).Outcome),
                "idempotence: first commit");
            BuiltGeneration again;
            Candidate second;
            check(BuildGeneration(workspace, "alpha", "a", 4096, again, "s2") && again.Receipt.GenerationId == generation.Receipt.GenerationId
                    && BuildCandidate(workspace, again, "t2", second, false),
                "idempotence: the same inputs rebuild the same generation id");
            const Snapshot stagingBefore = TakeSnapshot(workspace.Staging());
            const ProjectCommitResult result = CommitProjectRevision(second.Request);
            std::string error;
            check(IsCommitted(result.Outcome) && result.Generation == ProjectCommitGenerationDisposition::AdoptedExisting
                    && result.UndoBarrier.NewProjectRevision == 2 && TakeSnapshot(workspace.Staging()) == stagingBefore
                    && ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error),
                "idempotence: re-running the same import adopts the existing generation and leaves the new staged copy for the caller");
        }
        return check.Passed;
    }

    bool TestProjectCommitRejectsEscapesStaleBasesAndLockContention()
    {
        Checker check { true, "Project commit rejection test failed" };
        Workspace probe;
        check(probe.IsReady(), "probe workspace");

        // A fresh fixture with outside-project observation points for every case.
        struct Fixture
        {
            Workspace Space;
            BuiltGeneration Generation;
            Candidate Next;
            Snapshot Before;
            Snapshot ProjectBeforeWithoutGeneration;
            Snapshot OutsideBefore;
            std::string OldManifest;
            bool Ready = false;

            bool Prepare()
            {
                if (!Space.IsReady() || !CreateBaseProject(Space) || !BuildGeneration(Space, "alpha", "a", 4096, Generation))
                    return false;
                std::error_code error;
                fs::create_directories(Space.Base() / "outside", error);
                return true;
            }

            bool Finish()
            {
                Ready = BuildCandidate(Space, Generation, "t1", Next);
                OldManifest = ReadAll(Space.Manifest());
                Before = TakeSnapshot(Space.Base());
                ProjectBeforeWithoutGeneration = TakeSnapshot(Space.Project(), "Assets/fab");
                OutsideBefore = TakeSnapshot(Space.Base() / "outside");
                return Ready;
            }

            // Everything under the workspace (project, staging, outside) is unchanged.
            bool Unchanged() const { return TakeSnapshot(Space.Base()) == Before && ReadAll(Space.Manifest()) == OldManifest; }
            // Only a published generation (immutable, unreferenced, harmless) may differ.
            bool UnchangedExceptGeneration() const
            {
                return TakeSnapshot(Space.Project(), "Assets/fab") == ProjectBeforeWithoutGeneration
                    && TakeSnapshot(Space.Base() / "outside") == OutsideBefore && ReadAll(Space.Manifest()) == OldManifest;
            }
        };

        if (!IsProjectCommitSupported())
            return check.Passed;

        enum class Expect { Unchanged, UnchangedExceptGeneration, Unchecked };
        const auto rejects = [&](const std::string& label, Fixture& fixture, ProjectCommitError expected,
                                 Expect expectation = Expect::Unchanged)
        {
            const ProjectCommitResult result = CommitProjectRevision(fixture.Next.Request);
            check(result.Outcome == ProjectCommitOutcome::NotCommitted && result.Error == expected && !result.Message.empty(),
                label + ": expected error (got message: " + result.Message + ")");
            if (expectation == Expect::Unchanged)
                check(fixture.Unchanged(), label + ": nothing changed anywhere");
            else if (expectation == Expect::UnchangedExceptGeneration)
                check(fixture.UnchangedExceptGeneration(), label + ": nothing changed except an unreferenced published generation");
        };

        // ----- path escape attempts in every request path -----
        for (const char* escape : { "../outside/x", "/abs/x", "a/../../outside/x", "a//b", "a\\b", "C:/x", "./a", "a/.", "", "a/", "con", "a/b.", " lead" })
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), std::string("fixture for revision escape ") + escape);
            fixture.Next.Request.RevisionFiles[1].RelativePath = escape;
            rejects(std::string("revision file path '") + escape + "'", fixture, ProjectCommitError::PathEscape);
        }
        for (const char* escape : { "../outside/gen", "/abs/gen", "Assets/../../outside/gen", "", "Assets//fab/x" })
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), std::string("fixture for generation escape ") + escape);
            fixture.Next.Request.Generation->RelativeRoot = escape;
            rejects(std::string("generation root '") + escape + "'", fixture, ProjectCommitError::PathEscape);
        }
        for (const char* escape : { "../outside/m.spiralproject", "/abs/m", "", "a/../m" })
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), std::string("fixture for manifest escape ") + escape);
            fixture.Next.Request.ManifestRelativePath = escape;
            rejects(std::string("manifest path '") + escape + "'", fixture, ProjectCommitError::PathEscape);
        }

        // ----- link components: a symlinked directory cannot redirect a write outside the project -----
        struct LinkCase
        {
            const char* Name;
            const char* LinkPath;  // replaced by a symlink to <workspace>/outside
            size_t Mode;           // 0: revision files, 1: generation parent, 2: manifest
        };
        for (const LinkCase& linkCase : { LinkCase { "Scenes directory", "Scenes", 0 }, LinkCase { "Assets/Fab directory", "Assets/Fab", 0 },
                 LinkCase { "Assets directory", "Assets", 1 } })
        {
            Fixture fixture;
            check(fixture.Prepare(), std::string("fixture for link case ") + linkCase.Name);
            std::error_code e;
            if (linkCase.Mode == 1)
            {
                // Move Assets aside, then link it: the registry and receipts live behind the link.
                fs::rename(fixture.Space.Project() / "Assets", fixture.Space.Base() / "outside" / "Assets", e);
                fs::create_directory_symlink(fixture.Space.Base() / "outside" / "Assets", fixture.Space.Project() / "Assets", e);
            }
            else if (std::string_view(linkCase.LinkPath) == "Scenes")
            {
                fs::rename(fixture.Space.Project() / "Scenes", fixture.Space.Base() / "outside" / "Scenes", e);
                fs::create_directory_symlink(fixture.Space.Base() / "outside" / "Scenes", fixture.Space.Project() / "Scenes", e);
            }
            else
                fs::create_directory_symlink(fixture.Space.Base() / "outside", fixture.Space.Project() / linkCase.LinkPath, e);
            check(!e, std::string("symlink created for ") + linkCase.Name);
            if (!fixture.Finish())
            {
                // The candidate builder follows links to read the base; for the Assets case rebuild by hand.
                check(linkCase.Mode == 1, std::string("candidate builds for link case ") + linkCase.Name);
                continue;
            }
            rejects(std::string("link ") + linkCase.Name, fixture, ProjectCommitError::PathEscape,
                linkCase.Mode == 0 ? Expect::UnchangedExceptGeneration : Expect::Unchanged);
            check(TakeSnapshot(fixture.Space.Base() / "outside" / "Scenes").size() <= 1
                    && !fs::exists(fixture.Space.Base() / "outside" / "x"),
                std::string("link ") + linkCase.Name + ": no revision file was written through the link");
        }
        {
            // The manifest itself is a link.
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for manifest link");
            std::error_code e;
            fs::rename(fixture.Space.Manifest(), fixture.Space.Base() / "outside" / "real.spiralproject", e);
            fs::create_symlink(fixture.Space.Base() / "outside" / "real.spiralproject", fixture.Space.Manifest(), e);
            fixture.Before = TakeSnapshot(fixture.Space.Base());
            rejects("manifest is a symlink", fixture, ProjectCommitError::PathEscape, Expect::Unchecked);
            check(TakeSnapshot(fixture.Space.Base()) == fixture.Before, "manifest symlink: nothing changed");
        }

        // ----- stale bases, revisions, and malformed candidates -----
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for stale base");
            WriteAll(fixture.Space.Manifest(), ReadAll(fixture.Space.Manifest()) + "\n");
            fixture.OldManifest = ReadAll(fixture.Space.Manifest());
            fixture.Before = TakeSnapshot(fixture.Space.Base());
            rejects("manifest changed after the candidate was built", fixture, ProjectCommitError::BaseChanged);
        }
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for empty expected hash");
            fixture.Next.Request.ExpectedManifestSha256.clear();
            rejects("empty expected hash while a manifest exists", fixture, ProjectCommitError::BaseChanged);
        }
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for malformed expected hash");
            fixture.Next.Request.ExpectedManifestSha256 = std::string(64, 'A');
            rejects("uppercase expected hash", fixture, ProjectCommitError::InvalidRequest);
        }
        for (const u64 wrongRevision : { u64 { 0 }, u64 { 2 }, u64 { 7 } })
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for wrong revision");
            ProjectManifest manifest;
            std::string error;
            check(DeserializeProjectManifest(fixture.Next.ManifestBytes, manifest, error), "candidate manifest parses");
            manifest.ProjectRevision = wrongRevision;
            std::string bytes;
            check(SerializeProjectManifest(manifest, bytes, error), "wrong-revision manifest serializes");
            fixture.Next.Request.ManifestBytes = bytes;
            rejects("candidate revision " + std::to_string(wrongRevision) + " over base revision 0", fixture, ProjectCommitError::InvalidManifest);
        }
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for garbage candidate");
            fixture.Next.Request.ManifestBytes = "this is not a manifest";
            rejects("unparsable candidate manifest", fixture, ProjectCommitError::InvalidManifest);
        }
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for missing pointer target");
            ProjectManifest manifest;
            std::string error, bytes;
            check(DeserializeProjectManifest(fixture.Next.ManifestBytes, manifest, error), "candidate manifest parses");
            manifest.ScenePath = "Scenes/does-not-exist.spiral";
            check(SerializeProjectManifest(manifest, bytes, error), "missing-target manifest serializes");
            fixture.Next.Request.ManifestBytes = bytes;
            // Revision files are created first and must be cleaned up when the pointer check fails.
            rejects("candidate manifest names a missing file", fixture, ProjectCommitError::InvalidManifest, Expect::Unchecked);
            std::error_code e;
            check(ReadAll(fixture.Space.Manifest()) == fixture.OldManifest, "missing target: old manifest remains");
            check(!fs::exists(fixture.Space.Project() / fixture.Next.RevisionPaths[0])
                    && !fs::exists(fixture.Space.Project() / fixture.Next.RevisionPaths[1])
                    && !fs::exists(fixture.Space.Project() / fixture.Next.RevisionPaths[2]),
                "missing target: the revision files created before the check were removed");
        }
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for corrupt current manifest");
            WriteAll(fixture.Space.Manifest(), "garbage that is not a manifest");
            fixture.Next.Request.ExpectedManifestSha256 = Sha256Builder::HashString("garbage that is not a manifest");
            fixture.OldManifest = ReadAll(fixture.Space.Manifest());
            fixture.Before = TakeSnapshot(fixture.Space.Base());
            rejects("unparsable current manifest is not repaired", fixture, ProjectCommitError::InvalidManifest);
        }
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for duplicate names");
            fixture.Next.Request.RevisionFiles[2].RelativePath = fixture.Next.Request.RevisionFiles[0].RelativePath;
            rejects("duplicate revision file names", fixture, ProjectCommitError::InvalidRequest);
        }
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for case-fold duplicate names");
            std::string upper = fixture.Next.Request.RevisionFiles[0].RelativePath;
            for (char& character : upper)
                if (character >= 'a' && character <= 'z')
                    character = static_cast<char>(character - 'a' + 'A');
            fixture.Next.Request.RevisionFiles[2].RelativePath = upper;
            rejects("case-fold duplicate revision file names", fixture, ProjectCommitError::InvalidRequest);
        }
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for revision file inside generation");
            fixture.Next.Request.RevisionFiles[2].RelativePath = fixture.Next.Request.Generation->RelativeRoot + "/receipts.spiralfab";
            rejects("revision file inside the generation", fixture, ProjectCommitError::InvalidRequest);
        }

        // ----- revision files are created exclusively; the cleanup removes only what this commit made -----
        for (size_t blocked = 0; blocked < 3; ++blocked)
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for exclusive creation");
            const fs::path blocker = fixture.Space.Project() / fixture.Next.RevisionPaths[blocked];
            WriteAll(blocker, "someone else's bytes");
            fixture.ProjectBeforeWithoutGeneration = TakeSnapshot(fixture.Space.Project(), "Assets/fab");
            const ProjectCommitResult result = CommitProjectRevision(fixture.Next.Request);
            check(result.Outcome == ProjectCommitOutcome::NotCommitted && result.Error == ProjectCommitError::RevisionFileExists,
                "an existing revision file name fails the commit (index " + std::to_string(blocked) + ")");
            check(ReadAll(blocker) == "someone else's bytes", "the pre-existing file is never overwritten or deleted");
            check(TakeSnapshot(fixture.Space.Project(), "Assets/fab") == fixture.ProjectBeforeWithoutGeneration,
                "files created before the collision were removed again");
            check(ReadAll(fixture.Space.Manifest()) == fixture.OldManifest, "the old manifest remains after a collision");
        }

        // ----- lock contention -----
#if defined(__linux__)
        {
            Fixture fixture;
            check(fixture.Prepare() && fixture.Finish(), "fixture for lock contention");
            const int holder = open(fixture.Space.Project().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            check(holder >= 0 && flock(holder, LOCK_EX | LOCK_NB) == 0, "the test holds the project lock");
            rejects("another commit holds the project lock", fixture, ProjectCommitError::LockUnavailable);
            if (holder >= 0)
                close(holder);
            check(IsCommitted(CommitProjectRevision(fixture.Next.Request).Outcome), "the commit succeeds once the lock is released");
        }
#endif
        return check.Passed;
    }

    bool TestProjectCommitConcurrentWritersAreExcluded()
    {
        Checker check { true, "Project commit concurrency test failed" };
        if (!IsProjectCommitSupported())
            return true;

        for (const size_t writers : { size_t { 2 }, size_t { 6 } })
        {
            Workspace workspace;
            check(workspace.IsReady() && CreateBaseProject(workspace), "workspace");
            std::vector<BuiltGeneration> generations(writers);
            std::vector<Candidate> candidates(writers);
            for (size_t index = 0; index < writers; ++index)
            {
                const std::string label = "writer" + std::to_string(index);
                check(BuildGeneration(workspace, label, "a", 64 * 1024, generations[index], "s")
                        && BuildCandidate(workspace, generations[index], "t" + std::to_string(index), candidates[index]),
                    "candidate " + label + " builds from the shared base");
            }
            if (!check.Passed)
                return false;

            std::atomic<size_t> ready { 0 };
            std::atomic<bool> go { false };
            std::vector<ProjectCommitResult> results(writers);
            std::vector<std::thread> threads;
            for (size_t index = 0; index < writers; ++index)
                threads.emplace_back([&, index]()
                {
                    ready.fetch_add(1);
                    while (!go.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    results[index] = CommitProjectRevision(candidates[index].Request);
                });
            while (ready.load() < writers)
                std::this_thread::yield();
            go.store(true, std::memory_order_release);
            for (std::thread& thread : threads)
                thread.join();

            size_t winners = 0;
            size_t winner = 0;
            for (size_t index = 0; index < writers; ++index)
            {
                if (IsCommitted(results[index].Outcome))
                {
                    ++winners;
                    winner = index;
                }
                else
                    check(results[index].Error == ProjectCommitError::BaseChanged || results[index].Error == ProjectCommitError::LockUnavailable,
                        "a losing writer fails with BaseChanged or LockUnavailable, not corruption (" + results[index].Message + ")");
            }
            check(winners == 1, "exactly one of " + std::to_string(writers) + " concurrent writers commits (got " + std::to_string(winners) + ")");

            std::string error;
            check(ReadAll(workspace.Manifest()) == candidates[winner].ManifestBytes, "the manifest is the winner's candidate");
            check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error), "the final project fully validates: " + error);
            size_t publishedGenerations = 0;
            for (size_t index = 0; index < writers; ++index)
            {
                const bool published = fs::exists(workspace.Project() / fs::path(generations[index].RelativeRoot));
                publishedGenerations += published ? 1 : 0;
                check(published == (index == winner), "only the winner's generation is published");
                for (const std::string& path : candidates[index].RevisionPaths)
                    check(fs::exists(workspace.Project() / path) == (index == winner), "only the winner's revision files exist");
            }
            check(publishedGenerations == 1, "exactly one generation directory exists");
            FabProjectOrphanReport orphans;
            FabProjectState state;
            check(LoadFabProjectState(workspace.Project(), kManifestName, {}, state, error)
                    && FindFabProjectOrphans(state, workspace.Project(), kManifestName, orphans, error)
                    && orphans.UnreferencedGenerations.empty() && orphans.TemporaryFiles.empty(),
                "no orphans or temporaries remain after the race");
        }
        return check.Passed;
    }

    bool TestFabProjectStateDetectsTamperAndOrphans()
    {
        Checker check { true, "Fab project state test failed" };
        if (!IsProjectCommitSupported())
            return true;

        struct Mutation
        {
            const char* Name;
            bool StructuralOk;
            bool FullHashOk;
            void (*Apply)(const Workspace&, const BuiltGeneration&, const Candidate&);
        };
        const Mutation mutations[] = {
            { "untouched", true, true, [](const Workspace&, const BuiltGeneration&, const Candidate&) {} },
            { "flipped byte in a mesh artifact", true, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                { FlipByte(w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[0].GenerationRelativeCookedPath), 100); } },
            { "flipped byte in a texture artifact", true, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                { FlipByte(w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[2].GenerationRelativeCookedPath), 0); } },
            { "mesh artifact replaced by a same-size different file", true, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                { WriteFilled(w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[0].GenerationRelativeCookedPath), 4096, 999); } },
            { "deleted mesh artifact", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                { std::error_code e; fs::remove(w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[0].GenerationRelativeCookedPath), e); } },
            { "extra file in the generation", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                { WriteAll(w.Project() / fs::path(g.RelativeRoot) / "extra.bin", "x"); } },
            { "artifact replaced by a symlink to identical bytes", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                {
                    std::error_code e;
                    const fs::path file = w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[0].GenerationRelativeCookedPath);
                    fs::rename(file, w.Base() / "moved.bin", e);
                    fs::create_symlink(w.Base() / "moved.bin", file, e);
                } },
            { "generation directory replaced by a symlink", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                {
                    std::error_code e;
                    const fs::path directory = w.Project() / fs::path(g.RelativeRoot);
                    fs::rename(directory, w.Base() / "moved-generation", e);
                    fs::create_directory_symlink(w.Base() / "moved-generation", directory, e);
                } },
            { "material artifact is garbage", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                { WriteAll(w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[1].GenerationRelativeCookedPath), "not a material"); } },
            { "material artifact is deleted", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate&)
                { std::error_code e; fs::remove(w.Project() / fs::path(g.RelativeRoot) / fs::path(g.Receipt.Assets[1].GenerationRelativeCookedPath), e); } },
            { "receipts: an artifact hash digit is changed", true, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate& c)
                {
                    const fs::path file = w.Project() / c.RevisionPaths[2];
                    std::string bytes = ReadAll(file);
                    const size_t at = bytes.find(g.Receipt.Assets[0].ArtifactSha256);
                    bytes[at] = bytes[at] == '0' ? '1' : '0';
                    WriteAll(file, bytes);
                } },
            { "receipts: a generation id digit is changed", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate& c)
                {
                    const fs::path file = w.Project() / c.RevisionPaths[2];
                    std::string bytes = ReadAll(file);
                    const size_t at = bytes.find("GenerationId \"" + g.Receipt.GenerationId);
                    bytes[at + 14] = bytes[at + 14] == '0' ? '1' : '0';
                    WriteAll(file, bytes);
                } },
            { "receipts file is truncated", false, false,
                [](const Workspace& w, const BuiltGeneration&, const Candidate& c)
                {
                    const fs::path file = w.Project() / c.RevisionPaths[2];
                    std::string bytes = ReadAll(file);
                    bytes.resize(bytes.size() / 2);
                    WriteAll(file, bytes);
                } },
            { "receipts file is missing", false, false,
                [](const Workspace& w, const BuiltGeneration&, const Candidate& c)
                { std::error_code e; fs::remove(w.Project() / c.RevisionPaths[2], e); } },
            { "receipts file rolled back to an empty collection (registry orphans)", false, false,
                [](const Workspace& w, const BuiltGeneration&, const Candidate& c)
                {
                    std::string bytes;
                    std::string error;
                    SerializeFabReceiptCollection({}, bytes, error);
                    WriteAll(w.Project() / c.RevisionPaths[2], bytes);
                } },
            { "registry: a receipt asset is unregistered", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate& c)
                {
                    AssetRegistry registry;
                    registry.LoadFromFile(w.Project() / c.RevisionPaths[1]);
                    registry.RemoveAsset(g.TextureHandle);
                    registry.SaveToFile(w.Project() / c.RevisionPaths[1]);
                } },
            { "registry: an extra immutable asset has no receipt (orphan)", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate& c)
                {
                    AssetRegistry registry;
                    registry.LoadFromFile(w.Project() / c.RevisionPaths[1]);
                    AssetMetadata extra = g.RegistryEntries[0];
                    extra.Handle = 12345;
                    extra.SourcePath = GetFabProjectRegistrySourcePath(g.Receipt.StreamId, "mesh/ghost");
                    registry.RegisterAsset(extra);
                    registry.SaveToFile(w.Project() / c.RevisionPaths[1]);
                } },
            { "registry: an asset points at another generation root", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate& c)
                {
                    AssetRegistry registry;
                    registry.LoadFromFile(w.Project() / c.RevisionPaths[1]);
                    registry.CompareAndSwapAssetGeneration(g.MeshHandle, { AssetSourcePolicy::ImmutablePackage, g.CookedRoot },
                        { AssetSourcePolicy::ImmutablePackage, GetFabProjectGenerationRoot(std::string(64, 'e')) });
                    registry.SaveToFile(w.Project() / c.RevisionPaths[1]);
                } },
            { "registry: an asset is demoted to a physical file", false, false,
                [](const Workspace& w, const BuiltGeneration& g, const Candidate& c)
                {
                    AssetRegistry registry;
                    registry.LoadFromFile(w.Project() / c.RevisionPaths[1]);
                    registry.RemoveAsset(g.MeshHandle);
                    AssetMetadata physical;
                    physical.Handle = g.MeshHandle;
                    physical.Type = AssetType::Mesh;
                    physical.SourcePath = "Models/mesh.gltf";
                    physical.Name = "mesh";
                    registry.RegisterAsset(physical);
                    registry.SaveToFile(w.Project() / c.RevisionPaths[1]);
                } },
            { "manifest is truncated", false, false,
                [](const Workspace& w, const BuiltGeneration&, const Candidate&)
                {
                    std::string bytes = ReadAll(w.Manifest());
                    bytes.resize(bytes.size() - 7);
                    WriteAll(w.Manifest(), bytes);
                } },
        };

        for (const Mutation& mutation : mutations)
        {
            const std::string label = mutation.Name;
            Workspace workspace;
            BuiltGeneration generation;
            Candidate candidate;
            if (!workspace.IsReady() || !CreateBaseProject(workspace) || !BuildGeneration(workspace, "alpha", "a", 4096, generation)
                || !BuildCandidate(workspace, generation, "t1", candidate) || !IsCommitted(CommitProjectRevision(candidate.Request).Outcome))
            {
                check(false, label + ": committed fixture builds");
                continue;
            }
            mutation.Apply(workspace, generation, candidate);
            std::string structuralError, fullError;
            const bool structural = ProjectLoads(workspace, FabProjectValidationLevel::Structural, structuralError);
            const bool full = ProjectLoads(workspace, FabProjectValidationLevel::FullHash, fullError);
            check(structural == mutation.StructuralOk, label + ": structural result (" + structuralError + ")");
            check(full == mutation.FullHashOk, label + ": full-hash result (" + fullError + ")");
            if (!structural)
                check(!structuralError.empty(), label + ": a rejection carries a diagnostic");
        }

        // A stale registry after a source replacement: receipts name the new tip, registry the old root.
        {
            Workspace workspace;
            BuiltGeneration alpha, alpha2;
            Candidate first, second;
            check(workspace.IsReady() && CreateBaseProject(workspace) && BuildGeneration(workspace, "alpha", "a", 4096, alpha)
                    && BuildCandidate(workspace, alpha, "t1", first) && IsCommitted(CommitProjectRevision(first.Request).Outcome)
                    && BuildGeneration(workspace, "alpha", "b", 4096, alpha2, "s", alpha.Receipt.GenerationId)
                    && BuildCandidate(workspace, alpha2, "t2", second) && IsCommitted(CommitProjectRevision(second.Request).Outcome),
                "replacement fixture commits");
            std::string error;
            check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error), "replacement fixture validates: " + error);
            WriteAll(workspace.Project() / second.RevisionPaths[1], ReadAll(workspace.Project() / first.RevisionPaths[1]));
            check(!ProjectLoads(workspace, FabProjectValidationLevel::Structural, error),
                "a registry still pointing at the superseded generation is rejected");
        }

        // The read-only orphan report names unreferenced generations and atomic-write temporaries, and deletes nothing.
        {
            Workspace workspace;
            BuiltGeneration alpha, orphan;
            Candidate first;
            check(workspace.IsReady() && CreateBaseProject(workspace) && BuildGeneration(workspace, "alpha", "a", 4096, alpha)
                    && BuildCandidate(workspace, alpha, "t1", first) && IsCommitted(CommitProjectRevision(first.Request).Outcome)
                    && BuildGeneration(workspace, "orphan", "a", 512, orphan),
                "orphan fixture builds");
            std::error_code e;
            fs::create_directories(workspace.Project() / "Assets" / "fab", e);
            fs::copy(orphan.StagedDirectory, workspace.Project() / fs::path(orphan.RelativeRoot), fs::copy_options::recursive, e);
            WriteAll(workspace.Project() / (std::string(".") + kManifestName + ".tmp.123.4"), "partial");
            const Snapshot before = TakeSnapshot(workspace.Project());
            FabProjectState state;
            FabProjectOrphanReport report;
            std::string error;
            check(LoadFabProjectState(workspace.Project(), kManifestName, {}, state, error)
                    && FindFabProjectOrphans(state, workspace.Project(), kManifestName, report, error),
                "orphan scan runs: " + error);
            check(report.UnreferencedGenerations == std::vector<std::string> { orphan.Receipt.GenerationId }
                    && report.TemporaryFiles == std::vector<std::string> { std::string(".") + kManifestName + ".tmp.123.4" },
                "the orphan report names exactly the unreferenced generation and the temporary");
            check(TakeSnapshot(workspace.Project()) == before, "the orphan scan deletes nothing");
            check(ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error), "orphans do not invalidate the project: " + error);
        }
        return check.Passed;
    }

    bool TestFabImmutableMaterialLoad()
    {
        Checker check { true, "Immutable material load test failed" };
        if (!IsProjectCommitSupported())
            return true;

        Workspace workspace;
        BuiltGeneration generation;
        Candidate candidate;
        check(workspace.IsReady() && CreateBaseProject(workspace) && BuildGeneration(workspace, "alpha", "a", 4096, generation)
                && BuildCandidate(workspace, generation, "t1", candidate) && IsCommitted(CommitProjectRevision(candidate.Request).Outcome),
            "committed fixture builds");
        std::string error;
        FabProjectState state;
        check(LoadFabProjectState(workspace.Project(), kManifestName, {}, state, error), "fixture loads: " + error);

        const AssetMetadata* material = state.Registry.GetAsset(generation.MaterialHandle);
        const AssetMetadata* mesh = state.Registry.GetAsset(generation.MeshHandle);
        const AssetMetadata* texture = state.Registry.GetAsset(generation.TextureHandle);
        check(material && mesh && texture, "registry holds all three generation assets");
        if (!material || !mesh || !texture)
            return false;
        check(IsImmutableMaterialAsset(*material) && !IsImmutableMaterialAsset(*mesh) && !IsImmutableMaterialAsset(*texture),
            "only the immutable material is reported as an immutable material");
        AssetMetadata physicalMaterial = *material;
        physicalMaterial.SourcePolicy = AssetSourcePolicy::PhysicalFile;
        check(!IsImmutableMaterialAsset(physicalMaterial), "a physical-file material is not immutable");

        MaterialAsset loaded;
        check(LoadImmutableMaterialAsset(state.Registry, generation.MaterialHandle, loaded, error)
                && loaded.Name == "Fab Material alpha" && loaded.BaseColor.X == 0.5f && loaded.BaseColor.Y == 0.25f
                && loaded.BaseColor.Z == 0.125f && loaded.Metallic == 0.75f && loaded.Roughness == 0.5f
                && loaded.Textures.BaseColor == generation.TextureHandle,
            "the immutable material loads from its generation with its exact values");
        check(GetFabProjectMaterialPath(generation.MaterialHandle, material->CookedRoot, state.Registry.GetCookedArtifactBasePath())
                == (state.Registry.GetCookedArtifactBasePath() / fs::path(material->CookedRoot) / "materials"
                    / (std::to_string(generation.MaterialHandle) + ".spiralmat")).lexically_normal(),
            "the material path follows the generation layout");
        check(GetFabProjectMaterialPath(generation.MaterialHandle, material->CookedRoot, "relative/base").empty()
                && GetFabProjectMaterialPath(generation.MaterialHandle, "", state.Registry.GetCookedArtifactBasePath()).empty()
                && GetFabProjectMaterialPath(kInvalidAssetHandle, material->CookedRoot, state.Registry.GetCookedArtifactBasePath()).empty()
                && GetFabProjectMaterialPath(generation.MaterialHandle, "../x", state.Registry.GetCookedArtifactBasePath()).empty(),
            "a relative base, empty or escaping root, or invalid handle yields no path");

        MaterialAsset sentinel;
        sentinel.Name = "sentinel";
        const auto rejects = [&](const std::string& label, const AssetRegistry& registry, AssetHandle handle)
        {
            MaterialAsset target = sentinel;
            std::string rejectError;
            check(!LoadImmutableMaterialAsset(registry, handle, target, rejectError) && !rejectError.empty() && target.Name == "sentinel",
                label + ": rejected transactionally with a diagnostic");
        };
        rejects("a mesh handle", state.Registry, generation.MeshHandle);
        rejects("an unknown handle", state.Registry, 987654321);
        rejects("the invalid handle", state.Registry, kInvalidAssetHandle);
        AssetRegistry physicalRegistry;
        physicalRegistry.RegisterAsset(AssetType::Material, "Materials/Physical.spiralmat", "Physical");
        rejects("a physical-file material", physicalRegistry, physicalRegistry.GetAssets()[0].Handle);
        AssetRegistry relativeBase = state.Registry;
        relativeBase.SetCookedArtifactBasePath(fs::path("relative"));
        rejects("a registry without an absolute cooked base", relativeBase, generation.MaterialHandle);

        const fs::path file = workspace.Project() / fs::path(generation.RelativeRoot) / fs::path(generation.Receipt.Assets[1].GenerationRelativeCookedPath);
        std::error_code e;
        fs::rename(file, workspace.Base() / "real.spiralmat", e);
        fs::create_symlink(workspace.Base() / "real.spiralmat", file, e);
        rejects("a symlinked material artifact", state.Registry, generation.MaterialHandle);
        fs::remove(file, e);
        rejects("a missing material artifact", state.Registry, generation.MaterialHandle);
        WriteAll(file, "SpiralMaterial 2\nMetallic nan\n");
        rejects("a malformed material artifact", state.Registry, generation.MaterialHandle);
        WriteAll(file, "not a material");
        rejects("garbage in place of a material artifact", state.Registry, generation.MaterialHandle);
        return check.Passed;
    }

    bool MeasureProjectCommitLatency(size_t generationBytes)
    {
        using Clock = std::chrono::steady_clock;
        const auto millisecondsSince = [](Clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };
        Workspace workspace;
        BuiltGeneration generation;
        Candidate candidate;
        const auto buildStart = Clock::now();
        if (!workspace.IsReady() || !CreateBaseProject(workspace)
            || !BuildGeneration(workspace, "synthetic", "a", generationBytes, generation))
            return false;
        const double buildMs = millisecondsSince(buildStart);
        if (!BuildCandidate(workspace, generation, "t1", candidate))
            return false;

        const auto verifyStart = Clock::now();
        std::string error;
        const bool verified = VerifyGenerationDirectory(generation.StagedDirectory, generation.Generation.Artifacts, true, {}, error);
        const double verifyMs = millisecondsSince(verifyStart);

        std::vector<std::pair<ProjectCommitHook, double>> marks;
        const auto commitStart = Clock::now();
        candidate.Request.TestHook = [&](ProjectCommitHook point, std::string_view)
        {
            marks.emplace_back(point, millisecondsSince(commitStart));
            return ProjectCommitHookAction::Continue;
        };
        const ProjectCommitResult result = CommitProjectRevision(candidate.Request);
        const double commitMs = millisecondsSince(commitStart);

        const auto structuralStart = Clock::now();
        const bool structural = ProjectLoads(workspace, FabProjectValidationLevel::Structural, error);
        const double structuralMs = millisecondsSince(structuralStart);
        const auto fullStart = Clock::now();
        const bool full = ProjectLoads(workspace, FabProjectValidationLevel::FullHash, error);
        const double fullMs = millisecondsSince(fullStart);

        const double megabytes = static_cast<double>(generationBytes) / (1024.0 * 1024.0);
        std::cout << "ProjectCommitLatency generationMiB=" << megabytes << " synthesizeMs=" << buildMs
                  << " standaloneHashVerifyMs=" << verifyMs << " commitTotalMs=" << commitMs
                  << " structuralOpenMs=" << structuralMs << " fullHashValidateMs=" << fullMs << '\n';
        for (size_t index = 0; index < marks.size(); ++index)
            if (marks[index].first == ProjectCommitHook::GenerationVerified || marks[index].first == ProjectCommitHook::GenerationPublished
                || marks[index].first == ProjectCommitHook::BeforePointer || marks[index].first == ProjectCommitHook::AfterPointer
                || marks[index].first == ProjectCommitHook::BaseVerified)
                std::cout << "  hook " << static_cast<int>(marks[index].first) << " at " << marks[index].second << " ms\n";
        return verified && IsCommitted(result.Outcome) && structural && full;
    }
}
