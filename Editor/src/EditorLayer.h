#pragma once

#include <Engine.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

struct ImVec2;

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
    struct HistoryState;
    struct HistoryEntry;

    void DrawDockspace();
    void DrawMainMenuBar();
    void BuildDefaultDockLayout(unsigned int dockspaceId, const ImVec2& dockspaceSize);
    void DrawSceneHierarchyPanel();
    void DrawInspectorPanel();
    void DrawRendererBackendSelector();
    void ApplyEditorCameraStateToScene();
    void SyncEditorCameraStateFromMainCamera();
    void DrawViewportPanel();
    void DrawConsolePanel();
    void DrawProfilerPanel();
    void DrawProjectPanel();
    void DrawNewProjectDialog();
    bool DrawMaterialAssetControls(Engine::AssetHandle handle);
    void HandleAssetWatchEvents();
    void RunAssetWatchSmokeMutation();
    void RunGltfImportSmoke();
    void RunMaterialAssetSmoke();
    void RunUndoRedoSmoke();
    void RunSceneAuthoringSmoke();
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
    void RecordHistory(std::string label, HistoryState before);
    bool Undo();
    bool Redo();
    HistoryState CaptureHistoryState() const;
    void RestoreHistoryState(const HistoryState& state);
    void EnsureDefaultSceneEntities();

private:
    unsigned int m_FrameCounter = 0;
    float m_LastFrameMs = 0.0f;
    bool m_ShowDemoWindow = false;
    bool m_ResetDockLayout = true;
    bool m_CaptureViewportRequested = false;
    bool m_CaptureViewportComplete = false;
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
    bool m_ShowNewProjectDialog = false;
    std::string m_CaptureViewportPath = "output/captures/editor-viewport.bmp";
    std::string m_ProjectPath = "output/projects/default.spiralproject";
    std::string m_ScenePath = "output/scenes/sample.spiral";
    std::string m_AssetRegistryPath = "output/assets/sample.assets";
    std::string m_AssetWatchSmokePath = "output/assets/watch-smoke.mesh";
    std::string m_GltfImportSmokePath = "output/assets/gltf-smoke/triangle.gltf";
    std::string m_MaterialAssetSmokePath = "output/assets/material-smoke.spiralmat";
    Engine::ClearColor m_ClearColor;
    Engine::AssetRegistry m_AssetRegistry;
    Engine::AssetWatcher m_AssetWatcher;
    Engine::GltfImportResult m_LastGltfImport;
    Engine::MaterialLibrary m_MaterialLibrary;
    Engine::Scene m_ActiveScene { "Sample Scene" };
    Engine::Entity m_PrototypeMeshEntity;
    Engine::Entity m_DirectionalLightEntity;
    Engine::Entity m_PlayerStartEntity;
    Engine::Entity m_SelectedEntity;
    Engine::EditorCamera m_EditorCamera;
    std::array<float, 3> m_CameraPosition = { 0.0f, 0.0f, -3.35f };
    std::array<float, 3> m_CameraRotation = { 0.0f, 0.0f, 0.0f };
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

    struct HistoryState
    {
        Engine::Scene Scene { "History Scene" };
        Engine::AssetRegistry AssetRegistry;
        Engine::MaterialLibrary MaterialLibrary;
        Engine::ClearColor ClearColor;
        Engine::Entity SelectedEntity;
        std::array<float, 3> CameraPosition {};
        std::array<float, 3> CameraRotation {};
        float CameraFovDegrees = 60.0f;
        float CameraNearClip = 0.1f;
        float CameraFarClip = 100.0f;
    };

    struct HistoryEntry
    {
        std::string Label;
        HistoryState Before;
        HistoryState After;
    };

    std::vector<HistoryEntry> m_UndoHistory;
    std::vector<HistoryEntry> m_RedoHistory;
};
