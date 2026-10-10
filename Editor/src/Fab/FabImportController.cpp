#include "FabImportController.h"

#include "Engine/Assets/FabProjectState.h"
#include "Engine/Assets/FabZipStaging.h"
#include "Engine/Core/Sha256.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Scene/Scene.h"
#include "FabLicenseGate.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace Fab
{
    using Engine::u32;
    using Engine::u64;
    using Engine::u8;

    namespace fs = std::filesystem;

    namespace
    {
        enum class PhaseState : int
        {
            Idle,
            Running,
            Finished
        };

        enum class PhaseOutcome
        {
            Succeeded,
            Failed,
            Cancelled
        };

        struct PhaseResult
        {
            PhaseOutcome Outcome = PhaseOutcome::Failed;
            FabImportError Error = FabImportError::Internal;
            std::string Message;
        };

        PhaseResult Succeeded()
        {
            return { PhaseOutcome::Succeeded, FabImportError::None, {} };
        }

        PhaseResult Failed(FabImportError error, std::string message)
        {
            return { PhaseOutcome::Failed, error, std::move(message) };
        }

        PhaseResult Cancelled()
        {
            return { PhaseOutcome::Cancelled, FabImportError::None, "cancelled" };
        }

        constexpr std::string_view kPreviewListing = "https://www.fab.com/listings/preview";

        std::string NowUtc()
        {
            const std::time_t now = std::time(nullptr);
            std::tm utc {};
#if defined(_WIN32)
            gmtime_s(&utc, &now);
#else
            gmtime_r(&now, &utc);
#endif
            char buffer[32] {};
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
            return buffer;
        }

        const char* IntakeReasonText(FabIntakeReason reason)
        {
            switch (reason)
            {
                case FabIntakeReason::None: return "the content does not match the submitted kind";
                case FabIntakeReason::Missing: return "the path does not exist";
                case FabIntakeReason::NotRegularObject: return "the path is not a regular file or directory";
                case FabIntakeReason::Empty: return "the file is empty";
                case FabIntakeReason::TooLarge: return "the file is larger than the intake limit";
                case FabIntakeReason::ExtensionContentMismatch: return "the file extension contradicts its content";
                case FabIntakeReason::UnrecognizedContent: return "the content is not a ZIP, GLB, or glTF package";
                case FabIntakeReason::IncompleteZip: return "the ZIP archive is incomplete";
                case FabIntakeReason::FolderWithoutGltf: return "the folder contains no glTF or GLB";
                case FabIntakeReason::FolderTooLarge: return "the folder is larger than the intake limit";
                case FabIntakeReason::TooManyPaths: return "too many paths were submitted";
                case FabIntakeReason::IoError: return "the path could not be read";
            }
            return "the path was rejected";
        }

        std::string DisplayLeaf(const fs::path& path)
        {
            std::string name = path.filename().string();
            for (char& character : name)
                if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f)
                    character = '?';
            if (name.size() > 96)
                name.resize(96);
            return name;
        }

        Engine::FabImportReceipt MakeDeclaration(const FabProvenance& provenance, Engine::FabPackageFormat format,
            const std::string& sourceSha256, const std::string& acquiredAt)
        {
            Engine::FabImportReceipt receipt;
            receipt.ProductIdentity = provenance.ProductIdentity;
            receipt.ProductName = provenance.ProductName;
            receipt.Publisher = provenance.Publisher;
            receipt.VersionOrDownloadLabel = provenance.VersionOrDownloadLabel;
            receipt.PackageFormat = format;
            receipt.LicenseFamily = provenance.LicenseFamily;
            receipt.LicenseTier = provenance.LicenseTier;
            receipt.AttributionText = provenance.AttributionText;
            receipt.AttributionLink = provenance.AttributionLink;
            receipt.MetadataConfirmedByUser = true;
            receipt.NoAI = provenance.NoAI;
            receipt.GeneratedWithAI = provenance.GeneratedWithAI;
            receipt.SourceDigestKind = Engine::FabDigestKind::Sha256;
            receipt.SourceSha256 = sourceSha256;
            receipt.DiagnosticAcquiredAtUtc = acquiredAt;
            receipt.RawSourcePolicy = provenance.RawSourcePolicy;
            receipt.Relation = Engine::FabGenerationRelation::Initial;
            return receipt;
        }

        FabProvenance PreviewProvenance()
        {
            FabProvenance provenance;
            provenance.ProductIdentity = std::string(kPreviewListing);
            provenance.ProductName = "Preview";
            provenance.Publisher = "Preview";
            provenance.VersionOrDownloadLabel = "preview";
            provenance.LicenseFamily = Engine::FabLicenseFamily::FabStandard;
            provenance.LicenseTier = Engine::FabLicenseTier::Personal;
            return provenance;
        }

        std::string ReadFileBytes(const fs::path& path)
        {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }

        bool WriteFileBytes(const fs::path& path, std::string_view bytes)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(output);
        }

        std::string RegistryDirectoryOf(const Engine::ProjectManifest& manifest)
        {
            const size_t slash = manifest.AssetRegistryPath.rfind('/');
            return slash == std::string::npos ? std::string() : manifest.AssetRegistryPath.substr(0, slash);
        }

        std::string JoinProjectPath(std::string_view directory, std::string_view name)
        {
            return directory.empty() ? std::string(name) : std::string(directory) + "/" + std::string(name);
        }

        // "dir/Name.r3-ab12.ext" -> "dir/Name.r<revision>-<token>.ext"; an unrevised
        // name gains the suffix. The new name is unique per transaction, as
        // CommitProjectRevision requires for exclusively created files.
        std::string RevisionPath(std::string_view previous, u64 revision, std::string_view token)
        {
            const size_t slash = previous.rfind('/');
            const std::string_view directory = slash == std::string_view::npos ? std::string_view() : previous.substr(0, slash + 1);
            std::string_view name = slash == std::string_view::npos ? previous : previous.substr(slash + 1);
            std::string_view extension;
            const size_t dot = name.rfind('.');
            if (dot != std::string_view::npos && dot > 0)
            {
                extension = name.substr(dot);
                name = name.substr(0, dot);
            }
            const size_t marker = name.rfind(".r");
            if (marker != std::string_view::npos)
            {
                const std::string_view tail = name.substr(marker + 2);
                const size_t dash = tail.find('-');
                const auto isDigits = [](std::string_view text)
                {
                    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
                };
                const auto isToken = [](std::string_view text)
                {
                    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
                        { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
                };
                if (dash != std::string_view::npos && isDigits(tail.substr(0, dash)) && isToken(tail.substr(dash + 1)))
                    name = name.substr(0, marker);
            }
            return std::string(directory) + std::string(name) + ".r" + std::to_string(revision) + "-" + std::string(token)
                + std::string(extension);
        }

        std::string TransactionToken(u64 jobId, std::string_view generationId)
        {
            static std::atomic<u64> sequence { 0 };
            const u64 ticks = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
            return Engine::Sha256Builder::HashString(std::to_string(jobId) + ":" + std::string(generationId) + ":"
                       + std::to_string(ticks) + ":" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)))
                .substr(0, 12);
        }

        // Project facts derived once from a context, for cooking and for committing.
        struct ResolvedContext
        {
            Engine::ProjectManifest Manifest;
            fs::path FinalCookedBase;
            std::string RegistryDirectory;  // project-relative
            fs::path CookStagingRoot;
            bool DefaultCookStagingRoot = false;
        };

        bool ResolveContext(const FabImportProjectContext& context, ResolvedContext& out, std::string& error)
        {
            std::error_code filesystemError;
            if (context.ProjectRoot.empty() || !fs::is_directory(context.ProjectRoot, filesystemError))
            {
                error = "the project root is not an existing directory";
                return false;
            }
            if (!Engine::IsPortableProjectRelativePath(context.ManifestRelativePath))
            {
                error = "the manifest path is not a strict project-relative path";
                return false;
            }
            ResolvedContext resolved;
            if (!Engine::DeserializeProjectManifest(context.ManifestBytes, resolved.Manifest, error))
            {
                error = "the supplied manifest bytes are invalid: " + error;
                return false;
            }
            if (!Engine::ValidateFabReceiptCollection(context.Receipts, error))
            {
                error = "the supplied receipt collection is invalid: " + error;
                return false;
            }
            resolved.RegistryDirectory = RegistryDirectoryOf(resolved.Manifest);
            Engine::AssetRegistry probe;
            const fs::path base = resolved.RegistryDirectory.empty() ? context.ProjectRoot
                                                                     : context.ProjectRoot / resolved.RegistryDirectory;
            if (!probe.SetCookedArtifactBasePath(base))
            {
                error = "the registry directory is not a valid cooked artifact base";
                return false;
            }
            resolved.FinalCookedBase = probe.GetCookedArtifactBasePath();
            if (!context.Registry.GetCookedArtifactBasePath().empty()
                && context.Registry.GetCookedArtifactBasePath() != resolved.FinalCookedBase)
            {
                error = "the supplied registry's cooked artifact base is not the manifest registry's directory";
                return false;
            }
            resolved.DefaultCookStagingRoot = context.CookStagingRoot.empty();
            resolved.CookStagingRoot = resolved.DefaultCookStagingRoot ? context.ProjectRoot / ".fab-staging"
                                                                       : context.CookStagingRoot;
            out = std::move(resolved);
            return true;
        }
    }

    const char* ToString(FabImportState state)
    {
        switch (state)
        {
            case FabImportState::Idle: return "Idle";
            case FabImportState::Snapshotting: return "Snapshotting";
            case FabImportState::Preparing: return "Preparing";
            case FabImportState::AwaitingProvenance: return "AwaitingProvenance";
            case FabImportState::Cooking: return "Cooking";
            case FabImportState::ReadyToCommit: return "ReadyToCommit";
            case FabImportState::Committing: return "Committing";
            case FabImportState::Done: return "Done";
            case FabImportState::Failed: return "Failed";
            case FabImportState::Cancelled: return "Cancelled";
            case FabImportState::FailedAfterCommit: return "FailedAfterCommit";
        }
        return "Unknown";
    }

    bool IsTerminal(FabImportState state)
    {
        return state == FabImportState::Done || state == FabImportState::Failed || state == FabImportState::Cancelled
            || state == FabImportState::FailedAfterCommit;
    }

    const char* ToString(FabImportError error)
    {
        switch (error)
        {
            case FabImportError::None: return "None";
            case FabImportError::InvalidIntake: return "InvalidIntake";
            case FabImportError::Unsupported: return "Unsupported";
            case FabImportError::SnapshotRejected: return "SnapshotRejected";
            case FabImportError::ArchiveRejected: return "ArchiveRejected";
            case FabImportError::PackageRejected: return "PackageRejected";
            case FabImportError::ProvenanceInvalid: return "ProvenanceInvalid";
            case FabImportError::Conflict: return "Conflict";
            case FabImportError::StagingFailed: return "StagingFailed";
            case FabImportError::ProjectContextInvalid: return "ProjectContextInvalid";
            case FabImportError::StaleBase: return "StaleBase";
            case FabImportError::AssignmentRejected: return "AssignmentRejected";
            case FabImportError::CommitFailed: return "CommitFailed";
            case FabImportError::Internal: return "Internal";
        }
        return "Unknown";
    }

    bool ValidateFabProvenance(const FabProvenance& provenance, std::string& error)
    {
        if (provenance.LicenseFamily == Engine::FabLicenseFamily::Unknown)
        {
            error = "choose the license family shown on the Fab listing";
            return false;
        }
        if (provenance.LicenseFamily == Engine::FabLicenseFamily::FabStandard
            && provenance.LicenseTier != Engine::FabLicenseTier::Personal
            && provenance.LicenseTier != Engine::FabLicenseTier::Professional)
        {
            error = "choose the Personal or Professional tier you acquired the asset under";
            return false;
        }
        Engine::FabImportReceipt probe = MakeDeclaration(provenance, Engine::FabPackageFormat::Glb,
            std::string(64, '0'), "2026-01-01T00:00:00Z");
        probe.ExpandedTreeSha256 = std::string(64, '0');
        probe.ImporterVersion = "validation";
        probe.CookerVersion = "validation";
        probe.StreamId = Engine::ComputeFabStreamId(probe.ProductIdentity, probe.VersionOrDownloadLabel, probe.PackageFormat);
        probe.GenerationId = Engine::ComputeFabGenerationId(probe.StreamId, probe.SourceSha256, probe.ExpandedTreeSha256);
        Engine::FabImportedAssetRecord record;
        record.Type = Engine::AssetType::Mesh;
        record.SemanticRole = "mesh.main";
        record.Handle = Engine::ComputeFabStableAssetHandle(probe.StreamId, record.Type, record.SemanticRole);
        record.LogicalPath = "mesh/main";
        record.GenerationRelativeCookedPath = "meshes/x";
        record.ArtifactSha256 = std::string(64, '0');
        probe.Assets.push_back(std::move(record));
        return Engine::ValidateFabImportReceipt(probe, error);
    }

    std::string ComputeFabProvenanceDigest(const FabProvenance& provenance)
    {
        std::string canonical = "SpiralFabProvenanceV1\n";
        const auto add = [&canonical](std::string_view value)
        {
            canonical += std::to_string(value.size());
            canonical += ':';
            canonical += value;
            canonical += '\n';
        };
        add(provenance.ProductIdentity);
        add(provenance.ProductName);
        add(provenance.Publisher);
        add(provenance.VersionOrDownloadLabel);
        add(Engine::ToString(provenance.LicenseFamily));
        add(Engine::ToString(provenance.LicenseTier));
        add(provenance.AttributionText);
        add(provenance.AttributionLink);
        add(Engine::ToString(provenance.NoAI));
        add(Engine::ToString(provenance.GeneratedWithAI));
        add(Engine::ToString(provenance.RawSourcePolicy));
        return Engine::Sha256Builder::HashString(canonical);
    }

    fs::path DefaultFabPackageStagingRoot()
    {
        const char* cache = std::getenv("XDG_CACHE_HOME");
        if (cache && *cache && fs::path(cache).is_absolute())
            return fs::path(cache) / "Spiral" / "FabStaging";
        const char* home = std::getenv("HOME");
        if (home && *home && fs::path(home).is_absolute())
            return fs::path(home) / ".cache" / "Spiral" / "FabStaging";
        return {};
    }

    // Shared between the controller (main thread) and at most one worker. The
    // worker owns the staging members while a phase runs; the main thread owns
    // them while it does not. The phase flag's release/acquire pair is the hand-off.
    struct FabImportJob
    {
        u64 Id = 0;
        FabImportControllerConfig Config;
        FabIntakeRequest Request;
        std::shared_ptr<std::atomic<bool>> Cancel;

        std::atomic<int> Stage { static_cast<int>(FabImportState::Snapshotting) };
        std::atomic<u64> FilesCompleted { 0 };
        std::atomic<u64> FileCount { 0 };
        std::atomic<u64> BytesCompleted { 0 };
        std::atomic<u64> BytesTotal { 0 };

        std::atomic<PhaseState> Phase { PhaseState::Idle };
        std::mutex Mutex;
        std::condition_variable PhaseFinished;
        PhaseResult Result;

        // Snapshot phase products.
        Engine::FabStagingDirectory WorkDirectory;
        // The snapshot's own staging lives inside this directory, so it is declared
        // between the two and therefore destroyed after the snapshot.
        Engine::FabStagingDirectory SnapshotParent;
        std::unique_ptr<Engine::LocalPackageSnapshot> Snapshot;
        std::string SourceSha256;
        std::string TreeSha256;
        Engine::FabPackageFormat Format = Engine::FabPackageFormat::Unknown;
        FabImportSummary Summary;

        // Cook phase inputs (set by the main thread before the phase starts).
        Engine::FabImportReceipt Declaration;
        Engine::AssetRegistry BaseRegistry;
        Engine::FabReceiptCollection BaseReceipts;
        fs::path FinalCookedBase;
        fs::path CookStagingRoot;
        bool DefaultCookStagingRoot = false;

        // Cook phase products.
        Engine::FabGltfPreparedPackage Prepared;
        Engine::FabGltfStagedGeneration Staged;
        Engine::FabStagingDirectory CookDirectory;
        std::string StreamId;
        std::string GenerationId;
        bool HasDecision = false;
        Engine::FabReceiptDecisionKind Decision = Engine::FabReceiptDecisionKind::InvalidCandidate;
        std::string DecisionDiagnostic;

        bool IsCancelled() const { return Cancel->load(std::memory_order_acquire); }

        void Hook(FabImportHookPoint point) const
        {
            if (Config.TestHook)
                Config.TestHook(point);
        }
    };

    namespace
    {
        void Finish(FabImportJob& job, PhaseResult result)
        {
            {
                std::scoped_lock lock(job.Mutex);
                job.Result = std::move(result);
                job.Phase.store(PhaseState::Finished, std::memory_order_release);
            }
            job.PhaseFinished.notify_all();
        }

        PhaseResult ClassifyFailure(FabImportJob& job, FabImportError error, std::string message)
        {
            if (job.IsCancelled())
                return Cancelled();
            return Failed(error, std::move(message));
        }

        PhaseResult SnapshotAndPreview(FabImportJob& job)
        {
            job.Hook(FabImportHookPoint::PhaseStarted);
            if (job.IsCancelled())
                return Cancelled();
            if (!Engine::IsFabStagingSupported())
                return Failed(FabImportError::Unsupported, "Fab package staging is not implemented on this platform");

            const fs::path root = job.Config.PackageStagingRoot.empty() ? DefaultFabPackageStagingRoot()
                                                                        : job.Config.PackageStagingRoot;
            std::string error;
            if (root.empty() || !Engine::PrepareFabStagingRoot(root, error))
                return Failed(FabImportError::StagingFailed, "package staging root: " + error);
            if (!Engine::FabStagingDirectory::Create(root, "import", job.WorkDirectory, error))
                return Failed(FabImportError::StagingFailed, error);

            const FabIntakeClassification classification = ClassifyFabIntakePath(job.Request.Path, job.Config.IntakeLimits);
            if (classification.Reason != FabIntakeReason::None || classification.Kind != job.Request.Kind)
                return Failed(FabImportError::InvalidIntake,
                    std::string("the selected package was rejected at intake: ") + IntakeReasonText(classification.Reason));

            if (!job.WorkDirectory.CreateChild("snapshot", job.SnapshotParent, error))
                return Failed(FabImportError::StagingFailed, error);

            const auto isCancelled = [&job]() { return job.IsCancelled(); };
            fs::path sourceDirectory;
            Engine::FabStagingDirectory intermediateDirectory;
            if (job.Request.Kind == FabIntakeKind::Folder)
                sourceDirectory = job.Request.Path;
            else if (job.Request.Kind == FabIntakeKind::Zip)
            {
                Engine::FabStagingDirectory archiveDirectory;
                if (!job.WorkDirectory.CreateChild("archive", archiveDirectory, error))
                    return Failed(FabImportError::StagingFailed, error);
                Engine::FabStagedCopy copy;
                const u64 maximum = std::min(job.Config.IntakeLimits.MaximumFileBytes,
                    job.Config.ArchiveLimits.MaximumCompressedBytes);
                job.BytesTotal.store(classification.SizeBytes, std::memory_order_relaxed);
                if (!Engine::CopyRegularFileIntoStaging(job.Request.Path, archiveDirectory, "source.zip", maximum,
                        isCancelled, copy, error))
                    return ClassifyFailure(job, FabImportError::InvalidIntake, "ZIP intake: " + error);
                job.BytesCompleted.store(copy.Bytes, std::memory_order_relaxed);
                job.SourceSha256 = copy.Sha256;
                job.Hook(FabImportHookPoint::ArchiveCopied);
                if (job.IsCancelled())
                    return Cancelled();

                if (!job.WorkDirectory.CreateChild("extract", intermediateDirectory, error))
                    return Failed(FabImportError::StagingFailed, error);
                u64 extractedFiles = 0;
                u64 extractedBytes = 0;
                if (!Engine::ExtractFabZipToStaging(
                        Engine::FabArchiveInput::FromFile(archiveDirectory.GetPath() / "source.zip"),
                        job.Config.ArchiveLimits, intermediateDirectory, isCancelled, &extractedFiles, &extractedBytes, error))
                    return ClassifyFailure(job, FabImportError::ArchiveRejected, error);
                if (!job.WorkDirectory.RemoveChild("archive", error))
                    return Failed(FabImportError::StagingFailed, error);
                job.Hook(FabImportHookPoint::ArchiveExtracted);
                if (job.IsCancelled())
                    return Cancelled();
                sourceDirectory = intermediateDirectory.GetPath();
            }
            else if (job.Request.Kind == FabIntakeKind::Glb || job.Request.Kind == FabIntakeKind::Gltf)
            {
                if (!job.WorkDirectory.CreateChild("package", intermediateDirectory, error))
                    return Failed(FabImportError::StagingFailed, error);
                Engine::FabStagedCopy copy;
                job.BytesTotal.store(classification.SizeBytes, std::memory_order_relaxed);
                if (!Engine::CopyRegularFileIntoStaging(job.Request.Path, intermediateDirectory,
                        job.Request.Kind == FabIntakeKind::Glb ? "model.glb" : "model.gltf",
                        job.Config.IntakeLimits.MaximumFileBytes, isCancelled, copy, error))
                    return ClassifyFailure(job, FabImportError::InvalidIntake, "file intake: " + error);
                job.BytesCompleted.store(copy.Bytes, std::memory_order_relaxed);
                sourceDirectory = intermediateDirectory.GetPath();
            }
            else
                return Failed(FabImportError::InvalidIntake, "the selected path is not a supported package kind");

            Engine::LocalPackageSnapshotOptions options;
            options.Limits = job.Config.SnapshotLimits;
            options.IsCancelled = isCancelled;
            options.Progress = [&job](const Engine::LocalPackageSnapshotProgress& progress)
            {
                job.FilesCompleted.store(progress.FilesCompleted, std::memory_order_relaxed);
                job.FileCount.store(progress.FileCount, std::memory_order_relaxed);
                job.BytesCompleted.store(progress.BytesCompleted, std::memory_order_relaxed);
                job.BytesTotal.store(progress.AggregateBytes, std::memory_order_relaxed);
            };
            options.TestHook = job.Config.SnapshotTestHook;
            auto snapshot = std::make_unique<Engine::LocalPackageSnapshot>();
            if (!Engine::LocalPackageSnapshot::Create(sourceDirectory, job.SnapshotParent.GetPath(), options, *snapshot, error))
                return ClassifyFailure(job, FabImportError::SnapshotRejected,
                    job.Request.Kind == FabIntakeKind::Gltf
                        ? "a loose .gltf cannot carry its buffers and textures; select the package folder or ZIP instead: " + error
                        : error);
            job.Snapshot = std::move(snapshot);
            // The extraction/package copy is no longer needed. A leftover only costs disk
            // until job cleanup removes the whole work directory.
            intermediateDirectory.Remove(error);
            job.Hook(FabImportHookPoint::SnapshotCreated);
            if (job.IsCancelled())
                return Cancelled();

            job.TreeSha256 = job.Snapshot->GetTreeSha256();
            if (job.Request.Kind != FabIntakeKind::Zip)
                job.SourceSha256 = job.TreeSha256;
            std::string extension = fs::path(job.Snapshot->GetRootRelativePath()).extension().string();
            for (char& character : extension)
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            job.Format = extension == ".glb" ? Engine::FabPackageFormat::Glb
                : extension == ".gltf"       ? Engine::FabPackageFormat::Gltf
                                             : Engine::FabPackageFormat::Unknown;
            if (job.Format == Engine::FabPackageFormat::Unknown)
                return Failed(FabImportError::PackageRejected, "the package root is neither a .glb nor a .gltf");

            job.Stage.store(static_cast<int>(FabImportState::Preparing), std::memory_order_release);
            Engine::FabImportReceipt preview = MakeDeclaration(PreviewProvenance(), job.Format, job.SourceSha256, NowUtc());
            Engine::FabGltfPrepareOptions prepareOptions;
            prepareOptions.Limits = job.Config.GltfLimits;
            prepareOptions.ImageLimits = job.Config.ImageLimits;
            prepareOptions.IsCancelled = isCancelled;
            prepareOptions.TestHook = job.Config.CookTestHook;
            Engine::FabGltfPreparedPackage prepared;
            if (!Engine::PrepareFabGltfPackage(*job.Snapshot, preview, prepareOptions, prepared, error))
                return ClassifyFailure(job, FabImportError::PackageRejected, error);

            FabImportSummary summary;
            summary.SourceFileCount = job.Snapshot->GetEntries().size();
            for (const Engine::LocalPackageSnapshotEntry& entry : job.Snapshot->GetEntries())
                summary.SourceBytes += entry.SizeBytes;
            summary.VertexCount = prepared.VertexCount;
            summary.TriangleCount = prepared.TriangleCount;
            summary.PrimitiveInstanceCount = prepared.PrimitiveInstanceCount;
            summary.MaterialName = prepared.Material.Name;
            for (const Engine::TextureArtifact& texture : prepared.Textures)
                summary.Textures.push_back({ Engine::ToString(texture.Role),
                    texture.Mips.empty() ? 0u : texture.Mips.front().Width,
                    texture.Mips.empty() ? 0u : texture.Mips.front().Height });
            job.Summary = std::move(summary);
            job.Hook(FabImportHookPoint::PreviewPrepared);
            if (job.IsCancelled())
                return Cancelled();
            return Succeeded();
        }

        PhaseResult Cook(FabImportJob& job)
        {
            job.Hook(FabImportHookPoint::PhaseStarted);
            if (job.IsCancelled())
                return Cancelled();
            const auto isCancelled = [&job]() { return job.IsCancelled(); };
            std::string error;

            Engine::FabGltfPrepareOptions prepareOptions;
            prepareOptions.Limits = job.Config.GltfLimits;
            prepareOptions.ImageLimits = job.Config.ImageLimits;
            prepareOptions.IsCancelled = isCancelled;
            prepareOptions.TestHook = job.Config.CookTestHook;
            if (!Engine::PrepareFabGltfPackage(*job.Snapshot, job.Declaration, prepareOptions, job.Prepared, error))
                return ClassifyFailure(job, FabImportError::PackageRejected, error);
            job.Hook(FabImportHookPoint::CookPrepared);
            if (job.IsCancelled())
                return Cancelled();

            // The raw package bytes are not needed again; release them before staging.
            job.Snapshot.reset();
            if (!job.SnapshotParent.Remove(error) || !job.WorkDirectory.Remove(error))
                return Failed(FabImportError::StagingFailed, error);

            if (!Engine::PrepareFabStagingRoot(job.CookStagingRoot, error)
                || !Engine::FabStagingDirectory::Create(job.CookStagingRoot, "cook", job.CookDirectory, error))
                return Failed(FabImportError::StagingFailed, "cook staging: " + error);

            Engine::FabGltfStageOptions stageOptions;
            stageOptions.IsCancelled = isCancelled;
            stageOptions.TestHook = job.Config.CookTestHook;
            if (!Engine::StageFabGltfGeneration(job.Prepared, job.CookDirectory.GetPath(), stageOptions, job.Staged, error))
                return ClassifyFailure(job, FabImportError::StagingFailed, error);
            // StageFabGltfGeneration always creates the three artifact directories, but the
            // project commit requires the generation to hold exactly its receipt artifacts,
            // so a package without textures must not publish an empty textures directory.
            for (const char* name : { "meshes", "textures", "materials" })
            {
                std::error_code pruneError;
                const fs::path directory = job.CookDirectory.GetPath() / job.Staged.CookedRoot / name;
                if (fs::is_directory(fs::symlink_status(directory, pruneError)) && fs::is_empty(directory, pruneError))
                    fs::remove(directory, pruneError);
            }
            job.Hook(FabImportHookPoint::CookStaged);
            if (job.IsCancelled())
                return Cancelled();

            job.StreamId = job.Prepared.ReceiptDraft.StreamId;
            job.GenerationId = job.Prepared.ReceiptDraft.GenerationId;
            Engine::FabGltfCandidate candidate;
            if (!Engine::BuildFabGltfCandidate(job.BaseRegistry, job.BaseReceipts, job.Prepared, job.Staged,
                    job.FinalCookedBase, candidate, error))
            {
                job.HasDecision = candidate.Decision.Kind != Engine::FabReceiptDecisionKind::InvalidCandidate
                    || !candidate.Decision.Diagnostic.empty();
                job.Decision = candidate.Decision.Kind;
                job.DecisionDiagnostic = candidate.Decision.Diagnostic;
                if (candidate.Decision.Kind == Engine::FabReceiptDecisionKind::Conflict)
                    return Failed(FabImportError::Conflict, error);
                if (candidate.Decision.Kind == Engine::FabReceiptDecisionKind::CorruptPriorState)
                    return Failed(FabImportError::ProjectContextInvalid, error);
                return ClassifyFailure(job, FabImportError::PackageRejected, error);
            }
            job.HasDecision = true;
            job.Decision = candidate.Decision.Kind;
            job.DecisionDiagnostic = candidate.Decision.Diagnostic;
            job.Hook(FabImportHookPoint::CandidateBuilt);
            if (job.IsCancelled())
                return Cancelled();
            return Succeeded();
        }

        void RunSnapshotPhase(FabImportJob& job)
        {
            try
            {
                Finish(job, SnapshotAndPreview(job));
            }
            catch (...)
            {
                Finish(job, Failed(FabImportError::Internal, "the import worker failed on an exception"));
            }
        }

        void RunCookPhase(FabImportJob& job)
        {
            try
            {
                Finish(job, Cook(job));
            }
            catch (...)
            {
                Finish(job, Failed(FabImportError::Internal, "the import worker failed on an exception"));
            }
        }

        FabImportError MapCommitError(Engine::ProjectCommitError error)
        {
            switch (error)
            {
                case Engine::ProjectCommitError::Unsupported: return FabImportError::Unsupported;
                case Engine::ProjectCommitError::BaseChanged:
                case Engine::ProjectCommitError::LockUnavailable: return FabImportError::StaleBase;
                case Engine::ProjectCommitError::InvalidRequest:
                case Engine::ProjectCommitError::InvalidManifest:
                case Engine::ProjectCommitError::PathEscape: return FabImportError::ProjectContextInvalid;
                default: return FabImportError::CommitFailed;
            }
        }
    }

    FabImportController::FabImportController(FabImportControllerConfig config)
        : m_Config(std::move(config))
    {
    }

    FabImportController::~FabImportController()
    {
        Shutdown();
    }

    bool FabImportController::Reject(std::string message)
    {
        m_Status.LastRejection = std::move(message);
        return false;
    }

    bool FabImportController::IsBusy() const
    {
        return m_Job && m_Job->Phase.load(std::memory_order_acquire) != PhaseState::Idle;
    }

    void FabImportController::RequestCancel()
    {
        std::scoped_lock lock(m_CancelMutex);
        if (m_CancelFlag)
            m_CancelFlag->store(true, std::memory_order_release);
    }

    void FabImportController::StartPhase(void (*phase)(FabImportJob&), FabImportState state)
    {
        m_State = state;
        m_Job->Stage.store(static_cast<int>(state), std::memory_order_release);
        m_Job->FilesCompleted.store(0, std::memory_order_relaxed);
        m_Job->FileCount.store(0, std::memory_order_relaxed);
        m_Job->BytesCompleted.store(0, std::memory_order_relaxed);
        m_Job->BytesTotal.store(0, std::memory_order_relaxed);
        m_Job->Phase.store(PhaseState::Running, std::memory_order_release);
        // The worker captures the job record only, never the controller.
        Engine::JobSystem::Get().Submit([job = m_Job, phase]() { phase(*job); }, "FabImport");
    }

    bool FabImportController::Submit(const FabIntakeRequest& request)
    {
        m_Status.LastRejection.clear();
        if (m_State != FabImportState::Idle)
            return Reject(IsTerminal(m_State) ? "dismiss the finished import first" : "an import is already in progress");
        if (request.Path.empty() || request.Kind == FabIntakeKind::Unsupported)
            return Reject("the selected path is not a supported Fab package kind");

        auto job = std::make_shared<FabImportJob>();
        job->Id = m_NextJobId++;
        job->Config = m_Config;
        job->Request = request;
        {
            std::scoped_lock lock(m_CancelMutex);
            m_CancelFlag = std::make_shared<std::atomic<bool>>(false);
            job->Cancel = m_CancelFlag;
        }
        m_Job = std::move(job);
        m_CommitResult.reset();
        m_Status = {};
        m_Status.JobId = m_Job->Id;
        m_Status.SourceKind = request.Kind;
        m_Status.SourceOrigin = request.Origin;
        m_Status.SourceName = DisplayLeaf(request.Path);
        StartPhase(&RunSnapshotPhase, FabImportState::Snapshotting);
        return true;
    }

    void FabImportController::EnterTerminal(FabImportState state, FabImportError error, std::string message)
    {
        m_State = state;
        m_Status.Error = error;
        m_Status.Message = std::move(message);
    }

    void FabImportController::ReleaseJobResources()
    {
        if (!m_Job)
            return;
        std::string ignored;
        m_Job->Snapshot.reset();
        m_Job->SnapshotParent.Remove(ignored);
        m_Job->WorkDirectory.Remove(ignored);
        m_Job->CookDirectory.Remove(ignored);
        if (m_Job->DefaultCookStagingRoot && !m_Job->CookStagingRoot.empty())
        {
            std::error_code filesystemError;
            if (fs::is_directory(fs::symlink_status(m_Job->CookStagingRoot, filesystemError)))
                fs::remove(m_Job->CookStagingRoot, filesystemError);  // removes only an empty directory
        }
    }

    void FabImportController::PublishSnapshotPhase()
    {
        FabImportJob& job = *m_Job;
        PhaseResult result;
        {
            std::scoped_lock lock(job.Mutex);
            result = job.Result;
        }
        job.Phase.store(PhaseState::Idle, std::memory_order_release);
        if (job.IsCancelled() || result.Outcome == PhaseOutcome::Cancelled)
        {
            ReleaseJobResources();
            EnterTerminal(FabImportState::Cancelled, FabImportError::None, "the import was cancelled");
            return;
        }
        if (result.Outcome == PhaseOutcome::Failed)
        {
            ReleaseJobResources();
            EnterTerminal(FabImportState::Failed, result.Error, std::move(result.Message));
            return;
        }
        m_Status.Format = job.Format;
        m_Status.SourceSha256 = job.SourceSha256;
        m_Status.ExpandedTreeSha256 = job.TreeSha256;
        m_Status.Summary = job.Summary;
        m_Status.Provenance = {};
        m_Status.ProvenanceValid = false;
        ValidateFabProvenance(m_Status.Provenance, m_Status.ProvenanceError);
        m_Status.ProvenanceDigest = ComputeFabProvenanceDigest(m_Status.Provenance);
        m_State = FabImportState::AwaitingProvenance;
    }

    void FabImportController::PublishCookPhase()
    {
        FabImportJob& job = *m_Job;
        PhaseResult result;
        {
            std::scoped_lock lock(job.Mutex);
            result = job.Result;
        }
        job.Phase.store(PhaseState::Idle, std::memory_order_release);
        m_Status.StreamId = job.StreamId;
        m_Status.GenerationId = job.GenerationId;
        m_Status.HasDecision = job.HasDecision;
        m_Status.Decision = job.Decision;
        m_Status.DecisionDiagnostic = job.DecisionDiagnostic;
        if (job.IsCancelled() || result.Outcome == PhaseOutcome::Cancelled)
        {
            ReleaseJobResources();
            EnterTerminal(FabImportState::Cancelled, FabImportError::None, "the import was cancelled");
            return;
        }
        if (result.Outcome == PhaseOutcome::Failed)
        {
            ReleaseJobResources();
            EnterTerminal(FabImportState::Failed, result.Error, std::move(result.Message));
            return;
        }
        m_State = FabImportState::ReadyToCommit;
    }

    void FabImportController::Update()
    {
        if (!m_Job)
            return;
        const PhaseState phase = m_Job->Phase.load(std::memory_order_acquire);
        switch (m_State)
        {
            case FabImportState::Snapshotting:
            case FabImportState::Preparing:
                if (phase == PhaseState::Finished)
                    PublishSnapshotPhase();
                else
                    m_State = static_cast<FabImportState>(m_Job->Stage.load(std::memory_order_acquire));
                break;
            case FabImportState::Cooking:
                if (phase == PhaseState::Finished)
                    PublishCookPhase();
                break;
            case FabImportState::AwaitingProvenance:
            case FabImportState::ReadyToCommit:
                if (m_Job->IsCancelled())
                {
                    ReleaseJobResources();
                    EnterTerminal(FabImportState::Cancelled, FabImportError::None, "the import was cancelled");
                }
                break;
            default:
                break;
        }
    }

    bool FabImportController::SetProvenance(const FabProvenance& provenance)
    {
        m_Status.LastRejection.clear();
        if (m_State != FabImportState::AwaitingProvenance)
            return Reject("provenance can only be edited while awaiting provenance");
        m_Status.Provenance = provenance;
        if (m_Status.Provenance.LicenseFamily != Engine::FabLicenseFamily::FabStandard
            && m_Status.Provenance.LicenseFamily != Engine::FabLicenseFamily::Unknown)
            m_Status.Provenance.LicenseTier = Engine::FabLicenseTier::NotApplicable;
        std::string error;
        m_Status.ProvenanceValid = ValidateFabProvenance(m_Status.Provenance, error);
        m_Status.ProvenanceError = m_Status.ProvenanceValid ? std::string() : std::move(error);
        m_Status.ProvenanceDigest = ComputeFabProvenanceDigest(m_Status.Provenance);
        m_Status.ProvenanceConfirmed = false;
        return true;
    }

    bool FabImportController::ConfirmProvenance(const FabImportProjectContext& context, std::string_view expectedDigest)
    {
        m_Status.LastRejection.clear();
        if (m_State != FabImportState::AwaitingProvenance)
            return Reject("provenance can only be confirmed while awaiting provenance");
        // The licence-kind gate: Reference-Only, code-plugin, UE-only and Other
        // declarations cannot become importable source content, and say so plainly.
        const FabLicenseGateResult gate = EvaluateFabLicenseGate(m_Status.Provenance);
        if (gate.Verdict == FabLicenseVerdict::Refused)
            return Reject(gate.Message);
        if (!m_Status.ProvenanceValid)
            return Reject("provenance is not valid: " + m_Status.ProvenanceError);
        if (!expectedDigest.empty() && expectedDigest != m_Status.ProvenanceDigest)
            return Reject("the provenance changed since it was reviewed");
        ResolvedContext resolved;
        std::string error;
        if (!ResolveContext(context, resolved, error))
            return Reject(error);

        FabImportJob& job = *m_Job;
        job.Declaration = MakeDeclaration(m_Status.Provenance, job.Format, job.SourceSha256, NowUtc());
        job.BaseRegistry = context.Registry;
        job.BaseReceipts = context.Receipts;
        job.FinalCookedBase = resolved.FinalCookedBase;
        job.CookStagingRoot = resolved.CookStagingRoot;
        job.DefaultCookStagingRoot = resolved.DefaultCookStagingRoot;
        m_Status.ProvenanceConfirmed = true;
        StartPhase(&RunCookPhase, FabImportState::Cooking);
        return true;
    }

    bool FabImportController::Commit(const FabImportProjectContext& context)
    {
        m_Status.LastRejection.clear();
        if (m_State != FabImportState::ReadyToCommit)
            return Reject("an import can only be committed when it is ready to commit");
        ResolvedContext resolved;
        std::string error;
        if (!ResolveContext(context, resolved, error))
            return Reject(error);

        FabImportJob& job = *m_Job;
        const auto cancelBeforeCommit = [this, &job]()
        {
            if (!job.IsCancelled())
                return false;
            ReleaseJobResources();
            EnterTerminal(FabImportState::Cancelled, FabImportError::None, "the import was cancelled");
            return true;
        };
        job.Hook(FabImportHookPoint::CommitStarted);
        if (cancelBeforeCommit())
            return false;

        Engine::FabGltfCandidate candidate;
        if (!Engine::BuildFabGltfCandidate(context.Registry, context.Receipts, job.Prepared, job.Staged,
                resolved.FinalCookedBase, candidate, error))
            return Reject("the import no longer applies to the project: " + error);

        const Engine::AssetHandle meshHandle = job.Prepared.Assets[0].Handle;
        const Engine::AssetHandle materialHandle = job.Prepared.Assets[1].Handle;

        std::string sceneBytes = context.SceneBytes;
        bool assignmentApplied = false;
        if (context.Assignment)
        {
            const FabAssignmentTarget& target = *context.Assignment;
            const fs::path input = job.CookDirectory.GetPath() / "scene-in.spiral";
            const fs::path output = job.CookDirectory.GetPath() / "scene-out.spiral";
            Engine::Scene scene;
            if (!WriteFileBytes(input, context.SceneBytes) || !Engine::Scene::LoadFromFile(input, scene))
                return Reject("the supplied Scene bytes could not be loaded for the assignment");
            const Engine::Entity entity { target.Entity };
            Engine::MeshRendererComponent* meshRenderer = scene.IsEntityValid(entity)
                ? scene.TryGetMeshRendererComponent(entity) : nullptr;
            if (!meshRenderer)
                return Reject("the assignment target does not exist or has no mesh renderer");
            if (meshRenderer->MeshAsset != target.ExpectedMeshAsset
                || meshRenderer->MaterialAsset != target.ExpectedMaterialAsset)
                return Reject("the assignment target no longer holds the expected mesh and material");
            meshRenderer->MeshAsset = meshHandle;
            meshRenderer->MaterialAsset = materialHandle;
            if (!scene.SaveToFile(output))
                return Reject("the assigned Scene could not be serialized");
            sceneBytes = ReadFileBytes(output);
            if (sceneBytes.empty())
                return Reject("the assigned Scene could not be read back");
            assignmentApplied = true;
        }

        const bool exactReuse = !candidate.NeedsPublish;
        auto result = std::make_unique<FabImportCommitResult>();
        result->Decision = candidate.Decision.Kind;
        result->Receipt = candidate.Receipt;
        result->Material = candidate.Material;
        result->MeshAsset = meshHandle;
        result->MaterialHandle = materialHandle;
        for (size_t index = 2; index < job.Prepared.Assets.size(); ++index)
            result->TextureAssets.push_back(job.Prepared.Assets[index].Handle);
        m_Status.HasDecision = true;
        m_Status.Decision = candidate.Decision.Kind;
        m_Status.DecisionDiagnostic = candidate.Decision.Diagnostic;

        if (exactReuse && !assignmentApplied)
        {
            result->Manifest = resolved.Manifest;
            result->Registry = candidate.Registry;
            result->Receipts = candidate.Receipts;
            result->SceneBytes = context.SceneBytes;
            m_CommitResult = std::move(result);
            m_Status.ProjectChanged = false;
            m_Status.ProjectRevision = resolved.Manifest.ProjectRevision;
            m_Status.ManifestSha256 = Engine::Sha256Builder::HashString(context.ManifestBytes);
            ReleaseJobResources();
            EnterTerminal(FabImportState::Done, FabImportError::None, "this package was already imported");
            return true;
        }

        // Build the next revision. Registry directory, scene and receipts keep their
        // directories so relative cooked roots and the cooked base stay valid.
        const u64 revision = resolved.Manifest.ProjectRevision + 1;
        const std::string token = TransactionToken(job.Id, job.GenerationId);
        Engine::ProjectManifest next = resolved.Manifest;
        next.ScenePath = RevisionPath(resolved.Manifest.ScenePath, revision, token);
        next.AssetRegistryPath = RevisionPath(resolved.Manifest.AssetRegistryPath, revision, token);
        next.FabReceiptsPath = RevisionPath(resolved.Manifest.FabReceiptsPath.empty()
                ? JoinProjectPath(resolved.RegistryDirectory, "Receipts/FabReceipts.spiralfab")
                : resolved.Manifest.FabReceiptsPath,
            revision, token);
        next.ProjectRevision = revision;
        std::string manifestBytes;
        if (!Engine::SerializeProjectManifest(next, manifestBytes, error))
            return Reject("the next manifest is invalid: " + error);

        const fs::path scratchRegistry = job.CookDirectory.GetPath() / "registry.spiralassets";
        std::string receiptBytes;
        if (!candidate.Registry.SaveToFile(scratchRegistry))
            return Reject("the candidate registry could not be serialized");
        const std::string registryBytes = ReadFileBytes(scratchRegistry);
        if (registryBytes.empty() || !Engine::SerializeFabReceiptCollection(candidate.Receipts, receiptBytes, error))
            return Reject("the candidate registry or receipts could not be serialized");

        Engine::ProjectCommitRequest request;
        request.ProjectRoot = context.ProjectRoot;
        request.ManifestRelativePath = context.ManifestRelativePath;
        request.ManifestBytes = manifestBytes;
        request.ExpectedManifestSha256 = Engine::Sha256Builder::HashString(context.ManifestBytes);
        if (candidate.NeedsPublish)
        {
            Engine::ProjectCommitGeneration generation;
            generation.StagedDirectory = job.CookDirectory.GetPath() / job.Staged.CookedRoot;
            generation.RelativeRoot = JoinProjectPath(resolved.RegistryDirectory, job.Staged.CookedRoot);
            generation.Artifacts = Engine::GetFabGenerationArtifacts(candidate.Receipt);
            request.Generation = std::move(generation);
        }
        request.RevisionFiles = { { next.ScenePath, sceneBytes }, { next.AssetRegistryPath, registryBytes },
            { next.FabReceiptsPath, receiptBytes } };
        const std::shared_ptr<std::atomic<bool>> cancel = job.Cancel;
        request.IsCancelled = [cancel]() { return cancel->load(std::memory_order_acquire); };
        const fs::path projectRoot = context.ProjectRoot;
        const std::string manifestRelativePath = context.ManifestRelativePath;
        request.ValidateCommitted = [projectRoot, manifestRelativePath, revision, &candidate, &next](std::string& message)
        {
            Engine::FabProjectState state;
            Engine::FabProjectValidationOptions options;
            options.Level = Engine::FabProjectValidationLevel::FullHash;
            if (!Engine::LoadFabProjectState(projectRoot, manifestRelativePath, options, state, message))
                return false;
            Engine::Scene scene;
            if (state.Manifest != next || state.Manifest.ProjectRevision != revision || state.Receipts != candidate.Receipts
                || !Engine::Scene::LoadFromFile(projectRoot / fs::path(state.Manifest.ScenePath), scene))
            {
                message = "the committed project does not match the candidate";
                return false;
            }
            return true;
        };
        request.TestHook = m_Config.CommitTestHook;

        m_State = FabImportState::Committing;
        job.Hook(FabImportHookPoint::BeforeCommitCall);
        const Engine::ProjectCommitResult committed = Engine::CommitProjectRevision(request);

        m_Status.CommitOutcome = committed.Outcome;
        if (Engine::IsCommitted(committed.Outcome))
        {
            result->ProjectChanged = true;
            result->AssignmentApplied = assignmentApplied;
            result->Commit = committed;
            result->Manifest = next;
            result->Registry = candidate.Registry;
            result->Receipts = candidate.Receipts;
            result->SceneBytes = sceneBytes;
            m_CommitResult = std::move(result);
            m_Status.ProjectChanged = true;
            m_Status.ProjectRevision = revision;
            m_Status.ManifestSha256 = committed.CommittedManifestSha256;
            ReleaseJobResources();
            if (committed.Outcome == Engine::ProjectCommitOutcome::CommittedRecoveryRequired)
            {
                EnterTerminal(FabImportState::FailedAfterCommit, FabImportError::CommitFailed, committed.Message);
                return false;
            }
            EnterTerminal(FabImportState::Done, FabImportError::None,
                committed.Outcome == Engine::ProjectCommitOutcome::CommittedDurabilityUnconfirmed
                    ? "committed; durability of the manifest replacement is unconfirmed"
                    : "committed");
            return true;
        }

        const FabImportError mapped = MapCommitError(committed.Error);
        if (committed.Outcome == Engine::ProjectCommitOutcome::NotCommitted
            && mapped == FabImportError::StaleBase && committed.Generation == Engine::ProjectCommitGenerationDisposition::None)
        {
            // Nothing was published or written: the staged generation is intact and the
            // caller may retry with a fresh context.
            m_State = FabImportState::ReadyToCommit;
            return Reject(committed.Message);
        }
        ReleaseJobResources();
        if (committed.Outcome == Engine::ProjectCommitOutcome::Cancelled)
            EnterTerminal(FabImportState::Cancelled, FabImportError::None, "the import was cancelled before the manifest was replaced");
        else
            EnterTerminal(FabImportState::Failed, mapped, committed.Message);
        return false;
    }

    bool FabImportController::Dismiss()
    {
        m_Status.LastRejection.clear();
        if (!IsTerminal(m_State))
            return Reject("only a finished import can be dismissed");
        m_Job.reset();
        m_CommitResult.reset();
        m_Status = {};
        {
            std::scoped_lock lock(m_CancelMutex);
            m_CancelFlag.reset();
        }
        m_State = FabImportState::Idle;
        return true;
    }

    void FabImportController::Shutdown()
    {
        if (!m_Job)
            return;
        RequestCancel();
        {
            std::unique_lock lock(m_Job->Mutex);
            m_Job->PhaseFinished.wait(lock, [this]() { return m_Job->Phase.load(std::memory_order_acquire) != PhaseState::Running; });
        }
        ReleaseJobResources();
        if (!IsTerminal(m_State))
            EnterTerminal(FabImportState::Cancelled, FabImportError::None, "the import was cancelled");
        m_Job->Phase.store(PhaseState::Idle, std::memory_order_release);
    }

    FabImportStatus FabImportController::GetStatus() const
    {
        FabImportStatus status = m_Status;
        status.State = m_State;
        if (m_Job)
        {
            status.CancelRequested = m_Job->IsCancelled();
            status.FilesCompleted = m_Job->FilesCompleted.load(std::memory_order_relaxed);
            status.FileCount = m_Job->FileCount.load(std::memory_order_relaxed);
            status.BytesCompleted = m_Job->BytesCompleted.load(std::memory_order_relaxed);
            status.BytesTotal = m_Job->BytesTotal.load(std::memory_order_relaxed);
        }
        return status;
    }
}
