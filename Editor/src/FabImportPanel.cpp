#include "FabImportPanel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>

namespace
{
    namespace fs = std::filesystem;

    constexpr size_t MaximumPendingRequests = 8;
    constexpr size_t MaximumMessages = 12;

    template<size_t Size>
    void CopyInto(std::array<char, Size>& target, const std::string& value)
    {
        std::snprintf(target.data(), target.size(), "%s", value.c_str());
    }

    const char* DescribeReason(Fab::FabIntakeReason reason)
    {
        switch (reason)
        {
            case Fab::FabIntakeReason::None: return "accepted";
            case Fab::FabIntakeReason::Missing: return "the path does not exist";
            case Fab::FabIntakeReason::NotRegularObject: return "not a regular file or folder (links and special files are refused)";
            case Fab::FabIntakeReason::Empty: return "the file is empty";
            case Fab::FabIntakeReason::TooLarge: return "the file is larger than the import limit";
            case Fab::FabIntakeReason::ExtensionContentMismatch: return "the file extension contradicts its contents";
            case Fab::FabIntakeReason::UnrecognizedContent: return "not a .zip, .glb or .gltf Fab package";
            case Fab::FabIntakeReason::IncompleteZip: return "the ZIP is incomplete or truncated";
            case Fab::FabIntakeReason::FolderWithoutGltf: return "the folder holds no .gltf or .glb";
            case Fab::FabIntakeReason::FolderTooLarge: return "the folder exceeds the entry or depth limit";
            case Fab::FabIntakeReason::TooManyPaths: return "too many paths in one drop";
            case Fab::FabIntakeReason::IoError: return "the path could not be read";
        }
        return "unsupported";
    }

    bool IsInside(const fs::path& child, const fs::path& root)
    {
        if (root.empty())
            return false;
        const fs::path relative = child.lexically_normal().lexically_relative(root.lexically_normal());
        return !relative.empty() && *relative.begin() != "..";
    }
}

FabImportPanel::FabImportPanel()
    : m_Controller(std::make_unique<Fab::FabImportController>())
{
}

FabImportPanel::~FabImportPanel() = default;

const char* FabImportPanel::KindToken(Fab::FabIntakeKind kind)
{
    switch (kind)
    {
        case Fab::FabIntakeKind::Zip: return "zip";
        case Fab::FabIntakeKind::Glb: return "glb";
        case Fab::FabIntakeKind::Gltf: return "gltf";
        case Fab::FabIntakeKind::Folder: return "directory";
        case Fab::FabIntakeKind::Unsupported: return "unsupported";
    }
    return "unsupported";
}

const char* FabImportPanel::ReasonToken(Fab::FabIntakeReason reason)
{
    switch (reason)
    {
        case Fab::FabIntakeReason::None: return "none";
        case Fab::FabIntakeReason::Missing: return "missing";
        case Fab::FabIntakeReason::NotRegularObject: return "not_regular_object";
        case Fab::FabIntakeReason::Empty: return "empty";
        case Fab::FabIntakeReason::TooLarge: return "too_large";
        case Fab::FabIntakeReason::ExtensionContentMismatch: return "extension_content_mismatch";
        case Fab::FabIntakeReason::UnrecognizedContent: return "unrecognized_content";
        case Fab::FabIntakeReason::IncompleteZip: return "incomplete_zip";
        case Fab::FabIntakeReason::FolderWithoutGltf: return "folder_without_gltf";
        case Fab::FabIntakeReason::FolderTooLarge: return "folder_too_large";
        case Fab::FabIntakeReason::TooManyPaths: return "too_many_paths";
        case Fab::FabIntakeReason::IoError: return "io_error";
    }
    return "unsupported";
}

const char* FabImportPanel::OriginToken(Fab::FabIntakeOrigin origin)
{
    switch (origin)
    {
        case Fab::FabIntakeOrigin::Drop: return "drop";
        case Fab::FabIntakeOrigin::Download: return "download";
        case Fab::FabIntakeOrigin::Typed: return "typed";
    }
    return "typed";
}

void FabImportPanel::Initialize(Host host, Fab::FabImportControllerConfig config)
{
    m_Host = std::move(host);
    m_Controller->Shutdown();
    m_Config = std::move(config);
    m_Controller = std::make_unique<Fab::FabImportController>(m_Config);
    m_ScratchRoot = m_Config.PackageStagingRoot.empty() ? Fab::DefaultFabPackageStagingRoot()
                                                        : m_Config.PackageStagingRoot;
}

