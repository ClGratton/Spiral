#include "EditorLayer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <stdexcept>

EditorLayer::EditorLayer()
    : Engine::Layer("EditorLayer")
{
}

void EditorLayer::OnAttach()
{
    Engine::Log::Info("Editor layer attached");
    m_ClearColor = Engine::Renderer::GetClearColor();
    m_ConsoleLines.emplace_back("Editor booted");
    m_ConsoleLines.emplace_back("GLFW window backend active");
    m_ConsoleLines.emplace_back(std::string("Renderer backend: ") + Engine::Renderer::GetActiveBackendName());
    EnsureDefaultSceneEntities();
    m_CaptureViewportRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--capture-viewport");
    m_SaveSceneSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--save-scene-smoke");
    if (m_CaptureViewportRequested)
        m_ConsoleLines.emplace_back(std::string("Viewport capture requested: ") + m_CaptureViewportPath);
    if (m_SaveSceneSmokeRequested)
    {
        if (!SaveActiveScene())
            throw std::runtime_error("Scene save smoke failed");
    }

    const Engine::RendererBuildInfo& buildInfo = Engine::Renderer::GetBuildInfo();
    if (Engine::Renderer::GetActiveBackend() == Engine::RendererBackend::NVRHID3D12)
        m_ConsoleLines.emplace_back("Native D3D12 prototype mesh pass active");
    else if (!buildInfo.HasNVRHID3D12)
        m_ConsoleLines.emplace_back("Native D3D12 viewport unavailable in this executable; run the VS2022 build path on Windows");
    else
        m_ConsoleLines.emplace_back("Native D3D12 backend compiled, but it did not become the active renderer");
}

void EditorLayer::OnDetach()
{
    Engine::Log::Info("Editor layer detached");
}

void EditorLayer::OnUpdate(Engine::Timestep timestep)
{
    ++m_FrameCounter;
    m_LastFrameMs = timestep.GetMilliseconds();

    if (m_FrameCounter == 1)
    {
        Engine::JobSystem::Get().Submit([]
        {
            Engine::Log::Trace("Editor background validation job completed");
        }, "EditorValidation");
    }
}

void EditorLayer::OnUiRender()
{
    if (!ImGui::GetCurrentContext())
        return;

    DrawDockspace();
    DrawSceneHierarchyPanel();
    DrawInspectorPanel();
    DrawViewportPanel();
    DrawConsolePanel();
    DrawProfilerPanel();
    DrawProjectPanel();

    if (m_CaptureViewportRequested && !m_CaptureViewportComplete && m_FrameCounter >= 2)
    {
        const bool captured = Engine::Renderer::CaptureViewportToFile(m_CaptureViewportPath);
        m_CaptureViewportComplete = true;
        m_ConsoleLines.emplace_back(captured
            ? std::string("Viewport capture saved: ") + m_CaptureViewportPath
            : std::string("Viewport capture failed: ") + m_CaptureViewportPath);
    }

    if (m_ShowDemoWindow)
        ImGui::ShowDemoWindow(&m_ShowDemoWindow);
}

void EditorLayer::OnEvent(Engine::Event& event)
{
    Engine::Log::Trace("Editor received event: ", event.ToString());
}

void EditorLayer::DrawDockspace()
{
    static bool dockspaceOpen = true;
    static bool fullscreen = true;
    static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (fullscreen)
    {
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        windowFlags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    }
    windowFlags |= ImGuiWindowFlags_NoBackground;

    ImGui::Begin("Editor Dockspace", &dockspaceOpen, windowFlags);

    if (fullscreen)
        ImGui::PopStyleVar(2);

    DrawMainMenuBar();

    const ImGuiID dockspaceId = ImGui::GetID("MainEditorDockspace");
    if (m_ResetDockLayout)
    {
        BuildDefaultDockLayout(dockspaceId, ImGui::GetContentRegionAvail());
        m_ResetDockLayout = false;
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);
    ImGui::End();
}

