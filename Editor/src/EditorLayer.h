#pragma once

#include "Core/Notifications.h"
#include "EditorHistoryState.h"
#include "EditorMaterialControl.h"
#include "Fab/BrowserPanel.h"
#include "FabEditorAdoption.h"
#include "FabImportPanel.h"
#include "History/EditGesture.h"
#include "History/HistoryNaming.h"
#include "History/HistoryStore.h"

#include <Engine.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImVec2;
struct ImGuiContext;

enum class ViewportNavigationPreset
{
    Fusion,
    Unreal
};

class EditorLayer final : public Engine::Layer
{
public:
    EditorLayer();

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(Engine::Timestep timestep) override;
    void OnUiRender() override;
    void OnEvent(Engine::Event& event) override;

private:
    // The whole project state one undo snapshot holds (see EditorHistoryState.h).
    using HistoryState = EditorHistoryState;
    using HistorySnapshot = std::shared_ptr<const HistoryState>;
    using HistoryStoreType = EditorHistory::HistoryStore<HistoryState>;

    // The only seam between the history store and live project state.
    class HistoryAdapter final : public EditorHistory::IHistoryStateAdapter<HistoryState>
    {
    public:
        explicit HistoryAdapter(EditorLayer& owner) : m_Owner(owner) {}
        bool Restore(const HistorySnapshot& snapshot) override;
        Engine::u64 EstimateBytes(const HistoryState& state) const override;
        bool Equal(const HistoryState& first, const HistoryState& second) const override;

    private:
        EditorLayer& m_Owner;
    };

