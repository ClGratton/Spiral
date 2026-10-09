#pragma once

#include "FabIntake.h"

#include "Engine/Assets/AssetRegistry.h"
#include "Engine/Assets/CommonImage.h"
#include "Engine/Assets/FabArchive.h"
#include "Engine/Assets/FabGltfCook.h"
#include "Engine/Assets/FabImportReceipt.h"
#include "Engine/Assets/LocalPackageSnapshot.h"
#include "Engine/Assets/MaterialAsset.h"
#include "Engine/Assets/ProjectCommit.h"
#include "Engine/Assets/ProjectManifest.h"
#include "Engine/Core/Base.h"
#include "Engine/Scene/Entity.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Editor-independent Fab import controller: one headless state machine that
// composes the finished Engine authorities (ZIP staging, local package snapshot,
// glTF prepare/stage/candidate, receipt classification, ProjectCommit). It
// includes only Engine headers and never touches live Editor state; the Editor
// adopts the committed project from FabImportCommitResult after a successful
// Commit. Every method is main-thread-only except RequestCancel, which is safe
// from any thread (workers and test hooks included). Workers capture only a
// shared job record, never the controller.
namespace Fab
{
    enum class FabImportState
    {
        Idle,
        Snapshotting,
        Preparing,
        AwaitingProvenance,
        Cooking,
        ReadyToCommit,
        Committing,
        Done,
        Failed,
        Cancelled,
        // The manifest replace happened but the post-commit reload validation
        // failed: the disk is committed, never rolled back, and the Editor must
        // reload the project from the committed manifest.
        FailedAfterCommit
    };

    const char* ToString(FabImportState state);
    bool IsTerminal(FabImportState state);

    enum class FabImportError
    {
        None,
        InvalidIntake,
        Unsupported,
        SnapshotRejected,
        ArchiveRejected,
        PackageRejected,
        ProvenanceInvalid,
        Conflict,
        StagingFailed,
        ProjectContextInvalid,
        StaleBase,
        AssignmentRejected,
        CommitFailed,
        Internal
    };

    const char* ToString(FabImportError error);

    // User-confirmed receipt fields. The package format is derived from the
    // snapshot and the source digests are measured, so neither appears here.
    struct FabProvenance
    {
        std::string ProductIdentity;  // canonical https://www.fab.com/listings/<id>
        std::string ProductName;
        std::string Publisher;
        std::string VersionOrDownloadLabel;
        Engine::FabLicenseFamily LicenseFamily = Engine::FabLicenseFamily::Unknown;
        Engine::FabLicenseTier LicenseTier = Engine::FabLicenseTier::Unknown;
        std::string AttributionText;
        std::string AttributionLink;
        Engine::FabMetadataFlag NoAI = Engine::FabMetadataFlag::Unknown;
        Engine::FabMetadataFlag GeneratedWithAI = Engine::FabMetadataFlag::Unknown;
        Engine::FabRawSourcePolicy RawSourcePolicy = Engine::FabRawSourcePolicy::ExcludedFromProject;
    };

    // Pure. Applies the receipt validator to the declaration these fields would
    // produce (so no rule is duplicated) and additionally requires a chosen
    // license family, a chosen tier for Fab Standard, and a non-empty publisher.
    bool ValidateFabProvenance(const FabProvenance& provenance, std::string& error);
    // Lowercase SHA-256 over a domain-separated canonical encoding of every field.
    std::string ComputeFabProvenanceDigest(const FabProvenance& provenance);

    struct FabAssignmentTarget
    {
        Engine::EntityId Entity = Engine::kInvalidEntityId;
        // Compare-and-swap: the entity must still hold exactly these handles.
        Engine::AssetHandle ExpectedMeshAsset = Engine::kInvalidAssetHandle;
        Engine::AssetHandle ExpectedMaterialAsset = Engine::kInvalidAssetHandle;
    };

    // The Editor's view of the project at the moment of a call. Everything is a
    // value copy; the controller never retains Editor pointers.
    struct FabImportProjectContext
    {
        std::filesystem::path ProjectRoot;
        // Strict portable path of the manifest beneath ProjectRoot.
        std::string ManifestRelativePath;
        // Exactly the bytes read from disk; their SHA-256 is the commit's
        // compare-and-swap base. Every other manifest path is relative to ProjectRoot.
        std::string ManifestBytes;
        Engine::AssetRegistry Registry;
        Engine::FabReceiptCollection Receipts;
        // Serialized Scene to persist with the revision (the live state).
        std::string SceneBytes;
        std::optional<FabAssignmentTarget> Assignment;
        // Owner-only directory on the project filesystem for cook staging.
        // Empty means ProjectRoot/".fab-staging", removed again when empty.
        std::filesystem::path CookStagingRoot;
    };