void EditorLayer::DrawMainMenuBar()
{
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Project"))
            m_ConsoleLines.emplace_back("New Project workflow is not implemented yet");
        if (ImGui::MenuItem("Open Project"))
            m_ConsoleLines.emplace_back("Open Project workflow is not implemented yet");
        if (ImGui::MenuItem("Save Scene"))
            SaveActiveScene();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))
            Engine::Application::Get().Close();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowDemoWindow);
        if (ImGui::MenuItem("Reset Layout"))
            m_ResetDockLayout = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools"))
    {
        if (ImGui::MenuItem("Validate Project"))
            m_ConsoleLines.emplace_back("Validation queued");
        if (ImGui::MenuItem("Compile Shaders"))
            m_ConsoleLines.emplace_back("Shader compiler is not implemented yet");
        if (ImGui::MenuItem("Capture Viewport"))
        {
            m_CaptureViewportRequested = true;
            m_CaptureViewportComplete = false;
            m_ConsoleLines.emplace_back(std::string("Viewport capture queued: ") + m_CaptureViewportPath);
        }
        if (ImGui::MenuItem("Build Motion Pack"))
            m_ConsoleLines.emplace_back("Motion pack builder is not implemented yet");
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void EditorLayer::BuildDefaultDockLayout(unsigned int dockspaceId, const ImVec2& dockspaceSize)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

    ImGuiID centerDock = dockspaceId;
    ImGuiID leftDock = 0;
    ImGuiID rightDock = 0;
    ImGuiID rightTopDock = 0;
    ImGuiID rightBottomDock = 0;
    ImGuiID bottomDock = 0;

    ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Left, 0.20f, &leftDock, &centerDock);
    ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Right, 0.31f, &rightDock, &centerDock);
    ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Down, 0.28f, &bottomDock, &centerDock);
    ImGui::DockBuilderSplitNode(rightDock, ImGuiDir_Up, 0.50f, &rightTopDock, &rightBottomDock);

    ImGui::DockBuilderDockWindow("Viewport", centerDock);
    ImGui::DockBuilderDockWindow("Project", leftDock);
    ImGui::DockBuilderDockWindow("Console", bottomDock);
    ImGui::DockBuilderDockWindow("Inspector", rightTopDock);
    ImGui::DockBuilderDockWindow("Profiler", rightTopDock);
    ImGui::DockBuilderDockWindow("Scene Hierarchy", rightBottomDock);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorLayer::DrawSceneHierarchyPanel()
{
    ImGui::Begin("Scene Hierarchy");
    ImGui::TextUnformatted(m_ActiveScene.GetName().c_str());
    ImGui::Separator();

    if (ImGui::TreeNodeEx("World", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (const Engine::SceneEntity& entity : m_ActiveScene.GetEntities())
            ImGui::BulletText("%s", entity.Name.c_str());
        ImGui::TreePop();
    }

    ImGui::End();
}

void EditorLayer::DrawInspectorPanel()
{
    ImGui::Begin("Inspector");
    ImGui::TextUnformatted("Selected: Prototype Mesh");
    ImGui::Separator();
    ImGui::TextUnformatted("Transform");
    ImGui::DragFloat3("Position", m_Position.data(), 0.1f);
    ImGui::DragFloat3("Rotation", m_Rotation.data(), 0.5f);
    ImGui::DragFloat3("Scale", m_Scale.data(), 0.05f, 0.01f, 100.0f);
    ImGui::Separator();
    ImGui::TextUnformatted("Editor Camera");
    bool cameraChanged = false;
    cameraChanged |= ImGui::DragFloat3("Camera Position", m_CameraPosition.data(), 0.05f);
    cameraChanged |= ImGui::DragFloat3("Camera Rotation", m_CameraRotation.data(), 0.25f);
    cameraChanged |= ImGui::DragFloat("Vertical FOV", &m_CameraFovDegrees, 0.25f, 20.0f, 110.0f);
    cameraChanged |= ImGui::DragFloat("Near Clip", &m_CameraNearClip, 0.01f, 0.01f, 10.0f);
    cameraChanged |= ImGui::DragFloat("Far Clip", &m_CameraFarClip, 1.0f, 1.0f, 10000.0f);
    if (cameraChanged)
    {
        if (m_CameraFarClip <= m_CameraNearClip)
            m_CameraFarClip = m_CameraNearClip + 1.0f;

        m_EditorCamera.SetPosition({ m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] });
        m_EditorCamera.SetRotationDegrees({ m_CameraRotation[0], m_CameraRotation[1], m_CameraRotation[2] });
        m_EditorCamera.SetProjection({ m_CameraFovDegrees, m_CameraNearClip, m_CameraFarClip });
        Engine::Renderer::SetCameraView(m_EditorCamera.GetCameraView());
    }
    ImGui::TextDisabled("Aspect %.3f", m_EditorCamera.GetAspectRatio());
    ImGui::Separator();
    ImGui::TextUnformatted("Renderer");
    ImGui::Text("Active: %s", Engine::Renderer::GetActiveBackendName());
    DrawRendererBackendSelector();
    const Engine::RendererBuildInfo& buildInfo = Engine::Renderer::GetBuildInfo();
    if (!buildInfo.HasNVRHID3D12)
        ImGui::TextDisabled("Native viewport requires the Windows VS2022 build.");
    ImGui::Separator();
    if (ImGui::ColorEdit4("Clear Color", &m_ClearColor.R))
        Engine::Renderer::SetClearColor(m_ClearColor);
    ImGui::End();
}