    void DrawDockspace();
    void DrawMainMenuBar();
    void BuildDefaultDockLayout(unsigned int dockspaceId, const ImVec2& dockspaceSize);
    void DrawSceneHierarchyPanel();
    void DrawInspectorPanel();
    void DrawRendererBackendSelector();
    void ApplyEditorCameraStateToScene();
    void SyncEditorCameraStateFromMainCamera(bool discontinuousRelocation = false);
    void UpdateViewportNavigation(Engine::Timestep timestep);
    void BeginViewportCursorCapture();
    void ArmViewportCursorCapture();
    void EndViewportCursorCapture();
    void ClearViewportNavigationInput();
    bool TryAcquireViewportNavigationFocus();
    bool IsShiftNavigationModifierDown() const;
    void BeginFusionOrbitPivot();
    void ResetFusionNavigationPivotFromSelectionOrScene();
    bool RetargetFusionNavigationPivotToSelectedEntity();
    void SetFusionNavigationPivot(const Engine::Math::DVec3& pivot);
    bool SaveEditorSettings();
    void LoadEditorSettings();
    // Selection, viewport picking, and camera framing. Defined in EditorLayerPicking.cpp.
    struct ViewportPickGeometry;
    struct ViewportPickReport;
    struct FocusPlan;
    struct PickMeshData;
    enum class SelectionSource
    {
        Hierarchy,
        Viewport,
        Keyboard
    };
    // The one selection path: hierarchy rows, viewport clicks, typed picks and Esc all
    // end here, so the Inspector, Fusion pivot, selected-bounds overlay and hierarchy
    // highlight follow the same state. Returns whether the selection changed.
    bool SetSelectedEntity(Engine::Entity entity, SelectionSource source);
    bool ClearSelection(SelectionSource source);
    ViewportPickGeometry GetViewportPickGeometry() const;
    ViewportPickReport PickAtViewportPixel(double pixelX, double pixelY);
    ViewportPickReport PickAtViewportNormalized(double normalizedX, double normalizedY);
    void BeginViewportClick();
    void TrackViewportClickMotion(double x, double y);
    void FinishViewportClick();
    void CancelViewportClick();
    // F: frames the selected entity. Returns false (and says why in the Console) when
    // nothing can be framed. `animate` false jumps and flags a discontinuous relocation.
    bool FocusSelectedEntity(bool animate = true);
    bool FocusEntity(Engine::Entity entity, bool animate);
    bool FrameAllVisibleMeshes(bool animate = true);
    bool PlanFocusForEntity(Engine::Entity entity, FocusPlan& outPlan, std::string& outError);
    bool PlanFocusForAllMeshes(FocusPlan& outPlan, std::string& outError);
    void StartFocus(const FocusPlan& plan, bool animate);
    void AdvanceFocusAnimation(Engine::Timestep timestep);
    void CancelFocusAnimation();
    std::shared_ptr<const PickMeshData> GetPickMesh(Engine::AssetHandle mesh);
    EditorMaterialControlTransaction ExecuteViewportControlRequest(
        const EditorMaterialControlRequest& request, EditorMaterialControlTransaction transaction);
    void RunEditorViewportPickingHelperSmokeAfterDrain();
    void RunViewportClickSmoke();
    // Draws the dockspace and every EditorLayer panel through frames of a private,
    // headless ImGui context so ImGui's own stack, ID, and window assertions cover the
    // close-button, hidden-panel, and selection code. Returns true while it owns the UI phase.
    bool RunPanelUiSmokeFrame();
    void PublishEditorViewportPickingTarget();
    // Panels: close buttons, Window menu entries, persisted visibility.
    void LoadPanelVisibility();
    void PersistPanelVisibilityIfChanged();
    bool BeginClosablePanel(size_t panel, const char* title, int windowFlags = 0);
    void DrawViewportOverlays(const ImVec2& imageMin, const ImVec2& imageMax, bool hasNativeViewportTexture);
    bool CollectFramePoints(const Engine::SceneEntity& entity, bool visibleMeshesOnly,
        std::vector<Engine::Math::DVec3>& points, bool& usedBounds);
    void ResetPanelVisibility();
    void DrawViewportPanel();
    void DrawConsolePanel();
    void DrawProfilerPanel();
    void DrawProjectPanel();
    void PublishFramePacingPolicy();
    void PublishPresentationPolicy();
    void PublishColorPipelineSettings();
    Engine::EntityId GetSceneDebugSelectedEntityId() const;
    bool PublishSceneDebugVisualization();
    bool PublishProjectColorPipelineSettings(
        const Engine::RendererColorPipelineSettings& settings);
    bool ApplyProjectColorPipelineSettings(const Engine::RendererColorPipelineSettings& settings,
        EditorHistory::EditProperty property);
    void HandleProjectColorPipelineInput(
        const Engine::RendererColorPipelineSettings& settings, bool edited,
        EditorHistory::EditProperty property);
    void DrawNewProjectDialog();
    bool DrawMaterialAssetControls(Engine::AssetHandle handle);
    void HandleAssetWatchEvents();
    void RunAssetWatchSmokeMutation();
    void RunGltfImportSmoke();
    void RunMaterialAssetSmoke();
    void RunUndoRedoSmoke();
    void RunSceneAuthoringSmoke();
    void RunSceneRenderSnapshotSmoke();
    void RunFramePacingPolicySmoke();
    void RunColorPipelineSettingsSmoke();
    void RunEditorSettingsSmoke();
    void RunViewportNavigationSmoke();
    void RunPresentationPolicySmoke();
    void InitializeEditorMaterialControl();
    EditorMaterialControlTransaction ExecuteEditorMaterialControlRequest(
        const EditorMaterialControlRequest& request, Engine::u64 frame);
    EditorMaterialControlTransaction ExecuteEditorMaterialControlRequestCore(
        const EditorMaterialControlRequest& request, Engine::u64 frame);
    static bool ActionNeedsIdleHistory(EditorMaterialControlAction action);
    void RunEditorMaterialControlSmokeBeforeDrain();
    void RunEditorMaterialControlSmokeAfterDrain();
    void RunEditorMaterialControlLiveHelperSmokeAfterDrain();
    void RunEditorSceneControlV2HelperSmokeAfterDrain();
    void RunEditorMaterialControlCapacitySmokeBeforeDrain();
    void RunEditorMaterialControlCapacitySmokeAfterDrain();
    void RunEditorMaterialControlDurabilitySmokeBeforeDrain();
    void RunEditorMaterialControlDurabilitySmokeAfterDrain();
    void RunEditorMaterialControlRollbackFailureSmokeBeforeDrain();
    void RunEditorMaterialControlRollbackFailureSmokeAfterDrain();
    void ConfigureSceneOriginRasterSmoke();
    void AdvanceSceneOriginRasterSmoke();
    void CaptureSceneOriginRasterSmoke();
    bool OnFileDrop(Engine::FileDropEvent& event);
    bool ImportGltfAsset(const std::filesystem::path& sourcePath);
    bool SaveProject();
    bool LoadProject();
    bool CreateNewProject(std::string name, const std::filesystem::path& parentDirectory, bool overwriteExisting = false);
    Engine::Entity CreateSceneEntity(std::string name = "Entity");
    bool DeleteSelectedEntity();
    bool SaveActiveScene();
    bool SaveAssetRegistry();
    bool SaveMaterialAsset(Engine::AssetHandle handle);
    bool SaveMaterialAssets();
    // ---- Undo history (EditorLayerHistory.cpp) ----
    HistoryStoreType& History() { return *m_HistoryStore; }
    const HistoryStoreType& History() const { return *m_HistoryStore; }
    // A copy of the live project state, stamped with the live camera epoch and the
    // special entity ids. The non-const pointer lets the recorder stamp After.
    std::shared_ptr<HistoryState> CaptureHistorySnapshot() const;
    // exact == false keeps the live navigation camera when no entry between the live
    // state and the snapshot edited the camera (undo and redo); exact == true always
    // applies the snapshot's camera (typed-control rollback to a captured state).
    bool RestoreHistoryState(const HistoryState& state, bool preserveViewportCamera);
    bool RestoreHistoryStateExact(const HistoryState& state) { return RestoreHistoryState(state, false); }
    // Records one entry from `before` to the live state, or nothing when the two are
    // the same project content. `before` comes from CaptureBeforeSnapshot, which ended
    // any open Inspector gesture before the edit began.
    EditorHistory::HistoryResult RecordHistory(EditorHistory::HistoryLabel label, const HistorySnapshot& before);
    // Ends any open gesture, then copies the live state: the Before of an edit that is
    // about to mutate live state and be recorded with RecordHistory.
    HistorySnapshot CaptureBeforeSnapshot();
    // Captures Before, runs apply, and records one entry when apply returned true and
    // changed something. For checkboxes, combos, drops and menu commands.
    bool DiscreteEdit(const EditorHistory::HistoryLabel& label, const std::function<bool()>& apply);
    // Continuous Inspector and menu widgets. Call right after the widget with its
    // returned `edited` value; the widget must have edited a local copy. Returns whether
    // apply ran (the caller then owns any follow-up such as camera synchronisation).
    bool TrackedEdit(Engine::u64 entityKey, EditorHistory::EditProperty property, bool edited,
        const std::function<EditorHistory::HistoryLabel()>& makeLabel, const std::function<bool()>& apply);
    // Ends a gesture whose widget is gone (end of the UI frame, a project switch, a
    // typed mutation, an undo barrier). Safe when none is open.
    void FlushEditGesture();
    void FinishGesture(const EditorHistory::EditGestureResult& gesture);
    void AnnounceRecordResult(const EditorHistory::HistoryResult& result);
    void EndOfFrameEditGestureFlush();
    void ResetHistoryForProject(EditorHistory::HistoryLabel baseLabel);
    void InstallHistoryBarrier(const std::string& label, const std::string& reason);
    bool Undo();
    bool Redo();
    bool JumpToHistoryRow(size_t row);
    void AnnounceHistory(const std::string& text);
    void ApplyHistoryCommandLine(const Engine::ApplicationCommandLineArgs& args);
    // Edit menu, History panel, status text, shortcuts.
    void DrawEditMenu();
    void DrawHistoryPanel();
    void DrawClearHistoryPopup();
    void DrawHistoryStatus();
    // The announcement the status text currently shows, empty once its four seconds passed.
    std::string HistoryStatusText() const;
    void PollHistoryShortcuts();
    bool IsHistoryModalOpen() const;
    EditorHistory::HistoryCommandText UndoCommandText() const;
    EditorHistory::HistoryCommandText RedoCommandText() const;
    // `--editor-history-smoke`: drives the Inspector and the shortcuts through a private
    // headless ImGui context and checks the history it records. Returns true while it owns
    // the UI phase.
    bool RunEditorHistorySmoke();
    // `--editor-history-benchmark`: per-frame Inspector cost on a 1000-entity scene.
    bool RunEditorHistoryBenchmark();
    void ProbeWidget(EditorHistory::EditProperty property);
    // History cap a recorded edit leaves behind: Before + 1, bounded by the entry cap.
    // Byte-budget eviction can only make the real depth smaller.
    size_t PredictedUndoDepthAfterRecord(size_t depthBefore) const
    {
        return std::min(depthBefore + 1, History().Config().MaximumEntries);
    }
    // Fills the receipt's history block from the store; rows only for InspectHistory.
    void FillHistoryReceiptBlock(EditorHistoryControlReceipt& block, bool includeRows) const;
    EditorMaterialControlTransaction ExecuteHistoryControlRequest(
        const EditorMaterialControlRequest& request, EditorMaterialControlTransaction transaction);
    void EnsureDefaultSceneEntities();
    // The scene file carries no role marker for the Prototype Mesh, Directional
    // Light and Player Start entities, so a freshly loaded scene is the one place the
    // Editor resolves them by their persisted names. From then on they are tracked
    // by EntityId and history restores carry the ids.
    void AdoptDefaultEntitiesFromLoadedScene();