void FabImportPanel::Shutdown()
{
    m_Pending.clear();
    m_Controller->Shutdown();
    m_Validator.Shutdown();
    CleanupDownload();
}

void FabImportPanel::AddMessage(std::string message)
{
    if (m_Host.Log)
        m_Host.Log(message);
    m_Messages.push_back(std::move(message));
    if (m_Messages.size() > MaximumMessages)
        m_Messages.erase(m_Messages.begin());
}

void FabImportPanel::CleanupDownload()
{
    if (m_Cleaned)
        return;
    m_Cleaned = true;
    if (m_Active.Origin != Fab::FabIntakeOrigin::Download || !IsInside(m_Active.Path, m_DownloadStagingRoot))
        return;
    // Only a file the browser staged inside the app-owned download directory is
    // removed, together with its per-download parent when that is empty.
    std::error_code error;
    if (fs::is_regular_file(fs::symlink_status(m_Active.Path, error)))
        fs::remove(m_Active.Path, error);
    const fs::path parent = m_Active.Path.parent_path();
    if (IsInside(parent, m_DownloadStagingRoot) && fs::is_directory(fs::symlink_status(parent, error)))
        fs::remove(parent, error);  // removes only an empty directory
}

void FabImportPanel::Update()
{
    m_Controller->Update();
    const Fab::FabImportStatus status = m_Controller->GetStatus();
    if (status.JobId != m_ObservedJob)
    {
        m_ObservedJob = status.JobId;
        m_SourceChecked = false;
    }
    if (status.State == Fab::FabImportState::AwaitingProvenance && !m_SourceChecked)
    {
        m_SourceChecked = true;
        if (!m_ExpectedSourceSha256.empty() && m_ExpectedSourceSha256 != status.SourceSha256)
        {
            m_Note = "source_sha256_mismatch: the selected package does not match the expected SHA-256";
            AddMessage("Import cancelled: " + m_Note);
            m_Controller->RequestCancel();
        }
    }
    if (Fab::IsTerminal(status.State))
        CleanupDownload();
    if (status.State == Fab::FabImportState::Idle && !m_Pending.empty())
    {
        const Fab::FabIntakeRequest next = m_Pending.front();
        m_Pending.pop_front();
        SubmitNow(next);
    }
    m_Validator.Poll();
}

bool FabImportPanel::SubmitNow(const Fab::FabIntakeRequest& request)
{
    if (!m_Controller->Submit(request))
    {
        AddMessage("Import refused: " + m_Controller->GetStatus().LastRejection);
        return false;
    }
    m_Active = request;
    m_Cleaned = false;
    m_ExpectedSourceSha256.clear();
    m_Visible = true;
    AddMessage(std::string("Importing ") + KindToken(request.Kind) + " package from " + OriginToken(request.Origin));
    return true;
}

void FabImportPanel::Enqueue(const Fab::FabIntakeRequest& request)
{
    if (m_Controller->GetStatus().State == Fab::FabImportState::Idle && m_Pending.empty())
    {
        SubmitNow(request);
        return;
    }
    if (m_Pending.size() >= MaximumPendingRequests)
    {
        AddMessage("Too many imports are waiting; dismiss the current import first");
        return;
    }
    m_Pending.push_back(request);
    AddMessage("Another import is active; this one will start after it is dismissed");
    m_Visible = true;
}

void FabImportPanel::SubmitPaths(Fab::FabIntakeOrigin origin, std::span<const std::string> utf8Paths)
{
    const Fab::FabIntakePlan plan = Fab::PlanFabIntake(origin, utf8Paths, m_Config.IntakeLimits);
    for (const Fab::FabIntakeRejection& rejection : plan.Rejected)
        AddMessage("Not imported: " + rejection.DisplayName + " (" + DescribeReason(rejection.Reason) + ")");
    for (const Fab::FabIntakeRequest& request : plan.Accepted)
        Enqueue(request);
    if (!plan.Rejected.empty() || !plan.Accepted.empty())
        m_Visible = true;
}

