// EditorLayer members for the project location model and the Fab import
// integration. They live here so EditorLayer.cpp only carries the hook calls.
#include "EditorLayer.h"

#include "Engine/Assets/FabProjectState.h"
#include "Engine/Assets/ProjectManifest.h"
#include "Engine/Assets/FabZipStaging.h"
#include "Engine/Core/Sha256.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    bool IsInside(const fs::path& child, const fs::path& root)
    {
        if (root.empty() || child.empty())
            return false;
        const fs::path relative = child.lexically_normal().lexically_relative(root.lexically_normal());
        return !relative.empty() && *relative.begin() != "..";
    }

    fs::path ExecutableDirectory()
    {
#if defined(__linux__)
        std::error_code error;
        const fs::path executable = fs::read_symlink("/proc/self/exe", error);
        if (!error && !executable.empty())
            return executable.parent_path();
#endif
        std::error_code error2;
        return fs::current_path(error2);
    }

    // ${environment variable} when it is an absolute path, else $HOME/<fallback>.
    fs::path XdgBase(const char* variable, const char* homeFallback)
    {
        const char* value = std::getenv(variable);
        if (value && *value && fs::path(value).is_absolute())
            return fs::path(value);
        const char* home = std::getenv("HOME");
        if (home && *home && fs::path(home).is_absolute())
            return fs::path(home) / homeFallback;
        return {};
    }

    // Creates (0700) and canonicalizes an app-owned directory.
    bool PreparePrivateDirectory(const fs::path& path, fs::path& canonical, std::string& error)
    {
        if (path.empty())
        {
            error = "no home or XDG directory is available";
            return false;
        }
        if (!Engine::PrepareFabStagingRoot(path, error))
            return false;
        std::error_code filesystemError;
        canonical = fs::canonical(path, filesystemError);
        if (filesystemError || !canonical.is_absolute())
        {
            error = "the directory could not be canonicalized";
            return false;
        }
        return true;
    }

    std::string UniqueEntityName(const Engine::Scene& scene, std::string base)
    {
        if (base.empty())
            base = "Mesh";
        if (!scene.FindEntityByName(base))
            return base;
        for (unsigned int suffix = 2; suffix < 100000; ++suffix)
        {
            std::string candidate = base + " " + std::to_string(suffix);
            if (!scene.FindEntityByName(candidate))
                return candidate;
        }
        return base;
    }

    bool IsValidInboxLeaf(std::string_view leaf)
    {
        if (leaf.empty() || leaf.size() > 128 || leaf.front() == '.' || leaf.find("..") != std::string_view::npos)
            return false;
        return std::all_of(leaf.begin(), leaf.end(), [](char character)
        {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
        });
    }
}

// ---------------------------------------------------------------- project location

void EditorLayer::RefreshProjectLocation()
{
    const fs::path manifest(m_ProjectPath);
    if (manifest.is_absolute())
    {
        // A project named by absolute path keeps every manifest path relative to
        // the manifest's own directory.
        m_ProjectRoot = manifest.parent_path();
        m_ProjectManifestRelativePath = manifest.filename().generic_string();
        m_ProjectPathsRelativeToManifest = true;
        return;
    }
    // The default project keeps its legacy working-directory-relative paths.
    std::error_code error;
    m_ProjectRoot = fs::current_path(error);
    m_ProjectManifestRelativePath = manifest.lexically_normal().generic_string();
    m_ProjectPathsRelativeToManifest = false;
}

std::filesystem::path EditorLayer::ResolveProjectPath(std::string_view path, bool searchLegacyRoots) const
{
    const fs::path relative { std::string(path) };
    if (relative.is_absolute())
        return relative;
    if (m_ProjectPathsRelativeToManifest)
        return m_ProjectRoot / relative;
    return searchLegacyRoots ? Engine::AssetFileSystem::ResolvePath(path) : relative;
}

bool EditorLayer::RefreshManifestDigest()
{
    std::string digest;
    std::string error;
    if (!FabEditor::ReadManifestSha256({ m_ProjectRoot, m_ProjectManifestRelativePath }, digest, error))
    {
        Engine::Log::Error("Could not hash the project manifest: ", error);
        return false;
    }
    m_FabProject.ManifestSha256 = std::move(digest);
    return true;
}

void EditorLayer::ResetFabProjectState(const Engine::FabProjectState& loaded, std::string manifestSha256)
{
    m_FabProject.Receipts = loaded.Receipts;
    m_FabProject.ReceiptsPath = loaded.Manifest.FabReceiptsPath;
    m_FabProject.Revision = loaded.Manifest.ProjectRevision;
    m_FabProject.ManifestSha256 = std::move(manifestSha256);
    m_FabProject.StructuralStatus = "passed";
    m_FabProject.StructuralMessage = std::to_string(loaded.Receipts.Receipts.size()) + " receipt(s), revision "
        + std::to_string(loaded.Manifest.ProjectRevision);
}