    // Project location and Fab integration. Defined in EditorLayerFab.cpp so the
    // 7,000-line EditorLayer.cpp only carries the hook calls.
    void RefreshProjectLocation();
    std::filesystem::path ResolveProjectPath(std::string_view path, bool searchLegacyRoots) const;
    bool OpenCommandLineProject(const Engine::ApplicationCommandLineArgs& args);
    void InitializeFabIntegration();
    void ShutdownFabIntegration();
    void UpdateFabIntegration();
    void DrawFabIntegration();
    void PollFabDownloads();
    bool BuildFabImportProjectContext(std::optional<Fab::FabAssignmentTarget> assignment,
        Fab::FabImportProjectContext& out, std::string& error);
    bool AdoptFabImportCommit(std::string& error);
    bool ReloadCommittedFabProject(std::string& error);
    std::optional<Fab::FabAssignmentTarget> GetFabAssignmentCandidate() const;
    void ResetFabProjectState(const Engine::FabProjectState& loaded, std::string manifestSha256);
    bool RefreshManifestDigest();
    Engine::AssetHandle FindFabMaterialForMesh(Engine::AssetHandle mesh) const;
    Engine::Entity PlaceMeshAssetInScene(Engine::AssetHandle mesh, std::string& error,
        EditorHistory::HistorySource source = EditorHistory::HistorySource::User);
    bool OnFabAssetDrop(Engine::AssetHandle handle);
    void FillFabReceiptBlock(EditorFabControlReceipt& block) const;
    EditorMaterialControlTransaction ExecuteFabControlRequest(
        const EditorMaterialControlRequest& request, EditorMaterialControlTransaction transaction);
    void PublishEditorFabControlSmokeTarget();
    void RunEditorFabControlSmokeBeforeDrain();
    void RunEditorFabControlSmokeAfterDrain();
    // Draws the Fab import panel through every workflow state inside a private
    // ImGui context, headlessly, so its widget code runs under ImGui's own
    // stack and ID assertions. Returns true while it owns the UI phase.
    bool RunFabPanelUiSmokeFrame();

private:
    // The project root every manifest-relative path resolves beneath: the working
    // directory for the default project (cwd-relative manifests) and the manifest's
    // own directory for a project named with --project.
    std::filesystem::path m_ProjectRoot;
    std::string m_ProjectManifestRelativePath;
    bool m_ProjectPathsRelativeToManifest = false;
    FabEditor::ProjectFabState m_FabProject;
    FabImportPanel m_FabImport;
    Fab::BrowserPanel m_FabBrowser;
    bool m_FabIntegrationInitialized = false;
    bool m_EditorFabHelperSmokeRequested = false;
    bool m_EditorFabReopenSmokeRequested = false;
    bool m_EditorFabSmokeCompleted = false;
    bool m_EditorFabForceAdoptionFailureOnce = false;
    unsigned int m_EditorFabAdoptionReloads = 0;
    bool m_FabPanelUiSmokeRequested = false;
    ImGuiContext* m_FabPanelUiSmokeContext = nullptr;
    unsigned int m_FabPanelUiSmokeStage = 0;
    unsigned int m_FabPanelUiSmokeFrames = 0;
    double m_FabPanelUiSmokeStageStart = 0.0;
    std::string m_FabPanelUiSmokeFixtures;
    Engine::u64 m_EditorFabSmokeInitialRendererGeneration = 0;
    Engine::AssetHandle m_EditorFabSmokePrototypeMesh = Engine::kInvalidAssetHandle;
    Engine::AssetHandle m_EditorFabSmokePrototypeMaterial = Engine::kInvalidAssetHandle;

private:
    unsigned int m_FrameCounter = 0;
    float m_LastFrameMs = 0.0f;
    std::array<float, 240> m_FrameTimeHistory {};
    size_t m_FrameTimeHistoryCount = 0;
    size_t m_FrameTimeHistoryOffset = 0;
    std::optional<Engine::u64> m_LastFrameTimeSampledFrame;
    struct FrameTimeHistoryCondition
    {
        Engine::FramePacingMode Mode = Engine::FramePacingMode::Responsive;
        std::optional<double> TargetFramesPerSecond;
        Engine::SmoothFrametimeCandidate Candidate = Engine::SmoothFrametimeCandidate::InterFrame;
        Engine::PresentationPolicy RequestedPresentation = Engine::PresentationPolicy::Synchronized;
        Engine::PresentationActualMode ActualPresentation = Engine::PresentationActualMode::Unavailable;
        Engine::u64 PresentationGeneration = 0;