bool FabImportPanel::SelectTyped(const fs::path& path, std::string_view expectedKind,
    std::string_view expectedSha256, std::string& error)
{
    const Fab::FabImportStatus status = m_Controller->GetStatus();
    if (status.State != Fab::FabImportState::Idle)
    {
        error = "import_not_idle";
        return false;
    }
    const std::string utf8 = path.string();
    const Fab::FabIntakePlan plan = Fab::PlanFabIntake(
        Fab::FabIntakeOrigin::Typed, std::span<const std::string>(&utf8, 1), m_Config.IntakeLimits);
    if (plan.Accepted.size() != 1 || !plan.Rejected.empty())
    {
        error = std::string("intake_rejected_")
            + (plan.Rejected.empty() ? "unrecognized_content" : ReasonToken(plan.Rejected.front().Reason));
        return false;
    }
    if (expectedKind != KindToken(plan.Accepted.front().Kind))
    {
        error = "source_kind_mismatch";
        return false;
    }
    if (!SubmitNow(plan.Accepted.front()))
    {
        error = "submit_refused";
        return false;
    }
    m_ExpectedSourceSha256 = std::string(expectedSha256);
    return true;
}

bool FabImportPanel::ApplyProvenance(const Fab::FabProvenance& provenance)
{
    return m_Controller->SetProvenance(provenance);
}

bool FabImportPanel::Confirm(std::string_view expectedDigest, std::string& error)
{
    // The licence-kind gate, for the choices the controller cannot hold (code plugin,
    // Other) as well as every other one: the same verdict the typed action reports.
    {
        const Fab::FabImportStatus status = m_Controller->GetStatus();
        const Fab::FabLicenseGateResult gate = Fab::EvaluateFabLicenseGate(
            EffectiveLicenseChoice(status), status.Provenance.AttributionText, status.Provenance.NoAI);
        if (gate.Verdict == Fab::FabLicenseVerdict::Refused)
        {
            error = gate.Message;
            if (m_Host.Log)
                m_Host.Log("Fab provenance refused: " + error);
            return false;
        }
    }
    Fab::FabImportProjectContext context;
    if (!m_Host.BuildContext || !m_Host.BuildContext(std::nullopt, context, error))
    {
        if (error.empty())
            error = "the project is not available";
        return false;
    }
    if (!m_Controller->ConfirmProvenance(context, expectedDigest))
    {
        error = m_Controller->GetStatus().LastRejection;
        return false;
    }
    return true;
}

Fab::FabLicenseChoice FabImportPanel::EffectiveLicenseChoice(const Fab::FabImportStatus& status) const
{
    const Fab::FabLicenseChoice held = Fab::FabLicenseChoiceFromProvenance(status.Provenance);
    if (held == Fab::FabLicenseChoice::NotChosen && m_FormJob == status.JobId && m_FormDigest == status.ProvenanceDigest)
    {
        const size_t index = static_cast<size_t>(std::clamp(m_Form.LicenseChoice, 0, static_cast<int>(Fab::kFabLicenseChoices.size()) - 1));
        if (Fab::IsPanelOnlyLicenseChoice(Fab::kFabLicenseChoices[index]))
            return Fab::kFabLicenseChoices[index];
    }
    return held;
}

bool FabImportPanel::ChooseLicenseInForm(Fab::FabLicenseChoice choice)
{
    const Fab::FabImportStatus status = m_Controller->GetStatus();
    if (status.State != Fab::FabImportState::AwaitingProvenance)
        return false;
    SyncForm(status);
    for (size_t index = 0; index < Fab::kFabLicenseChoices.size(); ++index)
    {
        if (Fab::kFabLicenseChoices[index] == choice)
            m_Form.LicenseChoice = static_cast<int>(index);
    }
    m_Controller->SetProvenance(FormToProvenance());
    m_FormDigest = m_Controller->GetStatus().ProvenanceDigest;
    return true;
}

void FabImportPanel::AcknowledgeNoAiNotice()
{
    if (m_Form.NoAiAcknowledged)
        return;
    m_Form.NoAiAcknowledged = true;
    if (m_Host.Log)
        m_Host.Log("Fab import: the NoAI marking was acknowledged; the content must not be used to train or feed generative AI");
}