bool EditorLayer::OpenCommandLineProject(const Engine::ApplicationCommandLineArgs& args)
{
    bool provided = false;
    for (int index = 0; index < args.Count; ++index)
    {
        const std::string_view argument = args[index];
        provided |= argument == "--project" || argument.starts_with("--project=");
    }
    if (!provided)
        return false;
    const std::string_view value = args.GetOptionValue("--project");
    if (value.empty())
        throw std::runtime_error("--project requires a .spiralproject manifest path");

    std::error_code error;
    fs::path manifest = fs::absolute(fs::path(std::string(value)), error);
    if (error)
        throw std::runtime_error("--project path could not be resolved");
    const fs::path canonical = fs::weakly_canonical(manifest, error);
    manifest = error ? manifest.lexically_normal() : canonical;
    if (manifest.extension() != ".spiralproject")
        throw std::runtime_error("--project must name a .spiralproject manifest");

    m_ProjectPath = manifest.string();
    RefreshProjectLocation();
    if (fs::exists(manifest, error))
    {
        if (!LoadProject())
            throw std::runtime_error("could not open the --project manifest " + m_ProjectPath);
        return true;
    }

    // A named project that does not exist yet is created with the default scene,
    // entirely beneath its own directory.
    fs::create_directories(m_ProjectRoot / "Scenes", error);
    fs::create_directories(m_ProjectRoot / "Assets", error);
    if (error)
        throw std::runtime_error("could not create the --project directory");
    m_FabProject = {};
    m_ScenePath = "Scenes/Main.spiral";
    m_AssetRegistryPath = "Assets/assets.spiralassets";
    EnsureDefaultSceneEntities();
    if (!SaveProject())
        throw std::runtime_error("could not create the --project manifest " + m_ProjectPath);
    m_AssetRegistry.SetCookedArtifactBasePath(m_ProjectRoot / "Assets");
    m_ConsoleLines.emplace_back("Project created: " + m_ProjectPath);
    return true;
}

// ---------------------------------------------------------------- integration lifecycle

void EditorLayer::InitializeFabIntegration()
{
    FabImportPanel::Host host;
    host.BuildContext = [this](std::optional<Fab::FabAssignmentTarget> assignment,
                            Fab::FabImportProjectContext& context, std::string& error)
    {
        return BuildFabImportProjectContext(std::move(assignment), context, error);
    };
    host.AdoptCommit = [this](std::string& error) { return AdoptFabImportCommit(error); };
    host.SelectedAssignment = [this]() { return GetFabAssignmentCandidate(); };
    host.Log = [this](const std::string& message)
    {
        m_ConsoleLines.emplace_back(message);
        Engine::Log::Info("FabImport: ", message);
    };
    m_FabImport.Initialize(std::move(host));

    if (Engine::Application::Get().GetSpecification().Window.Headless)
    {
        m_FabIntegrationInitialized = true;
        return;
    }

    // The browser profile and download staging are app-owned, canonical, owner-only
    // and distinct, and never inside a project.
    fs::path profile;
    fs::path downloads;
    std::string error;
    const fs::path dataBase = XdgBase("XDG_DATA_HOME", ".local/share");
    const fs::path cacheBase = XdgBase("XDG_CACHE_HOME", ".cache");
    const bool prepared = PreparePrivateDirectory(
                              dataBase.empty() ? fs::path() : dataBase / "Spiral" / "FabBrowser" / "Profile", profile, error)
        && PreparePrivateDirectory(
            cacheBase.empty() ? fs::path() : cacheBase / "Spiral" / "FabBrowser" / "Downloads", downloads, error);
    if (!prepared || profile == downloads || IsInside(profile, m_ProjectRoot) || IsInside(downloads, m_ProjectRoot))
    {
        Engine::Log::Warn("Fab browser panel not configured: ",
            prepared ? "its directories would lie inside the project" : error);
        m_ConsoleLines.emplace_back("Fab browser is unavailable: " + (prepared ? std::string("unsafe directories") : error));
    }
    else
    {
        Fab::BrowserPanelConfig config;
        config.EditorDirectory = ExecutableDirectory();
        config.ProfileDirectory = profile;
        config.DownloadStagingDirectory = downloads;
        const Engine::ApplicationCommandLineArgs& commandLine = Engine::Application::Get().GetSpecification().CommandLineArgs;
        constexpr std::string_view providerOption = "--fab-provider-host=";
        for (int index = 1; index < commandLine.Count; ++index)
        {
            const std::string_view argument = commandLine[index];
            if (argument.starts_with(providerOption) && argument.size() > providerOption.size())
                config.ProviderHosts.emplace_back(argument.substr(providerOption.size()));
        }
        m_FabBrowser.Configure(std::move(config));
        m_FabImport.SetDownloadStagingRoot(downloads);
    }
    m_FabIntegrationInitialized = true;
}

void EditorLayer::ShutdownFabIntegration()
{
    if (!m_FabIntegrationInitialized)
        return;
    m_FabIntegrationInitialized = false;
    // Browser first: it pumps until its helper processes are gone, and must finish
    // before ImGui, the renderer and the window are torn down.
    m_FabBrowser.Shutdown();
    m_FabImport.Shutdown();
}

void EditorLayer::UpdateFabIntegration()
{
    m_FabImport.Update();
}

void EditorLayer::DrawFabIntegration()
{
    m_FabBrowser.Draw();
    m_FabImport.Draw();
}

void EditorLayer::PollFabDownloads()
{
    Fab::BrowserPanelDownload download;
    while (m_FabBrowser.TryTakeCompletedDownload(download))
    {
        const std::string path = download.StagedPath.string();
        // A completed download enters the controller exactly like a drop.
        m_FabImport.SubmitPaths(Fab::FabIntakeOrigin::Download, std::span<const std::string>(&path, 1));
    }
}

// ---------------------------------------------------------------- commit and adoption