        bool operator==(const FrameTimeHistoryCondition&) const = default;
    };
    std::optional<FrameTimeHistoryCondition> m_FrameTimeHistoryCondition;
    bool m_ShowDemoWindow = false;
    bool m_ResetDockLayout = true;
    bool m_CaptureViewportRequested = false;
    bool m_CaptureViewportComplete = false;
    bool m_RendererCapabilitySmokeRequested = false;
    bool m_RendererCapabilitySmokeComplete = false;
    bool m_SaveSceneSmokeRequested = false;
    bool m_AssetWatchSmokeRequested = false;
    bool m_AssetWatchSmokeTouched = false;
    bool m_GltfImportSmokeRequested = false;
    bool m_GltfImportSmokeCompleted = false;
    bool m_MaterialAssetSmokeRequested = false;
    bool m_MaterialAssetSmokeCompleted = false;
    bool m_ProjectSaveSmokeRequested = false;
    bool m_UndoRedoSmokeRequested = false;
    bool m_UndoRedoSmokeCompleted = false;
    bool m_SceneAuthoringSmokeRequested = false;
    bool m_SceneAuthoringSmokeCompleted = false;
    bool m_SceneRenderSnapshotSmokeRequested = false;
    bool m_SceneRenderSnapshotSmokeCompleted = false;
    bool m_SceneOriginRasterSmokeRequested = false;
    bool m_SceneOriginRasterSmokeCompleted = false;
    bool m_FramePacingPolicySmokeRequested = false;
    bool m_FramePacingPolicySmokeCompleted = false;
    bool m_ColorPipelineSettingsSmokeRequested = false;
    bool m_ColorPipelineSettingsSmokeCompleted = false;
    bool m_EditorSettingsSmokeRequested = false;
    bool m_EditorSettingsSmokeCompleted = false;
    bool m_ViewportNavigationSmokeRequested = false;
    bool m_ViewportNavigationSmokeCompleted = false;
    bool m_FramePacingNavigationTraceEnabled = false;
    Engine::u64 m_ViewportNavigationMutationFrames = 0;
    bool m_EventTraceEnabled = false;
    bool m_PresentationPolicySmokeRequested = false;
    bool m_PresentationPolicySmokeCompleted = false;
    bool m_EditorMaterialControlSmokeRequested = false;
    bool m_EditorMaterialControlSmokeCompleted = false;
    bool m_EditorMaterialControlLiveHelperSmokeRequested = false;
    bool m_EditorMaterialControlLiveHelperSmokeCompleted = false;
    bool m_EditorSceneControlV2HelperSmokeRequested = false;
    bool m_EditorSceneControlV2HelperSmokeCompleted = false;
    bool m_EditorMaterialControlCapacitySmokeRequested = false;
    bool m_EditorMaterialControlCapacitySmokeCompleted = false;
    bool m_EditorMaterialControlDurabilitySmokeRequested = false;
    bool m_EditorMaterialControlDurabilitySmokeCompleted = false;
    bool m_EditorMaterialControlRollbackFailureSmokeRequested = false;
    bool m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested = false;
    bool m_EditorMaterialControlRollbackFailureSmokeCompleted = false;
    bool m_EditorMaterialControlForceCommitFailureOnce = false;
    bool m_EditorMaterialControlForceLateResponseCollisionOnce = false;
    bool m_EditorMaterialControlForceParentSyncFailureOnce = false;
    bool m_EditorMaterialControlForceRollbackVerificationFailureOnce = false;
    unsigned int m_EditorMaterialControlSmokeStage = 0;
    Engine::u64 m_PresentationPolicySmokeTearingGeneration = 0;
    bool m_ShowNewProjectDialog = false;
    std::string m_CaptureViewportPath = "output/captures/editor-viewport.bmp";
    std::string m_ProjectPath = "output/projects/default.spiralproject";
    std::string m_EditorSettingsPath = "output/editor/engine-settings.spiralsettings";
    std::string m_ScenePath = "output/scenes/sample.spiral";
    std::string m_AssetRegistryPath = "output/assets/sample.assets";
    std::string m_AssetWatchSmokePath = "output/assets/watch-smoke.mesh";
    std::string m_GltfImportSmokePath = "output/assets/gltf-smoke/triangle.gltf";
    std::string m_MaterialAssetSmokePath = "output/assets/material-smoke.spiralmat";
    std::array<std::string, 3> m_SceneOriginRasterCapturePaths = {
        "output/captures/scene-origin-a.bmp",
        "output/captures/scene-origin-b.bmp",
        "output/captures/scene-origin-c.bmp"
    };
    Engine::AssetRegistry m_AssetRegistry;
    Engine::AssetWatcher m_AssetWatcher;
    Engine::GltfImportResult m_LastGltfImport;
    Engine::MaterialLibrary m_MaterialLibrary;
    Engine::FramePacingPolicy m_ProjectFramePacingPolicy;
    Engine::PresentationPolicy m_ProjectPresentationPolicy = Engine::PresentationPolicy::Synchronized;
    Engine::RendererColorPipelineSettings m_ProjectColorPipelineSettings;
    // Command-line policy is session-only; project serialization always keeps
    // the project-owned request intact.
    std::optional<Engine::PresentationPolicy> m_RuntimePresentationPolicyOverride;
    Engine::GameFramePacingSettings m_GameFramePacingSettings;
    Engine::Scene m_ActiveScene { "Sample Scene" };
    Engine::Entity m_PrototypeMeshEntity;
    Engine::Entity m_DirectionalLightEntity;
    Engine::Entity m_PlayerStartEntity;
    Engine::Entity m_SceneOriginRasterMeshEntity;
    Engine::Entity m_EditorMaterialControlSharedPeer;
    Engine::Entity m_SelectedEntity;
    Engine::SceneDebugView m_SceneDebugView = Engine::SceneDebugView::Lit;
    bool m_ShowSelectedBounds = true;
    bool m_ShowOccludedSelectionBounds = false;
    Engine::EditorCamera m_EditorCamera;
    Engine::CameraViewOriginTracker m_ViewportOriginTracker;
    bool m_ViewportDiscontinuousRelocationPending = true;
    bool m_ViewportHovered = false;
    bool m_ViewportFocused = false;
    bool m_ViewportNavigationFocusAvailable = false;
    bool m_ViewportFocusRequested = false;
    bool m_ViewportNavigationInputEnabled = false;
    bool m_WindowFocused = true;
    bool m_CursorCaptured = false;
    bool m_CursorCapturePending = false;
    bool m_CursorCaptureBaselineArmed = false;
    bool m_LeftMouseDown = false;
    bool m_RightMouseDown = false;
    bool m_MiddleMouseDown = false;
    bool m_HasMousePosition = false;
    bool m_FusionNavigationPivotValid = false;
    std::array<bool, 512> m_KeyDown {};
    double m_MouseX = 0.0;
    double m_MouseY = 0.0;
    double m_CursorRestoreX = 0.0;
    double m_CursorRestoreY = 0.0;
    float m_MouseDeltaX = 0.0f;
    float m_MouseDeltaY = 0.0f;
    float m_MouseWheelDelta = 0.0f;
    float m_ViewportNavigationSpeed = 4.0f;
    ViewportNavigationPreset m_ViewportNavigationPreset = ViewportNavigationPreset::Fusion;
    Engine::Math::DVec3 m_FusionNavigationPivot {};
    float m_ViewportImageX = 0.0f;
    float m_ViewportImageY = 0.0f;
    float m_ViewportImageWidth = 1600.0f;
    float m_ViewportImageHeight = 900.0f;
    std::shared_ptr<const Engine::SceneRenderSnapshot> m_FirstSceneRenderSnapshot;
    std::array<double, 3> m_CameraPosition = { 0.0, 0.0, -3.35 };
    std::array<float, 3> m_CameraRotation = { 0.0f, 0.0f, 0.0f };
    std::array<double, 3> m_FramePacingInitialCameraPosition {};
    std::array<float, 3> m_FramePacingInitialCameraRotation {};
    float m_CameraFovDegrees = 60.0f;
    float m_CameraNearClip = 0.1f;
    float m_CameraFarClip = 100.0f;
    unsigned int m_ReimportRequestCount = 0;
    Engine::AssetHandle m_SelectedAssetHandle = Engine::kInvalidAssetHandle;
    Engine::AssetType m_AssetBrowserTypeFilter = Engine::AssetType::Unknown;
    std::array<char, 128> m_AssetBrowserFilter {};
    std::array<char, 128> m_HierarchyFilter {};
    std::array<char, 512> m_GltfImportPath {};
    std::array<char, 128> m_NewProjectName { 'U', 'n', 't', 'i', 't', 'l', 'e', 'd' };
    std::array<char, 512> m_NewProjectParentPath { 'o', 'u', 't', 'p', 'u', 't', '/', 'p', 'r', 'o', 'j', 'e', 'c', 't', 's' };
    std::vector<std::string> m_ConsoleLines;
    EditorMaterialControlMailbox m_EditorMaterialControl;
    Engine::AssetHandle m_EditorMaterialControlSmokeMaterial = Engine::kInvalidAssetHandle;
    Engine::MaterialSurface m_EditorMaterialControlSmokeBefore;
    Engine::MaterialSurface m_EditorMaterialControlSmokeAfter;
    Engine::u64 m_EditorMaterialControlSmokeInitialRendererGeneration = 0;
    std::size_t m_EditorMaterialControlSmokeInitialUndoDepth = 0;
    std::size_t m_EditorMaterialControlSmokeInitialRedoDepth = 0;
    std::string m_EditorMaterialControlSmokePatchRequest;
    std::string m_EditorMaterialControlSmokePatchReceipt;
    Engine::Entity m_EditorSceneControlV2InitialSelection;
    Engine::TransformComponent m_EditorSceneControlV2PrototypeBefore;
    Engine::Math::SectorLocalPosition m_EditorSceneControlV2PrototypeAfterPosition;
    Engine::Math::Vec3 m_EditorSceneControlV2PrototypeAfterRotation;
    Engine::Math::Vec3 m_EditorSceneControlV2PrototypeAfterScale;
    Engine::TransformComponent m_EditorSceneControlV2CameraBefore;
    Engine::Math::SectorLocalPosition m_EditorSceneControlV2CameraAfterPosition;
    Engine::Math::Vec3 m_EditorSceneControlV2CameraAfterRotation;
    Engine::LightComponent m_EditorSceneControlV2LightBefore;
    Engine::LightComponent m_EditorSceneControlV2LightAfter;
    Engine::RendererColorPipelineSettings m_EditorSceneControlV2ColorBefore;
    Engine::RendererColorPipelineSettings m_EditorSceneControlV2ColorAfter;
    Engine::MeshRendererComponent m_EditorSceneControlV2MeshBefore;
    Engine::MeshRendererComponent m_EditorSceneControlV2MeshAfter;
    Engine::SceneDebugView m_EditorSceneControlV2DebugViewBefore =
        Engine::SceneDebugView::Lit;
    Engine::SceneDebugView m_EditorSceneControlV2DebugViewAfter =
        Engine::SceneDebugView::MaterialId;
    bool m_EditorSceneControlV2BoundsBefore = true;
    bool m_EditorSceneControlV2BoundsAfter = false;

