#pragma once

#include "FabEditorAdoption.h"
#include "Fab/FabImportController.h"
#include "Fab/FabIntake.h"
#include "Fab/FabLicenseGate.h"

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Fab import workflow's Editor face: it owns the one Fab::FabImportController,
// the intake queue (drops, completed browser downloads, typed selections), the
// provenance form and the ImGui window. The typed control and the UI call the
// same methods, so a typed action can never reach a state the UI cannot.
//
// It knows nothing about EditorLayer: the live project is reached only through
// FabImportPanelHost, supplied by the Editor.
class FabImportPanel
{
public:
    struct Host
    {
        // Builds a fresh controller context (exact manifest bytes, live registry,
        // receipts and serialized Scene) with an optional existing-entity assignment.
        std::function<bool(std::optional<Fab::FabAssignmentTarget>, Fab::FabImportProjectContext&, std::string&)>
            BuildContext;
        // Adopts Controller().GetCommitResult() into the live Editor after a commit
        // (or reloads the project from disk after FailedAfterCommit).
        std::function<bool(std::string&)> AdoptCommit;
        // The selected entity's current mesh/material as an assignment candidate.
        std::function<std::optional<Fab::FabAssignmentTarget>()> SelectedAssignment;
        std::function<void(const std::string&)> Log;
    };

    struct CommitReport
    {
        bool Ok = false;
        // True when the project was committed on disk but this call could not
        // finish: the Editor reloaded (or must reload) the committed project.
        bool CommittedOnDisk = false;
        std::string Error;
    };

    FabImportPanel();
    ~FabImportPanel();
    FabImportPanel(const FabImportPanel&) = delete;
    FabImportPanel& operator=(const FabImportPanel&) = delete;

    // Replaces the controller; only valid while no import is active.
    void Initialize(Host host, Fab::FabImportControllerConfig config = {});
    // Where completed browser downloads are staged; a finished import removes only
    // a download file that lies inside this directory.
    void SetDownloadStagingRoot(std::filesystem::path root) { m_DownloadStagingRoot = std::move(root); }
    // Cancels, waits for workers, removes every staging directory.
    void Shutdown();

    // Main thread, once per frame before the typed control drains.
    void Update();
    // Main thread, inside the ImGui frame.
    void Draw();
    bool IsVisible() const { return m_Visible; }
    void SetVisible(bool visible) { m_Visible = visible; }

    // Drop and completed-download intake: classify, then submit or queue.
    void SubmitPaths(Fab::FabIntakeOrigin origin, std::span<const std::string> utf8Paths);
    // Typed intake of one explicit path; nothing is queued. On failure `error` is a
    // stable token.
    bool SelectTyped(const std::filesystem::path& path, std::string_view expectedKind,
        std::string_view expectedSha256, std::string& error);

    // Stores the form fields exactly as the UI would; never confirms.
    bool ApplyProvenance(const Fab::FabProvenance& provenance);
    // The user's confirmation. The UI calls this only from the Confirm button's
    // click and the typed ConfirmFabProvenance action from its own request.
    bool Confirm(std::string_view expectedDigest, std::string& error);
    CommitReport Commit(std::optional<Fab::FabAssignmentTarget> assignment);
    void Cancel();
    bool Dismiss(std::string& error);
    // A project was replaced underneath the job: cancel and drop everything queued.
    void AbandonForProjectChange();

    Fab::FabImportController& Controller() { return *m_Controller; }
    const Fab::FabImportController& Controller() const { return *m_Controller; }
    FabEditor::ProjectValidator& Validator() { return m_Validator; }
    const FabEditor::ProjectValidator& Validator() const { return m_Validator; }
    // The most recent panel-level note (rejections, source mismatches).
    const std::string& GetNote() const { return m_Note; }
    // Selects a licence choice in the form exactly as the License combo does (the
    // headless smoke uses it for the choices the typed provenance cannot express).
    // Valid only while awaiting provenance.
    bool ChooseLicenseInForm(Fab::FabLicenseChoice choice);
    // Ticks the NoAI notice as its checkbox does.
    void AcknowledgeNoAiNotice();
    bool NoAiNoticeAcknowledged() const { return m_Form.NoAiAcknowledged; }