void EditorLayer::DrawRendererBackendSelector()
{
    const std::vector<Engine::RendererBackendOption>& options = Engine::Renderer::GetBackendOptions();
    const char* activeName = Engine::Renderer::GetActiveBackendName();

    if (!ImGui::BeginCombo("Backend", activeName))
        return;

    for (const Engine::RendererBackendOption& option : options)
    {
        const bool disabled = !option.Selectable;
        if (disabled)
            ImGui::BeginDisabled();

        if (ImGui::Selectable(option.Name, option.Active))
        {
            if (Engine::Renderer::RequestBackend(option.Backend))
                m_ConsoleLines.emplace_back(std::string("Renderer backend selected: ") + option.Name);
            else
                m_ConsoleLines.emplace_back(std::string("Renderer backend unavailable: ") + option.Name);
        }

        if (option.Active)
            ImGui::SetItemDefaultFocus();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && option.Detail && option.Detail[0] != '\0')
            ImGui::SetTooltip("%s", option.Detail);

        if (disabled)
            ImGui::EndDisabled();
    }

    ImGui::EndCombo();
}

void EditorLayer::DrawViewportPanel()
{
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoBackground);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.0f || size.y < 1.0f)
    {
        Engine::Renderer::SetViewportRect({});
        ImGui::End();
        return;
    }

    const auto viewportWidth = static_cast<Engine::u32>(size.x);
    const auto viewportHeight = static_cast<Engine::u32>(size.y);
    m_EditorCamera.SetViewportSize(viewportWidth, viewportHeight);
    Engine::Renderer::SetCameraView(m_EditorCamera.GetCameraView());

    const bool hasNativeViewportTexture = Engine::Renderer::PrepareViewportTexture(viewportWidth, viewportHeight);
    const Engine::u64 viewportTextureId = Engine::Renderer::GetViewportTextureId();

    if (hasNativeViewportTexture && viewportTextureId != 0)
        ImGui::Image(static_cast<ImTextureID>(viewportTextureId), size);
    else
        ImGui::InvisibleButton("ViewportCanvas", size);

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRect(min, max, IM_COL32(70, 80, 90, 255));

    Engine::RenderViewportRect viewportRect;
    viewportRect.X = static_cast<int>(min.x);
    viewportRect.Y = static_cast<int>(min.y);
    viewportRect.Width = static_cast<int>(max.x - min.x);
    viewportRect.Height = static_cast<int>(max.y - min.y);
    Engine::Renderer::SetViewportRect(viewportRect);

    const char* title = hasNativeViewportTexture ? "Renderer target" : "Renderer preview";
    const std::string subtitle = hasNativeViewportTexture
        ? std::string("Active backend: ") + Engine::Renderer::GetActiveBackendName() + "; native D3D12 prototype mesh pass"
        : std::string("Active backend: ") + Engine::Renderer::GetActiveBackendName() + "; native viewport unavailable";
    drawList->AddText(ImVec2(min.x + 18.0f, min.y + 18.0f), IM_COL32(230, 235, 240, 255), title);
    drawList->AddText(ImVec2(min.x + 18.0f, min.y + 40.0f), IM_COL32(150, 160, 170, 255), subtitle.c_str());

    const Engine::RendererBuildInfo& buildInfo = Engine::Renderer::GetBuildInfo();
    if (!hasNativeViewportTexture && !buildInfo.HasNVRHID3D12)
        drawList->AddText(ImVec2(min.x + 18.0f, min.y + 62.0f), IM_COL32(120, 130, 140, 255), "Open the VS2022 build for D3D12 rendering.");

    ImGui::End();
}

void EditorLayer::DrawConsolePanel()
{
    ImGui::Begin("Console");
    if (ImGui::Button("Clear"))
        m_ConsoleLines.clear();
    ImGui::SameLine();
    if (ImGui::Button("Add Test Message"))
        m_ConsoleLines.emplace_back("Manual console message");
    ImGui::Separator();

    for (const std::string& line : m_ConsoleLines)
        ImGui::TextWrapped("%s", line.c_str());

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::End();
}