bool EditorLayer::BuildFabImportProjectContext(std::optional<Fab::FabAssignmentTarget> assignment,
    Fab::FabImportProjectContext& out, std::string& error)
{
    FabEditor::ContextInputs inputs;
    inputs.Location = { m_ProjectRoot, m_ProjectManifestRelativePath };
    inputs.Registry = &m_AssetRegistry;
    inputs.FabProject = &m_FabProject;
    inputs.Scene = &m_ActiveScene;
    inputs.Assignment = std::move(assignment);
    inputs.ScratchRoot = m_FabImport.GetScratchRoot();
    return FabEditor::BuildImportContext(inputs, out, error);
}

std::optional<Fab::FabAssignmentTarget> EditorLayer::GetFabAssignmentCandidate() const
{
    const Engine::MeshRendererComponent* renderer = m_ActiveScene.TryGetMeshRendererComponent(m_SelectedEntity);
    if (!renderer || !m_ActiveScene.IsEntityValid(m_SelectedEntity))
        return std::nullopt;
    return Fab::FabAssignmentTarget { m_SelectedEntity.Id, renderer->MeshAsset, renderer->MaterialAsset };
}

bool EditorLayer::ReloadCommittedFabProject(std::string& error)
{
    // The manifest on disk is the commit pointer; the live state is rebuilt from it.
    ++m_EditorFabAdoptionReloads;
    m_ConsoleLines.emplace_back("Reloading the committed project from disk");
    const bool loaded = LoadProject();
    m_UndoHistory.clear();
    m_RedoHistory.clear();
    if (!loaded)
        error = "the committed project could not be reloaded from disk";
    return loaded;
}

bool EditorLayer::AdoptFabImportCommit(std::string& error)
{
    const Fab::FabImportCommitResult* result = m_FabImport.Controller().GetCommitResult();
    const Fab::FabImportStatus status = m_FabImport.Controller().GetStatus();
    if (!result || !result->ProjectChanged)
    {
        error = "there is no committed change to adopt";
        return false;
    }
    if (status.State == Fab::FabImportState::FailedAfterCommit)
        return ReloadCommittedFabProject(error);
    if (m_EditorFabForceAdoptionFailureOnce)
    {
        // Smoke seam: prove the recovery path rebuilds the live state from the
        // committed manifest when in-memory adoption cannot finish.
        m_EditorFabForceAdoptionFailureOnce = false;
        error = "injected adoption failure";
        return ReloadCommittedFabProject(error);
    }

    Engine::Scene assignedScene;
    if (result->AssignmentApplied
        && !FabEditor::DeserializeScene(result->SceneBytes, m_FabImport.GetScratchRoot(), assignedScene, error))
        return ReloadCommittedFabProject(error);

    FabEditor::AdoptionTargets targets { m_AssetRegistry, m_MaterialLibrary, m_FabProject, m_ScenePath, m_AssetRegistryPath };
    if (!FabEditor::AdoptCommitResult(*result, targets, error))
        return ReloadCommittedFabProject(error);

    // Undo barrier: undo snapshots embed the registry and materials, so undoing
    // across this commit would resurrect a registry without the committed assets.
    if (result->Commit.UndoBarrier.ClearUndoRedoHistory)
    {
        m_UndoHistory.clear();
        m_RedoHistory.clear();
    }
    if (result->AssignmentApplied)
    {
        m_ActiveScene = std::move(assignedScene);
        m_PrototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
        m_DirectionalLightEntity = m_ActiveScene.FindEntityByName("Directional Light");
        m_PlayerStartEntity = m_ActiveScene.FindEntityByName("Player Start");
        if (!m_ActiveScene.IsEntityValid(m_SelectedEntity))
            m_SelectedEntity = m_PrototypeMeshEntity ? m_PrototypeMeshEntity : m_ActiveScene.GetMainCameraEntity();
        SyncEditorCameraStateFromMainCamera(true);
        ResetFusionNavigationPivotFromSelectionOrScene();
    }
    // One resolver publication: older renderer snapshots keep their own roots.
    Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);
    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    const std::string summary = "Fab import adopted: project revision " + std::to_string(m_FabProject.Revision);
    m_ConsoleLines.emplace_back(summary);
    Engine::Log::Info(summary, " generation=", status.GenerationId);
    return true;
}

// ---------------------------------------------------------------- placing meshes

Engine::AssetHandle EditorLayer::FindFabMaterialForMesh(Engine::AssetHandle mesh) const
{
    // The material comes from the same Fab receipt that lists the mesh; later
    // receipts win so a replacement generation's pairing is preferred.
    const auto& receipts = m_FabProject.Receipts.Receipts;
    for (auto receipt = receipts.rbegin(); receipt != receipts.rend(); ++receipt)
    {
        bool hasMesh = false;
        Engine::AssetHandle material = Engine::kInvalidAssetHandle;
        for (const Engine::FabImportedAssetRecord& asset : receipt->Assets)
        {
            hasMesh |= asset.Type == Engine::AssetType::Mesh && asset.Handle == mesh;
            if (asset.Type == Engine::AssetType::Material)
                material = asset.Handle;
        }
        if (hasMesh && material != Engine::kInvalidAssetHandle && m_AssetRegistry.Contains(material))
            return material;
    }
    return Engine::kInvalidAssetHandle;
}

