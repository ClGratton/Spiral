// Headless Fab control smokes: the Editor publishes a target, an external Python
// producer (Scripts/TestEditorFabImport.sh) drives the typed schema-4 actions,
// and the Editor cross-checks the receipts against its own live state.
#include "EditorLayer.h"

#include "Engine/Assets/FabProjectState.h"
#include "Engine/Assets/ProjectManifest.h"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace
{
    struct SmokeCheck
    {
        std::string Failure;
        void Expect(bool condition, std::string_view what)
        {
            if (!condition && Failure.empty())
                Failure = std::string(what);
        }
    };
}

void EditorLayer::PublishEditorFabControlSmokeTarget()
{
    if (!m_EditorFabHelperSmokeRequested && !m_EditorFabReopenSmokeRequested)
        return;
    if (!Engine::Application::Get().GetSpecification().Window.Headless)
        throw std::runtime_error("the Fab control smokes require --headless");
    const Engine::SceneEntity* prototype = m_ActiveScene.TryGetEntity(m_PrototypeMeshEntity);
    if (!prototype || !prototype->MeshRenderer || !m_EditorMaterialControl.IsOpen())
        throw std::runtime_error("the Fab control smokes need the Prototype Mesh and an open mailbox");
    m_EditorFabSmokePrototypeMesh = prototype->MeshRenderer->MeshAsset;
    m_EditorFabSmokePrototypeMaterial = prototype->MeshRenderer->MaterialAsset;

    std::ostringstream target;
    target << "SpiralEditorFabControlTarget 1\n"
           << "Phase " << (m_EditorFabHelperSmokeRequested ? "import" : "reopen") << '\n'
           << "SessionId " << std::quoted(m_EditorMaterialControl.GetSessionId()) << '\n'
           << "InitialSelectedEntityId " << m_SelectedEntity.Id << '\n'
           << "PrototypeEntityId " << m_PrototypeMeshEntity.Id << '\n'
           << "PrototypeEntityName " << std::quoted(prototype->Name) << '\n'
           << "PrototypeMeshAsset " << m_EditorFabSmokePrototypeMesh << '\n'
           << "PrototypeMaterialAsset " << m_EditorFabSmokePrototypeMaterial << '\n'
           << "FabReceiptCount " << m_FabProject.Receipts.Receipts.size() << '\n'
           << "ProjectRevision " << m_FabProject.Revision << '\n';
    std::string error;
    if (!m_EditorMaterialControl.PublishFabControlTargetForSmoke(target.str(), error))
        throw std::runtime_error("could not publish the Fab control smoke target: " + error);
}

void EditorLayer::RunEditorFabControlSmokeBeforeDrain()
{
}