    // ---- Undo history state ----
    HistoryAdapter m_HistoryAdapter { *this };
    std::unique_ptr<HistoryStoreType> m_HistoryStore = std::make_unique<HistoryStoreType>(m_HistoryAdapter);
    EditorHistory::EditGestureTracker m_EditGestureTracker;
    // Before of the open Inspector gesture, kept so End can stamp After and detect "no change".
    HistorySnapshot m_EditGestureBefore;
    // Count of recorded entries that edited the viewport camera (see EditorHistoryState).
    Engine::u64 m_CameraEpoch = 0;
    // Latest history announcement: the menu-bar status text shows it for four seconds
    // (m_StatusExpiresAtMs on the steady millisecond clock), the Console keeps it, and
    // the typed receipt carries it.
    std::string m_LastHistoryAnnouncement;
    Engine::u64 m_StatusExpiresAtMs = 0;
    // The History panel's persistent eviction line (cleared when the history is re-based).
    std::string m_LastEvictionNotice;
    bool m_ShowClearHistoryPopup = false;
    // `--editor-history-budget-bytes=N` and `--editor-history-max-entries=N` shrink the
    // limits so a smoke can reach eviction without recording 512 entries.
    EditorHistory::HistoryConfig m_HistoryConfig;
    bool m_EditorHistorySmokeRequested = false;
    bool m_EditorHistorySmokeCompleted = false;
    bool m_EditorHistoryBenchmarkRequested = false;
    // Snapshots captured since start-up (a Scene, registry and library copy each). The
    // Inspector must add none while idle and two per gesture; the smokes assert it.
    mutable Engine::u64 m_HistorySnapshotCaptures = 0;
    // Smoke seam: receives the screen rectangle of every Inspector widget that records
    // history, so an in-process ImGui test can aim its pointer without hard-coded layout.
    std::function<void(EditorHistory::EditProperty, const ImVec2&, const ImVec2&)> m_WidgetProbe;
    // Smoke seam: receives the row index and screen rectangle of each History panel row.
    std::function<void(size_t, const ImVec2&, const ImVec2&)> m_HistoryRowProbe;