FabImportPanel::CommitReport FabImportPanel::Commit(std::optional<Fab::FabAssignmentTarget> assignment)
{
    CommitReport report;
    Fab::FabImportProjectContext context;
    if (!m_Host.BuildContext || !m_Host.BuildContext(std::move(assignment), context, report.Error))
    {
        if (report.Error.empty())
            report.Error = "the project is not available";
        return report;
    }
    const bool committed = m_Controller->Commit(context);
    const Fab::FabImportStatus status = m_Controller->GetStatus();
    if (!committed && status.State != Fab::FabImportState::FailedAfterCommit)
    {
        report.Error = status.LastRejection.empty() ? status.Message : status.LastRejection;
        return report;
    }
    if (status.ProjectChanged)
    {
        report.CommittedOnDisk = true;
        std::string adoptError;
        if (!m_Host.AdoptCommit || !m_Host.AdoptCommit(adoptError))
        {
            report.Error = "the project committed but could not be adopted: " + adoptError;
            AddMessage(report.Error);
            return report;
        }
    }
    if (status.State == Fab::FabImportState::FailedAfterCommit)
    {
        report.Error = status.Message;
        AddMessage("The import committed, but its post-commit validation failed; the project was reloaded from disk");
        return report;
    }
    report.Ok = true;
    AddMessage(status.ProjectChanged ? "Import committed; undo history was cleared"
                                     : "This package was already imported; nothing changed");
    return report;
}

void FabImportPanel::Cancel()
{
    m_Pending.clear();
    m_Controller->RequestCancel();
}

bool FabImportPanel::Dismiss(std::string& error)
{
    if (!m_Controller->Dismiss())
    {
        error = m_Controller->GetStatus().LastRejection;
        return false;
    }
    CleanupDownload();
    m_ExpectedSourceSha256.clear();
    m_Note.clear();
    return true;
}

void FabImportPanel::AbandonForProjectChange()
{
    m_Pending.clear();
    m_Controller->RequestCancel();
    m_Controller->Update();
    m_Controller->Shutdown();
    m_Controller->Dismiss();
    CleanupDownload();
    m_Validator.Shutdown();
}

Fab::FabProvenance FabImportPanel::FormToProvenance() const
{
    Fab::FabProvenance provenance;
    provenance.ProductIdentity = m_Form.ProductIdentity.data();
    provenance.ProductName = m_Form.ProductName.data();
    provenance.Publisher = m_Form.Publisher.data();
    provenance.VersionOrDownloadLabel = m_Form.Version.data();
    provenance.AttributionLink = m_Form.AttributionLink.data();
    provenance.AttributionText = m_Form.AttributionText.data();
    const size_t choiceIndex = static_cast<size_t>(std::clamp(m_Form.LicenseChoice, 0, static_cast<int>(Fab::kFabLicenseChoices.size()) - 1));
    const Fab::FabLicenseFields fields = Fab::FabLicenseFieldsForChoice(Fab::kFabLicenseChoices[choiceIndex]);
    provenance.LicenseFamily = fields.Family;
    provenance.LicenseTier = fields.Tier;
    const auto flag = [](int value)
    {
        return value == 1 ? Engine::FabMetadataFlag::No
                          : (value == 2 ? Engine::FabMetadataFlag::Yes : Engine::FabMetadataFlag::Unknown);
    };
    provenance.NoAI = flag(m_Form.NoAI);
    provenance.GeneratedWithAI = flag(m_Form.GeneratedWithAI);
    provenance.RawSourcePolicy = m_Form.RawSourcePolicy == 1 ? Engine::FabRawSourcePolicy::PrivateProjectOnly
                                                              : Engine::FabRawSourcePolicy::ExcludedFromProject;
    return provenance;
}

void FabImportPanel::ProvenanceToForm(const Fab::FabProvenance& provenance)
{
    CopyInto(m_Form.ProductIdentity, provenance.ProductIdentity);
    CopyInto(m_Form.ProductName, provenance.ProductName);
    CopyInto(m_Form.Publisher, provenance.Publisher);
    CopyInto(m_Form.Version, provenance.VersionOrDownloadLabel);
    CopyInto(m_Form.AttributionLink, provenance.AttributionLink);
    std::snprintf(m_Form.AttributionText.data(), m_Form.AttributionText.size(), "%s",
        provenance.AttributionText.c_str());
    const Fab::FabLicenseChoice held = Fab::FabLicenseChoiceFromProvenance(provenance);
    m_Form.LicenseChoice = 0;
    for (size_t index = 0; index < Fab::kFabLicenseChoices.size(); ++index)
    {
        if (Fab::kFabLicenseChoices[index] == held)
            m_Form.LicenseChoice = static_cast<int>(index);
    }
    const auto index = [](Engine::FabMetadataFlag value)
    {
        return value == Engine::FabMetadataFlag::No ? 1 : (value == Engine::FabMetadataFlag::Yes ? 2 : 0);
    };
    m_Form.NoAI = index(provenance.NoAI);
    m_Form.GeneratedWithAI = index(provenance.GeneratedWithAI);
    m_Form.RawSourcePolicy = provenance.RawSourcePolicy == Engine::FabRawSourcePolicy::PrivateProjectOnly ? 1 : 0;
}