void EditorLayer::DrawProfilerPanel()
{
    ImGui::Begin("Profiler");
    const Engine::RendererFrameTiming& timing = Engine::Renderer::GetLastFrameTiming();

    ImGui::Text("Frame: %u", m_FrameCounter);
    ImGui::Text("Last frame: %.3f ms", m_LastFrameMs);
    ImGui::Text("Workers: %u", Engine::JobSystem::Get().GetWorkerCount());
    ImGui::Separator();
    ImGui::Text("Renderer CPU: %.3f ms", timing.CpuMilliseconds);
    ImGui::Text("GPU timestamps: %s", Engine::ToString(timing.GpuStatus));
    if (timing.GpuStatus == Engine::RendererTimingStatus::Pending)
        ImGui::TextDisabled("Timestamp query API is present; backend resolve path is next.");

    if (timing.Passes.empty())
    {
        ImGui::TextDisabled("No renderer passes recorded yet");
    }
    else if (ImGui::BeginTable("PassTimings", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("CPU ms");
        ImGui::TableSetupColumn("GPU ms");
        ImGui::TableHeadersRow();

        for (const Engine::RendererPassTiming& pass : timing.Passes)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(pass.Name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f", pass.CpuMilliseconds);
            ImGui::TableSetColumnIndex(2);
            if (pass.GpuStatus == Engine::RendererTimingStatus::Ready)
                ImGui::Text("%.3f", pass.GpuMilliseconds);
            else
                ImGui::TextDisabled("%s", Engine::ToString(pass.GpuStatus));
        }

        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Overdraw, quad waste, ray density, texture residency coming later");
    ImGui::End();
}

void EditorLayer::DrawProjectPanel()
{
    ImGui::Begin("Project");
    ImGui::TextUnformatted("Guided workflows");
    ImGui::Separator();
    ImGui::BulletText("First playable");
    ImGui::BulletText("Visual style");
    ImGui::BulletText("Asset import");
    ImGui::BulletText("Performance validation");
    ImGui::Separator();
    ImGui::TextDisabled("Automation graph not implemented yet");
    ImGui::End();
}

bool EditorLayer::SaveActiveScene()
{
    SyncSceneFromEditorState();

    const bool saved = m_ActiveScene.SaveToFile(m_ScenePath);
    if (saved)
    {
        Engine::Scene loadedScene;
        const bool loaded = Engine::Scene::LoadFromFile(m_ScenePath, loadedScene);
        if (loaded)
            Engine::Log::Info("Scene saved and reload-validated: ", m_ScenePath);
        else
            Engine::Log::Error("Scene saved but reload validation failed: ", m_ScenePath);
        m_ConsoleLines.emplace_back(loaded
            ? std::string("Scene saved: ") + m_ScenePath
            : std::string("Scene saved but reload validation failed: ") + m_ScenePath);
        return loaded;
    }

    Engine::Log::Error("Scene save failed: ", m_ScenePath);
    m_ConsoleLines.emplace_back(std::string("Scene save failed: ") + m_ScenePath);
    return false;
}

void EditorLayer::SyncSceneFromEditorState()
{
    Engine::TransformComponent cameraTransform;
    cameraTransform.Position = { m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] };
    cameraTransform.RotationDegrees = { m_CameraRotation[0], m_CameraRotation[1], m_CameraRotation[2] };
    cameraTransform.Scale = { 1.0f, 1.0f, 1.0f };

    Engine::CameraComponent camera;
    camera.Primary = true;
    camera.Projection = { m_CameraFovDegrees, m_CameraNearClip, m_CameraFarClip };

    m_ActiveScene.SetMainCameraTransform(cameraTransform);
    m_ActiveScene.SetMainCamera(camera);

    if (Engine::TransformComponent* transform = m_ActiveScene.TryGetTransform(m_PrototypeMeshEntity))
    {
        transform->Position = { m_Position[0], m_Position[1], m_Position[2] };
        transform->RotationDegrees = { m_Rotation[0], m_Rotation[1], m_Rotation[2] };
        transform->Scale = { m_Scale[0], m_Scale[1], m_Scale[2] };
    }
}

void EditorLayer::EnsureDefaultSceneEntities()
{
    m_PrototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
    if (!m_PrototypeMeshEntity)
        m_PrototypeMeshEntity = m_ActiveScene.CreateEntity("Prototype Mesh");
    if (!m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity))
    {
        Engine::MeshRendererComponent meshRenderer;
        meshRenderer.MeshName = "Prototype Cube";
        m_ActiveScene.AddMeshRendererComponent(m_PrototypeMeshEntity, meshRenderer);
    }

    m_DirectionalLightEntity = m_ActiveScene.FindEntityByName("Directional Light");
    if (!m_DirectionalLightEntity)
        m_DirectionalLightEntity = m_ActiveScene.CreateEntity("Directional Light");
    if (!m_ActiveScene.TryGetLightComponent(m_DirectionalLightEntity))
    {
        Engine::LightComponent light;
        light.Type = Engine::LightType::Directional;
        light.Color = { 1.0f, 0.96f, 0.86f };
        light.Intensity = 3.0f;
        m_ActiveScene.AddLightComponent(m_DirectionalLightEntity, light);
        if (Engine::TransformComponent* transform = m_ActiveScene.TryGetTransform(m_DirectionalLightEntity))
            transform->RotationDegrees = { 45.0f, -35.0f, 0.0f };
    }

    m_PlayerStartEntity = m_ActiveScene.FindEntityByName("Player Start");
    if (!m_PlayerStartEntity)
        m_PlayerStartEntity = m_ActiveScene.CreateEntity("Player Start");
}