    // Click-to-select: a left press and release in the viewport image that stays under
    // a distance and time limit is a pick, anything longer is navigation. Tracked from
    // engine events, resolved when the button is released.
    struct ViewportClickState
    {
        bool Armed = false;
        double PressX = 0.0;
        double PressY = 0.0;
        double LastX = 0.0;
        double LastY = 0.0;
        double Movement = 0.0;
        std::chrono::steady_clock::time_point PressTime {};
    };
    ViewportClickState m_ViewportClick;
    // Latched by DrawViewportPanel: the pointer is over the viewport image itself and
    // no ImGui widget, popup, or active item owns it.
    bool m_ViewportPickAvailable = false;
    // The viewport image rectangle (m_ViewportImage*) was refreshed by the last UI
    // frame. Headless runs and a closed Viewport panel use a deterministic virtual
    // rectangle instead (see GetViewportPickGeometry).
    bool m_ViewportImageValid = false;
    // Scroll the hierarchy row of the selected entity into view once (selection came
    // from somewhere other than a hierarchy click).
    bool m_HierarchyScrollRequest = false;
    // Animated F framing: the camera eases from Start to Target; any navigation input
    // cancels it.
    struct FocusAnimationState
    {
        bool Active = false;
        std::array<double, 3> Start {};
        std::array<double, 3> Target {};
        double Elapsed = 0.0;
    };
    FocusAnimationState m_FocusAnimation;
    // Immutable mesh geometry for picking, keyed by asset handle and cooked root. A
    // legacy (empty) cooked root is mutable and is reloaded for every pick instead.
    std::vector<std::shared_ptr<const PickMeshData>> m_PickMeshCache;
    std::vector<std::shared_ptr<const PickMeshData>> m_PickMeshScratch;

