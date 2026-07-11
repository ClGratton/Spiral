#include "EditorLayer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace
{
    constexpr const char* AssetDragPayloadType = "SPIRAL_ASSET_HANDLE";
    constexpr int ProjectFormatVersion = 1;

    struct AssetDragPayload
    {
        Engine::AssetHandle Handle = Engine::kInvalidAssetHandle;
        Engine::AssetType Type = Engine::AssetType::Unknown;
    };

    struct ProjectManifest
    {
        std::string ScenePath;
        std::string AssetRegistryPath;
    };

    const char* ToLightTypeName(Engine::LightType type)
    {
        switch (type)
        {
            case Engine::LightType::Directional: return "Directional";
            case Engine::LightType::Point: return "Point";
            case Engine::LightType::Spot: return "Spot";
        }

        return "Directional";
    }

    bool DrawVec3Control(const char* label, Engine::Math::Vec3& value, float speed, float min = 0.0f, float max = 0.0f)
    {
        float values[3] = { value.X, value.Y, value.Z };
        if (!ImGui::DragFloat3(label, values, speed, min, max))
            return false;

        value = { values[0], values[1], values[2] };
        return true;
    }

    bool DrawAssetHandleControl(
        const char* label,
        Engine::AssetHandle& handle,
        const Engine::AssetRegistry& assetRegistry,
        Engine::AssetType expectedType)
    {
        Engine::u64 value = handle;
        bool changed = ImGui::InputScalar(label, ImGuiDataType_U64, &value);
        if (changed)
            handle = value;

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(AssetDragPayloadType))
            {
                if (payload->DataSize == sizeof(AssetDragPayload))
                {
                    const AssetDragPayload& droppedAsset = *static_cast<const AssetDragPayload*>(payload->Data);
                    if (droppedAsset.Type == expectedType && assetRegistry.Contains(droppedAsset.Handle))
                    {
                        handle = droppedAsset.Handle;
                        changed = true;
                    }
                }
            }

            ImGui::EndDragDropTarget();
        }

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag a registered %s asset here", Engine::ToString(expectedType));

        return changed;
    }

    bool AssetMatchesFilter(const Engine::AssetMetadata& metadata, const char* filter, Engine::AssetType typeFilter)
    {
        if (typeFilter != Engine::AssetType::Unknown && metadata.Type != typeFilter)
            return false;

        if (filter[0] == '\0')
            return true;

        std::string searchable = metadata.Name + " " + metadata.SourcePath + " " + Engine::ToString(metadata.Type);
        std::transform(
            searchable.begin(),
            searchable.end(),
            searchable.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

        std::string needle = filter;
        std::transform(
            needle.begin(),
            needle.end(),
            needle.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return searchable.find(needle) != std::string::npos;
    }

    std::string GetMeshDisplayName(const Engine::MeshRendererComponent& meshRenderer, const Engine::AssetRegistry& assetRegistry)
    {
        if (const Engine::AssetMetadata* meshAsset = assetRegistry.GetAsset(meshRenderer.MeshAsset))
            return meshAsset->Name;

        return meshRenderer.MeshName;
    }

    bool WriteTextFile(const std::filesystem::path& path, const std::string& text)
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);
        if (error)
            return false;

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output)
            return false;

        output << text;
        return true;
    }

    bool WriteBinaryFile(const std::filesystem::path& path, const void* data, std::size_t size)
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);
        if (error)
            return false;

        std::ofstream output(path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output)
            return false;

        output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        return static_cast<bool>(output);
    }

    bool WriteProjectManifest(const std::filesystem::path& path, const ProjectManifest& manifest)
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);
        if (error)
            return false;

        std::ofstream output(path, std::ios::out | std::ios::trunc);
        if (!output)
            return false;

        output << "SpiralProject " << ProjectFormatVersion << '\n';
        output << "Scene " << std::quoted(manifest.ScenePath) << '\n';
        output << "AssetRegistry " << std::quoted(manifest.AssetRegistryPath) << '\n';
        return static_cast<bool>(output);
    }

    bool ReadProjectManifest(const std::filesystem::path& path, ProjectManifest& outManifest)
    {
        std::ifstream input(path);
        if (!input)
            return false;

        std::string magic;
        int version = 0;
        if (!(input >> magic >> version) || magic != "SpiralProject" || version != ProjectFormatVersion)
            return false;

        ProjectManifest manifest;
        std::string key;
        while (input >> key)
        {
            if (key == "Scene")
                input >> std::quoted(manifest.ScenePath);
            else if (key == "AssetRegistry")
                input >> std::quoted(manifest.AssetRegistryPath);
            else
                return false;

            if (!input)
                return false;
        }

        if (manifest.ScenePath.empty() || manifest.AssetRegistryPath.empty())
            return false;

        outManifest = std::move(manifest);
        return true;
    }
}

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
    if (!std::filesystem::exists(m_ProjectPath) || !LoadProject())
        EnsureDefaultSceneEntities();
    SyncEditorCameraStateFromMainCamera();
    m_CaptureViewportRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--capture-viewport");
    m_SaveSceneSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--save-scene-smoke");
    m_AssetWatchSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--asset-watch-smoke");
    m_GltfImportSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--gltf-import-smoke");
    m_MaterialAssetSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--material-asset-smoke");
    m_ProjectSaveSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--project-save-smoke");
    if (m_AssetWatchSmokeRequested)
    {
        WriteTextFile(m_AssetWatchSmokePath, "asset watch smoke baseline\n");
        m_AssetRegistry.RegisterAsset(Engine::AssetType::Mesh, m_AssetWatchSmokePath, "Asset Watch Smoke");
    }
    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    m_ConsoleLines.emplace_back("File watching active: " + std::to_string(m_AssetWatcher.GetTrackedCount()) + " asset source(s)");
    if (m_CaptureViewportRequested)
        m_ConsoleLines.emplace_back(std::string("Viewport capture requested: ") + m_CaptureViewportPath);
    if (m_SaveSceneSmokeRequested)
    {
        if (!SaveActiveScene())
            throw std::runtime_error("Scene save smoke failed");
    }
    if (m_ProjectSaveSmokeRequested && (!SaveProject() || !LoadProject()))
        throw std::runtime_error("Project save/reopen smoke failed");

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

    RunAssetWatchSmokeMutation();
    RunGltfImportSmoke();
    RunMaterialAssetSmoke();
    HandleAssetWatchEvents();

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
    Engine::EventDispatcher dispatcher(event);
    dispatcher.Dispatch<Engine::FileDropEvent>(GE_BIND_EVENT_FN(EditorLayer::OnFileDrop));
    if (event.Handled)
        return;

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
            LoadProject();
        if (ImGui::MenuItem("Save Project"))
            SaveProject();
        if (ImGui::MenuItem("Save Scene"))
            SaveActiveScene();
        if (ImGui::MenuItem("Save Asset Registry"))
            SaveAssetRegistry();
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
        if (ImGui::MenuItem("Rescan Asset Sources"))
        {
            m_AssetWatcher.SyncRegistry(m_AssetRegistry);
            m_ConsoleLines.emplace_back("Asset source watcher resynced");
        }
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
        {
            ImGui::PushID(static_cast<int>(entity.EntityHandle.Id));
            const bool selected = entity.EntityHandle == m_SelectedEntity;
            if (ImGui::Selectable(entity.Name.c_str(), selected))
                m_SelectedEntity = entity.EntityHandle;
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    ImGui::End();
}

