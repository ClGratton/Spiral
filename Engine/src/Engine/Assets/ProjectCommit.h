#pragma once

#include "Engine/Core/Base.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    // Project commit: the engine-side primitive behind one atomic project revision.
    //
    // A revision never mutates a live file. It (1) publishes an immutable
    // generation directory with a no-replace rename, (2) creates the candidate
    // Scene/registry/receipt files exclusively, and (3) atomically replaces the
    // project manifest, which is the sole commit pointer. Everything before step 3
    // is unreferenced garbage if the process dies; step 3 is the commit instant.
    //
    // Crash and recovery table (the next open of the project follows the rule):
    //   before generation publish   old manifest; caller staging dir remains      old project loads; staging is the caller's orphan
    //   generation published        old manifest; extra generation directory      old project loads; a retry adopts the directory
    //                                                                               when every artifact verifies, else fails closed
    //   revision files created      old manifest; extra unreferenced files         old project loads; retry must use fresh file names
    //   during manifest replace     old manifest; ".<name>.tmp.*" sibling          old project loads; temp is garbage
    //   after manifest rename       new manifest, directory entry maybe volatile   committed; durability reported, never rolled back
    //   post-commit load failure    new manifest                                   recovery-required: reload from the committed manifest
    //   unparsable manifest         only pre-atomic writers or tampering           fail closed, no repair, no guessed revision
    // Nothing here ever deletes a published generation or a committed file.

    enum class ProjectCommitHook
    {
        Begin,
        Locked,
        BaseVerified,
        GenerationVerified,   // before the no-replace publish (or adoption) is attempted
        GenerationPublished,  // generation directory is final and verified
        BeforeRevisionFile,   // detail: the file's relative path
        RevisionFileWritten,  // detail: the file's relative path
        BeforePointer,        // last cancellation point
        AfterPointer          // the commit has happened; cancel and fail are ignored
    };

    // Test-facing seam. Production code passes no hook.
    enum class ProjectCommitHookAction
    {
        Continue,
        Cancel,
        Fail,
        // Abandon the transaction exactly here, without any cleanup, as if the
        // process had died. The returned outcome is then not meaningful; inspect
        // the disk.
        SimulateCrash
    };

    struct ProjectCommitFile
    {
        std::string RelativePath;
        std::string Bytes;
    };

    struct ProjectCommitArtifact
    {
        std::string RelativePath;  // beneath the generation directory
        std::string Sha256;        // lowercase hex of the file bytes
    };

    struct ProjectCommitGeneration
    {
        // Fully written directory on the same filesystem as the project, moved
        // into place by a no-replace rename. Empty means the generation must
        // already exist and verify (idempotent adopt-or-fail).
        std::filesystem::path StagedDirectory;
        std::string RelativeRoot;  // project-relative final location
        // Exhaustive: the directory must contain exactly these regular files.
        std::vector<ProjectCommitArtifact> Artifacts;
    };

    struct ProjectCommitRequest
    {
        std::filesystem::path ProjectRoot;
        std::string ManifestRelativePath;
        // Parsed with DeserializeProjectManifest before anything is written. The
        // transaction uses only ProjectRevision and the three named file paths.
        std::string ManifestBytes;
        // SHA-256 (lowercase hex) of the manifest the candidate was built from.
        // Empty requires the manifest to be absent. A mismatch is BaseChanged.
        std::string ExpectedManifestSha256;
        std::optional<ProjectCommitGeneration> Generation;
        // Created exclusively; a name that already exists fails the commit. Use
        // names that are unique per transaction.
        std::vector<ProjectCommitFile> RevisionFiles;

        std::function<bool()> IsCancelled;
        // Runs after the pointer replace. Returning false reports
        // CommittedRecoveryRequired; the disk stays committed.
        std::function<bool(std::string&)> ValidateCommitted;
        std::function<ProjectCommitHookAction(ProjectCommitHook, std::string_view)> TestHook;
    };

    enum class ProjectCommitOutcome
    {
        NotCommitted,
        Cancelled,
        Committed,
        // The manifest replacement is visible but its directory entry was not
        // confirmed durable; a power loss may resurrect the previous manifest.
        CommittedDurabilityUnconfirmed,
        CommittedRecoveryRequired
    };

    enum class ProjectCommitError
    {
        None,
        InvalidRequest,
        Unsupported,
        PathEscape,
        LockUnavailable,
        BaseChanged,
        InvalidManifest,
        GenerationMissing,
        GenerationMismatch,
        GenerationPublishFailed,
        RevisionFileExists,
        IoFailure,
        Cancelled,
        InjectedFailure,
        ReloadValidationFailed
    };

    enum class ProjectCommitGenerationDisposition
    {
        None,
        Published,
        AdoptedExisting
    };

    // Data the Editor needs to honor the undo barrier: undo snapshots embed the
    // registry and materials, so undoing across a committed revision would
    // resurrect a registry without the committed assets.
    struct ProjectCommitUndoBarrier
    {
        bool ClearUndoRedoHistory = false;
        u64 PreviousProjectRevision = 0;
        u64 NewProjectRevision = 0;
    };

    struct ProjectCommitResult
    {
        ProjectCommitOutcome Outcome = ProjectCommitOutcome::NotCommitted;
        ProjectCommitError Error = ProjectCommitError::None;
        std::string Message;
        ProjectCommitGenerationDisposition Generation = ProjectCommitGenerationDisposition::None;
        ProjectCommitUndoBarrier UndoBarrier;
        std::string CommittedManifestSha256;
    };

    inline bool IsCommitted(ProjectCommitOutcome outcome)
    {
        return outcome == ProjectCommitOutcome::Committed
            || outcome == ProjectCommitOutcome::CommittedDurabilityUnconfirmed
            || outcome == ProjectCommitOutcome::CommittedRecoveryRequired;
    }

    // False where the platform cannot provide no-replace directory publication
    // and descriptor-relative exclusive creation (currently everything but Linux).
    // Commit then fails closed with ProjectCommitError::Unsupported and changes
    // nothing.
    bool IsProjectCommitSupported();

    ProjectCommitResult CommitProjectRevision(const ProjectCommitRequest& request);

    // Verifies that `directory` holds exactly `artifacts`: only regular files,
    // no links or special objects, no extra files or directories. With
    // hashContents every file is streamed through SHA-256 and compared. outError
    // names the first violation.
    bool VerifyGenerationDirectory(const std::filesystem::path& directory,
        const std::vector<ProjectCommitArtifact>& artifacts, bool hashContents,
        const std::function<bool()>& isCancelled, std::string& outError);
}