    // Panel visibility (close buttons and the Window menu), persisted workspace-wide.
    enum PanelIndex : size_t
    {
        PanelSceneHierarchy,
        PanelInspector,
        PanelViewport,
        PanelContentBrowser,
        PanelConsole,
        PanelProfiler,
        PanelHistory,
        kPanelCount
    };
    std::array<bool, kPanelCount> m_PanelVisible { true, true, true, true, true, true, true };
    std::array<bool, kPanelCount> m_PanelVisiblePersisted { true, true, true, true, true, true, true };
    std::string m_PanelVisibilityPath = "output/editor/panel-visibility.spiralsettings";

    // Seam for `--editor-viewport-click-smoke`: the headless window has no cursor, so the
    // smoke supplies the press position here instead of reading it from the window.
    std::optional<std::array<double, 2>> m_ViewportClickCursorOverride;
    bool m_PanelUiSmokeRequested = false;
    ImGuiContext* m_PanelUiSmokeContext = nullptr;
    unsigned int m_PanelUiSmokeFrames = 0;
    bool m_ViewportClickSmokeRequested = false;
    bool m_ViewportClickSmokeCompleted = false;

    // `--editor-control-viewport-picking-helper-smoke`: an external helper drives
    // PickAtViewportPoint and FocusSelection through the mailbox.
    bool m_EditorViewportPickingHelperSmokeRequested = false;
    bool m_EditorViewportPickingHelperSmokeCompleted = false;
    Engine::Entity m_EditorViewportPickingInitialSelection;
    size_t m_EditorViewportPickingBaseUndoDepth = 0;
};