void FabImportPanel::SyncForm(const Fab::FabImportStatus& status)
{
    // The form mirrors the controller's provenance: a new job, or a change made
    // through the typed SetFabProvenance, reloads the widgets.
    if (status.JobId != m_FormJob || status.ProvenanceDigest != m_FormDigest)
    {
        if (status.JobId != m_FormJob)
            m_Form.NoAiAcknowledged = false;
        ProvenanceToForm(status.Provenance);
        m_FormJob = status.JobId;
        m_FormDigest = status.ProvenanceDigest;
    }
}

void FabImportPanel::Draw()
{
    if (!m_Visible)
        return;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Fab Import", &m_Visible))
    {
        ImGui::End();
        return;
    }

    const Fab::FabImportStatus status = m_Controller->GetStatus();
    if (status.State == Fab::FabImportState::Idle)
        DrawIdle();
    else
        DrawJob(status);

    ImGui::Separator();
    ImGui::TextUnformatted("Project validation");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", FabEditor::ProjectValidator::ToString(m_Validator.GetState()));
    if (!m_Validator.GetMessage().empty())
        ImGui::TextWrapped("%s", m_Validator.GetMessage().c_str());
    for (const std::string& message : m_Messages)
        ImGui::TextWrapped("%s", message.c_str());
    ImGui::End();
}

