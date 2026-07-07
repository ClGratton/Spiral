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
    void DrawViewportPanel();
    void DrawConsolePanel();
    void DrawProfilerPanel();
    void DrawProjectPanel();

private:
    unsigned int m_FrameCounter = 0;
    float m_LastFrameMs = 0.0f;
    bool m_ShowDemoWindow = false;
    bool m_ResetDockLayout = true;
    bool m_CaptureViewportRequested = false;
    bool m_CaptureViewportComplete = false;
    std::string m_CaptureViewportPath = "output/captures/editor-viewport.bmp";
    Engine::ClearColor m_ClearColor;
    std::array<float, 3> m_Position = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_Rotation = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_Scale = { 1.0f, 1.0f, 1.0f };
    std::vector<std::string> m_ConsoleLines;
};