void EditorLayer::DrawInspectorPanel()
{
    ImGui::Begin("Inspector");

    if (!m_ActiveScene.IsEntityValid(m_SelectedEntity))
        m_SelectedEntity = m_PrototypeMeshEntity;

    Engine::SceneEntity* selectedEntity = m_ActiveScene.TryGetEntity(m_SelectedEntity);
    if (!selectedEntity)
    {
        ImGui::TextDisabled("No entity selected");
        ImGui::End();
        return;
    }

    ImGui::Text("Selected: %s", selectedEntity->Name.c_str());
    ImGui::Separator();
    ImGui::PushID("TransformComponent");
    ImGui::TextUnformatted("Transform");

    bool transformChanged = false;
    transformChanged |= DrawVec3Control("Position", selectedEntity->Transform.Position, 0.1f);
    transformChanged |= DrawVec3Control("Rotation", selectedEntity->Transform.RotationDegrees, 0.5f);
    transformChanged |= DrawVec3Control("Scale", selectedEntity->Transform.Scale, 0.05f, 0.01f, 100.0f);
    if (transformChanged && selectedEntity->EntityHandle == m_ActiveScene.GetMainCameraEntity())
    {
        m_ActiveScene.SetMainCameraTransform(selectedEntity->Transform);
        SyncEditorCameraStateFromMainCamera();
    }
    ImGui::PopID();

    if (selectedEntity->Camera)
    {
        ImGui::Separator();
        ImGui::PushID("CameraComponent");
        ImGui::TextUnformatted("Camera Component");
        Engine::CameraComponent& camera = *selectedEntity->Camera;
        bool cameraChanged = false;
        cameraChanged |= ImGui::Checkbox("Primary", &camera.Primary);
        cameraChanged |= ImGui::DragFloat("Vertical FOV", &camera.Projection.VerticalFovDegrees, 0.25f, 20.0f, 110.0f);
        cameraChanged |= ImGui::DragFloat("Near Clip", &camera.Projection.NearClip, 0.01f, 0.01f, 10.0f);
        cameraChanged |= ImGui::DragFloat("Far Clip", &camera.Projection.FarClip, 1.0f, 1.0f, 10000.0f);
        if (camera.Projection.FarClip <= camera.Projection.NearClip)
            camera.Projection.FarClip = camera.Projection.NearClip + 1.0f;
        if (camera.Primary)
            m_ActiveScene.SetMainCameraEntity(selectedEntity->EntityHandle);
        if (cameraChanged && selectedEntity->EntityHandle == m_ActiveScene.GetMainCameraEntity())
        {
            m_ActiveScene.SetMainCamera(camera);
            SyncEditorCameraStateFromMainCamera();
        }
        ImGui::PopID();
    }

    if (selectedEntity->Light)
    {
        ImGui::Separator();
        ImGui::PushID("LightComponent");
        ImGui::TextUnformatted("Light Component");
        Engine::LightComponent& light = *selectedEntity->Light;
        const char* lightTypeName = ToLightTypeName(light.Type);
        if (ImGui::BeginCombo("Type", lightTypeName))
        {
            const Engine::LightType lightTypes[] = {
                Engine::LightType::Directional,
                Engine::LightType::Point,
                Engine::LightType::Spot
            };
            for (Engine::LightType candidate : lightTypes)
            {
                const bool selected = light.Type == candidate;
                if (ImGui::Selectable(ToLightTypeName(candidate), selected))
                    light.Type = candidate;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::ColorEdit3("Color", &light.Color.X);
        ImGui::DragFloat("Intensity", &light.Intensity, 0.05f, 0.0f, 100000.0f);
        ImGui::DragFloat("Range", &light.Range, 0.1f, 0.0f, 10000.0f);
        ImGui::DragFloat("Inner Cone", &light.InnerConeDegrees, 0.5f, 0.0f, 180.0f);
        ImGui::DragFloat("Outer Cone", &light.OuterConeDegrees, 0.5f, 0.0f, 180.0f);
        if (light.OuterConeDegrees < light.InnerConeDegrees)
            light.OuterConeDegrees = light.InnerConeDegrees;
        ImGui::Checkbox("Casts Shadows", &light.CastsShadows);
        ImGui::PopID();
    }

    if (selectedEntity->MeshRenderer)
    {
        ImGui::Separator();
        ImGui::PushID("MeshRendererComponent");
        ImGui::TextUnformatted("Mesh Renderer Component");
        Engine::MeshRendererComponent& meshRenderer = *selectedEntity->MeshRenderer;
        if (const Engine::AssetMetadata* meshAsset = m_AssetRegistry.GetAsset(meshRenderer.MeshAsset))
            meshRenderer.MeshName = meshAsset->Name;

        char meshName[128] = {};
        const std::string meshDisplayName = GetMeshDisplayName(meshRenderer, m_AssetRegistry);
        std::snprintf(meshName, sizeof(meshName), "%s", meshDisplayName.c_str());
        if (ImGui::InputText("Mesh Name", meshName, sizeof(meshName)))
        {
            meshRenderer.MeshName = meshName;
            m_AssetRegistry.SetAssetName(meshRenderer.MeshAsset, meshName);
        }
        DrawAssetHandleControl("Mesh Asset", meshRenderer.MeshAsset, m_AssetRegistry, Engine::AssetType::Mesh);
        DrawAssetHandleControl("Material Asset", meshRenderer.MaterialAsset, m_AssetRegistry, Engine::AssetType::Material);
        DrawMaterialAssetControls(meshRenderer.MaterialAsset);
        ImGui::Checkbox("Visible", &meshRenderer.Visible);
        ImGui::Checkbox("Casts Shadows", &meshRenderer.CastsShadows);
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::PushID("EditorCamera");
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
        ApplyEditorCameraStateToScene();
    }
    ImGui::TextDisabled("Aspect %.3f", m_EditorCamera.GetAspectRatio());
    ImGui::PopID();
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

void EditorLayer::DrawMaterialAssetControls(Engine::AssetHandle handle)
{
    Engine::MaterialAsset* material = m_MaterialLibrary.Get(handle);
    if (!material)
    {
        ImGui::TextDisabled("No loaded material asset for this handle");
        return;
    }

    ImGui::Separator();
    ImGui::PushID("MaterialAsset");
    ImGui::TextUnformatted("Material Asset");
    char materialName[128] = {};
    std::snprintf(materialName, sizeof(materialName), "%s", material->Name.c_str());
    if (ImGui::InputText("Material Name", materialName, sizeof(materialName)))
    {
        material->Name = materialName;
        m_AssetRegistry.SetAssetName(handle, materialName);
    }

    if (ImGui::BeginCombo("Shading Model", Engine::ToString(material->ShadingModel)))
    {
        const Engine::MaterialShadingModel models[] = {
            Engine::MaterialShadingModel::Standard,
            Engine::MaterialShadingModel::Unlit
        };
        for (Engine::MaterialShadingModel candidate : models)
        {
            const bool selected = material->ShadingModel == candidate;
            if (ImGui::Selectable(Engine::ToString(candidate), selected))
                material->ShadingModel = candidate;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (ImGui::BeginCombo("Alpha Mode", Engine::ToString(material->AlphaMode)))
    {
        const Engine::MaterialAlphaMode modes[] = {
            Engine::MaterialAlphaMode::Opaque,
            Engine::MaterialAlphaMode::Mask,
            Engine::MaterialAlphaMode::Blend
        };
        for (Engine::MaterialAlphaMode candidate : modes)
        {
            const bool selected = material->AlphaMode == candidate;
            if (ImGui::Selectable(Engine::ToString(candidate), selected))
                material->AlphaMode = candidate;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::Checkbox("Two Sided", &material->TwoSided);
    ImGui::ColorEdit3("Base Color", &material->BaseColor.X);
    ImGui::DragFloat("Metallic", &material->Metallic, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Roughness", &material->Roughness, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Normal Scale", &material->NormalScale, 0.01f, 0.0f, 4.0f);
    ImGui::DragFloat("Occlusion Strength", &material->OcclusionStrength, 0.01f, 0.0f, 1.0f);
    ImGui::ColorEdit3("Emissive Color", &material->EmissiveColor.X);
    ImGui::DragFloat("Emissive Strength", &material->EmissiveStrength, 0.01f, 0.0f, 10000.0f);
    if (material->AlphaMode == Engine::MaterialAlphaMode::Mask)
        ImGui::DragFloat("Alpha Cutoff", &material->AlphaCutoff, 0.01f, 0.0f, 1.0f);

    ImGui::Separator();
    ImGui::TextUnformatted("Texture Handles");
    const Engine::MaterialTextureSlot textureSlots[] = {
        Engine::MaterialTextureSlot::BaseColor,
        Engine::MaterialTextureSlot::Normal,
        Engine::MaterialTextureSlot::Orm,
        Engine::MaterialTextureSlot::Emissive,
        Engine::MaterialTextureSlot::Opacity,
        Engine::MaterialTextureSlot::CallistoControl
    };
    for (Engine::MaterialTextureSlot slot : textureSlots)
        DrawAssetHandleControl(
            Engine::ToString(slot), material->GetTexture(slot), m_AssetRegistry, Engine::AssetType::Texture);

    ImGui::Separator();
    ImGui::TextUnformatted("Callisto Controls");
    ImGui::DragFloat("Diffuse Fresnel", &material->DiffuseFresnelIntensity, 0.01f, 0.0f, 256.0f);
    ImGui::DragFloat("Retroreflection", &material->RetroreflectionIntensity, 0.01f, 0.0f, 256.0f);
    ImGui::DragFloat("Diffuse Falloff", &material->DiffuseFresnelFalloff, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Retroreflection Falloff", &material->RetroreflectionFalloff, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Smooth Terminator", &material->SmoothTerminator, 0.01f, -1.0f, 1.0f);
    material->ClampValues();

    if (ImGui::Button("Save Material"))
        SaveMaterialAsset(handle);
    ImGui::PopID();
}

void EditorLayer::ApplyEditorCameraStateToScene()
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
}

void EditorLayer::SyncEditorCameraStateFromMainCamera()
{
    const Engine::TransformComponent& cameraTransform = m_ActiveScene.GetMainCameraTransform();
    const Engine::CameraComponent& camera = m_ActiveScene.GetMainCamera();

    m_CameraPosition = { cameraTransform.Position.X, cameraTransform.Position.Y, cameraTransform.Position.Z };
    m_CameraRotation = { cameraTransform.RotationDegrees.X, cameraTransform.RotationDegrees.Y, cameraTransform.RotationDegrees.Z };
    m_CameraFovDegrees = camera.Projection.VerticalFovDegrees;
    m_CameraNearClip = camera.Projection.NearClip;
    m_CameraFarClip = camera.Projection.FarClip;

    m_EditorCamera.SetPosition(cameraTransform.Position);
    m_EditorCamera.SetRotationDegrees(cameraTransform.RotationDegrees);
    m_EditorCamera.SetProjection(camera.Projection);
    Engine::Renderer::SetCameraView(m_EditorCamera.GetCameraView());
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
    ImGui::TextUnformatted("Assets");
    ImGui::Separator();
    ImGui::TextUnformatted("Import glTF");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##GltfSource",
        "Source path (.gltf or .glb)",
        m_GltfImportPath.data(),
        m_GltfImportPath.size());
    if (ImGui::Button("Import glTF"))
        ImportGltfAsset(m_GltfImportPath.data());
    if (!m_LastGltfImport.Error.empty())
        ImGui::TextDisabled("%s", m_LastGltfImport.Error.c_str());
    else if (m_LastGltfImport.Succeeded)
        ImGui::TextDisabled("Cooked %zu mesh(es) to %s", m_LastGltfImport.Meshes.size(), m_LastGltfImport.CookedPath.string().c_str());
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
        "##AssetFilter",
        "Filter assets",
        m_AssetBrowserFilter.data(),
        m_AssetBrowserFilter.size());

    constexpr Engine::AssetType assetTypes[] = {
        Engine::AssetType::Unknown,
        Engine::AssetType::Mesh,
        Engine::AssetType::Material,
        Engine::AssetType::Texture,
        Engine::AssetType::Scene,
        Engine::AssetType::Shader,
        Engine::AssetType::Script,
        Engine::AssetType::Audio
    };
    const char* selectedTypeLabel = m_AssetBrowserTypeFilter == Engine::AssetType::Unknown
        ? "All Types"
        : Engine::ToString(m_AssetBrowserTypeFilter);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##AssetType", selectedTypeLabel))
    {
        for (Engine::AssetType type : assetTypes)
        {
            const char* typeLabel = type == Engine::AssetType::Unknown ? "All Types" : Engine::ToString(type);
            const bool isSelected = m_AssetBrowserTypeFilter == type;
            if (ImGui::Selectable(typeLabel, isSelected))
                m_AssetBrowserTypeFilter = type;
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }

        ImGui::EndCombo();
    }

    if (m_AssetRegistry.GetAssets().empty())
    {
        ImGui::TextDisabled("No registered assets");
    }
    else if (ImGui::BeginTable(
                 "RegisteredAssets",
                 3,
                 ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableHeadersRow();

        for (const Engine::AssetMetadata& metadata : m_AssetRegistry.GetAssets())
        {
            if (!AssetMatchesFilter(metadata, m_AssetBrowserFilter.data(), m_AssetBrowserTypeFilter))
                continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(metadata.SourcePath.c_str());
            if (ImGui::Selectable(metadata.Name.c_str(), m_SelectedAssetHandle == metadata.Handle, ImGuiSelectableFlags_SpanAllColumns))
                m_SelectedAssetHandle = metadata.Handle;
            if (ImGui::BeginDragDropSource())
            {
                const AssetDragPayload payload { metadata.Handle, metadata.Type };
                ImGui::SetDragDropPayload(AssetDragPayloadType, &payload, sizeof(payload));
                ImGui::TextUnformatted(metadata.Name.c_str());
                ImGui::TextDisabled("%s", Engine::ToString(metadata.Type));
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Handle: %llu\nSource: %s\nDrag to assign",
                    static_cast<unsigned long long>(metadata.Handle),
                    metadata.SourcePath.c_str());
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(Engine::ToString(metadata.Type));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(metadata.SourcePath.c_str());
        }

        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextDisabled(
        "Watching %zu source file(s); %zu missing; %u reimport request(s)",
        m_AssetWatcher.GetTrackedCount(),
        m_AssetWatcher.GetMissingCount(),
        m_ReimportRequestCount);
    ImGui::End();
}

void EditorLayer::HandleAssetWatchEvents()
{
    const std::vector<Engine::AssetWatchEvent> events = m_AssetWatcher.Poll(m_AssetRegistry);
    for (const Engine::AssetWatchEvent& event : events)
    {
        const Engine::AssetMetadata* metadata = m_AssetRegistry.GetAsset(event.Handle);
        const std::string name = metadata ? metadata->Name : event.SourcePath;
        if (event.EventType == Engine::AssetWatchEventType::Deleted)
        {
            m_ConsoleLines.emplace_back("Asset source missing: " + name + " (" + event.SourcePath + ")");
            Engine::Log::Warn("Asset source missing: ", event.SourcePath);
            continue;
        }

        ++m_ReimportRequestCount;
        m_ConsoleLines.emplace_back(
            "Reimport queued: " + name + " (" + Engine::ToString(event.Type) + ", "
            + Engine::ToString(event.EventType) + ")");
        Engine::Log::Info("Asset reimport queued: ", event.SourcePath, " (", Engine::ToString(event.EventType), ")");
    }
}

void EditorLayer::RunAssetWatchSmokeMutation()
{
    if (!m_AssetWatchSmokeRequested || m_AssetWatchSmokeTouched || m_FrameCounter < 1)
        return;

    m_AssetWatchSmokeTouched = WriteTextFile(
        m_AssetWatchSmokePath,
        "asset watch smoke changed payload\nwith a different size\n");
    if (!m_AssetWatchSmokeTouched)
        m_ConsoleLines.emplace_back("Asset watch smoke mutation failed");
}

void EditorLayer::RunGltfImportSmoke()
{
    if (!m_GltfImportSmokeRequested || m_GltfImportSmokeCompleted || m_FrameCounter < 1)
        return;

    constexpr const char* source = R"({
  "asset": { "version": "2.0" },
  "buffers": [
    { "uri": "triangle.bin", "byteLength": 36 }
  ],
  "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 36 } ],
  "accessors": [ { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" } ],
  "meshes": [ { "name": "Smoke Triangle", "primitives": [ { "attributes": { "POSITION": 0 }, "mode": 4 } ] } ]
})";
    constexpr std::array<float, 9> trianglePositions = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };
    const std::filesystem::path bufferPath = std::filesystem::path(m_GltfImportSmokePath).replace_extension(".bin");

    if (!WriteTextFile(m_GltfImportSmokePath, source)
        || !WriteBinaryFile(bufferPath, trianglePositions.data(), sizeof(trianglePositions))
        || !ImportGltfAsset(m_GltfImportSmokePath))
        throw std::runtime_error("glTF import smoke failed: " + m_LastGltfImport.Error);

    m_GltfImportSmokeCompleted = m_LastGltfImport.Meshes.size() == 1
        && m_LastGltfImport.Meshes.front().VertexCount == 3
        && m_LastGltfImport.Meshes.front().TriangleCount == 1
        && std::filesystem::exists(m_LastGltfImport.CookedPath);
    if (!m_GltfImportSmokeCompleted)
        throw std::runtime_error("glTF import smoke produced an invalid cooked mesh manifest");

    Engine::Log::Info("glTF import smoke passed: ", m_LastGltfImport.CookedPath.string());
}

bool EditorLayer::OnFileDrop(Engine::FileDropEvent& event)
{
    for (const std::string& path : event.GetPaths())
    {
        std::snprintf(m_GltfImportPath.data(), m_GltfImportPath.size(), "%s", path.c_str());
        ImportGltfAsset(path);
    }

    return true;
}

void EditorLayer::RunMaterialAssetSmoke()
{
    if (!m_MaterialAssetSmokeRequested || m_MaterialAssetSmokeCompleted || m_FrameCounter < 1)
        return;

    const Engine::AssetHandle textureHandle = m_AssetRegistry.RegisterAsset(
        Engine::AssetType::Texture,
        "output/assets/material-smoke-base.ktx2",
        "Material Smoke Base Color");
    const Engine::AssetHandle materialHandle = m_AssetRegistry.RegisterAsset(
        Engine::AssetType::Material,
        m_MaterialAssetSmokePath,
        "Material Smoke");

    Engine::MaterialAsset material;
    material.Name = "Material Smoke";
    material.AlphaMode = Engine::MaterialAlphaMode::Mask;
    material.TwoSided = true;
    material.BaseColor = { 0.2f, 0.4f, 0.8f };
    material.Metallic = 0.35f;
    material.Roughness = 0.28f;
    material.AlphaCutoff = 0.42f;
    material.Textures.BaseColor = textureHandle;
    material.Textures.Opacity = textureHandle;
    if (!m_MaterialLibrary.Set(materialHandle, material) || !SaveMaterialAsset(materialHandle))
        throw std::runtime_error("Material asset smoke could not save the material asset");

    Engine::MaterialLibrary loadedLibrary;
    if (!loadedLibrary.Load(materialHandle, m_MaterialAssetSmokePath))
        throw std::runtime_error("Material asset smoke could not reload the material asset");

    const Engine::MaterialAsset* loaded = loadedLibrary.Get(materialHandle);
    m_MaterialAssetSmokeCompleted = loaded
        && loaded->Name == material.Name
        && loaded->AlphaMode == Engine::MaterialAlphaMode::Mask
        && loaded->TwoSided
        && std::abs(loaded->Roughness - material.Roughness) < 0.0001f
        && loaded->Textures.BaseColor == textureHandle
        && loaded->Textures.Opacity == textureHandle;
    if (!m_MaterialAssetSmokeCompleted)
        throw std::runtime_error("Material asset smoke reload validation failed");

    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    Engine::Log::Info("Material asset smoke passed: ", m_MaterialAssetSmokePath);
}

bool EditorLayer::ImportGltfAsset(const std::filesystem::path& sourcePath)
{
    m_LastGltfImport = Engine::GltfImporter::Import(sourcePath, m_AssetRegistry);
    if (!m_LastGltfImport.Succeeded)
    {
        m_ConsoleLines.emplace_back("glTF import failed: " + m_LastGltfImport.Error);
        Engine::Log::Error("glTF import failed: ", m_LastGltfImport.Error);
        return false;
    }

    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    m_ConsoleLines.emplace_back(
        "glTF imported: " + m_LastGltfImport.SourcePath + " ("
        + std::to_string(m_LastGltfImport.Meshes.size()) + " mesh(es))");
    Engine::Log::Info(
        "glTF imported: ",
        m_LastGltfImport.SourcePath,
        " -> ",
        m_LastGltfImport.CookedPath.string());
    return true;
}

bool EditorLayer::SaveProject()
{
    if (!SaveActiveScene())
        return false;

    const ProjectManifest manifest { m_ScenePath, m_AssetRegistryPath };
    if (!WriteProjectManifest(m_ProjectPath, manifest))
    {
        Engine::Log::Error("Project save failed: ", m_ProjectPath);
        m_ConsoleLines.emplace_back("Project save failed: " + m_ProjectPath);
        return false;
    }

    Engine::Log::Info("Project saved: ", m_ProjectPath);
    m_ConsoleLines.emplace_back("Project saved: " + m_ProjectPath);
    return true;
}

bool EditorLayer::LoadProject()
{
    ProjectManifest manifest;
    if (!ReadProjectManifest(m_ProjectPath, manifest))
    {
        Engine::Log::Error("Could not load project manifest: ", m_ProjectPath);
        m_ConsoleLines.emplace_back("Project load failed: " + m_ProjectPath);
        return false;
    }

    Engine::AssetRegistry loadedRegistry;
    if (!loadedRegistry.LoadFromFile(manifest.AssetRegistryPath))
    {
        Engine::Log::Error("Could not load project asset registry: ", manifest.AssetRegistryPath);
        m_ConsoleLines.emplace_back("Project load failed: asset registry");
        return false;
    }

    Engine::Scene loadedScene;
    if (!Engine::Scene::LoadFromFile(manifest.ScenePath, loadedScene))
    {
        Engine::Log::Error("Could not load project scene: ", manifest.ScenePath);
        m_ConsoleLines.emplace_back("Project load failed: scene");
        return false;
    }

    Engine::MaterialLibrary loadedMaterials;
    for (const Engine::AssetMetadata& metadata : loadedRegistry.GetAssets())
    {
        if (metadata.Type != Engine::AssetType::Material)
            continue;

        const std::filesystem::path materialPath = Engine::AssetFileSystem::ResolvePath(metadata.SourcePath);
        if (!loadedMaterials.Load(metadata.Handle, materialPath))
        {
            Engine::Log::Error("Could not load project material: ", metadata.SourcePath);
            m_ConsoleLines.emplace_back("Project load failed: material " + metadata.SourcePath);
            return false;
        }
    }

    m_ScenePath = std::move(manifest.ScenePath);
    m_AssetRegistryPath = std::move(manifest.AssetRegistryPath);
    m_AssetRegistry = std::move(loadedRegistry);
    m_MaterialLibrary = std::move(loadedMaterials);
    m_ActiveScene = std::move(loadedScene);
    m_PrototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
    m_DirectionalLightEntity = m_ActiveScene.FindEntityByName("Directional Light");
    m_PlayerStartEntity = m_ActiveScene.FindEntityByName("Player Start");
    m_SelectedEntity = m_PrototypeMeshEntity ? m_PrototypeMeshEntity : m_ActiveScene.GetMainCameraEntity();
    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    SyncEditorCameraStateFromMainCamera();

    Engine::Log::Info("Project loaded: ", m_ProjectPath);
    m_ConsoleLines.emplace_back("Project loaded: " + m_ProjectPath);
    return true;
}

bool EditorLayer::SaveActiveScene()
{
    if (!SaveMaterialAssets())
        return false;

    if (!SaveAssetRegistry())
        return false;

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

bool EditorLayer::SaveAssetRegistry()
{
    const bool saved = m_AssetRegistry.SaveToFile(m_AssetRegistryPath);
    if (saved)
    {
        Engine::AssetRegistry loadedRegistry;
        const bool loaded = loadedRegistry.LoadFromFile(m_AssetRegistryPath);
        if (loaded)
            Engine::Log::Info("Asset registry saved and reload-validated: ", m_AssetRegistryPath);
        else
            Engine::Log::Error("Asset registry saved but reload validation failed: ", m_AssetRegistryPath);
        m_ConsoleLines.emplace_back(loaded
            ? std::string("Asset registry saved: ") + m_AssetRegistryPath
            : std::string("Asset registry saved but reload validation failed: ") + m_AssetRegistryPath);
        return loaded;
    }

    Engine::Log::Error("Asset registry save failed: ", m_AssetRegistryPath);
    m_ConsoleLines.emplace_back(std::string("Asset registry save failed: ") + m_AssetRegistryPath);
    return false;
}

bool EditorLayer::SaveMaterialAsset(Engine::AssetHandle handle)
{
    const Engine::AssetMetadata* metadata = m_AssetRegistry.GetAsset(handle);
    if (!metadata || metadata->Type != Engine::AssetType::Material)
        return false;

    const std::filesystem::path path = Engine::AssetFileSystem::ResolvePath(metadata->SourcePath);
    const bool saved = m_MaterialLibrary.Save(handle, path);
    if (saved)
    {
        m_AssetWatcher.Acknowledge(handle);
        m_ConsoleLines.emplace_back("Material saved: " + metadata->SourcePath);
        Engine::Log::Info("Material saved: ", metadata->SourcePath);
    }
    else
    {
        m_ConsoleLines.emplace_back("Material save failed: " + metadata->SourcePath);
        Engine::Log::Error("Material save failed: ", metadata->SourcePath);
    }

    return saved;
}

bool EditorLayer::SaveMaterialAssets()
{
    for (const Engine::AssetMetadata& metadata : m_AssetRegistry.GetAssets())
    {
        if (metadata.Type == Engine::AssetType::Material && m_MaterialLibrary.Get(metadata.Handle))
        {
            const std::filesystem::path path = Engine::AssetFileSystem::ResolvePath(metadata.SourcePath);
            if (!m_MaterialLibrary.Save(metadata.Handle, path))
            {
                Engine::Log::Error("Material save failed: ", metadata.SourcePath);
                return false;
            }

            m_AssetWatcher.Acknowledge(metadata.Handle);
        }
    }

    return true;
}

void EditorLayer::EnsureDefaultSceneEntities()
{
    const Engine::AssetHandle prototypeMeshAsset = m_AssetRegistry.RegisterAsset(
        Engine::AssetType::Mesh,
        "Engine/Generated/PrototypeCube.mesh",
        "Prototype Cube");
    const Engine::AssetHandle prototypeMaterialAsset = m_AssetRegistry.RegisterAsset(
        Engine::AssetType::Material,
        "output/assets/PrototypeDefault.spiralmat",
        "Prototype Default");
    if (!m_MaterialLibrary.Get(prototypeMaterialAsset))
    {
        const std::filesystem::path materialPath = Engine::AssetFileSystem::ResolvePath("output/assets/PrototypeDefault.spiralmat");
        if (!m_MaterialLibrary.Load(prototypeMaterialAsset, materialPath))
        {
            Engine::MaterialAsset prototypeMaterial;
            prototypeMaterial.Name = "Prototype Default";
            prototypeMaterial.BaseColor = { 0.72f, 0.78f, 0.92f };
            prototypeMaterial.Roughness = 0.45f;
            m_MaterialLibrary.Set(prototypeMaterialAsset, prototypeMaterial);
            SaveMaterialAsset(prototypeMaterialAsset);
        }
    }

    m_PrototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
    if (!m_PrototypeMeshEntity)
        m_PrototypeMeshEntity = m_ActiveScene.CreateEntity("Prototype Mesh");
    if (Engine::MeshRendererComponent* meshRenderer = m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity))
    {
        meshRenderer->MeshAsset = prototypeMeshAsset;
        meshRenderer->MaterialAsset = prototypeMaterialAsset;
        if (meshRenderer->MeshName.empty())
            meshRenderer->MeshName = "Prototype Cube";
    }
    else
    {
        Engine::MeshRendererComponent defaultMeshRenderer;
        defaultMeshRenderer.MeshAsset = prototypeMeshAsset;
        defaultMeshRenderer.MaterialAsset = prototypeMaterialAsset;
        defaultMeshRenderer.MeshName = "Prototype Cube";
        m_ActiveScene.AddMeshRendererComponent(m_PrototypeMeshEntity, defaultMeshRenderer);
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

    if (!m_SelectedEntity)
        m_SelectedEntity = m_PrototypeMeshEntity;
}