    enum class FabImportHookPoint
    {
        PhaseStarted,        // each worker phase, on the worker
        ArchiveCopied,       // ZIP bytes are in private staging and hashed
        ArchiveExtracted,    // members are in the extraction directory
        SnapshotCreated,
        PreviewPrepared,     // provenance-independent preparation succeeded
        CookPrepared,        // provenance-bound preparation succeeded
        CookStaged,          // generation is staged and hashed
        CandidateBuilt,      // classification done, ReadyToCommit about to be published
        CommitStarted,       // main thread, before the candidate is built
        BeforeCommitCall     // main thread, immediately before CommitProjectRevision
    };

    struct FabImportControllerConfig
    {
        // Private staging for hostile package bytes. Empty selects
        // DefaultFabPackageStagingRoot(). Must be on a local filesystem; the
        // controller creates it mode 0700 and refuses a looser directory.
        std::filesystem::path PackageStagingRoot;

        FabIntakeLimits IntakeLimits;
        Engine::FabArchiveLimits ArchiveLimits;
        Engine::LocalPackageSnapshotLimits SnapshotLimits;
        Engine::FabGltfLimits GltfLimits;
        Engine::CommonImageLimits ImageLimits;

        // Test-facing seams. Hooks run on the worker (or the main thread for the
        // commit points) and may call RequestCancel.
        std::function<void(FabImportHookPoint)> TestHook;
        std::function<void(Engine::LocalPackageSnapshotHookPoint, std::string_view)> SnapshotTestHook;
        std::function<void(Engine::FabGltfStage, std::string_view)> CookTestHook;
        std::function<Engine::ProjectCommitHookAction(Engine::ProjectCommitHook, std::string_view)> CommitTestHook;
    };

    // ${XDG_CACHE_HOME:-$HOME/.cache}/Spiral/FabStaging; empty when neither exists.
    std::filesystem::path DefaultFabPackageStagingRoot();

    struct FabImportTextureSummary
    {
        std::string Role;
        Engine::u32 Width = 0;
        Engine::u32 Height = 0;
    };

    // Provenance-independent content summary (no 3D preview exists before commit).
    struct FabImportSummary
    {
        Engine::u64 SourceFileCount = 0;
        Engine::u64 SourceBytes = 0;
        Engine::u32 VertexCount = 0;
        Engine::u32 TriangleCount = 0;
        Engine::u32 PrimitiveInstanceCount = 0;
        std::string MaterialName;
        std::vector<FabImportTextureSummary> Textures;
    };

    // Read-only snapshot for the UI and the typed control.
    struct FabImportStatus
    {
        FabImportState State = FabImportState::Idle;
        Engine::u64 JobId = 0;
        bool CancelRequested = false;

        FabIntakeKind SourceKind = FabIntakeKind::Unsupported;
        FabIntakeOrigin SourceOrigin = FabIntakeOrigin::Typed;
        std::string SourceName;  // leaf only, control bytes replaced

        // Progress of the active worker phase.
        Engine::u64 FilesCompleted = 0;
        Engine::u64 FileCount = 0;
        Engine::u64 BytesCompleted = 0;
        Engine::u64 BytesTotal = 0;

        // Terminal reason (Failed, Cancelled, FailedAfterCommit).
        FabImportError Error = FabImportError::None;
        std::string Message;
        // The most recent refused API call; state did not change.
        std::string LastRejection;

        // Valid from AwaitingProvenance.
        Engine::FabPackageFormat Format = Engine::FabPackageFormat::Unknown;
        std::string SourceSha256;
        std::string ExpandedTreeSha256;
        FabImportSummary Summary;

        FabProvenance Provenance;
        bool ProvenanceValid = false;
        std::string ProvenanceError;
        bool ProvenanceConfirmed = false;
        std::string ProvenanceDigest;

        // Valid from ReadyToCommit, and on Failed when the classification itself
        // refused the import (Conflict).
        std::string StreamId;
        std::string GenerationId;
        bool HasDecision = false;
        Engine::FabReceiptDecisionKind Decision = Engine::FabReceiptDecisionKind::InvalidCandidate;
        std::string DecisionDiagnostic;