Engine::Entity EditorLayer::PlaceMeshAssetInScene(Engine::AssetHandle mesh, std::string& error)
{
    const Engine::AssetMetadata* meshMetadata = m_AssetRegistry.GetAsset(mesh);
    if (!meshMetadata || meshMetadata->Type != Engine::AssetType::Mesh)
    {
        error = "mesh_asset_not_registered_mesh";
        return {};
    }
    Engine::AssetHandle material = FindFabMaterialForMesh(mesh);
    if (material == Engine::kInvalidAssetHandle)
    {
        // Not a Fab mesh: the project's default material.
        const Engine::MeshRendererComponent* prototype = m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity);
        material = prototype ? prototype->MaterialAsset : Engine::kInvalidAssetHandle;
    }

    const HistoryState before = CaptureHistoryState();
    const Engine::Entity entity = m_ActiveScene.CreateEntity(UniqueEntityName(m_ActiveScene, meshMetadata->Name));
    Engine::MeshRendererComponent renderer;
    renderer.MeshAsset = mesh;
    renderer.MaterialAsset = material;
    renderer.MeshName = meshMetadata->Name;
    if (!entity || !m_ActiveScene.AddMeshRendererComponent(entity, renderer))
    {
        error = "entity_creation_failed";
        if (entity)
            m_ActiveScene.DestroyEntity(entity);
        return {};
    }

    // In front of the editor camera, along its view direction.
    const double yaw = Engine::Math::DegreesToRadians(m_CameraRotation[1]);
    const double pitch = Engine::Math::DegreesToRadians(m_CameraRotation[0]);
    constexpr double placementDistance = 3.35;
    const Engine::Math::DVec3 position {
        m_CameraPosition[0] + std::sin(yaw) * std::cos(pitch) * placementDistance,
        m_CameraPosition[1] - std::sin(pitch) * placementDistance,
        m_CameraPosition[2] + std::cos(yaw) * std::cos(pitch) * placementDistance
    };
    if (!m_ActiveScene.SetEntityWorldPosition(entity, position))
    {
        m_ActiveScene.DestroyEntity(entity);
        error = "placement_position_is_not_publishable";
        return {};
    }
    m_SelectedEntity = entity;
    RecordHistory("Place mesh asset", before);
    return entity;
}

bool EditorLayer::OnFabAssetDrop(Engine::AssetHandle handle)
{
    std::string error;
    const Engine::Entity entity = PlaceMeshAssetInScene(handle, error);
    if (!entity)
    {
        m_ConsoleLines.emplace_back("Could not place the dropped mesh: " + error);
        return false;
    }
    m_ConsoleLines.emplace_back("Placed mesh asset " + std::to_string(handle) + " as entity " + std::to_string(entity.Id));
    ResetFusionNavigationPivotFromSelectionOrScene();
    return true;
}

// ---------------------------------------------------------------- typed control

void EditorLayer::FillFabReceiptBlock(EditorFabControlReceipt& block) const
{
    const Fab::FabImportStatus status = m_FabImport.Controller().GetStatus();
    const Fab::FabImportCommitResult* result = m_FabImport.Controller().GetCommitResult();
    const bool idle = status.State == Fab::FabImportState::Idle;

    block.State = Fab::ToString(status.State);
    block.JobId = status.JobId;
    block.CancelRequested = status.CancelRequested;
    block.FilesCompleted = status.FilesCompleted;
    block.FileCount = status.FileCount;
    block.BytesCompleted = status.BytesCompleted;
    block.BytesTotal = status.BytesTotal;
    block.SourceKind = idle ? "none" : FabImportPanel::KindToken(status.SourceKind);
    block.SourceOrigin = idle ? "none" : FabImportPanel::OriginToken(status.SourceOrigin);
    block.SourceName = status.SourceName;
    block.ErrorCode = Fab::ToString(status.Error);
    block.Message = status.Message;
    block.LastRejection = status.LastRejection;
    block.Note = m_FabImport.GetNote();
    block.Format = status.Format == Engine::FabPackageFormat::Unknown ? "none" : Engine::ToString(status.Format);
    block.SourceSha256 = status.SourceSha256;
    block.ExpandedTreeSha256 = status.ExpandedTreeSha256;
    block.SummaryVertices = status.Summary.VertexCount;
    block.SummaryTriangles = status.Summary.TriangleCount;
    block.SummaryPrimitives = status.Summary.PrimitiveInstanceCount;
    block.SummaryTextures = status.Summary.Textures.size();
    block.SummaryFiles = status.Summary.SourceFileCount;
    block.SummaryBytes = status.Summary.SourceBytes;
    block.SummaryMaterial = status.Summary.MaterialName;
    block.ProvenanceValid = status.ProvenanceValid;
    block.ProvenanceConfirmed = status.ProvenanceConfirmed;
    block.ProvenanceDigest = status.State == Fab::FabImportState::Idle ? std::string() : status.ProvenanceDigest;
    block.ProvenanceError = status.ProvenanceError;
    block.Relation = status.HasDecision ? Engine::ToString(status.Decision) : "none";
    block.StreamId = status.StreamId;
    block.GenerationId = status.GenerationId;
    block.ProjectChanged = status.ProjectChanged;
    block.AssignmentApplied = result && result->AssignmentApplied;
    block.CommitOutcome = idle ? "none" : [&]
    {
        switch (status.CommitOutcome)
        {
            case Engine::ProjectCommitOutcome::NotCommitted: return "NotCommitted";
            case Engine::ProjectCommitOutcome::Cancelled: return "Cancelled";
            case Engine::ProjectCommitOutcome::Committed: return "Committed";
            case Engine::ProjectCommitOutcome::CommittedDurabilityUnconfirmed: return "CommittedDurabilityUnconfirmed";
            case Engine::ProjectCommitOutcome::CommittedRecoveryRequired: return "CommittedRecoveryRequired";
        }
        return "NotCommitted";
    }();
    // Project-level facts: the revision and manifest digest the Editor currently holds.
    block.ManifestRevision = m_FabProject.Revision;
    block.ManifestSha256 = m_FabProject.ManifestSha256;
    if (result)
    {
        block.MeshAsset = result->MeshAsset;
        block.MaterialAsset = result->MaterialHandle;
        block.ResultHandles = { result->MeshAsset, result->MaterialHandle };
        block.ResultHandles.insert(block.ResultHandles.end(), result->TextureAssets.begin(), result->TextureAssets.end());
        block.ResultHandleCount = block.ResultHandles.size();
        if (block.ResultHandles.size() > EditorMaterialControlMailbox::MaximumFabResultHandles)
            block.ResultHandles.resize(EditorMaterialControlMailbox::MaximumFabResultHandles);
    }

    block.ProjectReceiptCount = m_FabProject.Receipts.Receipts.size();
    std::set<Engine::AssetHandle> meshes;
    std::set<Engine::AssetHandle> materials;
    for (const Engine::FabImportReceipt& receipt : m_FabProject.Receipts.Receipts)
    {
        for (const Engine::FabImportedAssetRecord& asset : receipt.Assets)
        {
            if (asset.Type == Engine::AssetType::Mesh)
                meshes.insert(asset.Handle);
            else if (asset.Type == Engine::AssetType::Material)
                materials.insert(asset.Handle);
        }
    }
    block.ProjectMeshAssets.assign(meshes.begin(), meshes.end());
    block.ProjectMaterialAssets.assign(materials.begin(), materials.end());
    block.ProjectStructural = m_FabProject.StructuralStatus;
    block.ProjectStructuralMessage = m_FabProject.StructuralMessage;
    block.ProjectValidation = FabEditor::ProjectValidator::ToString(m_FabImport.Validator().GetState());
    block.ProjectValidationMessage = m_FabImport.Validator().GetMessage();

    const Fab::BrowserPanelDiagnostics panel = m_FabBrowser.GetDiagnostics();
    block.PanelState = panel.State.empty() ? "NotStarted" : panel.State;
    block.PanelInitialized = panel.Initialized;
    block.PanelFailed = panel.Failed;
    block.PanelVisible = panel.Visible;
    block.PanelKeyboardOwnedByPage = panel.KeyboardOwnedByPage;
    block.PanelTextureValid = panel.TextureValid;
    block.PanelLoading = panel.Loading;
    block.PanelFramesReceived = panel.FramesReceived;
    block.PanelFrameWidth = panel.FrameWidth;
    block.PanelFrameHeight = panel.FrameHeight;
    block.PanelNavigationDenials = panel.NavigationDenials;
    block.PanelDownloadsCompleted = panel.DownloadsCompleted;
    block.PanelHost = panel.DisplayHost;
    block.PanelError = panel.Error;
}