void EditorLayer::RunEditorFabControlSmokeAfterDrain()
{
    const bool importPhase = m_EditorFabHelperSmokeRequested;
    if ((!importPhase && !m_EditorFabReopenSmokeRequested) || m_EditorFabSmokeCompleted)
        return;
    const auto receipt = [this](std::string_view id) { return m_EditorMaterialControl.FindTerminalReceipt(id); };
    if (!receipt(importPhase ? "fa-99-final" : "fb-99-final"))
        return;

    SmokeCheck check;
    const auto need = [&](std::string_view id) -> const EditorMaterialControlReceipt&
    {
        const EditorMaterialControlReceipt* value = receipt(id);
        if (!value)
        {
            check.Expect(false, std::string("missing receipt ") + std::string(id));
            static const EditorMaterialControlReceipt empty;
            return empty;
        }
        return *value;
    };
    const auto succeeded = [&](const EditorMaterialControlReceipt& value, std::string_view effect, std::string_view what)
    {
        check.Expect(value.Succeeded && value.Reason == "ok" && value.Effect == effect && value.PostconditionVerified, what);
    };
    const auto refused = [&](const EditorMaterialControlReceipt& value, std::string_view reason, std::string_view what)
    {
        check.Expect(!value.Succeeded && value.Reason == reason, what);
    };

    std::error_code filesystemError;
    Engine::ProjectManifest diskManifest;
    std::string manifestError;
    check.Expect(Engine::LoadProjectManifest(m_ProjectRoot / m_ProjectManifestRelativePath, diskManifest, manifestError),
        "the manifest on disk must load");
    check.Expect(diskManifest.ProjectRevision == m_FabProject.Revision
            && diskManifest.FabReceiptsPath == m_FabProject.ReceiptsPath && diskManifest.ScenePath == m_ScenePath
            && diskManifest.AssetRegistryPath == m_AssetRegistryPath,
        "the Editor's project pointers must equal the committed manifest");
    std::string diskDigest;
    FabEditor::ReadManifestSha256({ m_ProjectRoot, m_ProjectManifestRelativePath }, diskDigest, manifestError);
    check.Expect(diskDigest == m_FabProject.ManifestSha256, "the tracked manifest digest must equal the disk digest");
    const std::filesystem::path scratch = m_FabImport.GetScratchRoot();
    bool stagingClean = true;
    if (std::filesystem::is_directory(scratch, filesystemError))
        stagingClean = std::filesystem::directory_iterator(scratch) == std::filesystem::directory_iterator();
    check.Expect(stagingClean && !std::filesystem::exists(m_ProjectRoot / ".fab-staging", filesystemError),
        "no staging directory may remain");

    if (importPhase)
    {
        const EditorMaterialControlReceipt& baseline = need("fa-01-inspect");
        const EditorMaterialControlReceipt& confirmWrong = need("fa-06-confirm-wrong");
        const EditorMaterialControlReceipt& wrongGeneration = need("fa-09-commit-wrong-generation");
        const EditorMaterialControlReceipt& commit = need("fa-10-commit");
        const EditorMaterialControlReceipt& place = need("fa-13-place");
        const EditorMaterialControlReceipt& forced = need("fa-14-place-forced-rollback");
        const EditorMaterialControlReceipt& save = need("fa-15-save");

        succeeded(baseline, "ReadOnly", "baseline inspect");
        check.Expect(baseline.Fab.State == "Idle" && baseline.Fab.ProjectReceiptCount == 0, "baseline has no receipts");
        refused(confirmWrong, "fab_provenance_digest_mismatch", "a wrong provenance digest must not confirm");
        refused(wrongGeneration, "fab_generation_mismatch", "a wrong generation must not commit");
        succeeded(commit, "FabImportCommitted", "commit");
        check.Expect(commit.Persistence == "Committed" && commit.Saved && commit.Fab.ProjectChanged
                && commit.Fab.ManifestRevision == 1 && commit.UndoDepthAfter == 0 && commit.Fab.State == "Done",
            "the commit receipt must report revision 1, persistence, and the undo barrier");
        succeeded(place, "MeshAssetPlaced", "place");
        check.Expect(place.UndoDepthBefore == 0 && place.UndoDepthAfter == 1 && place.AfterMeshRendererPresent
                && !place.BeforeMeshRendererPresent && place.AfterMeshRenderer.MeshAsset == commit.Fab.MeshAsset
                && place.AfterMeshRenderer.MaterialAsset == commit.Fab.MaterialAsset,
            "a placed entity gets the receipt's mesh and material as one history entry");
        check.Expect(!forced.Succeeded && forced.Reason == "injected_postcondition_failure_rolled_back"
                && forced.RollbackVerified && forced.Effect == "RolledBack",
            "a failed placement must roll back with verification");
        succeeded(save, "ProjectSaved", "save");
        check.Expect(save.Persistence == "Saved" && save.Saved, "the save receipt must say saved");

        const Engine::Entity placed { place.EntityId };
        const Engine::MeshRendererComponent* renderer = m_ActiveScene.TryGetMeshRendererComponent(placed);
        check.Expect(renderer && renderer->MeshAsset == commit.Fab.MeshAsset && renderer->MaterialAsset == commit.Fab.MaterialAsset
                && m_SelectedEntity == placed,
            "the live placed entity must hold the committed handles and be selected");
        check.Expect(m_UndoHistory.size() == 1 && m_RedoHistory.empty(), "history holds exactly the placement");
        check.Expect(m_FabProject.Receipts.Receipts.size() == 1 && m_FabProject.Revision == 1, "one receipt at revision 1");
        const Engine::AssetMetadata* material = m_AssetRegistry.GetAsset(commit.Fab.MaterialAsset);
        check.Expect(material && Engine::IsImmutableMaterialAsset(*material) && m_MaterialLibrary.Get(commit.Fab.MaterialAsset),
            "the Fab material is immutable and loaded");
        check.Expect(material && !std::filesystem::exists(m_ProjectRoot / material->SourcePath, filesystemError),
            "an immutable material has no logical file");
        check.Expect(Engine::Renderer::GetPublishedArtifactResolverGeneration() > m_EditorFabSmokeInitialRendererGeneration,
            "the commit republished the artifact resolvers");
    }
    else
    {
        const EditorMaterialControlReceipt& reopened = need("fb-01-inspect");
        const EditorMaterialControlReceipt& patch = need("fb-05-material-patch-immutable");
        const EditorMaterialControlReceipt& save = need("fb-07-save");
        const EditorMaterialControlReceipt& reuse = need("fb-13-commit-reuse");
        const EditorMaterialControlReceipt& staleAssignment = need("fb-74-commit-stale-assignment");
        const EditorMaterialControlReceipt& replace = need("fb-75-commit-replace");
        const EditorMaterialControlReceipt& assigned = need("fb-77-entity-prototype");
        const EditorMaterialControlReceipt& assignBack = need("fb-80-assign-back");
        const EditorMaterialControlReceipt& assignStale = need("fb-81-assign-stale");
        const EditorMaterialControlReceipt& panelShow = need("fb-90-panel-show");
        const EditorMaterialControlReceipt& panelHide = need("fb-91-panel-hide");
        const EditorMaterialControlReceipt& panelInspect = need("fb-92-panel-inspect");

        check.Expect(reopened.Succeeded && reopened.Fab.ProjectReceiptCount == 1 && reopened.Fab.ProjectStructural == "passed"
                && reopened.Fab.ManifestRevision == 1,
            "the reopened project must validate structurally with its receipt");
        refused(patch, "immutable_material", "an imported material cannot be patched");
        succeeded(save, "ProjectSaved", "reopened save");
        succeeded(reuse, "FabImportReused", "identical reimport");
        check.Expect(!reuse.Fab.ProjectChanged && reuse.Persistence == "SessionOnly" && !reuse.Saved
                && reuse.Fab.ManifestRevision == 1 && reuse.Fab.Relation == "ExactReuse",
            "an identical reimport writes nothing");
        refused(staleAssignment, "compare_and_swap_state_mismatch", "a stale assignment must not commit");
        check.Expect(staleAssignment.Fab.State == "ReadyToCommit", "a refused assignment leaves the job ready");
        succeeded(replace, "FabImportCommitted", "replacement commit");
        check.Expect(replace.Fab.AssignmentApplied && replace.Fab.ManifestRevision == 2 && replace.Fab.ProjectChanged
                && replace.UndoDepthAfter == 0 && replace.Fab.Relation == "ReplaceSameStreamSource",
            "the replacement commit assigns the prototype at revision 2");
        succeeded(assigned, "ReadOnly", "assigned prototype");
        check.Expect(assigned.AfterMeshRenderer.MeshAsset == replace.Fab.MeshAsset
                && assigned.AfterMeshRenderer.MaterialAsset == replace.Fab.MaterialAsset,
            "the prototype renders the imported mesh and material");
        succeeded(assignBack, "MeshRendererAssetsSet", "typed re-assignment");
        check.Expect(assignBack.UndoDepthBefore == 0 && assignBack.UndoDepthAfter == 1
                && assignBack.AfterMeshRenderer.MeshAsset == m_EditorFabSmokePrototypeMesh
                && assignBack.AfterMeshRenderer.MaterialAsset == m_EditorFabSmokePrototypeMaterial,
            "the typed assignment is one history entry restoring the prototype's assets");
        refused(assignStale, "compare_and_swap_state_mismatch", "a stale mesh-renderer CAS must not apply");
        refused(panelShow, "headless_has_no_browser_panel", "a headless Editor has no browser panel");
        succeeded(panelHide, "FabPanelVisibilitySet", "panel hide");
        succeeded(panelInspect, "ReadOnly", "panel inspect");
        check.Expect(panelInspect.Fab.PanelState == "NotStarted" && !panelInspect.Fab.PanelVisible, "panel diagnostics");

        const Engine::MeshRendererComponent* prototype = m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity);
        check.Expect(prototype && prototype->MeshAsset == m_EditorFabSmokePrototypeMesh
                && prototype->MaterialAsset == m_EditorFabSmokePrototypeMaterial,
            "the live prototype holds its own assets again");
        check.Expect(m_UndoHistory.size() == 1, "history holds exactly the typed re-assignment");
        check.Expect(m_FabProject.Revision == 2 && m_FabProject.Receipts.Receipts.size() >= 2, "revision 2 with retained receipts");
        check.Expect(m_FabImport.Validator().GetState() == FabEditor::ProjectValidator::State::Passed,
            "the full-hash validation passed");
        check.Expect(m_FabImport.Controller().GetStatus().State == Fab::FabImportState::Idle, "the controller is idle");
        check.Expect(m_EditorFabAdoptionReloads == 1, "the injected adoption failure must recover by reloading from disk once");
    }

    if (!check.Failure.empty())
        throw std::runtime_error("Fab control smoke failed: " + check.Failure);

    Engine::Log::Info("EditorFabControlV5 phase=", importPhase ? "import" : "reopen",
        " producer=external-python identity=session-pid-project paths=leaf-only controller=typed-actions"
        " liveState=cross-checked staging=clean input=mailbox-no-ui-synthesis result=pass");
    m_EditorFabSmokeCompleted = true;
    Engine::Application::Get().Close();
}