        // Valid once Commit ran.
        bool ProjectChanged = false;
        Engine::ProjectCommitOutcome CommitOutcome = Engine::ProjectCommitOutcome::NotCommitted;
        Engine::u64 ProjectRevision = 0;
        std::string ManifestSha256;
    };

    // What the Editor adopts after Commit reaches Done (or FailedAfterCommit, where
    // it must reload from disk instead). When ProjectChanged is false (exact reuse
    // without an assignment) nothing was written and the Editor's state is current.
    struct FabImportCommitResult
    {
        Engine::FabReceiptDecisionKind Decision = Engine::FabReceiptDecisionKind::InvalidCandidate;
        bool ProjectChanged = false;
        bool AssignmentApplied = false;
        Engine::ProjectCommitResult Commit;
        // The committed manifest, registry, receipts and Scene (revision files).
        Engine::ProjectManifest Manifest;
        Engine::AssetRegistry Registry;
        Engine::FabReceiptCollection Receipts;
        std::string SceneBytes;
        Engine::FabImportReceipt Receipt;
        // The new immutable material for the Editor's MaterialLibrary.
        Engine::MaterialAsset Material;
        Engine::AssetHandle MeshAsset = Engine::kInvalidAssetHandle;
        Engine::AssetHandle MaterialHandle = Engine::kInvalidAssetHandle;
        std::vector<Engine::AssetHandle> TextureAssets;
    };

    struct FabImportJob;

    class FabImportController
    {
    public:
        explicit FabImportController(FabImportControllerConfig config = {});
        ~FabImportController();

        FabImportController(const FabImportController&) = delete;
        FabImportController& operator=(const FabImportController&) = delete;

        // Idle -> Snapshotting. The path is re-classified on the worker; a
        // request whose content no longer matches `request.Kind` is rejected.
        bool Submit(const FabIntakeRequest& request);

        // AwaitingProvenance only. Stores the fields (even if invalid, so a form
        // can be edited incrementally), clears any confirmation and reports
        // validity through GetStatus. Returns false only for a wrong state.
        bool SetProvenance(const FabProvenance& provenance);

        // AwaitingProvenance -> Cooking. Requires valid provenance (and, when
        // non-empty, that `expectedDigest` equals the current digest) and a usable
        // project context. This call is the user's confirmation: it sets
        // MetadataConfirmedByUser on the declaration, so a UI must call it only
        // from an explicit user click.
        bool ConfirmProvenance(const FabImportProjectContext& context, std::string_view expectedDigest = {});

        // ReadyToCommit -> Committing -> a terminal state (or back to ReadyToCommit
        // for a stale base / lock contention, with nothing written). Synchronous
        // on the calling (main) thread. Returns true when the transaction ran and
        // reached Done. A refused call leaves the state unchanged.
        bool Commit(const FabImportProjectContext& context);

        // Thread-safe. Accepted until the manifest replace; the transition to
        // Cancelled happens in Update when the worker (if any) has finished.
        void RequestCancel();

        // Main thread: publishes finished worker phases and applies pending cancels.
        void Update();

        // Terminal state -> Idle, discarding the result.
        bool Dismiss();

        // Cancels, waits for the worker, and removes every staging directory. The
        // destructor calls it.
        void Shutdown();

        FabImportStatus GetStatus() const;
        // Null until Commit ran.
        const FabImportCommitResult* GetCommitResult() const { return m_CommitResult.get(); }
        // True while a worker phase is running or has finished but not yet been published by Update.
        bool IsBusy() const;

    private:
        bool Reject(std::string message);
        void StartPhase(void (*phase)(FabImportJob&), FabImportState state);
        void PublishSnapshotPhase();
        void PublishCookPhase();
        void ReleaseJobResources();
        void EnterTerminal(FabImportState state, FabImportError error, std::string message);

        FabImportControllerConfig m_Config;
        FabImportState m_State = FabImportState::Idle;
        std::shared_ptr<FabImportJob> m_Job;
        Engine::u64 m_NextJobId = 1;
        FabImportStatus m_Status;
        std::mutex m_CancelMutex;
        std::shared_ptr<std::atomic<bool>> m_CancelFlag;
        std::unique_ptr<FabImportCommitResult> m_CommitResult;
    };
}