EditorMaterialControlTransaction EditorLayer::ExecuteFabControlRequest(
    const EditorMaterialControlRequest& request, EditorMaterialControlTransaction transaction)
{
    EditorMaterialControlReceipt& receipt = transaction.Receipt;
    const EditorFabControlRequest& fab = request.Fab;
    const Fab::FabImportStatus status = m_FabImport.Controller().GetStatus();

    const auto reject = [&](std::string reason)
    {
        receipt.Succeeded = false;
        receipt.Reason = std::move(reason);
        FillFabReceiptBlock(receipt.Fab);
        return std::move(transaction);
    };
    const auto succeed = [&](std::string_view effect, std::string_view recovery)
    {
        receipt.Succeeded = true;
        receipt.Reason = "ok";
        receipt.Effect = std::string(effect);
        receipt.Recovery = std::string(recovery);
        receipt.PostconditionVerified = true;
        receipt.UndoDepthAfter = m_UndoHistory.size();
        receipt.RedoDepthAfter = m_RedoHistory.size();
        FillFabReceiptBlock(receipt.Fab);
        return std::move(transaction);
    };
    const auto jobMismatch = [&]()
    {
        return fab.HasExpectedJobId && fab.ExpectedJobId != 0 && fab.ExpectedJobId != status.JobId;
    };

    switch (request.Action)
    {
        case EditorMaterialControlAction::InspectFabImport:
            if (jobMismatch())
                return reject("fab_job_mismatch");
            return succeed("ReadOnly", "None");

        case EditorMaterialControlAction::InspectFabPanel:
            return succeed("ReadOnly", "None");

        case EditorMaterialControlAction::SelectFabPackage:
        {
            const fs::path inbox = m_EditorMaterialControl.GetFabInboxPath();
            if (inbox.empty())
                return reject("fab_inbox_unavailable");
            if (!IsValidInboxLeaf(fab.InboxName))
                return reject("invalid_inbox_name");
            std::error_code filesystemError;
            if (!fs::is_directory(fs::symlink_status(inbox, filesystemError)))
                return reject("fab_inbox_unavailable");
            std::string error;
            if (!m_FabImport.SelectTyped(inbox / fab.InboxName, fab.ExpectedSourceKind, fab.ExpectedSourceSha256, error))
                return reject(error);
            return succeed("FabPackageSelected", "CancelFabImport");
        }

        case EditorMaterialControlAction::SetFabProvenance:
        {
            if (status.State != Fab::FabImportState::AwaitingProvenance)
                return reject("fab_state_not_awaiting_provenance");
            if (jobMismatch() || !fab.HasExpectedJobId)
                return reject("fab_job_mismatch");
            Fab::FabProvenance provenance;
            provenance.ProductIdentity = fab.ProductIdentity;
            provenance.ProductName = fab.ProductName;
            provenance.Publisher = fab.Publisher;
            provenance.VersionOrDownloadLabel = fab.VersionOrDownloadLabel;
            provenance.LicenseFamily = fab.LicenseFamily;
            provenance.LicenseTier = fab.LicenseTier;
            provenance.AttributionText = fab.AttributionText;
            provenance.AttributionLink = fab.AttributionLink;
            provenance.NoAI = fab.NoAI;
            provenance.GeneratedWithAI = fab.GeneratedWithAI;
            provenance.RawSourcePolicy = fab.RawSourcePolicy;
            if (!m_FabImport.ApplyProvenance(provenance))
                return reject("fab_state_not_awaiting_provenance");
            return succeed("FabProvenanceSet", "None");
        }

        case EditorMaterialControlAction::ConfirmFabProvenance:
        {
            if (status.State != Fab::FabImportState::AwaitingProvenance)
                return reject("fab_state_not_awaiting_provenance");
            if (jobMismatch() || !fab.HasExpectedJobId)
                return reject("fab_job_mismatch");
            if (!status.ProvenanceValid)
                return reject("fab_provenance_invalid");
            if (fab.ExpectedProvenanceDigest != status.ProvenanceDigest)
                return reject("fab_provenance_digest_mismatch");
            std::string error;
            if (!m_FabImport.Confirm(fab.ExpectedProvenanceDigest, error))
                return reject("fab_confirm_refused");
            return succeed("FabProvenanceConfirmed", "CancelFabImport");
        }

        case EditorMaterialControlAction::CommitFabImport:
        {
            if (status.State != Fab::FabImportState::ReadyToCommit)
                return reject("fab_state_not_ready_to_commit");
            if (jobMismatch() || !fab.HasExpectedJobId)
                return reject("fab_job_mismatch");
            if (fab.ExpectedGenerationId != status.GenerationId)
                return reject("fab_generation_mismatch");
            if (fab.ExpectedRelation != Engine::ToString(status.Decision))
                return reject("fab_relation_mismatch");
            std::optional<Fab::FabAssignmentTarget> assignment;
            if (fab.HasAssignment)
            {
                const Engine::Entity entity { request.EntityId };
                const Engine::SceneEntity* target = m_ActiveScene.TryGetEntity(entity);
                if (!target || target->Name != request.ExpectedEntityName)
                    return reject("stale_or_mismatched_entity_identity");
                if (!target->MeshRenderer || target->MeshRenderer->MeshAsset != fab.ExpectedMeshAsset
                    || target->MeshRenderer->MaterialAsset != fab.ExpectedMaterialAsset)
                    return reject("compare_and_swap_state_mismatch");
                assignment = Fab::FabAssignmentTarget { entity.Id, fab.ExpectedMeshAsset, fab.ExpectedMaterialAsset };
            }

            m_EditorFabForceAdoptionFailureOnce = m_EditorFabReopenSmokeRequested
                && request.RequestId == "fb-75-commit-replace";
            // The commit runs inside the handler: a project commit cannot be rolled
            // back, so this receipt is the terminal truth rather than a prediction.
            const FabImportPanel::CommitReport report = m_FabImport.Commit(std::move(assignment));
            const Fab::FabImportStatus after = m_FabImport.Controller().GetStatus();
            receipt.Persistence = after.ProjectChanged ? "Committed" : "SessionOnly";
            receipt.Saved = after.ProjectChanged;
            if (!report.Ok)
            {
                receipt.Effect = report.CommittedOnDisk ? "FabCommitRecoveryRequired" : "None";
                receipt.Recovery = report.CommittedOnDisk ? "ReloadProject" : "None";
                m_ConsoleLines.emplace_back("Fab commit refused: " + report.Error);
                return reject(report.CommittedOnDisk ? "fab_committed_but_not_adopted" : "fab_commit_refused");
            }
            return succeed(after.ProjectChanged ? "FabImportCommitted" : "FabImportReused", "None");
        }

        case EditorMaterialControlAction::CancelFabImport:
            if (jobMismatch() || !fab.HasExpectedJobId)
                return reject("fab_job_mismatch");
            if (Fab::IsTerminal(status.State) || status.State == Fab::FabImportState::Idle)
                return reject("fab_nothing_to_cancel");
            m_FabImport.Cancel();
            m_FabImport.Update();
            return succeed("FabCancelRequested", "None");

        case EditorMaterialControlAction::DismissFabImport:
        {
            if (jobMismatch() || !fab.HasExpectedJobId)
                return reject("fab_job_mismatch");
            std::string error;
            if (!m_FabImport.Dismiss(error))
                return reject("fab_dismiss_refused");
            return succeed("FabImportDismissed", "None");
        }

        case EditorMaterialControlAction::SaveProjectState:
        {
            if (fab.ExpectedManifestSha256.size() == 64)
            {
                std::string current;
                std::string digestError;
                if (!FabEditor::ReadManifestSha256({ m_ProjectRoot, m_ProjectManifestRelativePath }, current, digestError)
                    || current != fab.ExpectedManifestSha256)
                    return reject("compare_and_swap_state_mismatch");
            }
            if (!SaveProject())
                return reject("project_save_failed");
            receipt.Persistence = "Saved";
            receipt.Saved = true;
            return succeed("ProjectSaved", "None");
        }

        case EditorMaterialControlAction::ValidateProject:
        {
            std::string error;
            if (!m_FabImport.Validator().Start({ m_ProjectRoot, m_ProjectManifestRelativePath }, error))
                return reject("project_validation_running");
            return succeed("ProjectValidationStarted", "None");
        }

        case EditorMaterialControlAction::SetFabPanelVisible:
            if (fab.PanelVisible && Engine::Application::Get().GetSpecification().Window.Headless)
                return reject("headless_has_no_browser_panel");
            m_FabBrowser.SetVisible(fab.PanelVisible);
            return succeed("FabPanelVisibilitySet", "None");

        case EditorMaterialControlAction::PlaceMeshAsset:
        case EditorMaterialControlAction::SetEntityMeshRendererAssets:
            break;

        default:
            return reject("unsupported_action");
    }

    // ---- document mutations: one history entry, with a verified rollback ----
    struct Rollback
    {
        HistoryState State;
        std::vector<HistoryEntry> UndoHistory;
        std::vector<HistoryEntry> RedoHistory;
        bool FusionPivotValid = false;
        Engine::Math::DVec3 FusionPivot;
        bool ViewportDiscontinuousRelocationPending = false;
        std::size_t EntityCount = 0;
        bool MutationStarted = false;
    };
    const auto captureRollback = [this]()
    {
        auto state = std::make_shared<Rollback>();
        state->State = CaptureHistoryState();
        state->UndoHistory = m_UndoHistory;
        state->RedoHistory = m_RedoHistory;
        state->FusionPivotValid = m_FusionNavigationPivotValid;
        state->FusionPivot = m_FusionNavigationPivot;
        state->ViewportDiscontinuousRelocationPending = m_ViewportDiscontinuousRelocationPending;
        state->EntityCount = m_ActiveScene.GetEntities().size();
        return state;
    };
    const auto restoreRollback = [this](const std::shared_ptr<Rollback>& state, EditorMaterialControlReceipt& rolledBack)
    {
        const bool restored = !state->MutationStarted || RestoreHistoryState(state->State);
        m_UndoHistory = state->UndoHistory;
        m_RedoHistory = state->RedoHistory;
        m_FusionNavigationPivotValid = state->FusionPivotValid;
        m_FusionNavigationPivot = state->FusionPivot;
        m_ViewportDiscontinuousRelocationPending = state->ViewportDiscontinuousRelocationPending;
        rolledBack.UndoDepthAfter = m_UndoHistory.size();
        rolledBack.RedoDepthAfter = m_RedoHistory.size();
        rolledBack.SelectedEntityIdAfter = m_SelectedEntity.Id;
        return restored && m_UndoHistory.size() == state->UndoHistory.size()
            && m_RedoHistory.size() == state->RedoHistory.size() && m_SelectedEntity == state->State.SelectedEntity
            && m_ActiveScene.GetEntities().size() == state->EntityCount;
    };
    const auto injectedFailure = [this, requestId = request.RequestId](std::string& error)
    {
        if (!m_EditorFabHelperSmokeRequested || requestId != "fa-14-place-forced-rollback")
            return false;
        error = "injected_postcondition_failure";
        return true;
    };

    receipt.EntityId = Engine::kInvalidEntityId;
    if (request.Action == EditorMaterialControlAction::PlaceMeshAsset)
    {
        if (!request.HasExpectedSelectedEntityId || request.ExpectedSelectedEntityId != m_SelectedEntity.Id)
            return reject("compare_and_swap_state_mismatch");
        const Engine::AssetMetadata* meshMetadata = m_AssetRegistry.GetAsset(fab.MeshAsset);
        if (!meshMetadata || meshMetadata->Type != Engine::AssetType::Mesh)
            return reject("mesh_asset_not_registered_mesh");

        // Stage on a copy to predict the new entity (ids are deterministic).
        Engine::AssetHandle material = FindFabMaterialForMesh(fab.MeshAsset);
        if (material == Engine::kInvalidAssetHandle)
        {
            const Engine::MeshRendererComponent* prototype = m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity);
            material = prototype ? prototype->MaterialAsset : Engine::kInvalidAssetHandle;
        }
        Engine::Scene staged = m_ActiveScene;
        const Engine::Entity predicted = staged.CreateEntity(UniqueEntityName(staged, meshMetadata->Name));
        Engine::MeshRendererComponent renderer;
        renderer.MeshAsset = fab.MeshAsset;
        renderer.MaterialAsset = material;
        renderer.MeshName = meshMetadata->Name;
        if (!predicted || !staged.AddMeshRendererComponent(predicted, renderer))
            return reject("entity_creation_failed");

        const std::shared_ptr<Rollback> rollback = captureRollback();
        receipt.EntityId = predicted.Id;
        receipt.EntityName = staged.TryGetEntity(predicted)->Name;
        receipt.AffectedEntityCount = 1;
        receipt.AffectedEntityIds = { predicted.Id };
        receipt.BeforeMeshRendererPresent = false;
        receipt.AfterMeshRendererPresent = true;
        receipt.AfterMeshRenderer = renderer;
        receipt.MaterialHandle = material;
        receipt.SelectedEntityIdAfter = predicted.Id;
        receipt.SelectionCommitted = true;
        receipt.PivotRetargeted = true;
        receipt.Succeeded = true;
        receipt.Reason = "ok";
        receipt.Effect = "MeshAssetPlaced";
        receipt.Recovery = "UndoRedo";
        receipt.UndoDepthAfter = std::min<std::size_t>(rollback->UndoHistory.size() + 1, 128);
        receipt.RedoDepthAfter = 0;
        receipt.PostconditionVerified = true;
        FillFabReceiptBlock(receipt.Fab);
        transaction.Mutating = true;
        transaction.Commit = [this, meshHandle = fab.MeshAsset, predicted, rollback, injectedFailure](std::string& error)
        {
            rollback->MutationStarted = true;
            const Engine::Entity placed = PlaceMeshAssetInScene(meshHandle, error);
            if (!placed)
                return false;
            if (injectedFailure(error))
                return false;
            if (placed != predicted || m_SelectedEntity != placed
                || m_UndoHistory.size() != std::min<std::size_t>(rollback->UndoHistory.size() + 1, 128)
                || !m_RedoHistory.empty())
            {
                error = "placement_or_history_postcondition_mismatch";
                return false;
            }
            ResetFusionNavigationPivotFromSelectionOrScene();
            return true;
        };
        transaction.Rollback = [restoreRollback, rollback](EditorMaterialControlReceipt& rolledBack)
        {
            const bool restored = restoreRollback(rollback, rolledBack);
            rolledBack.SelectionCommitted = false;
            rolledBack.PivotRetargeted = false;
            return restored;
        };
        return transaction;
    }

    // SetEntityMeshRendererAssets
    const Engine::Entity entityHandle { request.EntityId };
    const Engine::SceneEntity* entity = m_ActiveScene.TryGetEntity(entityHandle);
    if (!entity || entity->Name != request.ExpectedEntityName)
        return reject("stale_or_mismatched_entity_identity");
    receipt.EntityId = entity->EntityHandle.Id;
    receipt.EntityName = entity->Name;
    receipt.AffectedEntityCount = 1;
    receipt.AffectedEntityIds = { entityHandle.Id };
    receipt.BeforeTransform = receipt.AfterTransform = entity->Transform;
    if (!entity->MeshRenderer)
        return reject("compare_and_swap_state_mismatch");
    receipt.BeforeMeshRendererPresent = receipt.AfterMeshRendererPresent = true;
    receipt.BeforeMeshRenderer = receipt.AfterMeshRenderer = *entity->MeshRenderer;
    receipt.MaterialHandle = entity->MeshRenderer->MaterialAsset;
    if (entity->MeshRenderer->MeshAsset != fab.ExpectedMeshAsset
        || entity->MeshRenderer->MaterialAsset != fab.ExpectedMaterialAsset)
        return reject("compare_and_swap_state_mismatch");
    const Engine::AssetMetadata* newMesh = m_AssetRegistry.GetAsset(fab.NewMeshAsset);
    const Engine::AssetMetadata* newMaterial = m_AssetRegistry.GetAsset(fab.NewMaterialAsset);
    if (!newMesh || newMesh->Type != Engine::AssetType::Mesh)
        return reject("mesh_asset_not_registered_mesh");
    if (!newMaterial || newMaterial->Type != Engine::AssetType::Material || !m_MaterialLibrary.Get(fab.NewMaterialAsset))
        return reject("material_asset_not_registered_material");
    if (fab.NewMeshAsset == fab.ExpectedMeshAsset && fab.NewMaterialAsset == fab.ExpectedMaterialAsset)
        return reject("no_change");

    const std::shared_ptr<Rollback> rollback = captureRollback();
    receipt.AfterMeshRenderer.MeshAsset = fab.NewMeshAsset;
    receipt.AfterMeshRenderer.MaterialAsset = fab.NewMaterialAsset;
    receipt.Succeeded = true;
    receipt.Reason = "ok";
    receipt.Effect = "MeshRendererAssetsSet";
    receipt.Recovery = "UndoRedo";
    receipt.UndoDepthAfter = std::min<std::size_t>(rollback->UndoHistory.size() + 1, 128);
    receipt.RedoDepthAfter = 0;
    receipt.PostconditionVerified = true;
    FillFabReceiptBlock(receipt.Fab);
    transaction.Mutating = true;
    transaction.Commit = [this, entityHandle, expectedName = request.ExpectedEntityName, fab, rollback](std::string& error)
    {
        Engine::SceneEntity* current = m_ActiveScene.TryGetEntity(entityHandle);
        if (!current || current->Name != expectedName || !current->MeshRenderer
            || current->MeshRenderer->MeshAsset != fab.ExpectedMeshAsset
            || current->MeshRenderer->MaterialAsset != fab.ExpectedMaterialAsset)
        {
            error = "state_changed_before_commit";
            return false;
        }
        rollback->MutationStarted = true;
        current->MeshRenderer->MeshAsset = fab.NewMeshAsset;
        current->MeshRenderer->MaterialAsset = fab.NewMaterialAsset;
        RecordHistory("Agent set mesh renderer assets", rollback->State);
        const Engine::MeshRendererComponent* applied = m_ActiveScene.TryGetMeshRendererComponent(entityHandle);
        if (!applied || applied->MeshAsset != fab.NewMeshAsset || applied->MaterialAsset != fab.NewMaterialAsset
            || m_UndoHistory.size() != std::min<std::size_t>(rollback->UndoHistory.size() + 1, 128)
            || !m_RedoHistory.empty())
        {
            error = "mesh_renderer_or_history_postcondition_mismatch";
            return false;
        }
        return true;
    };
    transaction.Rollback = [this, restoreRollback, rollback, entityHandle](EditorMaterialControlReceipt& rolledBack)
    {
        const bool restored = restoreRollback(rollback, rolledBack);
        const Engine::MeshRendererComponent* current = m_ActiveScene.TryGetMeshRendererComponent(entityHandle);
        return restored && current && current->MeshAsset == rolledBack.BeforeMeshRenderer.MeshAsset
            && current->MaterialAsset == rolledBack.BeforeMeshRenderer.MaterialAsset;
    };
    return transaction;
}