bool EditorLayer::RunFabPanelUiSmokeFrame()
{
    // Stages (each runs after the frame that displayed the previous state): idle,
    // import to AwaitingProvenance, incomplete / invalid / valid provenance,
    // confirm, ReadyToCommit and commit, Done and dismiss, a rejected package,
    // unsupported drops, a cancel, finish.
    if (m_FabPanelUiSmokeStage >= 14)
        return false;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!m_FabPanelUiSmokeContext)
    {
        m_FabPanelUiSmokeContext = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        m_FabPanelUiSmokeStageStart = now;
    }

    // One complete ImGui frame per call: ImGui asserts on unbalanced stacks and
    // duplicate identifiers when the frame ends.
    ImGui::NewFrame();
    m_FabImport.SetVisible(true);
    m_FabBrowser.SetVisible(m_FabPanelUiSmokeFrames % 2 == 0);
    m_FabBrowser.Draw();
    m_FabImport.Draw();
    ImGui::Render();
    ++m_FabPanelUiSmokeFrames;

    const auto fail = [this](const std::string& what)
    {
        ImGui::DestroyContext(m_FabPanelUiSmokeContext);
        m_FabPanelUiSmokeContext = nullptr;
        throw std::runtime_error("Fab panel UI smoke failed: " + what);
    };
    const auto advance = [&]()
    {
        ++m_FabPanelUiSmokeStage;
        m_FabPanelUiSmokeStageStart = now;
    };
    if (now - m_FabPanelUiSmokeStageStart > 20.0)
        fail("stage " + std::to_string(m_FabPanelUiSmokeStage) + " timed out in state "
            + Fab::ToString(m_FabImport.Controller().GetStatus().State));

    const Fab::FabImportStatus status = m_FabImport.Controller().GetStatus();
    const std::string good = m_FabPanelUiSmokeFixtures + "/good.glb";
    const auto submit = [&](const std::string& path)
    {
        m_FabImport.SubmitPaths(Fab::FabIntakeOrigin::Drop, std::span<const std::string>(&path, 1));
    };
    // Each stage runs after the frame that displayed the previous stage's state.
    Fab::FabProvenance provenance;
    provenance.ProductIdentity = "https://www.fab.com/listings/ui-smoke";
    provenance.ProductName = "UI Smoke";
    provenance.Publisher = "Smoke Publisher";
    provenance.VersionOrDownloadLabel = "v1";
    switch (m_FabPanelUiSmokeStage)
    {
        case 0:
            if (m_FabPanelUiSmokeFrames >= 3)
            {
                submit(good);
                advance();
            }
            break;
        case 1:
            if (status.State == Fab::FabImportState::AwaitingProvenance)
            {
                m_FabImport.ApplyProvenance(Fab::FabProvenance {});
                advance();
            }
            else if (Fab::IsTerminal(status.State))
                fail("the good package did not reach AwaitingProvenance: " + status.Message);
            break;
        case 2:
            // Incomplete (nothing chosen) was drawn; now a license without its attribution.
            provenance.LicenseFamily = Engine::FabLicenseFamily::CreativeCommonsAttribution;
            m_FabImport.ApplyProvenance(provenance);
            if (m_FabImport.Controller().GetStatus().ProvenanceValid)
                fail("CC-BY without attribution must be invalid");
            advance();
            break;
        case 3:
            provenance.LicenseFamily = Engine::FabLicenseFamily::CreativeCommonsAttribution;
            provenance.AttributionText = "Smoke by Publisher";
            provenance.AttributionLink = "https://www.fab.com/listings/ui-smoke";
            provenance.NoAI = Engine::FabMetadataFlag::No;
            provenance.GeneratedWithAI = Engine::FabMetadataFlag::No;
            m_FabImport.ApplyProvenance(provenance);
            if (!m_FabImport.Controller().GetStatus().ProvenanceValid)
                fail("a complete CC-BY provenance must be valid: " + m_FabImport.Controller().GetStatus().ProvenanceError);
            advance();
            break;
        case 4:
        {
            // The same method the Confirm button's click calls.
            std::string error;
            if (!m_FabImport.Confirm(status.ProvenanceDigest, error))
                fail("confirm refused: " + error);
            advance();
            break;
        }
        case 5:
            if (status.State == Fab::FabImportState::ReadyToCommit)
                advance();
            else if (Fab::IsTerminal(status.State))
                fail("cooking failed: " + status.Message);
            break;
        case 6:
        {
            // Assign to the selected entity's renderer when there is one.
            const FabImportPanel::CommitReport report = m_FabImport.Commit(GetFabAssignmentCandidate());
            if (!report.Ok)
                fail("commit failed: " + report.Error);
            advance();
            break;
        }
        case 7:
        {
            std::string error;
            if (status.State != Fab::FabImportState::Done || !status.ProjectChanged)
                fail("the import did not finish with a project change");
            const Fab::FabImportCommitResult* committed = m_FabImport.Controller().GetCommitResult();
            const Engine::MeshRendererComponent* prototype = m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity);
            if (!committed || !committed->AssignmentApplied || !prototype || prototype->MeshAsset != committed->MeshAsset
                || prototype->MaterialAsset != committed->MaterialHandle || !m_UndoHistory.empty() || !m_RedoHistory.empty()
                || m_EditorFabAdoptionReloads != 0)
                fail("in-memory adoption must apply the assignment, clear history, and not fall back to a reload");
            if (!m_FabImport.Dismiss(error))
                fail("dismiss refused: " + error);
            submit(m_FabPanelUiSmokeFixtures + "/badpng.glb");
            advance();
            break;
        }
        case 8:
            if (status.State == Fab::FabImportState::Failed)
                advance();
            else if (status.State == Fab::FabImportState::AwaitingProvenance)
                fail("a package with a corrupt texture must not reach AwaitingProvenance");
            break;
        case 9:
        {
            std::string error;
            m_FabImport.Dismiss(error);
            submit(m_FabPanelUiSmokeFixtures + "/notes.txt");
            submit(m_FabPanelUiSmokeFixtures + "/missing-package.zip");
            advance();
            break;
        }
        case 10:
            submit(good);
            advance();
            break;
        case 11:
            if (status.State == Fab::FabImportState::AwaitingProvenance)
            {
                m_FabImport.Cancel();
                advance();
            }
            else if (Fab::IsTerminal(status.State))
                fail("the cancel scenario ended early: " + status.Message);
            break;
        case 12:
            if (status.State == Fab::FabImportState::Cancelled)
            {
                std::string error;
                m_FabImport.Dismiss(error);
                if (m_FabProject.Revision != 1 || m_FabProject.Receipts.Receipts.size() != 1)
                    fail("the project should hold exactly the first import");
                advance();
            }
            break;
        case 13:
            ImGui::DestroyContext(m_FabPanelUiSmokeContext);
            m_FabPanelUiSmokeContext = nullptr;
            Engine::Log::Info("FabPanelUiSmokeV1 states=idle-snapshotting-awaiting-incomplete-invalid-valid-cooking-ready-done-"
                              "failed-rejected-cancelled frames=", m_FabPanelUiSmokeFrames,
                " browserPanel=placeholder-drawn input=no-ui-synthesis result=pass");
            advance();
            Engine::Application::Get().Close();
            break;
        default:
            break;
    }
    return true;
}