void FabImportPanel::DrawIdle()
{
    ImGui::TextWrapped("Drop a Fab .zip, .glb, .gltf or package folder onto the Editor, download one from the Fab browser, "
                       "or enter its path.");
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputTextWithHint("##FabPath", "Path to a .zip, .glb, .gltf or folder", m_PathField.data(), m_PathField.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(m_PathField[0] == '\0');
    if (ImGui::Button("Import"))
    {
        const std::string path = m_PathField.data();
        SubmitPaths(Fab::FabIntakeOrigin::Typed, std::span<const std::string>(&path, 1));
    }
    ImGui::EndDisabled();
    if (!m_Pending.empty())
        ImGui::TextDisabled("%zu import(s) waiting", m_Pending.size());
}

void FabImportPanel::DrawJob(const Fab::FabImportStatus& status)
{
    ImGui::Text("State: %s", Fab::ToString(status.State));
    ImGui::TextWrapped("Source: %s (%s, %s)", status.SourceName.c_str(), KindToken(status.SourceKind),
        OriginToken(status.SourceOrigin));

    const bool active = !Fab::IsTerminal(status.State);
    const bool working = status.State == Fab::FabImportState::Snapshotting
        || status.State == Fab::FabImportState::Preparing || status.State == Fab::FabImportState::Cooking
        || status.State == Fab::FabImportState::Committing;
    if (working)
    {
        const float fraction = status.BytesTotal > 0
            ? static_cast<float>(static_cast<double>(status.BytesCompleted) / static_cast<double>(status.BytesTotal))
            : (status.FileCount > 0
                    ? static_cast<float>(static_cast<double>(status.FilesCompleted) / static_cast<double>(status.FileCount))
                    : 0.0f);
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%llu / %llu files",
            static_cast<unsigned long long>(status.FilesCompleted), static_cast<unsigned long long>(status.FileCount));
        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), overlay);
    }
    if (status.CancelRequested && active)
        ImGui::TextDisabled("Cancelling...");

    if (status.State == Fab::FabImportState::AwaitingProvenance)
        DrawProvenanceForm(status);
    else if (status.State == Fab::FabImportState::ReadyToCommit)
        DrawReadyToCommit(status);

    if (!status.LastRejection.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", status.LastRejection.c_str());
    if (!m_Note.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", m_Note.c_str());
    if (!active)
    {
        const bool good = status.State == Fab::FabImportState::Done;
        ImGui::TextColored(good ? ImVec4(0.45f, 0.85f, 0.5f, 1.0f) : ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s",
            status.Message.c_str());
        if (status.HasDecision)
            ImGui::Text("Relation: %s", Engine::ToString(status.Decision));
        std::string ignored;
        if (ImGui::Button("Dismiss"))
            Dismiss(ignored);
    }
    else if (status.State != Fab::FabImportState::Committing)
    {
        if (ImGui::Button("Cancel import"))
            Cancel();
    }
}

void FabImportPanel::DrawProvenanceForm(const Fab::FabImportStatus& status)
{
    ImGui::Separator();
    ImGui::TextUnformatted("Package summary");
    ImGui::Text("Format %s: %u vertices, %u triangles, %u primitive instance(s)", Engine::ToString(status.Format),
        status.Summary.VertexCount, status.Summary.TriangleCount, status.Summary.PrimitiveInstanceCount);
    ImGui::Text("Material: %s; %llu source file(s), %llu bytes", status.Summary.MaterialName.c_str(),
        static_cast<unsigned long long>(status.Summary.SourceFileCount),
        static_cast<unsigned long long>(status.Summary.SourceBytes));
    for (const Fab::FabImportTextureSummary& texture : status.Summary.Textures)
        ImGui::BulletText("%s %ux%u", texture.Role.c_str(), texture.Width, texture.Height);
    ImGui::TextWrapped("Source SHA-256: %s", status.SourceSha256.c_str());
    ImGui::TextWrapped("Expanded tree SHA-256: %s", status.ExpandedTreeSha256.c_str());

    ImGui::Separator();
    ImGui::TextWrapped("Enter what the Fab listing shows. Spiral never chooses a license or tier for you, and confirming "
                       "records these declarations in the project.");
    SyncForm(status);
    bool edited = false;
    edited |= ImGui::InputTextWithHint("Listing URL", "https://www.fab.com/listings/<id>", m_Form.ProductIdentity.data(),
        m_Form.ProductIdentity.size());
    edited |= ImGui::InputText("Product name", m_Form.ProductName.data(), m_Form.ProductName.size());
    edited |= ImGui::InputText("Publisher", m_Form.Publisher.data(), m_Form.Publisher.size());
    edited |= ImGui::InputText("Version / download label", m_Form.Version.data(), m_Form.Version.size());

    // The licence and tier in one choice, as the listing's Details panel states them.
    // Reference-Only, code-plugin, UE-only and Other are listed so they can be declared
    // truthfully; confirming one of them is refused with the reason.
    static const std::array<std::string, Fab::kFabLicenseChoices.size()> licenseLabels = []
    {
        std::array<std::string, Fab::kFabLicenseChoices.size()> labels;
        for (size_t index = 0; index < labels.size(); ++index)
            labels[index] = std::string(Fab::FabLicenseChoiceLabel(Fab::kFabLicenseChoices[index]));
        return labels;
    }();
    static const std::array<const char*, Fab::kFabLicenseChoices.size()> licenseLabelPointers = []
    {
        std::array<const char*, Fab::kFabLicenseChoices.size()> pointers {};
        for (size_t index = 0; index < pointers.size(); ++index)
            pointers[index] = licenseLabels[index].c_str();
        return pointers;
    }();
    edited |= ImGui::Combo("License", &m_Form.LicenseChoice, licenseLabelPointers.data(), static_cast<int>(licenseLabelPointers.size()));
    const bool attributionShown = Fab::kFabLicenseChoices[static_cast<size_t>(std::clamp(
                                      m_Form.LicenseChoice, 0, static_cast<int>(Fab::kFabLicenseChoices.size()) - 1))]
            == Fab::FabLicenseChoice::CcBy
        || m_Form.AttributionText[0] != '\0';
    if (attributionShown)
    {
        edited |= ImGui::InputTextMultiline("Attribution text", m_Form.AttributionText.data(), m_Form.AttributionText.size(),
            ImVec2(-1.0f, 64.0f));
        edited |= ImGui::InputText("Attribution link", m_Form.AttributionLink.data(), m_Form.AttributionLink.size());
    }
    const char* flags[] = { "Unknown", "No", "Yes" };
    edited |= ImGui::Combo("Marked No-AI", &m_Form.NoAI, flags, IM_ARRAYSIZE(flags));
    if (m_Form.NoAI == 2)
    {
        // The Confirm button waits for this tick. Receipt schema 1 has no field for the
        // acknowledgement itself (and there is no provenance note field), so what is
        // recorded is the declared Marked No-AI value of a user-confirmed receipt; the
        // tick lasts for this job and is logged.
        bool acknowledged = m_Form.NoAiAcknowledged;
        if (ImGui::Checkbox("I acknowledge the NoAI marking", &acknowledged) && acknowledged)
            AcknowledgeNoAiNotice();
        else if (!acknowledged)
            m_Form.NoAiAcknowledged = false;
        ImGui::TextDisabled("NoAI content must not be used to train or feed generative AI. Tick the box to enable Confirm.");
    }
    edited |= ImGui::Combo("Generated with AI", &m_Form.GeneratedWithAI, flags, IM_ARRAYSIZE(flags));
    const char* policies[] = { "Exclude raw source from the project", "Keep raw source in the private project" };
    edited |= ImGui::Combo("Raw source", &m_Form.RawSourcePolicy, policies, IM_ARRAYSIZE(policies));

    if (edited)
    {
        const Fab::FabProvenance provenance = FormToProvenance();
        m_Controller->SetProvenance(provenance);
        const Fab::FabImportStatus updated = m_Controller->GetStatus();
        m_FormDigest = updated.ProvenanceDigest;
    }

    const Fab::FabImportStatus current = m_Controller->GetStatus();
    const Fab::FabLicenseGateResult gate = Fab::EvaluateFabLicenseGate(
        EffectiveLicenseChoice(current), current.Provenance.AttributionText, current.Provenance.NoAI);
    const bool refused = gate.Verdict == Fab::FabLicenseVerdict::Refused;
    if (refused)
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", gate.Message.c_str());
        ImGui::PopTextWrapPos();
    }
    else if (!current.ProvenanceValid)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", current.ProvenanceError.c_str());
    }
    const bool noAiPending = current.Provenance.NoAI == Engine::FabMetadataFlag::Yes && !m_Form.NoAiAcknowledged;
    const bool confirmEnabled = current.ProvenanceValid && !refused && !noAiPending;
    m_LastDraw = { Fab::FabLicenseChoiceLabel(EffectiveLicenseChoice(current)), gate.Verdict, confirmEnabled,
        current.Provenance.NoAI == Engine::FabMetadataFlag::Yes, gate.Message };
    ImGui::BeginDisabled(!confirmEnabled);
    if (ImGui::Button("Confirm provenance"))
    {
        // The only place the UI confirms: an explicit click on the visible button.
        std::string error;
        if (!Confirm(current.ProvenanceDigest, error))
        {
            m_Note = error;
            AddMessage("Confirm refused: " + error);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
}

void FabImportPanel::DrawReadyToCommit(const Fab::FabImportStatus& status)
{
    ImGui::Separator();
    ImGui::TextWrapped("Stream: %s", status.StreamId.c_str());
    ImGui::TextWrapped("Generation: %s", status.GenerationId.c_str());
    ImGui::Text("Relation: %s", status.HasDecision ? Engine::ToString(status.Decision) : "unknown");
    if (!status.DecisionDiagnostic.empty())
        ImGui::TextWrapped("%s", status.DecisionDiagnostic.c_str());

    std::optional<Fab::FabAssignmentTarget> target;
    if (m_Host.SelectedAssignment)
        target = m_Host.SelectedAssignment();
    ImGui::BeginDisabled(!target);
    ImGui::Checkbox("Assign to the selected entity's mesh renderer", &m_AssignToSelection);
    ImGui::EndDisabled();

    ImGui::TextWrapped("Committing writes a new project revision (the Scene and asset registry are saved with it) and "
                       "clears undo history. It cannot be undone.");
    const bool commitable = status.HasDecision
        && (status.Decision == Engine::FabReceiptDecisionKind::ExactReuse
            || status.Decision == Engine::FabReceiptDecisionKind::AddNewStream
            || status.Decision == Engine::FabReceiptDecisionKind::ReplaceSameStreamSource
            || status.Decision == Engine::FabReceiptDecisionKind::AddProductUpdateStream);
    ImGui::BeginDisabled(!commitable);
    if (ImGui::Button("Commit import"))
    {
        const CommitReport report = Commit(m_AssignToSelection && target ? target : std::nullopt);
        if (!report.Ok)
            m_Note = report.Error;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
}
