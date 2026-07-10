#pragma once

#include <Engine.h>

#include <array>
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
    void HandleAssetWatchEvents();
    void RunAssetWatchSmokeMutation();
    bool SaveActiveScene();
    bool SaveAssetRegistry();
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
    std::string m_CaptureViewportPath = "output/captures/editor-viewport.bmp";
    std::string m_ScenePath = "output/scenes/sample.spiral";
    std::string m_AssetRegistryPath = "output/assets/sample.assets";
    std::string m_AssetWatchSmokePath = "output/assets/watch-smoke.mesh";
    Engine::ClearColor m_ClearColor;
    Engine::AssetRegistry m_AssetRegistry;
    Engine::AssetWatcher m_AssetWatcher;
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
    std::vector<std::string> m_ConsoleLines;
};