    // What the provenance form last drew, so the headless smoke can check the licence
    // gate and the Confirm button without reading widget state.
    struct ProvenanceDrawRecord
    {
        std::string_view LicenseLabel;
        Fab::FabLicenseVerdict Verdict = Fab::FabLicenseVerdict::Incomplete;
        bool ConfirmEnabled = false;
        bool NoAiAcknowledgementShown = false;
        std::string GateMessage;
    };
    const ProvenanceDrawRecord& LastProvenanceDraw() const { return m_LastDraw; }
    const std::filesystem::path& GetScratchRoot() const { return m_ScratchRoot; }
    static const char* KindToken(Fab::FabIntakeKind kind);
    static const char* ReasonToken(Fab::FabIntakeReason reason);
    static const char* OriginToken(Fab::FabIntakeOrigin origin);

private:
    struct Form
    {
        std::array<char, 512> ProductIdentity {};
        std::array<char, 256> ProductName {};
        std::array<char, 256> Publisher {};
        std::array<char, 128> Version {};
        std::array<char, 512> AttributionLink {};
        std::vector<char> AttributionText = std::vector<char>(16 * 1024 + 1, '\0');
        // Index into Fab::kFabLicenseChoices: the licence and its tier in one choice.
        // CodePlugin and Other exist only here; the receipt cannot represent them.
        int LicenseChoice = 0;
        int NoAI = 0;          // Unknown, No, Yes
        // The user ticked the NoAI notice. Session-only: the receipt schema has no field
        // for it; the declared Marked No-AI value is what the receipt records.
        bool NoAiAcknowledged = false;
        int GeneratedWithAI = 0;
        int RawSourcePolicy = 0; // ExcludedFromProject, PrivateProjectOnly
    };

    // The licence choice the gate judges: the controller's provenance, or the form's
    // CodePlugin/Other choice while the form is in step with the controller.
    Fab::FabLicenseChoice EffectiveLicenseChoice(const Fab::FabImportStatus& status) const;
    ProvenanceDrawRecord m_LastDraw;
    Fab::FabProvenance FormToProvenance() const;
    void ProvenanceToForm(const Fab::FabProvenance& provenance);
    void SyncForm(const Fab::FabImportStatus& status);
    void DrawIdle();
    void DrawJob(const Fab::FabImportStatus& status);
    void DrawProvenanceForm(const Fab::FabImportStatus& status);
    void DrawReadyToCommit(const Fab::FabImportStatus& status);
    void AddMessage(std::string message);
    void Enqueue(const Fab::FabIntakeRequest& request);
    bool SubmitNow(const Fab::FabIntakeRequest& request);
    void CleanupDownload();

    Host m_Host;
    Fab::FabImportControllerConfig m_Config;
    std::unique_ptr<Fab::FabImportController> m_Controller;
    FabEditor::ProjectValidator m_Validator;
    std::filesystem::path m_ScratchRoot;
    std::filesystem::path m_DownloadStagingRoot;
    std::deque<Fab::FabIntakeRequest> m_Pending;
    Fab::FabIntakeRequest m_Active;
    std::string m_ExpectedSourceSha256;
    Engine::u64 m_ObservedJob = 0;
    bool m_SourceChecked = false;
    bool m_Cleaned = true;
    bool m_Visible = false;
    bool m_AssignToSelection = false;
    Engine::u64 m_FormJob = 0;
    std::string m_FormDigest;
    Form m_Form;
    std::array<char, 1024> m_PathField {};
    std::vector<std::string> m_Messages;
    std::string m_Note;
};
