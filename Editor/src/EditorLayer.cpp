#include "EditorLayer.h"

#include "Engine/Events/KeyEvent.h"
#include "Engine/Events/MouseEvent.h"
#include "Engine/Assets/TextureArtifact.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace
{
    constexpr const char* AssetDragPayloadType = "SPIRAL_ASSET_HANDLE";
    constexpr int ProjectFormatVersion = 6;
    constexpr int EditorSettingsFormatVersion = 1;

    bool HasCommandLineOption(const Engine::ApplicationCommandLineArgs& args,
        std::string_view option)
    {
        const std::string prefix = std::string(option) + "=";
        for (int index = 0; index < args.Count; ++index)
        {
            const std::string_view argument = args[index];
            if (argument == option || argument.starts_with(prefix))
                return true;
        }
        return false;
    }

    struct EditorSettings
    {
        ViewportNavigationPreset ViewportNavigation = ViewportNavigationPreset::Fusion;
    };

    struct AssetDragPayload
    {
        Engine::AssetHandle Handle = Engine::kInvalidAssetHandle;
        Engine::AssetType Type = Engine::AssetType::Unknown;
    };

    struct ProjectManifest
    {
        std::string ScenePath;
        std::string AssetRegistryPath;
        Engine::FramePacingPolicy FramePacingPolicy;
        Engine::PresentationPolicy PresentationPolicy = Engine::PresentationPolicy::Synchronized;
        Engine::RendererColorPipelineSettings ColorPipelineSettings;
    };

    const char* ToEditorSettingsNavigationPreset(ViewportNavigationPreset preset)
    {
        switch (preset)
        {
            case ViewportNavigationPreset::Fusion: return "Fusion";
            case ViewportNavigationPreset::Unreal: return "Unreal";
        }
        return "Fusion";
    }

    bool ParseEditorSettingsNavigationPreset(std::string_view text, ViewportNavigationPreset& outPreset)
    {
        if (text == "Fusion")
        {
            outPreset = ViewportNavigationPreset::Fusion;
            return true;
        }
        if (text == "Unreal")
        {
            outPreset = ViewportNavigationPreset::Unreal;
            return true;
        }
        return false;
    }

    bool ParseFramePacingMode(std::string_view text, Engine::FramePacingMode& outMode)
    {
        if (text == "Responsive")
        {
            outMode = Engine::FramePacingMode::Responsive;
            return true;
        }
        if (text == "SmoothFrametime")
        {
            outMode = Engine::FramePacingMode::SmoothFrametime;
            return true;
        }
        return false;
    }

    const char* ToManifestFramePacingMode(Engine::FramePacingMode mode)
    {
        switch (mode)
        {
            case Engine::FramePacingMode::Responsive: return "Responsive";
            case Engine::FramePacingMode::SmoothFrametime: return "SmoothFrametime";
        }
        return "Unknown";
    }

    bool ParsePresentationPolicy(std::string_view text, Engine::PresentationPolicy& outPolicy)
    {
        if (text == "Synchronized") { outPolicy = Engine::PresentationPolicy::Synchronized; return true; }
        if (text == "TearingAllowed") { outPolicy = Engine::PresentationPolicy::TearingAllowed; return true; }
        return false;
    }

    bool ValidateCapabilityDiagnostics(const Engine::RHI::DeviceCapabilities& capabilities, std::string& error)
    {
        if (capabilities.ProfileName.empty())
            error = "selected capability profile name is empty";
        else if (capabilities.Identity.Name.empty())
            error = "selected adapter name is empty";
        else if (!capabilities.AdapterSelection.HasSelection())
            error = "adapter selection has no selected candidate";
        else if (!capabilities.GetSelectedAdapter())
            error = "selected adapter index is outside the candidate list";
        else if (capabilities.AdapterSelection.Evaluations.size() != capabilities.AdapterCandidates.size())
            error = "candidate evaluation count does not match the candidate list";
        else if (!capabilities.GetSelectedAdapterEvaluation() || !capabilities.GetSelectedAdapterEvaluation()->Accepted)
            error = "selected adapter evaluation is not accepted";
        else if (capabilities.GetSelectedAdapter()->Identity.Name != capabilities.Identity.Name
            || capabilities.GetSelectedAdapter()->Identity.StableId != capabilities.Identity.StableId)
            error = "selected adapter identity does not match the published device identity";
        else if (capabilities.Formats.empty())
            error = "selected capability report has no queried formats";
        else if (!capabilities.GetCapabilityGroup(Engine::RHI::CapabilityGroupId::Phase3FrameTimingV1))
            error = "Phase 3 frame-timing capability group is missing";
        else if (!capabilities.GetCapabilityGroup(Engine::RHI::CapabilityGroupId::Phase3FrameTimingV1)->IsValid())
            error = "Phase 3 frame-timing capability group has an invalid lifecycle";
        else if (!capabilities.GetCapabilityGroup(Engine::RHI::CapabilityGroupId::Phase3TransientResourcesV1))
            error = "Phase 3 transient-resource capability group is missing";
        else if (!capabilities.GetCapabilityGroup(Engine::RHI::CapabilityGroupId::Phase3TransientResourcesV1)->IsValid())
            error = "Phase 3 transient-resource capability group has an invalid lifecycle";
        else
            return true;

        return false;
    }

    bool DrawVec3Control(const char* label, Engine::Math::Vec3& value, float speed, float min = 0.0f, float max = 0.0f)
    {
        float values[3] = { value.X, value.Y, value.Z };
        if (!ImGui::DragFloat3(label, values, speed, min, max))
            return false;

        value = { values[0], values[1], values[2] };
        return true;
    }

    bool DrawDVec3Control(const char* label, Engine::Math::DVec3& value, float speed)
    {
        double values[3] = { value.X, value.Y, value.Z };
        if (!ImGui::DragScalarN(label, ImGuiDataType_Double, values, 3, speed))
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

    std::string SanitizeFileStem(std::string_view value)
    {
        std::string stem;
        stem.reserve(value.size());
        for (unsigned char character : value)
        {
            if (std::isalnum(character) || character == '-' || character == '_')
                stem.push_back(static_cast<char>(character));
            else if (std::isspace(character) && !stem.empty() && stem.back() != '-')
                stem.push_back('-');
        }

        while (!stem.empty() && stem.back() == '-')
            stem.pop_back();
        return stem;
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

    bool WriteEditorSettings(const std::filesystem::path& path, const EditorSettings& settings)
    {
        std::error_code error;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, error);
        if (error)
            return false;

        const std::filesystem::path temporaryPath = path.string() + ".tmp";
        const std::filesystem::path backupPath = path.string() + ".bak";
        std::filesystem::remove(temporaryPath, error);
        error.clear();
        std::filesystem::remove(backupPath, error);
        error.clear();

        {
            std::ofstream output(temporaryPath, std::ios::out | std::ios::trunc);
            if (!output)
                return false;
            output << "SpiralEditorSettings " << EditorSettingsFormatVersion << '\n';
            output << "ViewportNavigationPreset " << ToEditorSettingsNavigationPreset(settings.ViewportNavigation) << '\n';
            output.flush();
            if (!output)
            {
                output.close();
                std::filesystem::remove(temporaryPath, error);
                return false;
            }
        }

        const bool hadExistingSettings = std::filesystem::exists(path, error);
        if (error)
        {
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
        if (hadExistingSettings)
        {
            std::filesystem::rename(path, backupPath, error);
            if (error)
            {
                std::filesystem::remove(temporaryPath, error);
                return false;
            }
        }

        std::filesystem::rename(temporaryPath, path, error);
        if (!error)
        {
            if (hadExistingSettings)
                std::filesystem::remove(backupPath, error);
            return true;
        }

        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
        if (hadExistingSettings)
            std::filesystem::rename(backupPath, path, ignored);
        return false;
    }

    bool ReadEditorSettings(const std::filesystem::path& path, EditorSettings& outSettings)
    {
        std::ifstream input(path);
        if (!input)
            return false;

        std::string magic;
        int version = 0;
        std::string key;
        std::string preset;
        if (!(input >> magic >> version >> key >> preset)
            || magic != "SpiralEditorSettings" || version != EditorSettingsFormatVersion
            || key != "ViewportNavigationPreset" || !input)
        {
            return false;
        }

        std::string unexpected;
        if (input >> unexpected)
            return false;

        EditorSettings settings;
        if (!ParseEditorSettingsNavigationPreset(preset, settings.ViewportNavigation))
            return false;

        outSettings = settings;
        return true;
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
        output << std::setprecision(std::numeric_limits<double>::max_digits10);

        output << "SpiralProject " << ProjectFormatVersion << '\n';
        output << "Scene " << std::quoted(manifest.ScenePath) << '\n';
        output << "AssetRegistry " << std::quoted(manifest.AssetRegistryPath) << '\n';
        output << "FramePacingMode " << ToManifestFramePacingMode(manifest.FramePacingPolicy.Mode) << '\n';
        output << "FramePacingTargetFps " << manifest.FramePacingPolicy.SmoothTargetFramesPerSecond << '\n';
        output << "PresentationPolicy " << Engine::ToString(manifest.PresentationPolicy) << '\n';
        output << "ManualExposureEV100 " << manifest.ColorPipelineSettings.ManualExposureEV100 << '\n';
        output << "PostToneMapSaturation " << manifest.ColorPipelineSettings.PostToneMapSaturation << '\n';
        output << "PostToneMapContrast " << manifest.ColorPipelineSettings.PostToneMapContrast << '\n';
        output << "ExposureMode " << Engine::ToString(manifest.ColorPipelineSettings.ExposureMode) << '\n';
        output << "CameraApertureFNumber " << manifest.ColorPipelineSettings.CameraApertureFNumber << '\n';
        output << "CameraShutterSeconds " << manifest.ColorPipelineSettings.CameraShutterSeconds << '\n';
        output << "CameraISO " << manifest.ColorPipelineSettings.CameraISO << '\n';
        return static_cast<bool>(output);
    }

    bool ReadProjectManifest(const std::filesystem::path& path, ProjectManifest& outManifest)
    {
        std::ifstream input(path);
        if (!input)
            return false;

        std::string magic;
        int version = 0;
        if (!(input >> magic >> version) || magic != "SpiralProject" || version < 1 || version > ProjectFormatVersion)
            return false;

        ProjectManifest manifest;
        bool readFramePacingMode = version == 1;
        bool readFramePacingTarget = version == 1;
        bool readPresentationPolicy = version < 3;
        bool readManualExposure = version < 4;
        bool readPostToneMapSaturation = version < 5;
        bool readPostToneMapContrast = version < 5;
        bool readExposureMode = version < 6;
        bool readCameraAperture = version < 6;
        bool readCameraShutter = version < 6;
        bool readCameraISO = version < 6;
        std::string key;
        while (input >> key)
        {
            if (key == "Scene")
                input >> std::quoted(manifest.ScenePath);
            else if (key == "AssetRegistry")
                input >> std::quoted(manifest.AssetRegistryPath);
            else if (version >= 2 && key == "FramePacingMode")
            {
                std::string mode;
                input >> mode;
                if (!ParseFramePacingMode(mode, manifest.FramePacingPolicy.Mode))
                    return false;
                readFramePacingMode = true;
            }
            else if (version >= 2 && key == "FramePacingTargetFps")
            {
                input >> manifest.FramePacingPolicy.SmoothTargetFramesPerSecond;
                readFramePacingTarget = true;
            }
            else if (version >= 3 && key == "PresentationPolicy")
            {
                std::string policy;
                if (!(input >> policy) || !ParsePresentationPolicy(policy, manifest.PresentationPolicy))
                    return false;
                readPresentationPolicy = true;
            }
            else if (version >= 4 && key == "ManualExposureEV100")
            {
                if (!(input >> manifest.ColorPipelineSettings.ManualExposureEV100))
                    return false;
                readManualExposure = true;
            }
            else if (version >= 5 && key == "PostToneMapSaturation")
            {
                if (!(input >> manifest.ColorPipelineSettings.PostToneMapSaturation))
                    return false;
                readPostToneMapSaturation = true;
            }
            else if (version >= 5 && key == "PostToneMapContrast")
            {
                if (!(input >> manifest.ColorPipelineSettings.PostToneMapContrast))
                    return false;
                readPostToneMapContrast = true;
            }
            else if (version >= 6 && key == "ExposureMode")
            {
                std::string mode;
                if (!(input >> mode)
                    || !Engine::ParseRendererExposureMode(mode, manifest.ColorPipelineSettings.ExposureMode))
                    return false;
                readExposureMode = true;
            }
            else if (version >= 6 && key == "CameraApertureFNumber")
            {
                if (!(input >> manifest.ColorPipelineSettings.CameraApertureFNumber))
                    return false;
                readCameraAperture = true;
            }
            else if (version >= 6 && key == "CameraShutterSeconds")
            {
                if (!(input >> manifest.ColorPipelineSettings.CameraShutterSeconds))
                    return false;
                readCameraShutter = true;
            }
            else if (version >= 6 && key == "CameraISO")
            {
                if (!(input >> manifest.ColorPipelineSettings.CameraISO))
                    return false;
                readCameraISO = true;
            }
            else
                return false;

            if (!input)
                return false;
        }

        if (!readFramePacingMode || !readFramePacingTarget || !readPresentationPolicy || !readManualExposure
            || !readPostToneMapSaturation || !readPostToneMapContrast
            || !readExposureMode || !readCameraAperture || !readCameraShutter || !readCameraISO
            || manifest.ScenePath.empty() || manifest.AssetRegistryPath.empty()
            || !Engine::IsValidFramePacingPolicy(manifest.FramePacingPolicy)
            || !Engine::IsValidRendererColorPipelineSettings(manifest.ColorPipelineSettings))
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
    m_ConsoleLines.emplace_back("Editor booted");
    m_ConsoleLines.emplace_back("GLFW window backend active");
    m_ConsoleLines.emplace_back(std::string("Renderer backend: ") + Engine::Renderer::GetActiveBackendName());
    const Engine::ApplicationCommandLineArgs& args = Engine::Application::Get().GetSpecification().CommandLineArgs;
    m_EventTraceEnabled = args.HasFlag("--editor-event-trace");
    const std::string_view presentationOverride = args.GetOptionValue("--presentation-policy");
    if (presentationOverride == "synchronized")
        m_RuntimePresentationPolicyOverride = Engine::PresentationPolicy::Synchronized;
    else if (presentationOverride == "tearing-allowed")
        m_RuntimePresentationPolicyOverride = Engine::PresentationPolicy::TearingAllowed;
    else if (!presentationOverride.empty())
        throw std::runtime_error("Invalid --presentation-policy; expected synchronized or tearing-allowed");
    LoadEditorSettings();
    m_RendererCapabilitySmokeRequested = args.HasFlag("--renderer-capability-smoke");
    const Engine::RHI::DeviceCapabilities* deviceCapabilities = Engine::Renderer::GetDeviceCapabilities();
    if (deviceCapabilities)
    {
        std::string diagnosticsError;
        if (!ValidateCapabilityDiagnostics(*deviceCapabilities, diagnosticsError))
        {
            Engine::Log::Error("Editor renderer capability diagnostics invalid: ", diagnosticsError);
            if (m_RendererCapabilitySmokeRequested)
                throw std::runtime_error("Renderer capability diagnostics smoke failed: " + diagnosticsError);
        }
        else
        {
            Engine::Log::Info(
                "Editor renderer capability diagnostics ready: profile=", deviceCapabilities->ProfileName,
                ", qualification=", Engine::RHI::ToString(deviceCapabilities->Qualification),
                ", adapter=", deviceCapabilities->Identity.Name,
                ", candidates=", deviceCapabilities->AdapterCandidates.size(),
                ", fallbacks=", deviceCapabilities->Fallbacks.size());
        }
    }
    else if (m_RendererCapabilitySmokeRequested)
    {
        throw std::runtime_error("Renderer capability diagnostics smoke requires a native renderer device");
    }
    if (!std::filesystem::exists(m_ProjectPath) || !LoadProject())
        EnsureDefaultSceneEntities();
    PublishFramePacingPolicy();
    PublishPresentationPolicy();
    PublishColorPipelineSettings();
    SyncEditorCameraStateFromMainCamera(true);
    ResetFusionNavigationPivotFromSelectionOrScene();
    m_FramePacingNavigationTraceEnabled = args.HasFlag("--frame-pacing-benchmark");
    m_FramePacingInitialCameraPosition = m_CameraPosition;
    m_FramePacingInitialCameraRotation = m_CameraRotation;
    m_CaptureViewportRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--capture-viewport");
    m_SaveSceneSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--save-scene-smoke");
    m_AssetWatchSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--asset-watch-smoke");
    m_GltfImportSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--gltf-import-smoke");
    m_MaterialAssetSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--material-asset-smoke");
    m_ProjectSaveSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--project-save-smoke");
    m_UndoRedoSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--undo-redo-smoke");
    m_SceneAuthoringSmokeRequested = Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-authoring-smoke");
    m_SceneRenderSnapshotSmokeRequested =
        Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-render-snapshot-smoke");
    m_SceneOriginRasterSmokeRequested =
        Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--scene-origin-raster-smoke");
    m_FramePacingPolicySmokeRequested =
        Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--frame-pacing-policy-smoke");
    m_ColorPipelineSettingsSmokeRequested =
        Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--color-pipeline-settings-smoke");
    m_EditorSettingsSmokeRequested =
        Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--editor-settings-smoke");
    m_ViewportNavigationSmokeRequested =
        Engine::Application::Get().GetSpecification().CommandLineArgs.HasFlag("--editor-viewport-navigation-smoke");
    m_PresentationPolicySmokeRequested = args.HasFlag("--presentation-policy-smoke");
    m_EditorMaterialControlSmokeRequested = args.HasFlag("--editor-control-mailbox-smoke");
    m_EditorMaterialControlLiveHelperSmokeRequested =
        args.HasFlag("--editor-control-live-helper-smoke");
    m_EditorMaterialControlCapacitySmokeRequested =
        args.HasFlag("--editor-control-capacity-smoke");
    m_EditorMaterialControlDurabilitySmokeRequested =
        args.HasFlag("--editor-control-durability-smoke");
    m_EditorMaterialControlRollbackFailureSmokeRequested =
        args.HasFlag("--editor-control-rollback-failure-smoke");
    m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested =
        args.HasFlag("--editor-control-postcommit-rollback-failure-smoke");
    const unsigned int controlSmokeCount =
        (m_EditorMaterialControlSmokeRequested ? 1u : 0u)
        + (m_EditorMaterialControlLiveHelperSmokeRequested ? 1u : 0u)
        + (m_EditorMaterialControlCapacitySmokeRequested ? 1u : 0u)
        + (m_EditorMaterialControlDurabilitySmokeRequested ? 1u : 0u)
        + (m_EditorMaterialControlRollbackFailureSmokeRequested ? 1u : 0u)
        + (m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested ? 1u : 0u);
    if (controlSmokeCount > 1)
        throw std::runtime_error("editor material-control smoke modes are mutually exclusive");
    if (m_SceneOriginRasterSmokeRequested)
        ConfigureSceneOriginRasterSmoke();
    if (m_AssetWatchSmokeRequested)
    {
        WriteTextFile(m_AssetWatchSmokePath, "asset watch smoke baseline\n");
        m_AssetRegistry.RegisterAsset(Engine::AssetType::Mesh, m_AssetWatchSmokePath, "Asset Watch Smoke");
    }
    InitializeEditorMaterialControl();
    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);
    if (m_EditorMaterialControlSmokeRequested
        || m_EditorMaterialControlLiveHelperSmokeRequested
        || m_EditorMaterialControlCapacitySmokeRequested
        || m_EditorMaterialControlDurabilitySmokeRequested
        || m_EditorMaterialControlRollbackFailureSmokeRequested
        || m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested)
    {
        m_EditorMaterialControlSmokeInitialRendererGeneration =
            Engine::Renderer::GetPublishedArtifactResolverGeneration();
        m_EditorMaterialControlSmokeInitialUndoDepth = m_UndoHistory.size();
        m_EditorMaterialControlSmokeInitialRedoDepth = m_RedoHistory.size();
    }
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
    else if (Engine::Renderer::GetActiveBackend() == Engine::RendererBackend::NVRHIVulkan)
        m_ConsoleLines.emplace_back("Native Vulkan editor presentation and Scene viewport rendering active");
    else if (!buildInfo.HasNVRHID3D12)
        m_ConsoleLines.emplace_back("Native D3D12 viewport unavailable in this executable; run the VS2022 build path on Windows");
    else
        m_ConsoleLines.emplace_back("Native D3D12 backend compiled, but it did not become the active renderer");
}

void EditorLayer::OnDetach()
{
    m_EditorMaterialControl.Close();
    if (m_FramePacingNavigationTraceEnabled)
    {
        const double deltaX = m_CameraPosition[0] - m_FramePacingInitialCameraPosition[0];
        const double deltaY = m_CameraPosition[1] - m_FramePacingInitialCameraPosition[1];
        const double deltaZ = m_CameraPosition[2] - m_FramePacingInitialCameraPosition[2];
        const double pitchDelta = static_cast<double>(m_CameraRotation[0] - m_FramePacingInitialCameraRotation[0]);
        const double yawDelta = static_cast<double>(m_CameraRotation[1] - m_FramePacingInitialCameraRotation[1]);
        const double rollDelta = static_cast<double>(m_CameraRotation[2] - m_FramePacingInitialCameraRotation[2]);
        const double translationDistance = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
        const double rotationDelta = std::sqrt(pitchDelta * pitchDelta + yawDelta * yawDelta + rollDelta * rollDelta);
        Engine::Log::Info("ViewportNavigationCaptureV1 preset=", ToEditorSettingsNavigationPreset(m_ViewportNavigationPreset),
            " mutationFrames=", m_ViewportNavigationMutationFrames,
            " initialPosition=", m_FramePacingInitialCameraPosition[0], ",", m_FramePacingInitialCameraPosition[1], ",", m_FramePacingInitialCameraPosition[2],
            " finalPosition=", m_CameraPosition[0], ",", m_CameraPosition[1], ",", m_CameraPosition[2],
            " translationDistance=", translationDistance, " rotationDeltaDegrees=", rotationDelta,
            " result=", m_ViewportNavigationMutationFrames > 0 && (translationDistance > 0.000001 || rotationDelta > 0.000001) ? "changed" : "unchanged");
    }
    EndViewportCursorCapture();
    ClearViewportNavigationInput();
    Engine::Log::Info("Editor layer detached");
}

void EditorLayer::OnUpdate(Engine::Timestep timestep)
{
    ++m_FrameCounter;
    m_LastFrameMs = timestep.GetMilliseconds();
    const Engine::RendererFrameTiming& completedTiming = Engine::Renderer::GetLastCompletedFrameTiming();
    const Engine::RendererPresentationPolicyDiagnostics presentation = Engine::Renderer::GetPresentationPolicyDiagnostics();
    const FrameTimeHistoryCondition condition {
        completedTiming.FramePacingPolicy.EffectiveMode,
        completedTiming.FramePacingPolicy.SmoothTargetFramesPerSecond,
        completedTiming.FramePacingPolicy.Candidate,
        presentation.Requested,
        presentation.Actual,
        presentation.SwapchainGeneration
    };
    if (m_FrameTimeHistoryCondition && *m_FrameTimeHistoryCondition != condition)
    {
        m_FrameTimeHistoryCount = 0;
        m_FrameTimeHistoryOffset = 0;
        m_LastFrameTimeSampledFrame.reset();
    }
    m_FrameTimeHistoryCondition = condition;
    if (completedTiming.StartToStartMilliseconds > 0.0
        && (!m_LastFrameTimeSampledFrame || *m_LastFrameTimeSampledFrame != completedTiming.FrameIndex))
    {
        m_FrameTimeHistory[m_FrameTimeHistoryOffset] = static_cast<float>(completedTiming.StartToStartMilliseconds);
        m_FrameTimeHistoryOffset = (m_FrameTimeHistoryOffset + 1) % m_FrameTimeHistory.size();
        m_FrameTimeHistoryCount = std::min(m_FrameTimeHistoryCount + 1, m_FrameTimeHistory.size());
        m_LastFrameTimeSampledFrame = completedTiming.FrameIndex;
    }
    PublishFramePacingPolicy();
    PublishPresentationPolicy();
    PublishColorPipelineSettings();
    RunPresentationPolicySmoke();

    RunAssetWatchSmokeMutation();
    RunGltfImportSmoke();
    RunMaterialAssetSmoke();
    RunUndoRedoSmoke();
    RunSceneAuthoringSmoke();
    RunFramePacingPolicySmoke();
    RunColorPipelineSettingsSmoke();
    RunEditorSettingsSmoke();
    RunViewportNavigationSmoke();
    AdvanceSceneOriginRasterSmoke();
    HandleAssetWatchEvents();
    UpdateViewportNavigation(timestep);

    m_EditorMaterialControl.EnsureProjectIdentity(m_ProjectPath);
    RunEditorMaterialControlSmokeBeforeDrain();
    RunEditorMaterialControlCapacitySmokeBeforeDrain();
    RunEditorMaterialControlDurabilitySmokeBeforeDrain();
    RunEditorMaterialControlRollbackFailureSmokeBeforeDrain();
    const Engine::u64 applicationFrame = Engine::Application::Get().GetFrameIndex();
    m_EditorMaterialControl.Drain(applicationFrame,
        [this](const EditorMaterialControlRequest& request, Engine::u64 frame)
        {
            return ExecuteEditorMaterialControlRequest(request, frame);
        });
    RunEditorMaterialControlSmokeAfterDrain();
    RunEditorMaterialControlLiveHelperSmokeAfterDrain();
    RunEditorMaterialControlCapacitySmokeAfterDrain();
    RunEditorMaterialControlDurabilitySmokeAfterDrain();
    RunEditorMaterialControlRollbackFailureSmokeAfterDrain();

    Engine::TrackedCameraViewRequest viewportViewRequest;
    viewportViewRequest.StableViewId = 1;
    viewportViewRequest.WorldPosition = m_EditorCamera.GetPosition();
    viewportViewRequest.CanonicalWorldPosition = m_ActiveScene.GetMainCameraTransform().GetPosition();
    viewportViewRequest.HasCanonicalWorldPosition = true;
    viewportViewRequest.RotationDegrees = m_EditorCamera.GetRotationDegrees();
    viewportViewRequest.Projection = m_EditorCamera.GetProjection();
    viewportViewRequest.AspectRatio = m_EditorCamera.GetAspectRatio();
    viewportViewRequest.DiscontinuousRelocation = m_ViewportDiscontinuousRelocationPending;
    const Engine::CameraView viewportView = m_ViewportOriginTracker.BuildView(
        viewportViewRequest, m_ActiveScene.GetWorldGridPolicy());
    Engine::Renderer::PublishSceneRenderSnapshot(
        m_ActiveScene.ExtractRenderSnapshot(Engine::Application::Get().GetFrameIndex(), viewportView));
    if (viewportView.Valid)
        m_ViewportDiscontinuousRelocationPending = false;
    RunSceneRenderSnapshotSmoke();

    if (m_FrameCounter == 1)
    {
        Engine::JobSystem::Get().Submit([]
        {
            Engine::Log::Trace("Editor background validation job completed");
        }, "EditorValidation");
    }
}

void EditorLayer::InitializeEditorMaterialControl()
{
    const Engine::ApplicationCommandLineArgs& args =
        Engine::Application::Get().GetSpecification().CommandLineArgs;
    const bool optionProvided = HasCommandLineOption(args, "--editor-control-dir");
    const std::string_view controlDirectory = args.GetOptionValue("--editor-control-dir");
    if (!optionProvided)
    {
        if (m_EditorMaterialControlSmokeRequested
            || m_EditorMaterialControlLiveHelperSmokeRequested
            || m_EditorMaterialControlCapacitySmokeRequested
            || m_EditorMaterialControlDurabilitySmokeRequested
            || m_EditorMaterialControlRollbackFailureSmokeRequested
            || m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested)
            throw std::runtime_error(
                "editor material-control smokes require --editor-control-dir=<absolute path>");
        return;
    }
    if (controlDirectory.empty())
        throw std::runtime_error("--editor-control-dir requires a non-empty absolute path");

    if (m_EditorMaterialControlSmokeRequested
        || m_EditorMaterialControlLiveHelperSmokeRequested
        || m_EditorMaterialControlCapacitySmokeRequested
        || m_EditorMaterialControlDurabilitySmokeRequested
        || m_EditorMaterialControlRollbackFailureSmokeRequested
        || m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested)
    {
        if (!Engine::Application::Get().GetSpecification().Window.Headless)
            throw std::runtime_error("editor material-control smoke requires --headless");
        if (!m_ActiveScene.IsEntityValid(m_PrototypeMeshEntity))
            throw std::runtime_error("editor material-control smoke requires Prototype Mesh");
        Engine::MeshRendererComponent* prototypeRenderer =
            m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity);
        if (!prototypeRenderer)
            throw std::runtime_error("editor material-control smoke requires a mesh renderer");

        m_EditorMaterialControlSmokeMaterial = m_AssetRegistry.RegisterAsset(
            Engine::AssetType::Material,
            "output/assets/editor-control-mailbox-smoke.spiralmat",
            "Editor Control Mailbox Smoke");
        Engine::MaterialAsset smokeMaterial;
        smokeMaterial.Name = "Editor Control Mailbox Smoke";
        m_EditorMaterialControlSmokeBefore = {
            { 0.62f, 0.22f, 0.14f }, 0.35f, 0.74f
        };
        m_EditorMaterialControlSmokeAfter = {
            { 0.21f, 0.43f, 0.67f }, 0.77f, 0.31f
        };
        if (!Engine::TrySetMaterialSurface(smokeMaterial, m_EditorMaterialControlSmokeBefore)
            || !Engine::IsValidMaterialAssetValues(smokeMaterial)
            || !m_MaterialLibrary.Set(m_EditorMaterialControlSmokeMaterial, smokeMaterial))
            throw std::runtime_error("could not stage editor material-control smoke material");
        prototypeRenderer->MaterialAsset = m_EditorMaterialControlSmokeMaterial;
        const Engine::MeshRendererComponent sharedRenderer = *prototypeRenderer;

        m_EditorMaterialControlSharedPeer =
            m_ActiveScene.FindEntityByName("Editor Control Shared Peer");
        if (!m_EditorMaterialControlSharedPeer)
            m_EditorMaterialControlSharedPeer =
                m_ActiveScene.CreateEntity("Editor Control Shared Peer");
        Engine::MeshRendererComponent* peerRenderer =
            m_ActiveScene.TryGetMeshRendererComponent(m_EditorMaterialControlSharedPeer);
        if (!peerRenderer)
            peerRenderer = m_ActiveScene.AddMeshRendererComponent(
                m_EditorMaterialControlSharedPeer, sharedRenderer);
        if (!peerRenderer)
            throw std::runtime_error("could not stage editor material-control shared peer");
        *peerRenderer = sharedRenderer;
        peerRenderer->MaterialAsset = m_EditorMaterialControlSmokeMaterial;
        if (m_EditorMaterialControlCapacitySmokeRequested)
        {
            for (unsigned int index = 0; index < 40; ++index)
            {
                const Engine::Entity extra = m_ActiveScene.CreateEntity(
                    "Editor Control Capacity Peer " + std::to_string(index));
                if (!m_ActiveScene.AddMeshRendererComponent(extra, sharedRenderer))
                    throw std::runtime_error(
                        "could not stage editor material-control capacity peer");
            }
        }
        m_SelectedEntity = m_ActiveScene.GetMainCameraEntity();
        ResetFusionNavigationPivotFromSelectionOrScene();
    }

    std::string error;
    if (!m_EditorMaterialControl.Initialize(
        std::filesystem::path(std::string(controlDirectory)), m_ProjectPath, error))
        throw std::runtime_error("could not initialize editor material control: " + error);
    if (m_EditorMaterialControlLiveHelperSmokeRequested
        && !m_EditorMaterialControl.PublishLiveTargetForSmoke(
            m_PrototypeMeshEntity.Id, "Prototype Mesh",
            m_EditorMaterialControlSmokeMaterial,
            m_EditorMaterialControlSmokeBefore,
            m_EditorMaterialControlSmokeAfter, error))
        throw std::runtime_error(
            "could not publish live helper smoke target: " + error);
}

EditorMaterialControlTransaction EditorLayer::ExecuteEditorMaterialControlRequest(
    const EditorMaterialControlRequest& request, Engine::u64 frame)
{
    EditorMaterialControlTransaction transaction;
    EditorMaterialControlReceipt& receipt = transaction.Receipt;
    receipt.Action = request.Action;
    receipt.ActionKnown = true;
    receipt.Frame = frame;
    receipt.RendererGeneration = Engine::Renderer::GetPublishedArtifactResolverGeneration();
    receipt.UndoDepthBefore = m_UndoHistory.size();
    receipt.UndoDepthAfter = m_UndoHistory.size();
    receipt.RedoDepthBefore = m_RedoHistory.size();
    receipt.RedoDepthAfter = m_RedoHistory.size();

    const auto reject = [&transaction, &receipt](std::string reason)
    {
        receipt.Succeeded = false;
        receipt.Reason = std::move(reason);
        return transaction;
    };

    const Engine::Entity entityHandle { request.EntityId };
    const Engine::SceneEntity* entity = m_ActiveScene.TryGetEntity(entityHandle);
    if (!entity || entity->Name != request.ExpectedEntityName)
        return reject("stale_or_mismatched_entity_identity");
    receipt.EntityId = entity->EntityHandle.Id;
    receipt.EntityName = entity->Name;
    receipt.MaterialHandle = request.MaterialHandle;
    if (!entity->MeshRenderer
        || entity->MeshRenderer->MaterialAsset != request.MaterialHandle)
        return reject("stale_or_mismatched_material_identity");
    const Engine::AssetMetadata* metadata = m_AssetRegistry.GetAsset(request.MaterialHandle);
    if (!metadata || metadata->Type != Engine::AssetType::Material)
        return reject("material_handle_is_not_registered_material");
    const Engine::MaterialAsset* currentMaterial = m_MaterialLibrary.Get(request.MaterialHandle);
    if (!currentMaterial || !Engine::IsValidMaterialAssetValues(*currentMaterial))
        return reject("material_state_is_not_publishable");

    receipt.Before = Engine::GetMaterialSurface(*currentMaterial);
    receipt.After = receipt.Before;
    receipt.AffectedEntityIds.push_back(request.EntityId);
    for (const Engine::SceneEntity& candidate : m_ActiveScene.GetEntities())
    {
        if (candidate.MeshRenderer
            && candidate.MeshRenderer->MaterialAsset == request.MaterialHandle)
        {
            ++receipt.AffectedEntityCount;
            if (candidate.EntityHandle.Id != request.EntityId
                && receipt.AffectedEntityIds.size()
                < EditorMaterialControlMailbox::MaximumAffectedEntityIds)
                receipt.AffectedEntityIds.push_back(candidate.EntityHandle.Id);
        }
    }
    std::sort(receipt.AffectedEntityIds.begin(), receipt.AffectedEntityIds.end());
    receipt.AffectedEntityIdsTruncated =
        receipt.AffectedEntityCount > receipt.AffectedEntityIds.size();

    if (request.Action == EditorMaterialControlAction::InspectMaterialSurface)
    {
        Engine::MaterialAsset publishedMaterial;
        Engine::u64 publishedGeneration = 0;
        std::string readbackError;
        const bool readback = Engine::Renderer::ResolvePublishedMaterialAsset(
            request.MaterialHandle, publishedMaterial, publishedGeneration, readbackError);
        receipt.RendererGeneration = publishedGeneration;
        receipt.RendererReadbackVerified = readback
            && publishedGeneration == Engine::Renderer::GetPublishedArtifactResolverGeneration()
            && Engine::GetMaterialSurface(publishedMaterial) == receipt.Before;
        if (!receipt.RendererReadbackVerified)
            return reject("renderer_material_readback_mismatch");
        receipt.Succeeded = true;
        receipt.Reason = "ok";
        receipt.Effect = "ReadOnly";
        return transaction;
    }

    if (!request.HasExpectedSurface || !request.HasNewSurface
        || !request.SharedMaterialScope)
        return reject("patch_contract_is_incomplete");
    if (receipt.Before != request.ExpectedSurface)
        return reject("compare_and_swap_state_mismatch");

    Engine::Math::DVec3 targetPivot;
    if (!m_ActiveScene.TryGetEntityApproximateWorldPosition(entityHandle, targetPivot))
        return reject("entity_has_no_finite_world_position");
    Engine::MaterialAsset stagedMaterial = *currentMaterial;
    if (!Engine::TrySetMaterialSurface(stagedMaterial, request.NewSurface)
        || !Engine::IsValidMaterialAssetValues(stagedMaterial)
        || Engine::GetMaterialSurface(stagedMaterial) != request.NewSurface)
        return reject("new_material_state_is_not_publishable");

    const Engine::u64 generationBeforePublication = receipt.RendererGeneration;
    receipt.Succeeded = true;
    receipt.Reason = "ok";
    receipt.Effect = "SharedMaterialSurfacePatched";
    receipt.Recovery = "UndoRedo";
    receipt.After = request.NewSurface;
    receipt.RendererGeneration = generationBeforePublication + 1;
    receipt.UndoDepthAfter = std::min<std::size_t>(
        receipt.UndoDepthBefore + 1, 128);
    receipt.RedoDepthAfter = 0;
    receipt.SelectionCommitted = true;
    receipt.PivotRetargeted = true;
    receipt.RendererReadbackVerified = true;

    struct RollbackState
    {
        HistoryState State;
        std::vector<HistoryEntry> UndoHistory;
        std::vector<HistoryEntry> RedoHistory;
        bool FusionPivotValid = false;
        Engine::Math::DVec3 FusionPivot;
        bool MutationStarted = false;
    };
    auto rollback = std::make_shared<RollbackState>();
    rollback->State = CaptureHistoryState();
    rollback->UndoHistory = m_UndoHistory;
    rollback->RedoHistory = m_RedoHistory;
    rollback->FusionPivotValid = m_FusionNavigationPivotValid;
    rollback->FusionPivot = m_FusionNavigationPivot;

    transaction.Mutating = true;
    transaction.Commit = [this, request, entityHandle, targetPivot,
                              generationBeforePublication, stagedMaterial,
                              rollback](std::string& error)
    {
        const Engine::SceneEntity* currentEntity = m_ActiveScene.TryGetEntity(entityHandle);
        Engine::MaterialAsset* mutableMaterial = m_MaterialLibrary.Get(request.MaterialHandle);
        if (!currentEntity || currentEntity->Name != request.ExpectedEntityName
            || !currentEntity->MeshRenderer
            || currentEntity->MeshRenderer->MaterialAsset != request.MaterialHandle
            || !mutableMaterial
            || Engine::GetMaterialSurface(*mutableMaterial) != request.ExpectedSurface)
        {
            error = "state_changed_before_commit";
            return false;
        }

        rollback->MutationStarted = true;
        *mutableMaterial = stagedMaterial;
        m_SelectedEntity = entityHandle;
        SetFusionNavigationPivot(targetPivot);
        Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);

        Engine::MaterialAsset publishedMaterial;
        Engine::u64 publishedGeneration = 0;
        std::string readbackError;
        const bool readback = Engine::Renderer::ResolvePublishedMaterialAsset(
            request.MaterialHandle, publishedMaterial, publishedGeneration, readbackError);
        const bool selectionValid = m_SelectedEntity == entityHandle;
        const bool pivotValid = m_FusionNavigationPivotValid
            && m_FusionNavigationPivot.X == targetPivot.X
            && m_FusionNavigationPivot.Y == targetPivot.Y
            && m_FusionNavigationPivot.Z == targetPivot.Z;
        if (m_EditorMaterialControlForceCommitFailureOnce)
        {
            m_EditorMaterialControlForceCommitFailureOnce = false;
            error = "injected_renderer_readback_failure";
            return false;
        }
        if (!readback || publishedGeneration != generationBeforePublication + 1
            || Engine::GetMaterialSurface(publishedMaterial) != request.NewSurface
            || !selectionValid || !pivotValid)
        {
            error = readbackError.empty()
                ? "renderer_or_editor_readback_mismatch" : "renderer_readback_failed";
            return false;
        }

        RecordHistory("Agent patch shared material surface", rollback->State);
        if (m_UndoHistory.size()
                != std::min<std::size_t>(rollback->UndoHistory.size() + 1, 128)
            || !m_RedoHistory.empty())
        {
            error = "history_commit_mismatch";
            return false;
        }
        if (m_EditorMaterialControlForceLateResponseCollisionOnce)
        {
            m_EditorMaterialControlForceLateResponseCollisionOnce = false;
            std::string collisionError;
            if (!m_EditorMaterialControl.PublishRawResponseForSmoke(
                request.RequestId, "late response collision\n", collisionError))
            {
                error = "late_response_collision_injection_failed";
                return false;
            }
        }
        if (m_EditorMaterialControlForceParentSyncFailureOnce)
        {
            m_EditorMaterialControlForceParentSyncFailureOnce = false;
            m_EditorMaterialControl.InjectParentDirectorySyncFailureForSmoke();
        }
        return true;
    };
    transaction.Rollback = [this, rollback, request](EditorMaterialControlReceipt& rolledBack)
    {
        const bool stateRestored = !rollback->MutationStarted || RestoreHistoryState(rollback->State);
        m_UndoHistory = rollback->UndoHistory;
        m_RedoHistory = rollback->RedoHistory;
        m_FusionNavigationPivot = rollback->FusionPivot;
        m_FusionNavigationPivotValid = rollback->FusionPivotValid;
        rolledBack.RendererGeneration =
            Engine::Renderer::GetPublishedArtifactResolverGeneration();
        rolledBack.UndoDepthAfter = m_UndoHistory.size();
        rolledBack.RedoDepthAfter = m_RedoHistory.size();
        rolledBack.SelectionCommitted = false;
        rolledBack.PivotRetargeted = false;
        Engine::MaterialAsset publishedMaterial;
        Engine::u64 publishedGeneration = 0;
        std::string readbackError;
        rolledBack.RendererReadbackVerified =
            Engine::Renderer::ResolvePublishedMaterialAsset(request.MaterialHandle,
                publishedMaterial, publishedGeneration, readbackError)
            && publishedGeneration == rolledBack.RendererGeneration
            && Engine::GetMaterialSurface(publishedMaterial) == rolledBack.Before;
        const bool verified = stateRestored
            && m_UndoHistory.size() == rollback->UndoHistory.size()
            && m_RedoHistory.size() == rollback->RedoHistory.size()
            && m_FusionNavigationPivotValid == rollback->FusionPivotValid
            && (!m_FusionNavigationPivotValid || (m_FusionNavigationPivot.X == rollback->FusionPivot.X
                && m_FusionNavigationPivot.Y == rollback->FusionPivot.Y
                && m_FusionNavigationPivot.Z == rollback->FusionPivot.Z))
            && rolledBack.RendererReadbackVerified;
        if (m_EditorMaterialControlForceRollbackVerificationFailureOnce)
        {
            m_EditorMaterialControlForceRollbackVerificationFailureOnce = false;
            return false;
        }
        return verified;
    };
    return transaction;
}

void EditorLayer::RunEditorMaterialControlSmokeBeforeDrain()
{
    if (!m_EditorMaterialControlSmokeRequested
        || m_EditorMaterialControlSmokeCompleted
        || m_EditorMaterialControlSmokeStage != 0)
        return;

    const Engine::SceneEntity* target = m_ActiveScene.TryGetEntity(m_PrototypeMeshEntity);
    if (!target)
        throw std::runtime_error("editor material-control smoke target disappeared");
    const std::string& session = m_EditorMaterialControl.GetSessionId();
    std::string error;
    if (m_EditorMaterialControl.PublishRequestForSmoke(
        ".hidden-request", "invalid", error))
        throw std::runtime_error("editor material-control accepted a leading-dot request ID");
    error.clear();
    const std::string collisionRequest = EditorMaterialControlMailbox::FormatPatchRequest(
        "smoke-01-response-collision", session, target->EntityHandle.Id, target->Name,
        m_EditorMaterialControlSmokeMaterial, m_EditorMaterialControlSmokeBefore,
        m_EditorMaterialControlSmokeAfter);
    if (!m_EditorMaterialControl.PublishRawResponseForSmoke(
            "smoke-01-response-collision", "occupied\n", error)
        || !m_EditorMaterialControl.PublishRequestForSmoke(
            "smoke-01-response-collision", collisionRequest, error))
        throw std::runtime_error("could not stage response-collision smoke: " + error);

    std::string unknownField = EditorMaterialControlMailbox::FormatPatchRequest(
        "smoke-02-unknown-field", session, target->EntityHandle.Id, target->Name,
        m_EditorMaterialControlSmokeMaterial, m_EditorMaterialControlSmokeBefore,
        m_EditorMaterialControlSmokeAfter);
    unknownField += "UnexpectedField 1\n";
    if (!m_EditorMaterialControl.PublishRequestForSmoke(
            "smoke-02-unknown-field", unknownField, error))
        throw std::runtime_error("could not stage unknown-field smoke: " + error);

    const std::string wrongSession = EditorMaterialControlMailbox::FormatInspectRequest(
        "smoke-03-wrong-session", "not-this-session", target->EntityHandle.Id,
        target->Name, m_EditorMaterialControlSmokeMaterial);
    if (!m_EditorMaterialControl.PublishRequestForSmoke(
            "smoke-03-wrong-session", wrongSession, error))
        throw std::runtime_error("could not stage wrong-session smoke: " + error);

    m_EditorMaterialControlForceCommitFailureOnce = true;
    const std::string rollback = EditorMaterialControlMailbox::FormatPatchRequest(
        "smoke-04-rollback", session, target->EntityHandle.Id, target->Name,
        m_EditorMaterialControlSmokeMaterial, m_EditorMaterialControlSmokeBefore,
        m_EditorMaterialControlSmokeAfter);
    if (!m_EditorMaterialControl.PublishRequestForSmoke(
            "smoke-04-rollback", rollback, error))
        throw std::runtime_error("could not stage rollback smoke: " + error);
    m_EditorMaterialControlSmokeStage = 1;
}

void EditorLayer::RunEditorMaterialControlSmokeAfterDrain()
{
    if (!m_EditorMaterialControlSmokeRequested
        || m_EditorMaterialControlSmokeCompleted)
        return;
    const Engine::SceneEntity* target = m_ActiveScene.TryGetEntity(m_PrototypeMeshEntity);
    const Engine::MaterialAsset* material =
        m_MaterialLibrary.Get(m_EditorMaterialControlSmokeMaterial);
    if (!target || !material)
        throw std::runtime_error("editor material-control smoke state disappeared");

    if (m_EditorMaterialControlSmokeStage == 1)
    {
        const auto rejected = [this](std::string_view id)
        {
            const EditorMaterialControlReceipt* receipt =
                m_EditorMaterialControl.FindTerminalReceipt(id);
            return receipt && !receipt->Succeeded;
        };
        const bool collisionRejected =
            m_EditorMaterialControl.GetResponseCollisionCount() == 1
            && !m_EditorMaterialControl.FindTerminalReceipt("smoke-01-response-collision");
        const EditorMaterialControlReceipt* rolledBack =
            m_EditorMaterialControl.FindTerminalReceipt("smoke-04-rollback");
        const bool invalidRejected = rejected("smoke-02-unknown-field")
            && rejected("smoke-03-wrong-session")
            && rolledBack && !rolledBack->Succeeded
            && rolledBack->Reason == "injected_renderer_readback_failure_rolled_back"
            && rolledBack->Effect == "RolledBack"
            && rolledBack->Recovery == "None"
            && rolledBack->Before == m_EditorMaterialControlSmokeBefore
            && rolledBack->After == m_EditorMaterialControlSmokeBefore
            && rolledBack->RendererReadbackVerified;
        const bool unchanged = Engine::GetMaterialSurface(*material)
                == m_EditorMaterialControlSmokeBefore
            && Engine::Renderer::GetPublishedArtifactResolverGeneration()
                == m_EditorMaterialControlSmokeInitialRendererGeneration + 2
            && m_UndoHistory.size() == m_EditorMaterialControlSmokeInitialUndoDepth
            && m_RedoHistory.size() == m_EditorMaterialControlSmokeInitialRedoDepth
            && m_SelectedEntity == m_ActiveScene.GetMainCameraEntity()
            && m_FusionNavigationPivotValid
            && m_FusionNavigationPivot.X == 0.0
            && m_FusionNavigationPivot.Y == 0.0
            && m_FusionNavigationPivot.Z == 0.0
            && m_ActiveScene.IsEntityValid(m_EditorMaterialControlSharedPeer);
        if (!collisionRejected || !invalidRejected || !unchanged)
            throw std::runtime_error(
                "editor material-control invalid transactionality smoke failed");
        m_EditorMaterialControlSmokeInitialRendererGeneration =
            Engine::Renderer::GetPublishedArtifactResolverGeneration();

        const std::string& session = m_EditorMaterialControl.GetSessionId();
        std::string error;
        const std::string inspect = EditorMaterialControlMailbox::FormatInspectRequest(
            "smoke-05-inspect", session, target->EntityHandle.Id, target->Name,
            m_EditorMaterialControlSmokeMaterial);
        m_EditorMaterialControlForceLateResponseCollisionOnce = true;
        const std::string lateCollision = EditorMaterialControlMailbox::FormatPatchRequest(
            "smoke-06-late-collision", session, target->EntityHandle.Id, target->Name,
            m_EditorMaterialControlSmokeMaterial,
            m_EditorMaterialControlSmokeBefore,
            m_EditorMaterialControlSmokeAfter);
        m_EditorMaterialControlSmokePatchRequest =
            EditorMaterialControlMailbox::FormatPatchRequest(
                "smoke-07-patch", session, target->EntityHandle.Id, target->Name,
                m_EditorMaterialControlSmokeMaterial,
                m_EditorMaterialControlSmokeBefore,
                m_EditorMaterialControlSmokeAfter);
        std::string oversized(EditorMaterialControlMailbox::MaximumRequestBytes + 1, 'x');
        if (!m_EditorMaterialControl.PublishRequestForSmoke(
                "smoke-05-inspect", inspect, error)
            || !m_EditorMaterialControl.PublishRequestForSmoke(
                "smoke-06-late-collision", lateCollision, error)
            || !m_EditorMaterialControl.PublishRequestForSmoke(
                "smoke-07-patch", m_EditorMaterialControlSmokePatchRequest, error)
            || !m_EditorMaterialControl.PublishRequestForSmoke(
                "smoke-08-oversized", oversized, error))
            throw std::runtime_error("could not stage valid control smoke: " + error);
        m_EditorMaterialControlSmokeStage = 2;
        return;
    }

    if (m_EditorMaterialControlSmokeStage == 2)
    {
        const EditorMaterialControlReceipt* inspect =
            m_EditorMaterialControl.FindTerminalReceipt("smoke-05-inspect");
        const EditorMaterialControlReceipt* patch =
            m_EditorMaterialControl.FindTerminalReceipt("smoke-07-patch");
        const EditorMaterialControlReceipt* oversized =
            m_EditorMaterialControl.FindTerminalReceipt("smoke-08-oversized");
        const bool lateCollisionRolledBack =
            !m_EditorMaterialControl.FindTerminalReceipt("smoke-06-late-collision")
            && std::filesystem::is_regular_file(m_EditorMaterialControl.GetRoot()
                / "responses" / "smoke-06-late-collision.collision.response")
            && m_EditorMaterialControl.GetResponseCollisionCount() == 2;
        Engine::Math::DVec3 targetPosition;
        const bool pivotExpected = m_ActiveScene.TryGetEntityApproximateWorldPosition(
            m_PrototypeMeshEntity, targetPosition);
        const bool affectedExactly = patch && patch->AffectedEntityCount == 2
            && patch->AffectedEntityIds.size() == 2
            && !patch->AffectedEntityIdsTruncated
            && std::find(patch->AffectedEntityIds.begin(), patch->AffectedEntityIds.end(),
                m_PrototypeMeshEntity.Id) != patch->AffectedEntityIds.end()
            && std::find(patch->AffectedEntityIds.begin(), patch->AffectedEntityIds.end(),
                m_EditorMaterialControlSharedPeer.Id) != patch->AffectedEntityIds.end();
        const bool committed = inspect && inspect->Succeeded
            && inspect->Effect == "ReadOnly"
            && inspect->Before == m_EditorMaterialControlSmokeBefore
            && inspect->After == m_EditorMaterialControlSmokeBefore
            && inspect->RendererReadbackVerified
            && patch && patch->Succeeded
            && patch->Before == m_EditorMaterialControlSmokeBefore
            && patch->After == m_EditorMaterialControlSmokeAfter
            && patch->Recovery == "UndoRedo"
            && patch->RendererReadbackVerified
            && patch->RendererGeneration
                == m_EditorMaterialControlSmokeInitialRendererGeneration + 3
            && patch->UndoDepthBefore == m_EditorMaterialControlSmokeInitialUndoDepth
            && patch->UndoDepthAfter == m_EditorMaterialControlSmokeInitialUndoDepth + 1
            && patch->RedoDepthBefore == m_EditorMaterialControlSmokeInitialRedoDepth
            && patch->RedoDepthAfter == 0
            && patch->SelectionCommitted && patch->PivotRetargeted
            && affectedExactly
            && Engine::GetMaterialSurface(*material)
                == m_EditorMaterialControlSmokeAfter
            && m_SelectedEntity == m_PrototypeMeshEntity
            && pivotExpected && m_FusionNavigationPivotValid
            && m_FusionNavigationPivot.X == targetPosition.X
            && m_FusionNavigationPivot.Y == targetPosition.Y
            && m_FusionNavigationPivot.Z == targetPosition.Z
            && oversized && !oversized->Succeeded
            && oversized->Reason == "oversized_request_rejected"
            && lateCollisionRolledBack;
        const std::string* patchText =
            m_EditorMaterialControl.FindTerminalText("smoke-07-patch");
        if (!committed || !patchText)
            throw std::runtime_error("editor material-control valid commit smoke failed");
        m_EditorMaterialControlSmokePatchReceipt = *patchText;

        std::string error;
        const std::string secondOversized(
            EditorMaterialControlMailbox::MaximumRequestBytes + 2, 'y');
        if (!m_EditorMaterialControl.PublishRequestForSmoke(
                "smoke-07-patch", m_EditorMaterialControlSmokePatchRequest, error)
            || !m_EditorMaterialControl.PublishRequestForSmoke(
                "smoke-08-oversized", secondOversized, error))
            throw std::runtime_error("could not stage idempotent retry smoke: " + error);
        m_EditorMaterialControlSmokeStage = 3;
        return;
    }

    if (m_EditorMaterialControlSmokeStage == 3)
    {
        const std::string* retryText =
            m_EditorMaterialControl.FindTerminalText("smoke-07-patch");
        const bool idempotent = retryText
            && *retryText == m_EditorMaterialControlSmokePatchReceipt
            && Engine::GetMaterialSurface(*material)
                == m_EditorMaterialControlSmokeAfter
            && Engine::Renderer::GetPublishedArtifactResolverGeneration()
                == m_EditorMaterialControlSmokeInitialRendererGeneration + 3
            && m_UndoHistory.size()
                == m_EditorMaterialControlSmokeInitialUndoDepth + 1
            && m_EditorMaterialControl.GetResponseCollisionCount() == 3
            && std::filesystem::is_regular_file(m_EditorMaterialControl.GetRoot()
                / "responses" / "smoke-08-oversized.collision.response")
            && !std::filesystem::exists(m_EditorMaterialControl.GetRoot()
                / "requests" / "smoke-08-oversized.request");
        if (!idempotent)
            throw std::runtime_error("editor material-control idempotent retry smoke failed");
        std::string changedDuplicate = m_EditorMaterialControlSmokePatchRequest;
        changedDuplicate += "UnexpectedField 2\n";
        const std::string thirdOversized(
            EditorMaterialControlMailbox::MaximumRequestBytes + 3, 'z');
        std::string error;
        if (!m_EditorMaterialControl.PublishRequestForSmoke(
                "smoke-07-patch", changedDuplicate, error)
            || !m_EditorMaterialControl.PublishRequestForSmoke(
                "smoke-08-oversized", thirdOversized, error))
            throw std::runtime_error("could not stage changed duplicate smoke: " + error);
        m_EditorMaterialControlSmokeStage = 4;
        return;
    }

    if (m_EditorMaterialControlSmokeStage == 4)
    {
        const std::string* retryText =
            m_EditorMaterialControl.FindTerminalText("smoke-07-patch");
        const bool conflictsConsumed = retryText
            && *retryText == m_EditorMaterialControlSmokePatchReceipt
            && m_EditorMaterialControl.GetResponseCollisionCount() == 5
            && Engine::GetMaterialSurface(*material)
                == m_EditorMaterialControlSmokeAfter
            && Engine::Renderer::GetPublishedArtifactResolverGeneration()
                == m_EditorMaterialControlSmokeInitialRendererGeneration + 3
            && m_UndoHistory.size()
                == m_EditorMaterialControlSmokeInitialUndoDepth + 1
            && !std::filesystem::exists(m_EditorMaterialControl.GetRoot()
                / "requests" / "smoke-07-patch.request")
            && !std::filesystem::exists(m_EditorMaterialControl.GetRoot()
                / "requests" / "smoke-08-oversized.request");
        if (!conflictsConsumed)
            throw std::runtime_error(
                "editor material-control stable conflict smoke failed");
        std::string thirdConflict = m_EditorMaterialControlSmokePatchRequest;
        thirdConflict += "UnexpectedField 3\n";
        std::string error;
        if (!m_EditorMaterialControl.PublishRequestForSmoke(
            "smoke-07-patch", thirdConflict, error))
            throw std::runtime_error("could not stage third conflict smoke: " + error);
        m_EditorMaterialControlSmokeStage = 5;
        return;
    }

    if (m_EditorMaterialControlSmokeStage == 5)
    {
        const std::string* retryText =
            m_EditorMaterialControl.FindTerminalText("smoke-07-patch");
        const bool thirdConflictConsumed = retryText
            && *retryText == m_EditorMaterialControlSmokePatchReceipt
            && m_EditorMaterialControl.GetResponseCollisionCount() == 6
            && Engine::GetMaterialSurface(*material)
                == m_EditorMaterialControlSmokeAfter
            && Engine::Renderer::GetPublishedArtifactResolverGeneration()
                == m_EditorMaterialControlSmokeInitialRendererGeneration + 3
            && m_UndoHistory.size()
                == m_EditorMaterialControlSmokeInitialUndoDepth + 1
            && !std::filesystem::exists(m_EditorMaterialControl.GetRoot()
                / "requests" / "smoke-07-patch.request");
        if (!thirdConflictConsumed || !Undo())
            throw std::runtime_error(
                "editor material-control changed-duplicate/undo smoke failed");
        const Engine::MaterialAsset* undone =
            m_MaterialLibrary.Get(m_EditorMaterialControlSmokeMaterial);
        if (!undone || Engine::GetMaterialSurface(*undone)
                != m_EditorMaterialControlSmokeBefore
            || !Redo())
            throw std::runtime_error("editor material-control undo smoke failed");
        const Engine::MaterialAsset* redone =
            m_MaterialLibrary.Get(m_EditorMaterialControlSmokeMaterial);
        if (!redone || Engine::GetMaterialSurface(*redone)
                != m_EditorMaterialControlSmokeAfter
            || m_SelectedEntity != m_PrototypeMeshEntity)
            throw std::runtime_error("editor material-control redo smoke failed");
        const bool projectGuardClosed = !m_EditorMaterialControl.EnsureProjectIdentity(
                "output/projects/editor-control-different.spiralproject")
            && !m_EditorMaterialControl.IsAcceptingRequests()
            && std::filesystem::is_regular_file(
                m_EditorMaterialControl.GetRoot() / "session.closed");
        if (!projectGuardClosed)
            throw std::runtime_error("editor material-control project guard smoke failed");

        Engine::Log::Info(
            "EditorMaterialControlMailboxV1 interface=private-filesystem "
            "actions=inspect,select-patch-shared session=exact schema=1 "
            "mainThread=before-scene-snapshot invalid=transactional-pass "
            "rollback=readback-and-receipt-pass inspect=pass commit=pass "
            "idempotency=exact-bytes conflicts=A-B-C-consumed "
            "oversized=non-replayable-A-B-C shared=2 "
            "history=exactly-one undo=pass redo=pass save=not-invoked "
            "projectGuard=canonical-every-drain "
            "publication=visible-rename-authoritative "
            "input=mailbox-no-ui-synthesis pollCadenceMs=16 directoryPolls=",
            m_EditorMaterialControl.GetDirectoryPollCount(),
            " result=pass");
        m_ConsoleLines.emplace_back("Editor material-control mailbox smoke passed");
        m_EditorMaterialControlSmokeCompleted = true;
        m_EditorMaterialControlSmokeStage = 6;
    }
}

void EditorLayer::RunEditorMaterialControlLiveHelperSmokeAfterDrain()
{
    if (!m_EditorMaterialControlLiveHelperSmokeRequested
        || m_EditorMaterialControlLiveHelperSmokeCompleted)
        return;
    const EditorMaterialControlReceipt* inspect =
        m_EditorMaterialControl.FindTerminalReceipt("live-helper-inspect");
    const EditorMaterialControlReceipt* patch =
        m_EditorMaterialControl.FindTerminalReceipt("live-helper-set");
    if (!inspect || !patch)
        return;

    const Engine::MaterialAsset* material =
        m_MaterialLibrary.Get(m_EditorMaterialControlSmokeMaterial);
    const bool affected = patch->AffectedEntityCount == 2
        && patch->AffectedEntityIds.size() == 2
        && !patch->AffectedEntityIdsTruncated
        && std::find(patch->AffectedEntityIds.begin(), patch->AffectedEntityIds.end(),
            m_PrototypeMeshEntity.Id) != patch->AffectedEntityIds.end()
        && std::find(patch->AffectedEntityIds.begin(), patch->AffectedEntityIds.end(),
            m_EditorMaterialControlSharedPeer.Id) != patch->AffectedEntityIds.end();
    const bool valid = inspect->Succeeded
        && inspect->Action == EditorMaterialControlAction::InspectMaterialSurface
        && inspect->Effect == "ReadOnly" && inspect->Recovery == "None"
        && inspect->EntityId == m_PrototypeMeshEntity.Id
        && inspect->MaterialHandle == m_EditorMaterialControlSmokeMaterial
        && inspect->Before == m_EditorMaterialControlSmokeBefore
        && inspect->After == m_EditorMaterialControlSmokeBefore
        && !inspect->SelectionCommitted && !inspect->PivotRetargeted
        && inspect->RendererReadbackVerified
        && patch->Succeeded
        && patch->Action == EditorMaterialControlAction::SelectEntityPatchMaterialSurface
        && patch->Effect == "SharedMaterialSurfacePatched"
        && patch->Recovery == "UndoRedo"
        && patch->EntityId == m_PrototypeMeshEntity.Id
        && patch->MaterialHandle == m_EditorMaterialControlSmokeMaterial
        && patch->Before == m_EditorMaterialControlSmokeBefore
        && patch->After == m_EditorMaterialControlSmokeAfter
        && patch->SelectionCommitted && patch->PivotRetargeted
        && patch->RendererReadbackVerified && affected
        && material && Engine::GetMaterialSurface(*material)
            == m_EditorMaterialControlSmokeAfter
        && m_UndoHistory.size() == m_EditorMaterialControlSmokeInitialUndoDepth + 1
        && Engine::Renderer::GetPublishedArtifactResolverGeneration()
            == m_EditorMaterialControlSmokeInitialRendererGeneration + 1
        && m_EditorMaterialControl.GetDirectoryPollCount() >= 2
        && m_EditorMaterialControl.GetCadenceSkipCount() > 0;
    if (!valid)
        throw std::runtime_error("live external editor material-control helper smoke failed");

    Engine::Log::Info(
        "EditorMaterialControlLiveHelperV1 producer=external-python requests=fresh "
        "inspect=semantic-pass set=semantic-pass close=editor-condition "
        "identity=session-pid-project affected=bounded-exact "
        "pollCadenceMs=16 idleSkips=", m_EditorMaterialControl.GetCadenceSkipCount(),
        " directoryPolls=", m_EditorMaterialControl.GetDirectoryPollCount(),
        " result=pass");
    m_EditorMaterialControlLiveHelperSmokeCompleted = true;
    Engine::Application::Get().Close();
}

void EditorLayer::RunEditorMaterialControlCapacitySmokeBeforeDrain()
{
    if (!m_EditorMaterialControlCapacitySmokeRequested
        || m_EditorMaterialControlCapacitySmokeCompleted
        || m_EditorMaterialControlSmokeStage != 0)
        return;
    const Engine::SceneEntity* target = m_ActiveScene.TryGetEntity(m_PrototypeMeshEntity);
    if (!target)
        throw std::runtime_error("editor material-control capacity target disappeared");
    for (std::size_t index = 0;
        index <= EditorMaterialControlMailbox::MaximumTerminalRequests; ++index)
    {
        std::ostringstream requestId;
        requestId << "capacity-" << std::setfill('0') << std::setw(4) << index;
        const std::string request = EditorMaterialControlMailbox::FormatInspectRequest(
            requestId.str(), m_EditorMaterialControl.GetSessionId(),
            target->EntityHandle.Id, target->Name,
            m_EditorMaterialControlSmokeMaterial);
        std::string error;
        if (!m_EditorMaterialControl.PublishRequestForSmoke(
            requestId.str(), request, error))
            throw std::runtime_error(
                "could not stage editor material-control capacity smoke: " + error);
    }
    m_EditorMaterialControlSmokeStage = 1;
}

void EditorLayer::RunEditorMaterialControlCapacitySmokeAfterDrain()
{
    if (!m_EditorMaterialControlCapacitySmokeRequested
        || m_EditorMaterialControlCapacitySmokeCompleted
        || m_EditorMaterialControl.IsAcceptingRequests())
        return;
    const std::filesystem::path pending = m_EditorMaterialControl.GetRoot()
        / "requests" / "capacity-0256.request";
    const std::filesystem::path unexpectedResponse = m_EditorMaterialControl.GetRoot()
        / "responses" / "capacity-0256.response";
    const EditorMaterialControlReceipt* bounded =
        m_EditorMaterialControl.FindTerminalReceipt("capacity-0000");
    const bool valid = m_EditorMaterialControl.GetTerminalCount()
            == EditorMaterialControlMailbox::MaximumTerminalRequests
        && std::filesystem::is_regular_file(pending)
        && !std::filesystem::exists(unexpectedResponse)
        && std::filesystem::is_regular_file(
            m_EditorMaterialControl.GetRoot() / "session.closed")
        && Engine::Renderer::GetPublishedArtifactResolverGeneration()
            == m_EditorMaterialControlSmokeInitialRendererGeneration
        && m_UndoHistory.size() == m_EditorMaterialControlSmokeInitialUndoDepth
        && bounded && bounded->Succeeded
        && bounded->AffectedEntityCount == 42
        && bounded->AffectedEntityIds.size()
            == EditorMaterialControlMailbox::MaximumAffectedEntityIds
        && bounded->AffectedEntityIdsTruncated
        && std::is_sorted(bounded->AffectedEntityIds.begin(),
            bounded->AffectedEntityIds.end())
        && std::find(bounded->AffectedEntityIds.begin(),
            bounded->AffectedEntityIds.end(), m_PrototypeMeshEntity.Id)
            != bounded->AffectedEntityIds.end();
    if (!valid)
        throw std::runtime_error("editor material-control capacity retention smoke failed");
    Engine::Log::Info(
        "EditorMaterialControlCapacityV1 retained=256 accepting=no "
        "pendingUnclaimed=1 response=absent session=closed "
        "affectedTotal=42 affectedSample=32 truncated=yes result=pass");
    m_EditorMaterialControlCapacitySmokeCompleted = true;
    Engine::Application::Get().Close();
}

void EditorLayer::RunEditorMaterialControlDurabilitySmokeBeforeDrain()
{
    if (!m_EditorMaterialControlDurabilitySmokeRequested
        || m_EditorMaterialControlDurabilitySmokeCompleted
        || m_EditorMaterialControlSmokeStage != 0)
        return;
    const Engine::SceneEntity* target = m_ActiveScene.TryGetEntity(m_PrototypeMeshEntity);
    if (!target)
        throw std::runtime_error("editor material-control durability target disappeared");
    const std::string request = EditorMaterialControlMailbox::FormatPatchRequest(
        "durability-visible-success", m_EditorMaterialControl.GetSessionId(),
        target->EntityHandle.Id, target->Name, m_EditorMaterialControlSmokeMaterial,
        m_EditorMaterialControlSmokeBefore, m_EditorMaterialControlSmokeAfter);
    std::string error;
    m_EditorMaterialControlForceParentSyncFailureOnce = true;
    if (!m_EditorMaterialControl.PublishRequestForSmoke(
        "durability-visible-success", request, error))
        throw std::runtime_error(
            "could not stage editor material-control durability smoke: " + error);
    m_EditorMaterialControlSmokeStage = 1;
}

void EditorLayer::RunEditorMaterialControlDurabilitySmokeAfterDrain()
{
    if (!m_EditorMaterialControlDurabilitySmokeRequested
        || m_EditorMaterialControlDurabilitySmokeCompleted
        || m_EditorMaterialControlSmokeStage != 1)
        return;
    const EditorMaterialControlReceipt* receipt =
        m_EditorMaterialControl.FindTerminalReceipt("durability-visible-success");
    if (!receipt)
        return;
    const Engine::MaterialAsset* material =
        m_MaterialLibrary.Get(m_EditorMaterialControlSmokeMaterial);
    const bool valid = receipt->Succeeded
        && receipt->Effect == "SharedMaterialSurfacePatched"
        && receipt->Before == m_EditorMaterialControlSmokeBefore
        && receipt->After == m_EditorMaterialControlSmokeAfter
        && receipt->RendererReadbackVerified
        && material && Engine::GetMaterialSurface(*material)
            == m_EditorMaterialControlSmokeAfter
        && Engine::Renderer::GetPublishedArtifactResolverGeneration()
            == m_EditorMaterialControlSmokeInitialRendererGeneration + 1
        && m_UndoHistory.size() == m_EditorMaterialControlSmokeInitialUndoDepth + 1
        && m_EditorMaterialControl.GetTerminalCount() == 1
        && m_EditorMaterialControl.GetDurabilityDegradationCount() == 1
        && !m_EditorMaterialControl.IsAcceptingRequests()
        && std::filesystem::is_regular_file(m_EditorMaterialControl.GetRoot()
            / "responses" / "durability-visible-success.response")
        && std::filesystem::is_regular_file(m_EditorMaterialControl.GetRoot()
            / "session.closed")
        && !std::filesystem::exists(m_EditorMaterialControl.GetRoot()
            / "requests" / "durability-visible-success.request");
    if (!valid)
        throw std::runtime_error(
            "editor material-control visible-publication durability smoke failed");
    Engine::Log::Info(
        "EditorMaterialControlDurabilityV1 visibility=rename-authoritative "
        "parentSync=injected-failure committed=preserved rollback=no "
        "acceptance=closed crashDurability=degraded result=pass");
    m_EditorMaterialControlDurabilitySmokeCompleted = true;
    m_EditorMaterialControlSmokeStage = 2;
    Engine::Application::Get().Close();
}

void EditorLayer::RunEditorMaterialControlRollbackFailureSmokeBeforeDrain()
{
    if ((!m_EditorMaterialControlRollbackFailureSmokeRequested
            && !m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested)
        || m_EditorMaterialControlRollbackFailureSmokeCompleted
        || m_EditorMaterialControlSmokeStage != 0)
    {
        return;
    }

    const Engine::SceneEntity* target = m_ActiveScene.TryGetEntity(m_PrototypeMeshEntity);
    if (!target)
        throw std::runtime_error("editor material-control rollback-failure target disappeared");

    const bool postCommit = m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested;
    const std::string requestId = postCommit
        ? "rollback-failure-postcommit" : "rollback-failure-commit";
    m_EditorMaterialControlForceRollbackVerificationFailureOnce = true;
    m_EditorMaterialControlForceCommitFailureOnce = !postCommit;
    m_EditorMaterialControlForceLateResponseCollisionOnce = postCommit;
    const std::string request = EditorMaterialControlMailbox::FormatPatchRequest(
        requestId, m_EditorMaterialControl.GetSessionId(), target->EntityHandle.Id,
        target->Name, m_EditorMaterialControlSmokeMaterial,
        m_EditorMaterialControlSmokeBefore, m_EditorMaterialControlSmokeAfter);
    std::string error;
    if (!m_EditorMaterialControl.PublishRequestForSmoke(requestId, request, error))
        throw std::runtime_error(
            "could not stage editor material-control rollback-failure smoke: " + error);
    m_EditorMaterialControlSmokeStage = 1;
}

void EditorLayer::RunEditorMaterialControlRollbackFailureSmokeAfterDrain()
{
    if ((!m_EditorMaterialControlRollbackFailureSmokeRequested
            && !m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested)
        || m_EditorMaterialControlRollbackFailureSmokeCompleted
        || m_EditorMaterialControlSmokeStage != 1
        || m_EditorMaterialControl.IsAcceptingRequests())
    {
        return;
    }

    const bool postCommit = m_EditorMaterialControlPostCommitRollbackFailureSmokeRequested;
    const std::string requestId = postCommit
        ? "rollback-failure-postcommit" : "rollback-failure-commit";
    bool requestWasRequeued = false;
    std::error_code iterationError;
    for (std::filesystem::directory_iterator iterator(
             m_EditorMaterialControl.GetRoot() / "requests", iterationError), end;
         !iterationError && iterator != end; iterator.increment(iterationError))
    {
        if (iterator->path().filename().string().find(requestId) != std::string::npos)
            requestWasRequeued = true;
    }

    const EditorMaterialControlReceipt* receipt =
        m_EditorMaterialControl.FindTerminalReceipt(requestId);
    const bool receiptIsRecoveryRequired = receipt && !receipt->Succeeded
        && receipt->Reason == (postCommit
            ? "postcommit_rollback_verification_failed"
            : "rollback_verification_failed")
        && receipt->Effect == "RecoveryRequired"
        && receipt->Recovery == "RestartSession";
    const Engine::MaterialAsset* material =
        m_MaterialLibrary.Get(m_EditorMaterialControlSmokeMaterial);
    const bool stateActuallyRestored = material
        && Engine::GetMaterialSurface(*material) == m_EditorMaterialControlSmokeBefore
        && m_UndoHistory.size() == m_EditorMaterialControlSmokeInitialUndoDepth
        && m_RedoHistory.size() == m_EditorMaterialControlSmokeInitialRedoDepth
        && m_SelectedEntity == m_ActiveScene.GetMainCameraEntity()
        && Engine::Renderer::GetPublishedArtifactResolverGeneration()
            == m_EditorMaterialControlSmokeInitialRendererGeneration + 2;
    const bool closed = !m_EditorMaterialControl.IsAcceptingRequests()
        && std::filesystem::is_regular_file(
            m_EditorMaterialControl.GetRoot() / "session.closed");
    if (iterationError || requestWasRequeued || !receiptIsRecoveryRequired
        || !stateActuallyRestored || !closed)
    {
        throw std::runtime_error(
            "editor material-control rollback-verification failure smoke failed");
    }

    Engine::Log::Info(
        "EditorMaterialControlRollbackFailureV1 path=",
        postCommit ? "postcommit-publication" : "commit",
        " closeReason=",
        postCommit ? "postcommit_rollback_verification_failed"
                   : "rollback_verification_failed",
        " effect=RecoveryRequired acknowledgement=typed",
        " rolledBackClaim=no requestRequeued=no session=closed result=pass");
    m_EditorMaterialControlRollbackFailureSmokeCompleted = true;
    m_EditorMaterialControlSmokeStage = 2;
    Engine::Application::Get().Close();
}

void EditorLayer::OnUiRender()
{
    if (!ImGui::GetCurrentContext())
        return;

    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && io.KeyCtrl)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
            Undo();
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
            Redo();
    }

    DrawDockspace();
    DrawSceneHierarchyPanel();
    DrawInspectorPanel();
    DrawViewportPanel();
    DrawConsolePanel();
    DrawProfilerPanel();
    DrawProjectPanel();
    DrawNewProjectDialog();

    if (m_CaptureViewportRequested && !m_CaptureViewportComplete && m_FrameCounter >= 2)
    {
        const bool captured = Engine::Renderer::CaptureViewportToFile(m_CaptureViewportPath);
        m_CaptureViewportComplete = true;
        m_ConsoleLines.emplace_back(captured
            ? std::string("Viewport capture saved: ") + m_CaptureViewportPath
            : std::string("Viewport capture failed: ") + m_CaptureViewportPath);
    }

    CaptureSceneOriginRasterSmoke();

    if (m_ShowDemoWindow)
        ImGui::ShowDemoWindow(&m_ShowDemoWindow);

    if (const auto marker = Engine::Renderer::GetOpticalResponseMarker(Engine::Application::Get().GetFrameIndex()))
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::GetForegroundDrawList(viewport)->AddRectFilled(viewport->Pos,
            { viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y }, IM_COL32(255, 255, 255, 255));
        Engine::Log::Info("OpticalResponseMarkerV1 markerId=", marker->MarkerId,
            " frame=", marker->ApplicationFrameIndex, " inputFrame=", marker->InputFrameIndex,
            " inputQpc=", marker->InputQpcTick, " contrast=white result=drawn");
        Engine::Renderer::ClearOpticalResponseMarker(marker->ApplicationFrameIndex);
    }
}

void EditorLayer::OnEvent(Engine::Event& event)
{
    Engine::EventDispatcher dispatcher(event);
    dispatcher.Dispatch<Engine::FileDropEvent>(GE_BIND_EVENT_FN(EditorLayer::OnFileDrop));
    if (event.Handled)
        return;

    if (event.GetEventType() == Engine::EventType::WindowFocus)
    {
        m_WindowFocused = static_cast<const Engine::WindowFocusEvent&>(event).IsFocused();
        if (!m_WindowFocused)
        {
            EndViewportCursorCapture();
            ClearViewportNavigationInput();
        }
    }
    else if (event.GetEventType() == Engine::EventType::KeyPressed)
    {
        const auto& keyEvent = static_cast<const Engine::KeyPressedEvent&>(event);
        const int key = keyEvent.GetKeyCode();
        if (key >= 0 && key < static_cast<int>(m_KeyDown.size()))
            m_KeyDown[static_cast<size_t>(key)] = true;
        if (!keyEvent.IsRepeat() && key == 'F' && m_ViewportNavigationInputEnabled)
            FocusSelectedEntity();
    }
    else if (event.GetEventType() == Engine::EventType::KeyReleased)
    {
        const int key = static_cast<const Engine::KeyReleasedEvent&>(event).GetKeyCode();
        if (key >= 0 && key < static_cast<int>(m_KeyDown.size()))
            m_KeyDown[static_cast<size_t>(key)] = false;
    }
    else if (event.GetEventType() == Engine::EventType::MouseButtonPressed)
    {
        const int button = static_cast<const Engine::MouseButtonPressedEvent&>(event).GetMouseButton();
        m_LeftMouseDown |= button == 0;
        m_RightMouseDown |= button == 1;
        m_MiddleMouseDown |= button == 2;
        const bool capturesNavigation = m_ViewportNavigationPreset == ViewportNavigationPreset::Unreal
            ? (button == 0 || button == 1 || button == 2)
            : button == 2;
        if (capturesNavigation && TryAcquireViewportNavigationFocus())
        {
            if (m_ViewportNavigationPreset == ViewportNavigationPreset::Fusion && IsShiftNavigationModifierDown())
                BeginFusionOrbitPivot();
            BeginViewportCursorCapture();
        }
    }
    else if (event.GetEventType() == Engine::EventType::MouseButtonReleased)
    {
        const int button = static_cast<const Engine::MouseButtonReleasedEvent&>(event).GetMouseButton();
        if (button == 0)
            m_LeftMouseDown = false;
        else if (button == 1)
            m_RightMouseDown = false;
        else if (button == 2)
            m_MiddleMouseDown = false;
        if (!m_LeftMouseDown && !m_RightMouseDown && !m_MiddleMouseDown)
            EndViewportCursorCapture();
    }
    else if (event.GetEventType() == Engine::EventType::MouseMoved)
    {
        const auto& mouseEvent = static_cast<const Engine::MouseMovedEvent&>(event);
        if (m_HasMousePosition && m_CursorCaptured && m_CursorCaptureBaselineArmed)
        {
            m_MouseDeltaX += mouseEvent.GetX() - static_cast<float>(m_MouseX);
            m_MouseDeltaY += mouseEvent.GetY() - static_cast<float>(m_MouseY);
        }
        m_MouseX = mouseEvent.GetX();
        m_MouseY = mouseEvent.GetY();
        m_HasMousePosition = true;
    }
    else if (event.GetEventType() == Engine::EventType::MouseScrolled)
    {
        if (TryAcquireViewportNavigationFocus())
            m_MouseWheelDelta += static_cast<const Engine::MouseScrolledEvent&>(event).GetYOffset();
    }

    if (m_EventTraceEnabled)
        Engine::Log::Trace("Editor received event: ", event.ToString());
}

void EditorLayer::ConfigureSceneOriginRasterSmoke()
{
    if (Engine::Renderer::GetActiveBackend() != Engine::RendererBackend::NVRHID3D12)
        throw std::runtime_error("Scene origin raster smoke requires the active NVRHI D3D12 backend");

    const Engine::AssetHandle defaultMeshAsset = m_AssetRegistry.FindAssetByPath(
        Engine::AssetType::Mesh, std::string(Engine::GetDefaultSceneMeshSourcePath()));
    if (defaultMeshAsset == Engine::kInvalidAssetHandle)
        throw std::runtime_error("Scene origin raster smoke requires the published default scene mesh artifact");

    Engine::AssetHandle defaultMaterialAsset = Engine::kInvalidAssetHandle;
    const Engine::Entity prototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
    if (const Engine::MeshRendererComponent* prototypeMesh =
            m_ActiveScene.TryGetMeshRendererComponent(prototypeMeshEntity))
    {
        defaultMaterialAsset = prototypeMesh->MaterialAsset;
    }
    const Engine::AssetMetadata* defaultMaterialMetadata = m_AssetRegistry.GetAsset(defaultMaterialAsset);
    if (!defaultMaterialMetadata
        || defaultMaterialMetadata->Type != Engine::AssetType::Material
        || !m_MaterialLibrary.Get(defaultMaterialAsset))
    {
        throw std::runtime_error("Scene origin raster smoke requires the loaded prototype material asset");
    }

    constexpr double base = 1000000000000.0;
    constexpr double sectorBoundaryOffset = 2048.0;
    m_ActiveScene = Engine::Scene("Scene Origin Raster Smoke");
    m_SceneOriginRasterMeshEntity = m_ActiveScene.CreateEntity("Scene Origin Raster Mesh");
    Engine::MeshRendererComponent meshRenderer;
    meshRenderer.MeshAsset = defaultMeshAsset;
    meshRenderer.MaterialAsset = defaultMaterialAsset;
    m_ActiveScene.AddMeshRendererComponent(m_SceneOriginRasterMeshEntity, meshRenderer);
    m_ActiveScene.SetEntityWorldPosition(m_SceneOriginRasterMeshEntity, { base + sectorBoundaryOffset - 0.5, 0.0, 0.0 });
    m_ActiveScene.SetEntityWorldPosition(m_ActiveScene.GetMainCameraEntity(), { base + sectorBoundaryOffset - 0.5, 0.0, -3.35 });
    m_SelectedEntity = m_SceneOriginRasterMeshEntity;
    SyncEditorCameraStateFromMainCamera(true);
}

void EditorLayer::AdvanceSceneOriginRasterSmoke()
{
    if (!m_SceneOriginRasterSmokeRequested || m_SceneOriginRasterSmokeCompleted)
        return;

    constexpr double base = 1000000000000.0;
    constexpr double sectorBoundaryOffset = 2048.0;
    constexpr double caseA = base + sectorBoundaryOffset - 0.5;
    constexpr double caseB = base + sectorBoundaryOffset + 0.5;
    Engine::TransformComponent* meshTransform = m_ActiveScene.TryGetTransform(m_SceneOriginRasterMeshEntity);
    if (!meshTransform)
        throw std::runtime_error("Scene origin raster smoke lost its mesh entity");

    if (m_FrameCounter == 3)
    {
        m_ActiveScene.SetEntityWorldPosition(m_SceneOriginRasterMeshEntity, { caseA, 0.0, 0.0 });
        m_CameraPosition = { caseA, 0.0, -3.35 };
    }
    else if (m_FrameCounter == 4)
    {
        m_ActiveScene.SetEntityWorldPosition(m_SceneOriginRasterMeshEntity, { caseB, 0.0, 0.0 });
        m_CameraPosition = { caseA, 0.0, -3.35 };
    }
    else if (m_FrameCounter == 5)
    {
        m_ActiveScene.SetEntityWorldPosition(m_SceneOriginRasterMeshEntity, { caseB, 0.0, 0.0 });
        m_CameraPosition = { caseB, 0.0, -3.35 };
    }
    else
    {
        return;
    }

    ApplyEditorCameraStateToScene();
    Engine::Math::DVec3 cameraPosition;
    if (m_ActiveScene.TryGetEntityApproximateWorldPosition(m_ActiveScene.GetMainCameraEntity(), cameraPosition))
        m_EditorCamera.SetPosition(cameraPosition);
}

void EditorLayer::CaptureSceneOriginRasterSmoke()
{
    if (!m_SceneOriginRasterSmokeRequested
        || m_SceneOriginRasterSmokeCompleted
        || m_FrameCounter < 3
        || m_FrameCounter > 5)
    {
        return;
    }

    const size_t caseIndex = static_cast<size_t>(m_FrameCounter - 3);
    if (!Engine::Renderer::CaptureViewportToFile(m_SceneOriginRasterCapturePaths[caseIndex]))
        throw std::runtime_error("Scene origin raster smoke could not capture case " + std::to_string(caseIndex));

    const std::shared_ptr<const Engine::SceneRasterFrame> rasterFrame = Engine::Renderer::GetLastSceneRasterFrame();
    if (!rasterFrame
        || !rasterFrame->HasValidView
        || rasterFrame->SnapshotFrameIndex != Engine::Application::Get().GetFrameIndex()
        || rasterFrame->Instances.size() != 1
        || rasterFrame->IssuedDrawCount != 1
        || rasterFrame->Instances[0].SourceEntity != m_SceneOriginRasterMeshEntity.Id)
    {
        throw std::runtime_error("Scene origin raster smoke did not draw the current immutable snapshot epoch");
    }

    constexpr double base = 1000000000000.0;
    constexpr double sectorBoundaryOffset = 2048.0;
    constexpr double caseA = base + sectorBoundaryOffset - 0.5;
    constexpr double caseB = base + sectorBoundaryOffset + 0.5;
    const double expectedOriginX = m_FrameCounter == 5 ? caseB : caseA;
    const float expectedRelativeX = m_FrameCounter == 4 ? 1.0f : 0.0f;
    const Engine::SceneRasterInstance& instance = rasterFrame->Instances[0];
    if (rasterFrame->TranslationOrigin.X != expectedOriginX
        || instance.TranslationOrigin.X != expectedOriginX
        || instance.TranslationOriginPosition.Sector.X != (m_FrameCounter == 5 ? 244140626LL : 244140625LL)
        || instance.Position.Sector.X != (m_FrameCounter == 3 ? 244140625LL : 244140626LL)
        || instance.CameraRelativePosition.X != expectedRelativeX)
    {
        throw std::runtime_error("Scene origin raster smoke observed mixed view and mesh epochs");
    }

    const char caseName = static_cast<char>('A' + caseIndex);
    Engine::Log::Info(
        "D3D12 scene origin raster case ", caseName,
        ": frame=", rasterFrame->SnapshotFrameIndex,
        ", entity=", instance.SourceEntity,
        ", draws=", rasterFrame->IssuedDrawCount,
        ", sectorX=", instance.Position.Sector.X,
        ", localX=", instance.Position.Local.X,
        ", originSectorX=", instance.TranslationOriginPosition.Sector.X,
        ", originLocalX=", instance.TranslationOriginPosition.Local.X,
        ", originX=", rasterFrame->TranslationOrigin.X,
        ", relativeX=", instance.CameraRelativePosition.X);

    if (m_FrameCounter == 5)
    {
        m_SceneOriginRasterSmokeCompleted = true;
        Engine::Log::Info("D3D12 scene origin raster smoke passed");
    }
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
            m_ShowNewProjectDialog = true;
        if (ImGui::MenuItem("Open Project"))
            LoadProject();
        if (ImGui::MenuItem("Save Project"))
            SaveProject();
        if (ImGui::MenuItem("Save Scene"))
            SaveActiveScene();
        if (ImGui::MenuItem("Save Asset Registry"))
            SaveAssetRegistry();
        ImGui::Separator();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_UndoHistory.empty()))
            Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_RedoHistory.empty()))
            Redo();
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

    if (ImGui::BeginMenu("Settings"))
    {
        if (ImGui::BeginMenu("Project Settings"))
        {
            ImGui::TextUnformatted("Frame Pacing");
            ImGui::TextDisabled("Responsive has no intentional pacing wait.");
            ImGui::TextDisabled("Smooth Frametime is opt-in InterFrame pacing; VSync/VRR/tearing stay separate.");

            ImGui::Separator();
            ImGui::TextUnformatted("Presentation policy");
            ImGui::TextDisabled("TearingAllowed permits a native immediate/tearing path when supported; it never proves VRR active.");
            const Engine::PresentationPolicy presentationPolicies[] = { Engine::PresentationPolicy::Synchronized, Engine::PresentationPolicy::TearingAllowed };
            if (ImGui::BeginCombo("Presentation", Engine::ToString(m_ProjectPresentationPolicy)))
            {
                for (Engine::PresentationPolicy policy : presentationPolicies)
                {
                    const bool selected = policy == m_ProjectPresentationPolicy;
                    if (ImGui::Selectable(Engine::ToString(policy), selected)) m_ProjectPresentationPolicy = policy;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            const Engine::FramePacingMode modes[] = {
                Engine::FramePacingMode::Responsive,
                Engine::FramePacingMode::SmoothFrametime
            };
            if (ImGui::BeginCombo("Project default", Engine::ToString(m_ProjectFramePacingPolicy.Mode)))
            {
                for (Engine::FramePacingMode mode : modes)
                {
                    const bool selected = mode == m_ProjectFramePacingPolicy.Mode;
                    if (ImGui::Selectable(Engine::ToString(mode), selected))
                        m_ProjectFramePacingPolicy.Mode = mode;
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            double target = m_ProjectFramePacingPolicy.SmoothTargetFramesPerSecond;
            if (ImGui::InputDouble("Smooth target FPS", &target, 1.0, 10.0, "%.2f")
                && Engine::IsValidSmoothTargetFramesPerSecond(target))
            {
                m_ProjectFramePacingPolicy.SmoothTargetFramesPerSecond = target;
            }
            if (!Engine::IsValidSmoothTargetFramesPerSecond(target))
                ImGui::TextDisabled("Target must be finite and between %.0f and %.0f FPS; current saved value remains unchanged.",
                    Engine::kMinimumSmoothTargetFramesPerSecond, Engine::kMaximumSmoothTargetFramesPerSecond);

            ImGui::Separator();
            ImGui::TextUnformatted("Color pipeline");
            const Engine::RendererExposureMode exposureModes[] = {
                Engine::RendererExposureMode::ManualEV100,
                Engine::RendererExposureMode::CameraCalibration
            };
            if (ImGui::BeginCombo("Exposure mode", Engine::ToString(m_ProjectColorPipelineSettings.ExposureMode)))
            {
                for (Engine::RendererExposureMode mode : exposureModes)
                {
                    const bool selected = mode == m_ProjectColorPipelineSettings.ExposureMode;
                    if (ImGui::Selectable(Engine::ToString(mode), selected))
                    {
                        Engine::RendererColorPipelineSettings edited = m_ProjectColorPipelineSettings;
                        edited.ExposureMode = mode;
                        ApplyProjectColorPipelineSettings(edited);
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            double manualExposureEV100 = m_ProjectColorPipelineSettings.ManualExposureEV100;
            const bool manualExposureEdited = ImGui::InputDouble(
                "Manual EV100", &manualExposureEV100, 0.25, 1.0, "%.2f");
            Engine::RendererColorPipelineSettings editedManualExposure =
                m_ProjectColorPipelineSettings;
            editedManualExposure.ManualExposureEV100 = manualExposureEV100;
            HandleProjectColorPipelineInput(editedManualExposure, manualExposureEdited);
            const Engine::RendererColorPipelineSettings previewColorPipelineSettings { manualExposureEV100 };
            if (!Engine::IsValidRendererColorPipelineSettings(previewColorPipelineSettings))
                ImGui::TextDisabled("Manual EV100 must be finite and between %.0f and %.0f; current saved value remains unchanged.",
                    Engine::kMinimumManualExposureEV100, Engine::kMaximumManualExposureEV100);

            double apertureFNumber = m_ProjectColorPipelineSettings.CameraApertureFNumber;
            const bool apertureEdited = ImGui::InputDouble(
                "Aperture f-number", &apertureFNumber, 0.1, 1.0, "%.2f");
            Engine::RendererColorPipelineSettings editedAperture = m_ProjectColorPipelineSettings;
            editedAperture.CameraApertureFNumber = apertureFNumber;
            HandleProjectColorPipelineInput(editedAperture, apertureEdited);
            Engine::RendererColorPipelineSettings previewApertureSettings = m_ProjectColorPipelineSettings;
            previewApertureSettings.CameraApertureFNumber = apertureFNumber;
            if (!Engine::IsValidRendererColorPipelineSettings(previewApertureSettings))
                ImGui::TextDisabled("Aperture must be finite and between %.1f and %.0f; current saved value remains unchanged.",
                    Engine::kMinimumCameraApertureFNumber, Engine::kMaximumCameraApertureFNumber);

            double shutterSeconds = m_ProjectColorPipelineSettings.CameraShutterSeconds;
            const bool shutterEdited = ImGui::InputDouble(
                "Shutter seconds", &shutterSeconds, 0.001, 0.01, "%.5f");
            Engine::RendererColorPipelineSettings editedShutter = m_ProjectColorPipelineSettings;
            editedShutter.CameraShutterSeconds = shutterSeconds;
            HandleProjectColorPipelineInput(editedShutter, shutterEdited);
            Engine::RendererColorPipelineSettings previewShutterSettings = m_ProjectColorPipelineSettings;
            previewShutterSettings.CameraShutterSeconds = shutterSeconds;
            if (!Engine::IsValidRendererColorPipelineSettings(previewShutterSettings))
                ImGui::TextDisabled("Shutter must be finite and between %.5f and %.0f seconds; current saved value remains unchanged.",
                    Engine::kMinimumCameraShutterSeconds, Engine::kMaximumCameraShutterSeconds);

            double cameraISO = m_ProjectColorPipelineSettings.CameraISO;
            const bool cameraISOEdited = ImGui::InputDouble(
                "ISO", &cameraISO, 10.0, 100.0, "%.0f");
            Engine::RendererColorPipelineSettings editedCameraISO = m_ProjectColorPipelineSettings;
            editedCameraISO.CameraISO = cameraISO;
            HandleProjectColorPipelineInput(editedCameraISO, cameraISOEdited);
            Engine::RendererColorPipelineSettings previewISOSettings = m_ProjectColorPipelineSettings;
            previewISOSettings.CameraISO = cameraISO;
            if (!Engine::IsValidRendererColorPipelineSettings(previewISOSettings))
                ImGui::TextDisabled("ISO must be finite and between %.0f and %.0f; current saved value remains unchanged.",
                    Engine::kMinimumCameraISO, Engine::kMaximumCameraISO);

            double postToneMapSaturation = m_ProjectColorPipelineSettings.PostToneMapSaturation;
            const bool saturationEdited = ImGui::InputDouble(
                "Post-tone-map saturation", &postToneMapSaturation, 0.05, 0.25, "%.2f");
            Engine::RendererColorPipelineSettings editedSaturation = m_ProjectColorPipelineSettings;
            editedSaturation.PostToneMapSaturation = postToneMapSaturation;
            HandleProjectColorPipelineInput(editedSaturation, saturationEdited);
            Engine::RendererColorPipelineSettings previewSaturationSettings = m_ProjectColorPipelineSettings;
            previewSaturationSettings.PostToneMapSaturation = postToneMapSaturation;
            if (!Engine::IsValidRendererColorPipelineSettings(previewSaturationSettings))
                ImGui::TextDisabled("Saturation must be finite and between %.0f and %.0f; current saved value remains unchanged.",
                    Engine::kMinimumPostToneMapSaturation, Engine::kMaximumPostToneMapSaturation);

            double postToneMapContrast = m_ProjectColorPipelineSettings.PostToneMapContrast;
            const bool contrastEdited = ImGui::InputDouble(
                "Post-tone-map contrast", &postToneMapContrast, 0.05, 0.25, "%.2f");
            Engine::RendererColorPipelineSettings editedContrast = m_ProjectColorPipelineSettings;
            editedContrast.PostToneMapContrast = postToneMapContrast;
            HandleProjectColorPipelineInput(editedContrast, contrastEdited);
            Engine::RendererColorPipelineSettings previewContrastSettings = m_ProjectColorPipelineSettings;
            previewContrastSettings.PostToneMapContrast = postToneMapContrast;
            if (!Engine::IsValidRendererColorPipelineSettings(previewContrastSettings))
                ImGui::TextDisabled("Contrast must be finite and between %.0f and %.0f; current saved value remains unchanged.",
                    Engine::kMinimumPostToneMapContrast, Engine::kMaximumPostToneMapContrast);

            if (ImGui::Button("Save Project Settings"))
            {
                if (SaveProject())
                    m_ConsoleLines.emplace_back("Project settings saved");
            }
            PublishFramePacingPolicy();
            PublishPresentationPolicy();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Engine Settings"))
        {
            ImGui::TextUnformatted("Rendering");
            ImGui::Text("Active backend: %s", Engine::Renderer::GetActiveBackendName());
            DrawRendererBackendSelector();
            const Engine::RendererBuildInfo& buildInfo = Engine::Renderer::GetBuildInfo();
            if (!buildInfo.HasNVRHID3D12)
                ImGui::TextDisabled("Native viewport requires the Windows VS2022 build.");

            ImGui::Separator();
            ImGui::TextUnformatted("Viewport Navigation");
            if (ImGui::BeginCombo("Preset", ToEditorSettingsNavigationPreset(m_ViewportNavigationPreset)))
            {
                const ViewportNavigationPreset presets[] = {
                    ViewportNavigationPreset::Fusion,
                    ViewportNavigationPreset::Unreal
                };
                for (ViewportNavigationPreset preset : presets)
                {
                    const bool selected = preset == m_ViewportNavigationPreset;
                    if (ImGui::Selectable(ToEditorSettingsNavigationPreset(preset), selected))
                    {
                        const ViewportNavigationPreset previous = m_ViewportNavigationPreset;
                        m_ViewportNavigationPreset = preset;
                        ClearViewportNavigationInput();
                        EndViewportCursorCapture();
                        if (!SaveEditorSettings())
                            m_ViewportNavigationPreset = previous;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Global editor setting: %s", m_EditorSettingsPath.c_str());
            ImGui::TextDisabled("Fusion: cursor wheel zoom, MMB pan, Shift+MMB orbit. F sets a selected-origin pivot; Fit is deferred.");
            ImGui::EndMenu();
        }

        ImGui::EndMenu();
    }

    if (m_ProjectColorPipelineInteractionBefore && !ImGui::IsAnyItemActive())
    {
        if (m_ProjectColorPipelineInteractionChanged
            && m_ProjectColorPipelineInteractionBefore->ProjectColorPipelineSettings
                != m_ProjectColorPipelineSettings)
        {
            RecordHistory(std::move(m_ProjectColorPipelineInteractionLabel),
                std::move(*m_ProjectColorPipelineInteractionBefore));
        }
        m_ProjectColorPipelineInteractionBefore.reset();
        m_ProjectColorPipelineInteractionItemId = 0;
        m_ProjectColorPipelineInteractionChanged = false;
        m_ProjectColorPipelineInteractionLabel.clear();
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
    ImGuiID leftTopDock = 0;
    ImGuiID leftBottomDock = 0;
    ImGuiID bottomDock = 0;

    ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Left, 0.20f, &leftDock, &centerDock);
    ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Right, 0.31f, &rightDock, &centerDock);
    ImGui::DockBuilderSplitNode(centerDock, ImGuiDir_Down, 0.24f, &bottomDock, &centerDock);
    ImGui::DockBuilderSplitNode(leftDock, ImGuiDir_Down, 0.52f, &leftBottomDock, &leftTopDock);

    ImGui::DockBuilderDockWindow("Viewport", centerDock);
    ImGui::DockBuilderDockWindow("Scene Hierarchy", leftTopDock);
    ImGui::DockBuilderDockWindow("Content Browser", leftBottomDock);
    ImGui::DockBuilderDockWindow("Profiler", bottomDock);
    ImGui::DockBuilderDockWindow("Console", bottomDock);
    ImGui::DockBuilderDockWindow("Inspector", rightDock);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorLayer::DrawSceneHierarchyPanel()
{
    bool createEntityRequested = false;
    Engine::Entity deleteEntityRequested;
    ImGui::Begin("Scene Hierarchy");
    ImGui::TextUnformatted(m_ActiveScene.GetName().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("+"))
        CreateSceneEntity();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Create empty entity");
    ImGui::SameLine();
    const bool canDelete = m_ActiveScene.IsEntityValid(m_SelectedEntity)
        && m_SelectedEntity != m_ActiveScene.GetMainCameraEntity();
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::SmallButton("Delete"))
        DeleteSelectedEntity();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(canDelete ? "Delete selected entity" : "The primary camera cannot be deleted");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##HierarchyFilter", "Filter entities", m_HierarchyFilter.data(), m_HierarchyFilter.size());

    if (ImGui::TreeNodeEx("World", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (const Engine::SceneEntity& entity : m_ActiveScene.GetEntities())
        {
            if (m_HierarchyFilter[0] != '\0')
            {
                std::string entityName = entity.Name;
                std::string filter = m_HierarchyFilter.data();
                std::transform(entityName.begin(), entityName.end(), entityName.begin(), [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
                std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
                if (entityName.find(filter) == std::string::npos)
                    continue;
            }

            ImGui::PushID(static_cast<int>(entity.EntityHandle.Id));
            const bool selected = entity.EntityHandle == m_SelectedEntity;
            if (ImGui::Selectable(entity.Name.c_str(), selected))
            {
                m_SelectedEntity = entity.EntityHandle;
                RetargetFusionNavigationPivotToSelectedEntity();
            }
            if (ImGui::BeginPopupContextItem("EntityContext"))
            {
                if (ImGui::MenuItem("Create Empty"))
                    createEntityRequested = true;
                const bool isMainCamera = entity.EntityHandle == m_ActiveScene.GetMainCameraEntity();
                if (ImGui::MenuItem("Delete", nullptr, false, !isMainCamera))
                    deleteEntityRequested = entity.EntityHandle;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    if (createEntityRequested)
        CreateSceneEntity();
    if (deleteEntityRequested)
    {
        m_SelectedEntity = deleteEntityRequested;
        DeleteSelectedEntity();
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

    const HistoryState inspectorState = CaptureHistoryState();
    bool historyStateChanged = false;
    char entityName[128] = {};
    std::snprintf(entityName, sizeof(entityName), "%s", selectedEntity->Name.c_str());
    if (ImGui::InputText("Name", entityName, sizeof(entityName)))
    {
        selectedEntity->Name = entityName;
        historyStateChanged = true;
    }
    ImGui::Separator();
    ImGui::PushID("TransformComponent");
    ImGui::TextUnformatted("Transform");

    bool transformChanged = false;
    Engine::Math::DVec3 worldPosition;
    if (m_ActiveScene.TryGetEntityApproximateWorldPosition(selectedEntity->EntityHandle, worldPosition))
    {
        const Engine::Math::DVec3 displayedPosition = worldPosition;
        if (DrawDVec3Control("Position", worldPosition, 0.1f))
        {
            if (worldPosition.X != displayedPosition.X)
                transformChanged |= m_ActiveScene.SetEntityWorldPositionAxis(selectedEntity->EntityHandle, 0, worldPosition.X);
            if (worldPosition.Y != displayedPosition.Y)
                transformChanged |= m_ActiveScene.SetEntityWorldPositionAxis(selectedEntity->EntityHandle, 1, worldPosition.Y);
            if (worldPosition.Z != displayedPosition.Z)
                transformChanged |= m_ActiveScene.SetEntityWorldPositionAxis(selectedEntity->EntityHandle, 2, worldPosition.Z);
        }
    }
    transformChanged |= DrawVec3Control("Rotation", selectedEntity->Transform.RotationDegrees, 0.5f);
    if (!selectedEntity->Camera)
        transformChanged |= DrawVec3Control("Scale", selectedEntity->Transform.Scale, 0.05f, 0.01f, 100.0f);
    if (transformChanged && selectedEntity->EntityHandle == m_ActiveScene.GetMainCameraEntity())
        SyncEditorCameraStateFromMainCamera(true);
    else if (transformChanged)
        RetargetFusionNavigationPivotToSelectedEntity();
    historyStateChanged |= transformChanged;
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
        cameraChanged |= ImGui::ColorEdit3("Background Color", &camera.BackgroundColor.X);
        if (camera.Projection.FarClip <= camera.Projection.NearClip)
            camera.Projection.FarClip = camera.Projection.NearClip + 1.0f;
        if (camera.Primary)
            m_ActiveScene.SetMainCameraEntity(selectedEntity->EntityHandle);
        if (cameraChanged && selectedEntity->EntityHandle == m_ActiveScene.GetMainCameraEntity())
        {
            m_ActiveScene.SetMainCamera(camera);
            SyncEditorCameraStateFromMainCamera();
        }
        historyStateChanged |= cameraChanged;
        ImGui::PopID();
    }

    if (selectedEntity->Light)
    {
        ImGui::Separator();
        ImGui::PushID("LightComponent");
        ImGui::TextUnformatted("Light Component");
        const Engine::LightComponent originalLight = *selectedEntity->Light;
        Engine::LightComponent editedLight = originalLight;
        bool lightChanged = false;
        const char* lightTypeName = Engine::ToString(editedLight.Type);
        if (ImGui::BeginCombo("Type", lightTypeName))
        {
            const Engine::LightType lightTypes[] = {
                Engine::LightType::Directional,
                Engine::LightType::Point,
                Engine::LightType::Spot
            };
            for (Engine::LightType candidate : lightTypes)
            {
                const bool selected = editedLight.Type == candidate;
                if (ImGui::Selectable(Engine::ToString(candidate), selected))
                {
                    editedLight.Type = candidate;
                    editedLight.PhotometricUnit = Engine::GetLightPhotometricUnit(candidate);
                    lightChanged = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        lightChanged |= ImGui::ColorEdit3("Color", &editedLight.Color.X);
        const double photometricStep = editedLight.Type == Engine::LightType::Directional ? 100.0 : 10.0;
        if (ImGui::InputDouble(Engine::GetLightPhotometricControlLabel(editedLight.Type),
            &editedLight.PhotometricValue, photometricStep, photometricStep * 10.0, "%.2f"))
        {
            lightChanged = true;
        }
        lightChanged |= ImGui::DragFloat("Range", &editedLight.Range, 0.1f, 0.0f, 10000.0f);
        lightChanged |= ImGui::DragFloat("Inner Cone", &editedLight.InnerConeDegrees, 0.5f, 0.0f, 180.0f);
        lightChanged |= ImGui::DragFloat("Outer Cone", &editedLight.OuterConeDegrees, 0.5f, 0.0f, 180.0f);
        if (editedLight.OuterConeDegrees < editedLight.InnerConeDegrees)
            editedLight.OuterConeDegrees = editedLight.InnerConeDegrees;
        lightChanged |= ImGui::Checkbox("Casts Shadows", &editedLight.CastsShadows);

        bool lightEditAccepted = true;
        if (lightChanged)
        {
            lightEditAccepted = Engine::IsValidLightComponent(editedLight)
                && m_ActiveScene.AddLightComponent(selectedEntity->EntityHandle, editedLight);
            historyStateChanged |= lightEditAccepted;
        }
        const Engine::LightComponent& readoutLight = lightEditAccepted ? editedLight : originalLight;
        Engine::PhotometricLightReadout photometricReadout;
        if (Engine::TryBuildPhotometricLightReadout(
            readoutLight, m_ProjectColorPipelineSettings, photometricReadout))
        {
            ImGui::Text("%s: %.2f %s | effective EV100 %.2f | exposure scale %.6g",
                Engine::ToString(photometricReadout.Type), photometricReadout.Value,
                Engine::GetLightPhotometricUnitSymbol(photometricReadout.Unit),
                photometricReadout.EffectiveExposureEV100, photometricReadout.ExposureScale);
        }
        if (!lightEditAccepted)
        {
            ImGui::TextDisabled("Photometric value must be finite and within the %s range; prior valid state is retained.",
                Engine::GetLightPhotometricUnitSymbol(originalLight.PhotometricUnit));
        }
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
        bool meshChanged = false;
        if (ImGui::InputText("Mesh Name", meshName, sizeof(meshName)))
        {
            meshRenderer.MeshName = meshName;
            m_AssetRegistry.SetAssetName(meshRenderer.MeshAsset, meshName);
            meshChanged = true;
        }
        meshChanged |= DrawAssetHandleControl("Mesh Asset", meshRenderer.MeshAsset, m_AssetRegistry, Engine::AssetType::Mesh);
        meshChanged |= DrawAssetHandleControl("Material Asset", meshRenderer.MaterialAsset, m_AssetRegistry, Engine::AssetType::Material);
        if (ImGui::CollapsingHeader("Material Properties"))
            meshChanged |= DrawMaterialAssetControls(meshRenderer.MaterialAsset);
        meshChanged |= ImGui::Checkbox("Visible", &meshRenderer.Visible);
        meshChanged |= ImGui::Checkbox("Casts Shadows", &meshRenderer.CastsShadows);
        historyStateChanged |= meshChanged;
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("AddComponentPopup");
    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        if (!selectedEntity->Camera && ImGui::MenuItem("Camera"))
        {
            Engine::CameraComponent camera;
            camera.Primary = false;
            if (m_ActiveScene.AddCameraComponent(selectedEntity->EntityHandle, camera))
                historyStateChanged = true;
            else
                m_ConsoleLines.emplace_back("Camera component requires unit transform scale");
        }
        if (!selectedEntity->Light && ImGui::MenuItem("Light"))
        {
            m_ActiveScene.AddLightComponent(selectedEntity->EntityHandle);
            historyStateChanged = true;
        }
        if (!selectedEntity->MeshRenderer && ImGui::MenuItem("Mesh Renderer"))
        {
            m_ActiveScene.AddMeshRendererComponent(selectedEntity->EntityHandle);
            historyStateChanged = true;
        }
        if (selectedEntity->Camera && selectedEntity->Light && selectedEntity->MeshRenderer)
            ImGui::TextDisabled("All available components are attached");
        ImGui::EndPopup();
    }

    if (historyStateChanged)
        RecordHistory("Inspector edit", inspectorState);
    ImGui::End();
}

bool EditorLayer::DrawMaterialAssetControls(Engine::AssetHandle handle)
{
    Engine::MaterialAsset* material = m_MaterialLibrary.Get(handle);
    if (!material)
    {
        ImGui::TextDisabled("No loaded material asset for this handle");
        return false;
    }

    ImGui::PushID("MaterialAsset");
    bool materialChanged = false;
    char materialName[128] = {};
    std::snprintf(materialName, sizeof(materialName), "%s", material->Name.c_str());
    if (ImGui::InputText("Material Name", materialName, sizeof(materialName)))
    {
        material->Name = materialName;
        m_AssetRegistry.SetAssetName(handle, materialName);
        materialChanged = true;
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
            {
                material->ShadingModel = candidate;
                materialChanged = true;
            }
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
            {
                material->AlphaMode = candidate;
                materialChanged = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    materialChanged |= ImGui::Checkbox("Two Sided", &material->TwoSided);
    materialChanged |= ImGui::ColorEdit3("Base Color", &material->BaseColor.X);
    materialChanged |= ImGui::DragFloat("Metallic", &material->Metallic, 0.01f, 0.0f, 1.0f);
    materialChanged |= ImGui::DragFloat("Roughness", &material->Roughness, 0.01f, 0.0f, 1.0f);
    materialChanged |= ImGui::DragFloat("Normal Scale", &material->NormalScale, 0.01f, 0.0f, 4.0f);
    materialChanged |= ImGui::DragFloat("Occlusion Strength", &material->OcclusionStrength, 0.01f, 0.0f, 1.0f);
    materialChanged |= ImGui::ColorEdit3("Emissive Color", &material->EmissiveColor.X);
    materialChanged |= ImGui::DragFloat("Emissive Strength", &material->EmissiveStrength, 0.01f, 0.0f, 10000.0f);
    if (material->AlphaMode == Engine::MaterialAlphaMode::Mask)
        materialChanged |= ImGui::DragFloat("Alpha Cutoff", &material->AlphaCutoff, 0.01f, 0.0f, 1.0f);

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
    {
        materialChanged |= DrawAssetHandleControl(
            Engine::ToString(slot), material->GetTexture(slot), m_AssetRegistry, Engine::AssetType::Texture);
        const Engine::MaterialTextureSampler samplers[] = {
            Engine::MaterialTextureSampler::LinearWrap,
            Engine::MaterialTextureSampler::LinearClamp,
            Engine::MaterialTextureSampler::PointWrap,
            Engine::MaterialTextureSampler::PointClamp
        };
        const std::string samplerLabel = std::string(Engine::ToString(slot)) + " Sampling";
        if (ImGui::BeginCombo(samplerLabel.c_str(), Engine::ToString(material->GetSampler(slot))))
        {
            for (Engine::MaterialTextureSampler candidate : samplers)
            {
                const bool selected = material->GetSampler(slot) == candidate;
                if (ImGui::Selectable(Engine::ToString(candidate), selected))
                {
                    material->GetSampler(slot) = candidate;
                    materialChanged = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Callisto Controls");
    materialChanged |= ImGui::DragFloat("Diffuse Fresnel", &material->DiffuseFresnelIntensity, 0.01f, 0.0f, 256.0f);
    materialChanged |= ImGui::DragFloat("Retroreflection", &material->RetroreflectionIntensity, 0.01f, 0.0f, 256.0f);
    materialChanged |= ImGui::DragFloat("Diffuse Falloff", &material->DiffuseFresnelFalloff, 0.01f, 0.0f, 1.0f);
    materialChanged |= ImGui::DragFloat("Retroreflection Falloff", &material->RetroreflectionFalloff, 0.01f, 0.0f, 1.0f);
    materialChanged |= ImGui::DragFloat("Smooth Terminator", &material->SmoothTerminator, 0.01f, -1.0f, 1.0f);
    material->ClampValues();

    if (ImGui::Button("Save Material"))
        SaveMaterialAsset(handle);
    if (materialChanged)
        Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);
    ImGui::PopID();
    return materialChanged;
}

void EditorLayer::ApplyEditorCameraStateToScene()
{
    const Engine::Entity mainCameraEntity = m_ActiveScene.GetMainCameraEntity();
    m_ActiveScene.SetEntityWorldPosition(
        mainCameraEntity,
        { m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] });
    if (Engine::TransformComponent* cameraTransform = m_ActiveScene.TryGetTransform(mainCameraEntity))
    {
        cameraTransform->RotationDegrees = { m_CameraRotation[0], m_CameraRotation[1], m_CameraRotation[2] };
        cameraTransform->Scale = { 1.0f, 1.0f, 1.0f };
    }

    Engine::CameraComponent camera = m_ActiveScene.GetMainCamera();
    camera.Primary = true;
    camera.Projection = { m_CameraFovDegrees, m_CameraNearClip, m_CameraFarClip };

    m_ActiveScene.SetMainCamera(camera);
}

void EditorLayer::SyncEditorCameraStateFromMainCamera(bool discontinuousRelocation)
{
    const Engine::TransformComponent& cameraTransform = m_ActiveScene.GetMainCameraTransform();
    const Engine::CameraComponent& camera = m_ActiveScene.GetMainCamera();

    Engine::Math::DVec3 cameraPosition;
    if (!m_ActiveScene.TryGetEntityApproximateWorldPosition(m_ActiveScene.GetMainCameraEntity(), cameraPosition))
        cameraPosition = {};
    m_CameraPosition = { cameraPosition.X, cameraPosition.Y, cameraPosition.Z };
    m_CameraRotation = { cameraTransform.RotationDegrees.X, cameraTransform.RotationDegrees.Y, cameraTransform.RotationDegrees.Z };
    m_CameraFovDegrees = camera.Projection.VerticalFovDegrees;
    m_CameraNearClip = camera.Projection.NearClip;
    m_CameraFarClip = camera.Projection.FarClip;

    m_EditorCamera.SetPosition(cameraPosition);
    m_EditorCamera.SetRotationDegrees(cameraTransform.RotationDegrees);
    m_EditorCamera.SetProjection(camera.Projection);
    m_ViewportDiscontinuousRelocationPending |= discontinuousRelocation;
    Engine::Renderer::SetClearColor({ camera.BackgroundColor.X, camera.BackgroundColor.Y, camera.BackgroundColor.Z, 1.0f });
}

void EditorLayer::BeginViewportCursorCapture()
{
    if (m_CursorCaptured || m_CursorCapturePending)
        return;

    Engine::Window& window = Engine::Application::Get().GetWindow();
    window.GetCursorPosition(m_CursorRestoreX, m_CursorRestoreY);
    m_CursorCapturePending = true;
    m_CursorCaptureBaselineArmed = false;
    m_HasMousePosition = false;
    m_MouseDeltaX = 0.0f;
    m_MouseDeltaY = 0.0f;
}

void EditorLayer::ArmViewportCursorCapture()
{
    if (!m_CursorCapturePending)
        return;

    if (!m_LeftMouseDown && !m_RightMouseDown && !m_MiddleMouseDown)
    {
        m_CursorCapturePending = false;
        return;
    }

    Engine::Window& window = Engine::Application::Get().GetWindow();
    m_CursorCaptured = true;
    m_CursorCapturePending = false;
    m_CursorCaptureBaselineArmed = false;
    m_HasMousePosition = false;
    m_MouseDeltaX = 0.0f;
    m_MouseDeltaY = 0.0f;
    window.SetCursorMode(Engine::CursorMode::Disabled);
    window.GetCursorPosition(m_MouseX, m_MouseY);
    m_HasMousePosition = true;
    m_CursorCaptureBaselineArmed = true;
}

void EditorLayer::EndViewportCursorCapture()
{
    if (!m_CursorCaptured && !m_CursorCapturePending)
        return;

    if (m_CursorCaptured)
    {
        Engine::Window& window = Engine::Application::Get().GetWindow();
        window.SetCursorMode(Engine::CursorMode::Normal);
        window.SetCursorPosition(m_CursorRestoreX, m_CursorRestoreY);
    }
    m_CursorCaptured = false;
    m_CursorCapturePending = false;
    m_CursorCaptureBaselineArmed = false;
    m_HasMousePosition = false;
    m_MouseDeltaX = 0.0f;
    m_MouseDeltaY = 0.0f;
}

void EditorLayer::ClearViewportNavigationInput()
{
    m_LeftMouseDown = false;
    m_RightMouseDown = false;
    m_MiddleMouseDown = false;
    m_KeyDown.fill(false);
    m_MouseDeltaX = 0.0f;
    m_MouseDeltaY = 0.0f;
    m_MouseWheelDelta = 0.0f;
    m_HasMousePosition = false;
    m_ViewportNavigationFocusAvailable = false;
    m_ViewportFocusRequested = false;
}

bool EditorLayer::TryAcquireViewportNavigationFocus()
{
    if (m_ViewportNavigationInputEnabled)
        return true;
    if (!m_ViewportNavigationFocusAvailable)
        return false;

    // GLFW events arrive before the next ImGui frame. Latch a qualifying
    // hovered-viewport press/scroll so Update can consume it immediately;
    // DrawViewportPanel gives the viewport actual ImGui focus later this frame.
    m_ViewportFocusRequested = true;
    m_ViewportNavigationInputEnabled = true;
    return true;
}

bool EditorLayer::IsShiftNavigationModifierDown() const
{
    constexpr int leftShift = 340;
    constexpr int rightShift = 344;
    return m_KeyDown[static_cast<size_t>(leftShift)] || m_KeyDown[static_cast<size_t>(rightShift)];
}

void EditorLayer::BeginFusionOrbitPivot()
{
    if (m_FusionNavigationPivotValid)
        return;

    ResetFusionNavigationPivotFromSelectionOrScene();
}

void EditorLayer::SetFusionNavigationPivot(const Engine::Math::DVec3& pivot)
{
    m_FusionNavigationPivot = pivot;
    m_FusionNavigationPivotValid = true;
}

void EditorLayer::ResetFusionNavigationPivotFromSelectionOrScene()
{
    if (RetargetFusionNavigationPivotToSelectedEntity())
        return;

    bool hasVisibleMeshPosition = false;
    Engine::Math::DVec3 minimum {};
    Engine::Math::DVec3 maximum {};
    const Engine::Math::WorldGridPolicy& worldGridPolicy = m_ActiveScene.GetWorldGridPolicy();
    for (const Engine::SceneEntity& entity : m_ActiveScene.GetEntities())
    {
        if (!entity.MeshRenderer || !entity.MeshRenderer->Visible)
            continue;

        Engine::Math::DVec3 position;
        if (!entity.Transform.TryGetApproximateWorldPosition(worldGridPolicy, position))
            continue;

        if (!hasVisibleMeshPosition)
        {
            minimum = position;
            maximum = position;
            hasVisibleMeshPosition = true;
            continue;
        }

        minimum.X = std::min(minimum.X, position.X);
        minimum.Y = std::min(minimum.Y, position.Y);
        minimum.Z = std::min(minimum.Z, position.Z);
        maximum.X = std::max(maximum.X, position.X);
        maximum.Y = std::max(maximum.Y, position.Y);
        maximum.Z = std::max(maximum.Z, position.Z);
    }

    if (hasVisibleMeshPosition)
    {
        SetFusionNavigationPivot({
            minimum.X + (maximum.X - minimum.X) * 0.5,
            minimum.Y + (maximum.Y - minimum.Y) * 0.5,
            minimum.Z + (maximum.Z - minimum.Z) * 0.5
        });
        return;
    }

    const float yaw = Engine::Math::DegreesToRadians(m_CameraRotation[1]);
    const float pitch = Engine::Math::DegreesToRadians(m_CameraRotation[0]);
    const Engine::Math::DVec3 forward {
        std::sin(yaw) * std::cos(pitch),
        -std::sin(pitch),
        std::cos(yaw) * std::cos(pitch)
    };
    constexpr double fallbackDistance = 3.35;
    SetFusionNavigationPivot({
        m_CameraPosition[0] + forward.X * fallbackDistance,
        m_CameraPosition[1] + forward.Y * fallbackDistance,
        m_CameraPosition[2] + forward.Z * fallbackDistance
    });
}

bool EditorLayer::RetargetFusionNavigationPivotToSelectedEntity()
{
    if (!m_ActiveScene.IsEntityValid(m_SelectedEntity)
        || m_SelectedEntity == m_ActiveScene.GetMainCameraEntity())
        return false;

    Engine::Math::DVec3 selectedPosition;
    if (!m_ActiveScene.TryGetEntityApproximateWorldPosition(m_SelectedEntity, selectedPosition))
        return false;

    SetFusionNavigationPivot(selectedPosition);
    return true;
}

bool EditorLayer::SaveEditorSettings()
{
    const EditorSettings settings { m_ViewportNavigationPreset };
    if (WriteEditorSettings(m_EditorSettingsPath, settings))
        return true;

    Engine::Log::Error("Editor settings save failed: ", m_EditorSettingsPath);
    m_ConsoleLines.emplace_back("Engine settings save failed: " + m_EditorSettingsPath);
    return false;
}

void EditorLayer::LoadEditorSettings()
{
    m_ViewportNavigationPreset = ViewportNavigationPreset::Fusion;
    std::error_code error;
    if (!std::filesystem::exists(m_EditorSettingsPath, error))
    {
        Engine::Log::Info("Editor settings missing; using Fusion navigation default: ", m_EditorSettingsPath);
        return;
    }
    if (error)
    {
        Engine::Log::Warn("Editor settings path unavailable; using Fusion navigation default: ", error.message());
        return;
    }

    EditorSettings settings;
    if (!ReadEditorSettings(m_EditorSettingsPath, settings))
    {
        Engine::Log::Warn("Editor settings read rejected; using Fusion navigation default: ", m_EditorSettingsPath);
        return;
    }

    m_ViewportNavigationPreset = settings.ViewportNavigation;
    Engine::Log::Info("Editor settings loaded: viewportNavigation=", ToEditorSettingsNavigationPreset(m_ViewportNavigationPreset));
}

void EditorLayer::FocusSelectedEntity()
{
    Engine::Math::DVec3 focusPosition;
    if (!m_ActiveScene.TryGetEntityApproximateWorldPosition(m_SelectedEntity, focusPosition))
        return;

    const float yaw = Engine::Math::DegreesToRadians(m_CameraRotation[1]);
    const float pitch = Engine::Math::DegreesToRadians(m_CameraRotation[0]);
    const Engine::Math::DVec3 forward {
        std::sin(yaw) * std::cos(pitch),
        -std::sin(pitch),
        std::cos(yaw) * std::cos(pitch)
    };
    constexpr double focusDistance = 3.35;
    m_CameraPosition = {
        focusPosition.X - forward.X * focusDistance,
        focusPosition.Y - forward.Y * focusDistance,
        focusPosition.Z - forward.Z * focusDistance
    };
    SetFusionNavigationPivot(focusPosition);
    m_EditorCamera.SetPosition({ m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] });
    ApplyEditorCameraStateToScene();
    m_ViewportDiscontinuousRelocationPending = true;
}

void EditorLayer::UpdateViewportNavigation(Engine::Timestep timestep)
{
    if (!m_ViewportNavigationInputEnabled)
    {
        EndViewportCursorCapture();
        m_MouseWheelDelta = 0.0f;
        return;
    }

    ArmViewportCursorCapture();

    const auto keyDown = [this](int key)
    {
        return key >= 0 && key < static_cast<int>(m_KeyDown.size()) && m_KeyDown[static_cast<size_t>(key)];
    };
    constexpr int keyLeft = 263;
    constexpr int keyRight = 262;
    constexpr int keyDownArrow = 264;
    constexpr int keyUp = 265;
    constexpr int keyPageUp = 266;
    constexpr int keyPageDown = 267;

    const float yaw = Engine::Math::DegreesToRadians(m_CameraRotation[1]);
    const float pitch = Engine::Math::DegreesToRadians(m_CameraRotation[0]);
    const Engine::Math::DVec3 forward {
        std::sin(yaw) * std::cos(pitch),
        -std::sin(pitch),
        std::cos(yaw) * std::cos(pitch)
    };
    const Engine::Math::DVec3 right { std::cos(yaw), 0.0, -std::sin(yaw) };
    const Engine::Math::DVec3 up {
        forward.Y * right.Z - forward.Z * right.Y,
        forward.Z * right.X - forward.X * right.Z,
        forward.X * right.Y - forward.Y * right.X
    };
    const auto dot = [] (const Engine::Math::DVec3& lhs, const Engine::Math::DVec3& rhs)
    {
        return lhs.X * rhs.X + lhs.Y * rhs.Y + lhs.Z * rhs.Z;
    };
    bool changed = false;

    if (m_ViewportNavigationPreset == ViewportNavigationPreset::Unreal && m_RightMouseDown && m_MouseWheelDelta != 0.0f)
    {
        m_ViewportNavigationSpeed = std::clamp(m_ViewportNavigationSpeed + m_MouseWheelDelta, 0.25f, 128.0f);
        changed = true;
    }
    else if (m_MouseWheelDelta != 0.0f)
    {
        if (m_ViewportNavigationPreset == ViewportNavigationPreset::Unreal)
        {
            const double distance = static_cast<double>(m_MouseWheelDelta) * m_ViewportNavigationSpeed * 0.25;
            m_CameraPosition[0] += forward.X * distance;
            m_CameraPosition[1] += forward.Y * distance;
            m_CameraPosition[2] += forward.Z * distance;
            changed = true;
        }
        else
        {
        BeginFusionOrbitPivot();
        const Engine::Math::DVec3 cameraPosition { m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] };
        const Engine::Math::DVec3 pivotOffset {
            m_FusionNavigationPivot.X - cameraPosition.X,
            m_FusionNavigationPivot.Y - cameraPosition.Y,
            m_FusionNavigationPivot.Z - cameraPosition.Z
        };
        const double fallbackDepth = std::sqrt(dot(pivotOffset, pivotOffset));
        const double navigationDepth = std::max(
            static_cast<double>(m_EditorCamera.GetProjection().NearClip) * 2.0,
            dot(pivotOffset, forward) > 0.0 ? dot(pivotOffset, forward) : fallbackDepth);
        const double cursorX = m_CursorCaptured ? m_CursorRestoreX : m_MouseX;
        const double cursorY = m_CursorCaptured ? m_CursorRestoreY : m_MouseY;
        const double normalizedX = ((cursorX - m_ViewportImageX) / m_ViewportImageWidth - 0.5) * 2.0;
        const double normalizedY = (0.5 - (cursorY - m_ViewportImageY) / m_ViewportImageHeight) * 2.0;
        const double tangent = std::tan(Engine::Math::DegreesToRadians(m_EditorCamera.GetProjection().VerticalFovDegrees) * 0.5);
        const double aspect = std::max(0.001, static_cast<double>(m_EditorCamera.GetAspectRatio()));
        Engine::Math::DVec3 ray {
            forward.X + right.X * normalizedX * tangent * aspect + up.X * normalizedY * tangent,
            forward.Y + right.Y * normalizedX * tangent * aspect + up.Y * normalizedY * tangent,
            forward.Z + right.Z * normalizedX * tangent * aspect + up.Z * normalizedY * tangent
        };
        const double rayLength = std::sqrt(dot(ray, ray));
        if (rayLength > 0.0)
        {
            ray.X /= rayLength;
            ray.Y /= rayLength;
            ray.Z /= rayLength;
            const double rayForward = std::max(0.001, dot(ray, forward));
            const double rayDistance = navigationDepth / rayForward;
            const Engine::Math::DVec3 anchor {
                cameraPosition.X + ray.X * rayDistance,
                cameraPosition.Y + ray.Y * rayDistance,
                cameraPosition.Z + ray.Z * rayDistance
            };
            const double requestedScale = std::pow(0.85, static_cast<double>(m_MouseWheelDelta));
            const double minimumScale = std::min(1.0, static_cast<double>(m_EditorCamera.GetProjection().NearClip) * 2.0 / navigationDepth);
            const double scale = std::clamp(requestedScale, minimumScale, 32.0);
            m_CameraPosition = {
                anchor.X + (cameraPosition.X - anchor.X) * scale,
                anchor.Y + (cameraPosition.Y - anchor.Y) * scale,
                anchor.Z + (cameraPosition.Z - anchor.Z) * scale
            };
            changed = true;
        }
        }
    }
    m_MouseWheelDelta = 0.0f;

    if (m_ViewportNavigationPreset == ViewportNavigationPreset::Fusion && m_MiddleMouseDown)
    {
        if (IsShiftNavigationModifierDown())
        {
            BeginFusionOrbitPivot();
            if (m_MouseDeltaX != 0.0f || m_MouseDeltaY != 0.0f)
            {
                const Engine::Math::DVec3 offset {
                    m_CameraPosition[0] - m_FusionNavigationPivot.X,
                    m_CameraPosition[1] - m_FusionNavigationPivot.Y,
                    m_CameraPosition[2] - m_FusionNavigationPivot.Z
                };
                const double forwardComponent = dot(offset, forward);
                const double rightComponent = dot(offset, right);
                const double upComponent = dot(offset, up);
                m_CameraRotation[0] = std::clamp(m_CameraRotation[0] + m_MouseDeltaY * 0.15f, -89.0f, 89.0f);
                m_CameraRotation[1] += m_MouseDeltaX * 0.15f;
                const float orbitYaw = Engine::Math::DegreesToRadians(m_CameraRotation[1]);
                const float orbitPitch = Engine::Math::DegreesToRadians(m_CameraRotation[0]);
                const Engine::Math::DVec3 orbitForward {
                    std::sin(orbitYaw) * std::cos(orbitPitch),
                    -std::sin(orbitPitch),
                    std::cos(orbitYaw) * std::cos(orbitPitch)
                };
                const Engine::Math::DVec3 orbitRight { std::cos(orbitYaw), 0.0, -std::sin(orbitYaw) };
                const Engine::Math::DVec3 orbitUp {
                    orbitForward.Y * orbitRight.Z - orbitForward.Z * orbitRight.Y,
                    orbitForward.Z * orbitRight.X - orbitForward.X * orbitRight.Z,
                    orbitForward.X * orbitRight.Y - orbitForward.Y * orbitRight.X
                };
                m_CameraPosition = {
                    m_FusionNavigationPivot.X + orbitForward.X * forwardComponent + orbitRight.X * rightComponent + orbitUp.X * upComponent,
                    m_FusionNavigationPivot.Y + orbitForward.Y * forwardComponent + orbitRight.Y * rightComponent + orbitUp.Y * upComponent,
                    m_FusionNavigationPivot.Z + orbitForward.Z * forwardComponent + orbitRight.Z * rightComponent + orbitUp.Z * upComponent
                };
                changed = true;
            }
        }
        else
        {
            BeginFusionOrbitPivot();
            const Engine::Math::DVec3 pivotOffset {
                m_FusionNavigationPivot.X - m_CameraPosition[0],
                m_FusionNavigationPivot.Y - m_CameraPosition[1],
                m_FusionNavigationPivot.Z - m_CameraPosition[2]
            };
            const double navigationDepth = std::max(0.001, dot(pivotOffset, forward));
            const double worldUnitsPerPixel = 2.0 * navigationDepth
                * std::tan(Engine::Math::DegreesToRadians(m_EditorCamera.GetProjection().VerticalFovDegrees) * 0.5)
                / std::max(1.0f, m_ViewportImageHeight);
            const Engine::Math::DVec3 translation {
                -right.X * m_MouseDeltaX * worldUnitsPerPixel + up.X * m_MouseDeltaY * worldUnitsPerPixel,
                -right.Y * m_MouseDeltaX * worldUnitsPerPixel + up.Y * m_MouseDeltaY * worldUnitsPerPixel,
                -right.Z * m_MouseDeltaX * worldUnitsPerPixel + up.Z * m_MouseDeltaY * worldUnitsPerPixel
            };
            m_CameraPosition[0] += translation.X;
            m_CameraPosition[1] += translation.Y;
            m_CameraPosition[2] += translation.Z;
            m_FusionNavigationPivot.X += translation.X;
            m_FusionNavigationPivot.Y += translation.Y;
            m_FusionNavigationPivot.Z += translation.Z;
            changed |= m_MouseDeltaX != 0.0f || m_MouseDeltaY != 0.0f;
        }
    }
    else if (m_ViewportNavigationPreset == ViewportNavigationPreset::Unreal && ((m_LeftMouseDown && m_RightMouseDown) || m_MiddleMouseDown))
    {
        const double panScale = static_cast<double>(m_ViewportNavigationSpeed) * 0.01;
        m_CameraPosition[0] += right.X * m_MouseDeltaX * panScale;
        m_CameraPosition[1] -= m_MouseDeltaY * panScale;
        m_CameraPosition[2] += right.Z * m_MouseDeltaX * panScale;
        changed |= m_MouseDeltaX != 0.0f || m_MouseDeltaY != 0.0f;
    }
    else if (m_ViewportNavigationPreset == ViewportNavigationPreset::Unreal && m_LeftMouseDown)
    {
        m_CameraRotation[1] += m_MouseDeltaX * 0.15f;
        const double distance = static_cast<double>(-m_MouseDeltaY) * m_ViewportNavigationSpeed * 0.025;
        m_CameraPosition[0] += forward.X * distance;
        m_CameraPosition[1] += forward.Y * distance;
        m_CameraPosition[2] += forward.Z * distance;
        changed |= m_MouseDeltaX != 0.0f || m_MouseDeltaY != 0.0f;
    }
    else if (m_ViewportNavigationPreset == ViewportNavigationPreset::Unreal && m_RightMouseDown)
    {
        m_CameraRotation[0] = std::clamp(m_CameraRotation[0] + m_MouseDeltaY * 0.15f, -89.0f, 89.0f);
        m_CameraRotation[1] += m_MouseDeltaX * 0.15f;
        changed |= m_MouseDeltaX != 0.0f || m_MouseDeltaY != 0.0f;
    }
    m_MouseDeltaX = 0.0f;
    m_MouseDeltaY = 0.0f;

    if (m_ViewportNavigationPreset == ViewportNavigationPreset::Unreal && m_RightMouseDown)
    {
        const double distance = static_cast<double>(m_ViewportNavigationSpeed * timestep.GetSeconds());
        const auto translate = [&] (const Engine::Math::DVec3& direction)
        {
            m_CameraPosition[0] += direction.X * distance;
            m_CameraPosition[1] += direction.Y * distance;
            m_CameraPosition[2] += direction.Z * distance;
            changed = true;
        };
        if (keyDown('W') || keyDown(keyUp)) translate(forward);
        if (keyDown('S') || keyDown(keyDownArrow)) translate({ -forward.X, -forward.Y, -forward.Z });
        if (keyDown('A') || keyDown(keyLeft)) translate({ -right.X, -right.Y, -right.Z });
        if (keyDown('D') || keyDown(keyRight)) translate(right);
        if (keyDown('E') || keyDown(keyPageUp)) translate({ 0.0, 1.0, 0.0 });
        if (keyDown('Q') || keyDown(keyPageDown)) translate({ 0.0, -1.0, 0.0 });
    }

    if (!changed)
        return;

    ++m_ViewportNavigationMutationFrames;

    m_EditorCamera.SetPosition({ m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] });
    m_EditorCamera.SetRotationDegrees({ m_CameraRotation[0], m_CameraRotation[1], m_CameraRotation[2] });
    ApplyEditorCameraStateToScene();
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
        EndViewportCursorCapture();
        ClearViewportNavigationInput();
        Engine::Renderer::SetViewportRect({});
        ImGui::End();
        return;
    }

    const auto viewportWidth = static_cast<Engine::u32>(size.x);
    const auto viewportHeight = static_cast<Engine::u32>(size.y);
    m_EditorCamera.SetViewportSize(viewportWidth, viewportHeight);

    const bool hasNativeViewportTexture = Engine::Renderer::PrepareViewportTexture(viewportWidth, viewportHeight);
    const Engine::u64 viewportTextureId = Engine::Renderer::GetViewportTextureId();

    if (hasNativeViewportTexture && viewportTextureId != 0)
    {
        ImGui::Image(static_cast<ImTextureID>(viewportTextureId), size);
        Engine::Renderer::MarkViewportTextureQueued(viewportTextureId);
    }
    else
        ImGui::InvisibleButton("ViewportCanvas", size);

    m_ViewportHovered = ImGui::IsItemHovered();
    if (m_ViewportFocusRequested || ImGui::IsItemClicked(ImGuiMouseButton_Left)
        || ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsItemClicked(ImGuiMouseButton_Middle))
    {
        ImGui::SetWindowFocus();
        m_ViewportFocusRequested = false;
    }
    m_ViewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const ImGuiIO& io = ImGui::GetIO();
    m_ViewportNavigationFocusAvailable = m_WindowFocused && m_ViewportHovered
        && !io.WantTextInput && !ImGui::IsAnyItemActive();
    m_ViewportNavigationInputEnabled = m_WindowFocused
        && !io.WantTextInput
        && (m_CursorCaptured || (m_ViewportNavigationFocusAvailable && m_ViewportFocused));

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    m_ViewportImageX = min.x;
    m_ViewportImageY = min.y;
    m_ViewportImageWidth = std::max(1.0f, max.x - min.x);
    m_ViewportImageHeight = std::max(1.0f, max.y - min.y);
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
        ? std::string("Active backend: ") + Engine::Renderer::GetActiveBackendName() + "; native Scene mesh pass"
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
    if (m_RendererCapabilitySmokeRequested && !m_RendererCapabilitySmokeComplete)
        ImGui::SetNextWindowFocus();
    ImGui::Begin("Profiler");
    const Engine::RendererFrameTiming& timing = Engine::Renderer::GetLastCompletedFrameTiming();

    ImGui::Text("Frame: %u", m_FrameCounter);
    ImGui::Text("Last frame: %.3f ms", m_LastFrameMs);
    if (m_FrameTimeHistoryCount > 0)
    {
        const float graphMaximum = std::max(16.667f,
            *std::max_element(m_FrameTimeHistory.begin(), m_FrameTimeHistory.end()) * 1.1f);
        const int graphOffset = m_FrameTimeHistoryCount == m_FrameTimeHistory.size()
            ? static_cast<int>(m_FrameTimeHistoryOffset) : 0;
        ImGui::TextUnformatted("Renderer FrameStart-to-FrameStart cadence (ms)");
        const ImVec2 graphPosition = ImGui::GetCursorScreenPos();
        const ImVec2 graphSize(ImGui::GetContentRegionAvail().x, 90.0f);
        ImGui::PlotLines("##FrameTimeHistory", m_FrameTimeHistory.data(),
            static_cast<int>(m_FrameTimeHistoryCount), graphOffset, nullptr,
            0.0f, graphMaximum, graphSize);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 gridColor = IM_COL32(255, 255, 255, 45);
        for (int step = 0; step <= 2; ++step)
        {
            const float fraction = static_cast<float>(step) * 0.5f;
            const float y = graphPosition.y + graphSize.y * fraction;
            drawList->AddLine({ graphPosition.x, y }, { graphPosition.x + graphSize.x, y }, gridColor);
            char label[24];
            std::snprintf(label, sizeof(label), "%.1f ms", graphMaximum * (1.0f - fraction));
            drawList->AddText({ graphPosition.x + 5.0f, y + (step == 2 ? -ImGui::GetTextLineHeight() : 2.0f) },
                IM_COL32(220, 220, 220, 220), label);
        }
    }
    ImGui::Text("Workers: %u", Engine::JobSystem::Get().GetWorkerCount());
    ImGui::Separator();
    ImGui::Text("Renderer CPU: %.3f ms", timing.CpuMilliseconds);
    ImGui::Text("Renderer timing frame: %llu", static_cast<unsigned long long>(timing.FrameIndex));
    ImGui::Text("GPU timestamps: %s", Engine::ToString(timing.GpuStatus));
    if (timing.GpuStatus == Engine::RendererTimingStatus::Pending)
        ImGui::TextDisabled("GPU timing for this exact frame has not retired yet.");
    else if (timing.GpuStatus == Engine::RendererTimingStatus::Ready)
        ImGui::Text("Renderer GPU: %.3f ms", timing.GpuMilliseconds);

    ImGui::Separator();
    ImGui::TextUnformatted("Frame pacing policy");
    bool framePacingChanged = false;
    const Engine::FramePacingOverride runtimeOptions[] = {
        Engine::FramePacingOverride::InheritProject,
        Engine::FramePacingOverride::Responsive,
        Engine::FramePacingOverride::SmoothFrametime
    };
    if (ImGui::BeginCombo("Runtime game setting", Engine::ToString(m_GameFramePacingSettings.GetRuntimeOverride())))
    {
        for (Engine::FramePacingOverride option : runtimeOptions)
        {
            const bool selected = option == m_GameFramePacingSettings.GetRuntimeOverride();
            if (ImGui::Selectable(Engine::ToString(option), selected))
            {
                if (m_GameFramePacingSettings.SetRuntimeOverride(option, m_ProjectFramePacingPolicy))
                    framePacingChanged = true;
                else
                    m_ConsoleLines.emplace_back("Runtime Smooth Frametime override rejected: invalid project target");
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::TextDisabled("Smooth uses InterFrame after prior Present before next input.");
    ImGui::TextDisabled("SubmissionGate is exercised only by explicit benchmark/smoke CLI conditions.");

    if (framePacingChanged)
        PublishFramePacingPolicy();

    const Engine::ResolvedFramePacingPolicy framePacing =
        m_GameFramePacingSettings.Resolve(m_ProjectFramePacingPolicy);
    const Engine::RendererPresentationPolicyDiagnostics presentation = Engine::Renderer::GetPresentationPolicyDiagnostics();
    const std::string framePacingDescription = Engine::DescribeFramePacingPolicy(framePacing);
    ImGui::Text("Project: %s", Engine::ToString(framePacing.ProjectMode));
    ImGui::Text("Runtime override: %s", Engine::ToString(framePacing.RuntimeOverride));
    ImGui::Text("Effective: %s", Engine::ToString(framePacing.EffectiveMode));
    if (framePacing.SmoothTargetFramesPerSecond)
        ImGui::Text("Requested target: %.2f FPS", *framePacing.SmoothTargetFramesPerSecond);
    if (timing.StartToStartMilliseconds > 0.0)
        ImGui::Text("Observed engine cadence: %.2f FPS (FrameStart-to-FrameStart)", 1000.0 / timing.StartToStartMilliseconds);
    else
        ImGui::TextDisabled("Observed engine cadence: awaiting a consecutive frame start");
    const std::string cadencePreviousFrame = timing.CadencePreviousFrameIndex
        ? std::to_string(*timing.CadencePreviousFrameIndex) : "unavailable";
    const std::string limitingSourceFrame = timing.EffectiveLimitingSourceFrameIndex
        ? std::to_string(*timing.EffectiveLimitingSourceFrameIndex) : "unavailable";
    ImGui::Text("Cadence terminal frame: %llu; previous: %s", static_cast<unsigned long long>(timing.FrameIndex),
        cadencePreviousFrame.c_str());
    ImGui::Text("Effective limiter: %s; source frame: %s (not display cadence)", Engine::ToString(timing.EffectiveLimitingSource),
        limitingSourceFrame.c_str());
    ImGui::TextDisabled("Evidence-qualified; synchronized presentation does not infer monitor refresh or display cadence.");
    ImGui::Text("Presentation project: %s; requested: %s; actual: %s", Engine::ToString(m_ProjectPresentationPolicy),
        Engine::ToString(presentation.Requested), Engine::ToString(presentation.Actual));
    if (m_RuntimePresentationPolicyOverride)
        ImGui::Text("Presentation runtime override: %s (not serialized)", Engine::ToString(*m_RuntimePresentationPolicyOverride));
    ImGui::Text("Capability: %s; fallback: %s; generation: %llu", presentation.Capability.c_str(), presentation.FallbackReason.c_str(),
        static_cast<unsigned long long>(presentation.SwapchainGeneration));
    ImGui::TextDisabled("VRR-active, monitor state, display cadence, and input-to-display remain unavailable.");
    if (timing.InputLatencySourceFrameIndex && timing.InputToSimulationMilliseconds
        && timing.InputToRenderSubmissionMilliseconds && timing.InputToPresentMilliseconds)
    {
        ImGui::Text("Input sample frame: %llu", static_cast<unsigned long long>(*timing.InputLatencySourceFrameIndex));
        ImGui::Text("Input to simulation / submit / Present: %.3f / %.3f / %.3f ms",
            *timing.InputToSimulationMilliseconds, *timing.InputToRenderSubmissionMilliseconds,
            *timing.InputToPresentMilliseconds);
    }
    else
        ImGui::TextDisabled("Input stage intervals: unavailable");
    ImGui::TextDisabled("Input to display / click-to-photon: unavailable");
    ImGui::TextWrapped("%s", framePacingDescription.c_str());

    const Engine::RendererPresentationTiming& presentationTiming = timing.Presentation;
    if (presentationTiming.UsesWaitableSwapchain)
    {
        ImGui::Text("Swapchain pacing: waitable, queue depth %u", presentationTiming.MaximumFrameLatency);
        ImGui::Text("Frame-latency wait: %.3f ms", presentationTiming.FrameLatencyWaitMilliseconds);
        ImGui::Text("Present: %.3f ms (%s)", presentationTiming.PresentMilliseconds, presentationTiming.PresentSucceeded ? "ok" : "pending");
    }
    else
    {
        ImGui::TextDisabled("Swapchain pacing: unavailable on this backend");
    }

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
    if (m_RendererCapabilitySmokeRequested && !m_RendererCapabilitySmokeComplete)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader("Renderer Capabilities", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const Engine::RHI::DeviceCapabilities* capabilities = Engine::Renderer::GetDeviceCapabilities();
        if (!capabilities)
        {
            ImGui::TextDisabled("No native renderer capability report is available");
        }
        else
        {
            const Engine::RendererCapabilityReasonDiagnostics reasonDiagnostics =
                Engine::BuildRendererCapabilityReasonDiagnostics(*capabilities);
            ImGui::Text("Profile: %s", capabilities->ProfileName.c_str());
            ImGui::Text("Qualification: %s", Engine::RHI::ToString(capabilities->Qualification));
            ImGui::Text("Backend: %s", Engine::RHI::ToString(capabilities->ActiveBackend));
            ImGui::Text("Adapter: %s", capabilities->Identity.Name.c_str());
            ImGui::Text("Type: %s", Engine::RHI::ToString(capabilities->Identity.Type));
            if (!capabilities->Identity.StableId.empty())
                ImGui::TextWrapped("Stable ID: %s", capabilities->Identity.StableId.c_str());
            if (!capabilities->Identity.DriverVersion.empty())
                ImGui::Text("Driver/runtime: %s", capabilities->Identity.DriverVersion.c_str());
            ImGui::Text("Vendor/device: %04X:%04X", capabilities->Identity.VendorId, capabilities->Identity.DeviceId);
            ImGui::Text("Dedicated video memory: %.1f MiB",
                static_cast<double>(capabilities->Identity.DedicatedVideoMemoryBytes) / (1024.0 * 1024.0));

            if (ImGui::BeginTable("CapabilityQueues", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Queue");
                ImGui::TableSetupColumn("Available");
                ImGui::TableSetupColumn("Dedicated");
                ImGui::TableHeadersRow();
                const auto drawQueue = [](const char* name, bool available, const char* dedicated)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(name);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(available ? "yes" : "no");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(dedicated);
                };
                drawQueue("Graphics", capabilities->Queues.Graphics, "n/a");
                drawQueue("Compute", capabilities->Queues.Compute, capabilities->Queues.DedicatedCompute ? "yes" : "no");
                drawQueue("Copy", capabilities->Queues.Copy, capabilities->Queues.DedicatedCopy ? "yes" : "no");
                drawQueue("Present", capabilities->Queues.Present, "n/a");
                ImGui::EndTable();
            }

            if (ImGui::TreeNode("Feature lifecycle"))
            {
                if (ImGui::BeginTable("CapabilityFeatures", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Feature");
                    ImGui::TableSetupColumn("Advertised");
                    ImGui::TableSetupColumn("Enabled");
                    ImGui::TableSetupColumn("Implemented");
                    ImGui::TableSetupColumn("Exercised");
                    ImGui::TableSetupColumn("Detail");
                    ImGui::TableHeadersRow();
                    for (Engine::u32 index = 0; index < static_cast<Engine::u32>(Engine::RHI::DeviceFeature::Count); ++index)
                    {
                        const auto feature = static_cast<Engine::RHI::DeviceFeature>(index);
                        const Engine::RHI::CapabilityState& state = capabilities->GetFeature(feature);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(Engine::RHI::ToString(feature));
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(state.Advertised ? "yes" : "no");
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(state.Enabled ? "yes" : "no");
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(state.Implemented ? "yes" : "no");
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(state.Exercised ? "yes" : "no");
                        ImGui::TableSetColumnIndex(5);
                        ImGui::TextWrapped("%s", state.Detail.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Consumer capability groups"))
            {
                if (ImGui::BeginTable("CapabilityGroups", 7, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Group");
                    ImGui::TableSetupColumn("Profile");
                    ImGui::TableSetupColumn("Preferred");
                    ImGui::TableSetupColumn("Selected");
                    ImGui::TableSetupColumn("Implemented");
                    ImGui::TableSetupColumn("Exercised");
                    ImGui::TableSetupColumn("Qualification");
                    ImGui::TableHeadersRow();
                    for (const Engine::RHI::CapabilityGroupState& group : capabilities->CapabilityGroups)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(Engine::RHI::ToString(group.Group));
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(group.ProfileName.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(Engine::RHI::ToString(group.PreferredPath));
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(Engine::RHI::ToString(group.SelectedPath));
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(group.Implemented ? "yes" : "no");
                        ImGui::TableSetColumnIndex(5);
                        ImGui::TextUnformatted(group.Exercised ? "yes" : "no");
                        ImGui::TableSetColumnIndex(6);
                        ImGui::TextUnformatted(Engine::RHI::ToString(group.Qualification));
                    }
                    ImGui::EndTable();
                }
                for (const Engine::RHI::CapabilityGroupState& group : capabilities->CapabilityGroups)
                {
                    for (const std::string& fallback : group.Fallbacks)
                        ImGui::BulletText("%s fallback: %s", Engine::RHI::ToString(group.Group), fallback.c_str());
                    for (const std::string& reason : group.UnsupportedReasons)
                        ImGui::BulletText("%s unavailable path: %s", Engine::RHI::ToString(group.Group), reason.c_str());
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Queried formats"))
            {
                for (const Engine::RHI::FormatCapability& format : capabilities->Formats)
                {
                    const std::string usages = Engine::RHI::FormatUsagesToString(format.Usages);
                    ImGui::BulletText("%s: %s; sample mask 0x%X",
                        Engine::RHI::ToString(format.Value), usages.c_str(), format.SampleCountMask);
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Fallbacks", "Fallbacks (%zu)", reasonDiagnostics.SelectedFallbacks.size()))
            {
                if (reasonDiagnostics.SelectedFallbacks.empty())
                    ImGui::TextDisabled("No selected-device fallbacks recorded");
                for (const std::string& fallback : reasonDiagnostics.SelectedFallbacks)
                    ImGui::BulletText("%s", fallback.c_str());
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Adapter candidates", "Adapter candidates (%zu)", reasonDiagnostics.AdapterCandidates.size()))
            {
                for (const Engine::RendererAdapterCandidateDiagnostics& candidateDiagnostics : reasonDiagnostics.AdapterCandidates)
                {
                    const Engine::RHI::AdapterCandidate& candidate = capabilities->AdapterCandidates[candidateDiagnostics.CandidateIndex];
                    ImGui::PushID(static_cast<int>(candidateDiagnostics.CandidateIndex));
                    const bool open = ImGui::TreeNode("Candidate", "%s - %s%s",
                        candidateDiagnostics.Name.c_str(),
                        candidateDiagnostics.Accepted ? "accepted" : "rejected",
                        candidateDiagnostics.Selected ? ", selected" : "");
                    if (open)
                    {
                        if (!candidate.Identity.StableId.empty())
                            ImGui::TextWrapped("Stable ID: %s", candidate.Identity.StableId.c_str());
                        ImGui::Text("API: %u.%u; max 2D texture: %u", candidate.ApiMajor, candidate.ApiMinor, candidate.MaximumTextureDimension2D);
                        for (const std::string& fallback : candidateDiagnostics.Fallbacks)
                            ImGui::BulletText("Fallback: %s", fallback.c_str());
                        for (const std::string& rejection : candidateDiagnostics.RejectionReasons)
                            ImGui::BulletText("Rejected: %s", rejection.c_str());
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }

            if (m_RendererCapabilitySmokeRequested && !m_RendererCapabilitySmokeComplete)
            {
                const Engine::RHI::CapabilityGroupState* timingGroup = capabilities->GetCapabilityGroup(
                    Engine::RHI::CapabilityGroupId::Phase3FrameTimingV1);
                const Engine::RHI::CapabilityGroupState* transientGroup = capabilities->GetCapabilityGroup(
                    Engine::RHI::CapabilityGroupId::Phase3TransientResourcesV1);
                if (timingGroup && transientGroup && timingGroup->Exercised && transientGroup->Exercised
                    && timingGroup->Qualification >= Engine::RHI::QualificationLevel::Presentation
                    && transientGroup->Qualification >= Engine::RHI::QualificationLevel::Presentation)
                {
                    Engine::Log::Info(
                        "Editor renderer capability diagnostics rendered: profile=", capabilities->ProfileName,
                        ", adapter=", capabilities->Identity.Name,
                        ", qualification=", Engine::RHI::ToString(capabilities->Qualification),
                        ", formats=", capabilities->Formats.size(),
                        ", features=", static_cast<Engine::u32>(Engine::RHI::DeviceFeature::Count),
                        ", groups=", capabilities->CapabilityGroups.size(),
                        ", candidates=", reasonDiagnostics.AdapterCandidates.size());
                    Engine::Log::Info(
                        "Editor renderer capability group exercised: group=", Engine::RHI::ToString(timingGroup->Group),
                        ", profile=", timingGroup->ProfileName,
                        ", preferredPath=", Engine::RHI::ToString(timingGroup->PreferredPath),
                        ", selectedPath=", Engine::RHI::ToString(timingGroup->SelectedPath),
                        ", implemented=", timingGroup->Implemented ? "yes" : "no",
                        ", exercised=", timingGroup->Exercised ? "yes" : "no",
                        ", qualification=", Engine::RHI::ToString(timingGroup->Qualification),
                        ", deviceQualification=", Engine::RHI::ToString(capabilities->Qualification),
                        ", adapter=", capabilities->Identity.Name);
                    Engine::Log::Info(
                        "Editor renderer capability group exercised: group=", Engine::RHI::ToString(transientGroup->Group),
                        ", profile=", transientGroup->ProfileName,
                        ", preferredPath=", Engine::RHI::ToString(transientGroup->PreferredPath),
                        ", selectedPath=", Engine::RHI::ToString(transientGroup->SelectedPath),
                        ", implemented=", transientGroup->Implemented ? "yes" : "no",
                        ", exercised=", transientGroup->Exercised ? "yes" : "no",
                        ", qualification=", Engine::RHI::ToString(transientGroup->Qualification),
                        ", deviceQualification=", Engine::RHI::ToString(capabilities->Qualification),
                        ", adapter=", capabilities->Identity.Name);
                    m_RendererCapabilitySmokeComplete = true;
                }
            }
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Overdraw, quad waste, ray density, texture residency coming later");
    ImGui::End();
}

void EditorLayer::DrawProjectPanel()
{
    ImGui::Begin("Content Browser");
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

void EditorLayer::PublishFramePacingPolicy()
{
    Engine::Renderer::SetFramePacingPolicy(m_GameFramePacingSettings.Resolve(m_ProjectFramePacingPolicy));
}

void EditorLayer::PublishPresentationPolicy()
{
    Engine::Renderer::SetPresentationPolicy(m_RuntimePresentationPolicyOverride.value_or(m_ProjectPresentationPolicy));
}

void EditorLayer::PublishColorPipelineSettings()
{
    if (!Engine::Renderer::SetColorPipelineSettings(m_ProjectColorPipelineSettings))
        Engine::Log::Error("Rejected invalid project color pipeline settings");
}

bool EditorLayer::PublishProjectColorPipelineSettings(
    const Engine::RendererColorPipelineSettings& settings)
{
    if (settings == m_ProjectColorPipelineSettings
        || !Engine::IsValidRendererColorPipelineSettings(settings))
    {
        return false;
    }
    if (!Engine::Renderer::SetColorPipelineSettings(settings))
        return false;
    m_ProjectColorPipelineSettings = settings;
    return true;
}

bool EditorLayer::ApplyProjectColorPipelineSettings(
    const Engine::RendererColorPipelineSettings& settings, std::string label)
{
    const HistoryState before = CaptureHistoryState();
    if (!PublishProjectColorPipelineSettings(settings))
        return false;
    RecordHistory(std::move(label), before);
    return true;
}

void EditorLayer::HandleProjectColorPipelineInput(
    const Engine::RendererColorPipelineSettings& settings, bool edited, std::string label)
{
    const unsigned int itemId = ImGui::GetItemID();
    const auto finalizePendingInteraction = [this]()
    {
        if (m_ProjectColorPipelineInteractionBefore
            && m_ProjectColorPipelineInteractionChanged
            && m_ProjectColorPipelineInteractionBefore->ProjectColorPipelineSettings
                != m_ProjectColorPipelineSettings)
        {
            RecordHistory(std::move(m_ProjectColorPipelineInteractionLabel),
                std::move(*m_ProjectColorPipelineInteractionBefore));
        }
        m_ProjectColorPipelineInteractionBefore.reset();
        m_ProjectColorPipelineInteractionItemId = 0;
        m_ProjectColorPipelineInteractionChanged = false;
        m_ProjectColorPipelineInteractionLabel.clear();
    };

    if (ImGui::IsItemActivated())
    {
        if (m_ProjectColorPipelineInteractionBefore)
            finalizePendingInteraction();
        m_ProjectColorPipelineInteractionBefore = CaptureHistoryState();
        m_ProjectColorPipelineInteractionItemId = itemId;
        m_ProjectColorPipelineInteractionChanged = false;
        m_ProjectColorPipelineInteractionLabel = label;
    }

    if (edited)
    {
        if (!m_ProjectColorPipelineInteractionBefore)
        {
            m_ProjectColorPipelineInteractionBefore = CaptureHistoryState();
            m_ProjectColorPipelineInteractionItemId = itemId;
            m_ProjectColorPipelineInteractionLabel = label;
        }
        m_ProjectColorPipelineInteractionChanged |=
            PublishProjectColorPipelineSettings(settings);
    }

    if (ImGui::IsItemDeactivatedAfterEdit()
        && m_ProjectColorPipelineInteractionItemId == itemId)
        finalizePendingInteraction();
}

void EditorLayer::RunPresentationPolicySmoke()
{
    if (!m_PresentationPolicySmokeRequested || m_PresentationPolicySmokeCompleted)
        return;

    const Engine::u64 frame = Engine::Application::Get().GetFrameIndex();
    if (frame == 0)
    {
        m_ProjectPresentationPolicy = Engine::PresentationPolicy::TearingAllowed;
        PublishPresentationPolicy();
        Engine::Log::Info("PresentationPolicySmokeV1 state=request-tearing frame=", frame);
        return;
    }

    const Engine::RendererPresentationPolicyDiagnostics diagnostics = Engine::Renderer::GetPresentationPolicyDiagnostics();
    if (frame == 1)
    {
        if (diagnostics.Requested != Engine::PresentationPolicy::TearingAllowed || diagnostics.SwapchainGeneration == 0)
            throw std::runtime_error("presentation policy smoke did not commit the tearing request");
        m_PresentationPolicySmokeTearingGeneration = diagnostics.SwapchainGeneration;
        Engine::Log::Info("PresentationPolicySmokeV1 state=tearing-resolved requested=", Engine::ToString(diagnostics.Requested),
            " capability=", diagnostics.Capability, " actual=", Engine::ToString(diagnostics.Actual),
            " fallback=", diagnostics.FallbackReason, " generation=", diagnostics.SwapchainGeneration,
            " effectiveFrame=", diagnostics.EffectiveApplicationFrame);
        return;
    }
    if (frame == 2)
    {
        if (diagnostics.SwapchainGeneration != m_PresentationPolicySmokeTearingGeneration)
            throw std::runtime_error("unsupported presentation fallback recreated while its request was stable");
        const Engine::u32 width = Engine::Application::Get().GetWindow().GetWidth();
        const Engine::u32 height = Engine::Application::Get().GetWindow().GetHeight();
        Engine::Application::Get().GetWindow().SetSize(width > 128 ? width - 64 : width, height > 128 ? height - 64 : height);
        m_ProjectPresentationPolicy = Engine::PresentationPolicy::Synchronized;
        PublishPresentationPolicy();
        Engine::Log::Info("PresentationPolicySmokeV1 state=stable-request generation=", diagnostics.SwapchainGeneration,
            " resize=requested restore=Synchronized");
        return;
    }
    if (frame >= 4)
    {
        if (diagnostics.Requested != Engine::PresentationPolicy::Synchronized
            || diagnostics.Actual == Engine::PresentationActualMode::Unavailable
            || diagnostics.SwapchainGeneration <= m_PresentationPolicySmokeTearingGeneration
            || diagnostics.LastSuccessfulPresentGeneration != diagnostics.SwapchainGeneration
            || diagnostics.LastSuccessfulPresentApplicationFrame < diagnostics.EffectiveApplicationFrame)
            throw std::runtime_error("presentation policy smoke did not restore synchronized presentation after resize");
        Engine::Log::Info("PresentationPolicySmokeV1 state=pass requested=", Engine::ToString(diagnostics.Requested),
            " capability=", diagnostics.Capability, " actual=", Engine::ToString(diagnostics.Actual),
            " fallback=", diagnostics.FallbackReason, " generation=", diagnostics.SwapchainGeneration,
            " effectiveFrame=", diagnostics.EffectiveApplicationFrame,
            " lastPresentGeneration=", diagnostics.LastSuccessfulPresentGeneration,
            " lastPresentFrame=", diagnostics.LastSuccessfulPresentApplicationFrame,
            " resize=pass imgui=pass present=pass");
        m_PresentationPolicySmokeCompleted = true;
    }
}

void EditorLayer::DrawNewProjectDialog()
{
    if (m_ShowNewProjectDialog)
    {
        ImGui::OpenPopup("New Project");
        m_ShowNewProjectDialog = false;
    }

    if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::InputText("Name", m_NewProjectName.data(), m_NewProjectName.size());
    ImGui::InputText("Location", m_NewProjectParentPath.data(), m_NewProjectParentPath.size());

    const std::string projectName = m_NewProjectName.data();
    const std::string fileStem = SanitizeFileStem(projectName);
    const std::filesystem::path projectRoot = std::filesystem::path(m_NewProjectParentPath.data()) / fileStem;
    const std::filesystem::path manifestPath = projectRoot / (fileStem + ".spiralproject");
    const bool valid = !projectName.empty() && !fileStem.empty() && m_NewProjectParentPath[0] != '\0';
    std::error_code pathError;
    const bool alreadyExists = valid && std::filesystem::exists(manifestPath, pathError);

    if (valid)
        ImGui::TextDisabled("%s", manifestPath.string().c_str());
    if (alreadyExists)
        ImGui::TextDisabled("A project already exists at this location");

    ImGui::BeginDisabled(!valid || alreadyExists || pathError);
    if (ImGui::Button("Create"))
    {
        if (CreateNewProject(projectName, m_NewProjectParentPath.data()))
            ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
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
    Engine::MeshArtifact artifact;
    std::string artifactError;
    m_GltfImportSmokeCompleted = m_GltfImportSmokeCompleted
        && Engine::Renderer::ResolvePublishedMeshArtifact(m_LastGltfImport.MeshAsset, artifact, artifactError)
        && artifact.Vertices.size() == 3 && artifact.Indices.size() == 3;
    if (!m_GltfImportSmokeCompleted)
        throw std::runtime_error("glTF import smoke produced an invalid cooked mesh artifact: " + artifactError);

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
    material.Samplers.BaseColor = Engine::MaterialTextureSampler::PointClamp;
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
        && loaded->Textures.Opacity == textureHandle
        && loaded->Samplers.BaseColor == Engine::MaterialTextureSampler::PointClamp
        && loaded->Samplers.Opacity == Engine::MaterialTextureSampler::LinearWrap;
    if (!m_MaterialAssetSmokeCompleted)
        throw std::runtime_error("Material asset smoke reload validation failed");

    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    Engine::Log::Info("Material asset smoke passed: ", m_MaterialAssetSmokePath);
}

void EditorLayer::RunUndoRedoSmoke()
{
    if (!m_UndoRedoSmokeRequested || m_UndoRedoSmokeCompleted || m_FrameCounter < 1)
        return;

    Engine::TransformComponent* transform = m_ActiveScene.TryGetTransform(m_PrototypeMeshEntity);
    if (!transform)
        throw std::runtime_error("Undo/redo smoke could not find the prototype transform");

    Engine::Math::DVec3 originalPosition;
    if (!m_ActiveScene.TryGetEntityApproximateWorldPosition(m_PrototypeMeshEntity, originalPosition))
        throw std::runtime_error("Undo/redo smoke could not compose the prototype position");
    const double originalX = originalPosition.X;
    const HistoryState before = CaptureHistoryState();
    originalPosition.X += 2.0;
    m_ActiveScene.SetEntityWorldPositionAxis(m_PrototypeMeshEntity, 0, originalPosition.X);
    RecordHistory("Undo/redo smoke", before);

    const bool undone = Undo();
    const Engine::TransformComponent* restoredTransform = m_ActiveScene.TryGetTransform(m_PrototypeMeshEntity);
    Engine::Math::DVec3 restoredPosition;
    const bool restored = restoredTransform
        && m_ActiveScene.TryGetEntityApproximateWorldPosition(m_PrototypeMeshEntity, restoredPosition)
        && std::abs(restoredPosition.X - originalX) < 0.0001;

    const bool redone = Redo();
    const Engine::TransformComponent* reappliedTransform = m_ActiveScene.TryGetTransform(m_PrototypeMeshEntity);
    Engine::Math::DVec3 reappliedPosition;
    const bool reapplied = reappliedTransform
        && m_ActiveScene.TryGetEntityApproximateWorldPosition(m_PrototypeMeshEntity, reappliedPosition)
        && std::abs(reappliedPosition.X - (originalX + 2.0)) < 0.0001
        && Engine::Math::IsCanonical(reappliedTransform->GetPosition(), m_ActiveScene.GetWorldGridPolicy());
    if (!undone || !restored || !redone || !reapplied)
        throw std::runtime_error("Undo/redo smoke did not restore the prototype transform");

    m_UndoRedoSmokeCompleted = true;
    Engine::Log::Info("Undo/redo smoke passed");
}

void EditorLayer::RunSceneAuthoringSmoke()
{
    if (!m_SceneAuthoringSmokeRequested || m_SceneAuthoringSmokeCompleted || m_FrameCounter < 1)
        return;

    if (!CreateNewProject("Authoring Smoke", "output/projects/smoke", true))
        throw std::runtime_error("Scene authoring smoke could not create a project");

    const Engine::Entity authoredEntity = CreateSceneEntity("Authored Entity");
    const Engine::MeshRendererComponent* prototypeRenderer = m_ActiveScene.TryGetMeshRendererComponent(m_PrototypeMeshEntity);
    if (!authoredEntity || !prototypeRenderer)
        throw std::runtime_error("Scene authoring smoke could not create its entity or find the prototype renderer");

    const HistoryState beforeAssignment = CaptureHistoryState();
    m_ActiveScene.AddMeshRendererComponent(authoredEntity, *prototypeRenderer);
    RecordHistory("Assign prototype mesh", beforeAssignment);
    m_SelectedEntity = authoredEntity;

    if (!DeleteSelectedEntity() || m_ActiveScene.IsEntityValid(authoredEntity))
        throw std::runtime_error("Scene authoring smoke could not delete its entity");
    if (!Undo() || !m_ActiveScene.FindEntityByName("Authored Entity"))
        throw std::runtime_error("Scene authoring smoke could not undo entity deletion");
    if (!Redo() || m_ActiveScene.FindEntityByName("Authored Entity"))
        throw std::runtime_error("Scene authoring smoke could not redo entity deletion");
    if (!Undo())
        throw std::runtime_error("Scene authoring smoke could not restore its entity for persistence validation");

    if (!SaveProject() || !LoadProject())
        throw std::runtime_error("Scene authoring smoke could not save and reopen its project");

    const Engine::Entity loadedEntity = m_ActiveScene.FindEntityByName("Authored Entity");
    if (!loadedEntity || !m_ActiveScene.TryGetMeshRendererComponent(loadedEntity))
        throw std::runtime_error("Scene authoring smoke did not preserve the authored mesh entity");

    m_SceneAuthoringSmokeCompleted = true;
    Engine::Log::Info("Scene authoring smoke passed");
}

void EditorLayer::RunSceneRenderSnapshotSmoke()
{
    if (!m_SceneRenderSnapshotSmokeRequested || m_SceneRenderSnapshotSmokeCompleted)
        return;

    const std::shared_ptr<const Engine::SceneRenderSnapshot> snapshot =
        Engine::Renderer::GetSceneRenderSnapshot();
    if (!snapshot || snapshot->FrameIndex != Engine::Application::Get().GetFrameIndex())
        throw std::runtime_error("Scene render snapshot smoke did not publish the current frame");

    size_t expectedMeshes = 0;
    size_t expectedLights = 0;
    size_t expectedCameras = 0;
    for (const Engine::SceneEntity& entity : m_ActiveScene.GetEntities())
    {
        if (entity.MeshRenderer && entity.MeshRenderer->Visible)
            ++expectedMeshes;
        if (entity.Light)
            ++expectedLights;
        if (entity.Camera)
            ++expectedCameras;
    }

    const Engine::Entity mainCamera = m_ActiveScene.GetMainCameraEntity();
    const bool hasMainCameraRecord = std::any_of(
        snapshot->Cameras.begin(),
        snapshot->Cameras.end(),
        [mainCamera](const Engine::SceneRenderCamera& camera)
        {
            return camera.SourceEntity == mainCamera.Id && camera.Main;
        });
    if (snapshot->MainCameraEntity != mainCamera.Id
        || snapshot->Meshes.size() != expectedMeshes
        || snapshot->Lights.size() != expectedLights
        || snapshot->Cameras.size() != expectedCameras
        || snapshot->Views.size() != 1
        || !snapshot->Views[0].Camera.Valid
        || snapshot->Views[0].Camera.StableViewId != 1
        || !hasMainCameraRecord)
    {
        throw std::runtime_error("Scene render snapshot smoke did not match the active Scene extraction");
    }

    if (!m_FirstSceneRenderSnapshot)
    {
        if (!snapshot->Views[0].Camera.TemporalHistoryInvalidated)
            throw std::runtime_error("Scene render snapshot smoke did not invalidate the initial discontinuous viewport epoch");
        m_FirstSceneRenderSnapshot = snapshot;
        return;
    }

    if (snapshot == m_FirstSceneRenderSnapshot
        || m_FirstSceneRenderSnapshot->FrameIndex >= snapshot->FrameIndex)
    {
        throw std::runtime_error("Scene render snapshot smoke did not retain an immutable older epoch");
    }
    if (m_FirstSceneRenderSnapshot->Views[0].Camera.StableViewId != 1
        || !m_FirstSceneRenderSnapshot->Views[0].Camera.TemporalHistoryInvalidated
        || snapshot->Views[0].Camera.TemporalHistoryInvalidated)
    {
        throw std::runtime_error("Scene render snapshot smoke did not consume its viewport discontinuity exactly once");
    }

    Engine::Log::Info(
        "Scene render snapshot smoke passed: frame=", snapshot->FrameIndex,
        ", previousFrame=", m_FirstSceneRenderSnapshot->FrameIndex,
        ", meshes=", snapshot->Meshes.size(),
        ", lights=", snapshot->Lights.size(),
        ", cameras=", snapshot->Cameras.size());
    m_SceneRenderSnapshotSmokeCompleted = true;
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
    Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);
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

void EditorLayer::RunFramePacingPolicySmoke()
{
    if (!m_FramePacingPolicySmokeRequested || m_FramePacingPolicySmokeCompleted || m_FrameCounter < 1)
        return;

    const std::filesystem::path smokeRoot = "output/projects/frame-pacing-policy-smoke";
    const std::filesystem::path legacyManifestPath = smokeRoot / "legacy.spiralproject";
    const std::filesystem::path v2ManifestPath = smokeRoot / "v2.spiralproject";
    const std::filesystem::path invalidManifestPath = smokeRoot / "invalid.spiralproject";
    const bool legacyWritten = WriteTextFile(legacyManifestPath,
        "SpiralProject 1\nScene \"legacy.spiral\"\nAssetRegistry \"legacy.spiralassets\"\n");
    ProjectManifest legacyManifest;
    const bool legacyLoaded = legacyWritten && ReadProjectManifest(legacyManifestPath, legacyManifest)
        && legacyManifest.FramePacingPolicy.Mode == Engine::FramePacingMode::Responsive
        && legacyManifest.PresentationPolicy == Engine::PresentationPolicy::Synchronized;
    const bool v2Written = WriteTextFile(v2ManifestPath,
        "SpiralProject 2\nScene \"v2.spiral\"\nAssetRegistry \"v2.spiralassets\"\nFramePacingMode Responsive\nFramePacingTargetFps 60\n");
    ProjectManifest v2Manifest;
    const bool v2Migrated = v2Written && ReadProjectManifest(v2ManifestPath, v2Manifest)
        && v2Manifest.PresentationPolicy == Engine::PresentationPolicy::Synchronized;

    const ProjectManifest beforeInvalidRead {
        "unchanged.spiral",
        "unchanged.spiralassets",
        {},
        Engine::PresentationPolicy::Synchronized,
        {}
    };
    ProjectManifest invalidReadTarget = beforeInvalidRead;
    const bool invalidWritten = WriteTextFile(invalidManifestPath,
        "SpiralProject 3\nScene \"invalid.spiral\"\nAssetRegistry \"invalid.spiralassets\"\n"
        "FramePacingMode SmoothFrametime\nFramePacingTargetFps 60\nPresentationPolicy NotARealPolicy\n");
    const bool invalidRejected = invalidWritten && !ReadProjectManifest(invalidManifestPath, invalidReadTarget)
        && invalidReadTarget.ScenePath == beforeInvalidRead.ScenePath
        && invalidReadTarget.AssetRegistryPath == beforeInvalidRead.AssetRegistryPath;

    const Engine::FramePacingPolicy previousPolicy = m_ProjectFramePacingPolicy;
    const Engine::PresentationPolicy previousPresentationPolicy = m_ProjectPresentationPolicy;
    m_ProjectFramePacingPolicy = { Engine::FramePacingMode::SmoothFrametime, 144.0 };
    m_ProjectPresentationPolicy = Engine::PresentationPolicy::TearingAllowed;
    const bool savedAndReloaded = SaveProject() && LoadProject()
        && m_ProjectFramePacingPolicy.Mode == Engine::FramePacingMode::SmoothFrametime
        && m_ProjectFramePacingPolicy.SmoothTargetFramesPerSecond == 144.0
        && m_ProjectPresentationPolicy == Engine::PresentationPolicy::TearingAllowed;
    if (!savedAndReloaded)
    {
        m_ProjectFramePacingPolicy = previousPolicy;
        m_ProjectPresentationPolicy = previousPresentationPolicy;
    }

    m_FramePacingPolicySmokeCompleted = true;
    if (!legacyLoaded || !v2Migrated || !invalidRejected || !savedAndReloaded)
        throw std::runtime_error("Frame-pacing policy smoke failed");

    const Engine::ResolvedFramePacingPolicy resolved = m_GameFramePacingSettings.Resolve(m_ProjectFramePacingPolicy);
    Engine::Log::Info("FramePacingPolicySmokeV1 legacy=pass v2Migration=pass invalid=transactional-rejected saveReopen=pass ",
        Engine::DescribeFramePacingPolicy(resolved), " result=pass");
    m_ConsoleLines.emplace_back("Frame pacing policy smoke passed: " + Engine::DescribeFramePacingPolicy(resolved));
}

void EditorLayer::RunColorPipelineSettingsSmoke()
{
    if (!m_ColorPipelineSettingsSmokeRequested || m_ColorPipelineSettingsSmokeCompleted || m_FrameCounter < 1)
        return;

    const std::filesystem::path smokeRoot = "output/projects/color-pipeline-settings-smoke";
    const std::filesystem::path v3ManifestPath = smokeRoot / "v3.spiralproject";
    const std::filesystem::path v4ManifestPath = smokeRoot / "v4.spiralproject";
    const std::filesystem::path v5ManifestPath = smokeRoot / "v5.spiralproject";
    const std::filesystem::path v6ManifestPath = smokeRoot / "v6.spiralproject";
    const std::filesystem::path orderManifestPath = smokeRoot / "v6-order-independent.spiralproject";
    const std::filesystem::path orderRoundTripManifestPath = smokeRoot / "v6-order-roundtrip.spiralproject";
    const std::filesystem::path boundaryManifestPath = smokeRoot / "v6-boundary-roundtrip.spiralproject";
    const std::filesystem::path invalidBoundsPath = smokeRoot / "invalid-bounds.spiralproject";
    const std::filesystem::path invalidNonfinitePath = smokeRoot / "invalid-nonfinite.spiralproject";
    const std::filesystem::path invalidSaturationPath = smokeRoot / "invalid-saturation.spiralproject";
    const std::filesystem::path invalidContrastPath = smokeRoot / "invalid-contrast.spiralproject";
    const std::filesystem::path invalidExposureModePath = smokeRoot / "invalid-exposure-mode.spiralproject";
    const std::filesystem::path invalidCameraExposurePath = smokeRoot / "invalid-camera-exposure.spiralproject";

    const bool v3Written = WriteTextFile(v3ManifestPath,
        "SpiralProject 3\nScene \"v3.spiral\"\nAssetRegistry \"v3.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n");
    ProjectManifest v3Manifest;
    const bool v3Migrated = v3Written && ReadProjectManifest(v3ManifestPath, v3Manifest)
        && v3Manifest.ColorPipelineSettings.ManualExposureEV100 == 0.0
        && v3Manifest.ColorPipelineSettings.PostToneMapSaturation == 1.0
        && v3Manifest.ColorPipelineSettings.PostToneMapContrast == 1.0;

    const bool v4Written = WriteTextFile(v4ManifestPath,
        "SpiralProject 4\nScene \"v4.spiral\"\nAssetRegistry \"v4.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 -2\n");
    ProjectManifest v4Manifest;
    const bool v4Loaded = v4Written && ReadProjectManifest(v4ManifestPath, v4Manifest)
        && v4Manifest.ColorPipelineSettings.ManualExposureEV100 == -2.0
        && v4Manifest.ColorPipelineSettings.PostToneMapSaturation == 1.0
        && v4Manifest.ColorPipelineSettings.PostToneMapContrast == 1.0;

    const bool v5Written = WriteTextFile(v5ManifestPath,
        "SpiralProject 5\nScene \"v5.spiral\"\nAssetRegistry \"v5.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 -1\nPostToneMapSaturation 0.25\nPostToneMapContrast 1.5\n");
    ProjectManifest v5Manifest;
    const bool v5Loaded = v5Written && ReadProjectManifest(v5ManifestPath, v5Manifest)
        && v5Manifest.ColorPipelineSettings.ManualExposureEV100 == -1.0
        && v5Manifest.ColorPipelineSettings.PostToneMapSaturation == 0.25
        && v5Manifest.ColorPipelineSettings.PostToneMapContrast == 1.5
        && v5Manifest.ColorPipelineSettings.ExposureMode == Engine::RendererExposureMode::ManualEV100
        && v5Manifest.ColorPipelineSettings.CameraApertureFNumber == 1.0
        && v5Manifest.ColorPipelineSettings.CameraShutterSeconds == 1.0
        && v5Manifest.ColorPipelineSettings.CameraISO == 100.0;

    const bool v6Written = WriteTextFile(v6ManifestPath,
        "SpiralProject 6\nScene \"v6.spiral\"\nAssetRegistry \"v6.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 -1\nPostToneMapSaturation 0.25\nPostToneMapContrast 1.5\n"
        "ExposureMode CameraCalibration\nCameraApertureFNumber 2\nCameraShutterSeconds 0.25\nCameraISO 200\n");
    ProjectManifest v6Manifest;
    const bool v6Loaded = v6Written && ReadProjectManifest(v6ManifestPath, v6Manifest)
        && v6Manifest.ColorPipelineSettings.ManualExposureEV100 == -1.0
        && v6Manifest.ColorPipelineSettings.PostToneMapSaturation == 0.25
        && v6Manifest.ColorPipelineSettings.PostToneMapContrast == 1.5
        && v6Manifest.ColorPipelineSettings.ExposureMode == Engine::RendererExposureMode::CameraCalibration
        && v6Manifest.ColorPipelineSettings.CameraApertureFNumber == 2.0
        && v6Manifest.ColorPipelineSettings.CameraShutterSeconds == 0.25
        && v6Manifest.ColorPipelineSettings.CameraISO == 200.0
        && std::abs(Engine::EffectiveExposureEV100(v6Manifest.ColorPipelineSettings) - 3.0) < 0.0001;

    const bool orderWritten = WriteTextFile(orderManifestPath,
        "SpiralProject 6\nScene \"order.spiral\"\nAssetRegistry \"order.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 -1\nPostToneMapSaturation 0.25\nPostToneMapContrast 1.5\n"
        "ExposureMode CameraCalibration\nCameraApertureFNumber 64\n"
        "CameraShutterSeconds 0.000125\nCameraISO 102400\n");
    ProjectManifest orderManifest;
    const bool orderLoaded = orderWritten && ReadProjectManifest(orderManifestPath, orderManifest)
        && orderManifest.ColorPipelineSettings.ExposureMode == Engine::RendererExposureMode::CameraCalibration
        && orderManifest.ColorPipelineSettings.CameraApertureFNumber == 64.0
        && orderManifest.ColorPipelineSettings.CameraShutterSeconds == 0.000125
        && orderManifest.ColorPipelineSettings.CameraISO == 102400.0
        && std::abs(Engine::EffectiveExposureEV100(orderManifest.ColorPipelineSettings)
            - std::log2(32000.0)) < 0.0001;
    ProjectManifest orderRoundTripManifest;
    const bool orderRoundTrip = orderLoaded
        && WriteProjectManifest(orderRoundTripManifestPath, orderManifest)
        && ReadProjectManifest(orderRoundTripManifestPath, orderRoundTripManifest)
        && orderRoundTripManifest.ColorPipelineSettings == orderManifest.ColorPipelineSettings;

    ProjectManifest boundaryManifest {
        "boundary.spiral",
        "boundary.spiralassets",
        { Engine::FramePacingMode::Responsive, 60.0 },
        Engine::PresentationPolicy::Synchronized,
        {}
    };
    boundaryManifest.ColorPipelineSettings.ExposureMode = Engine::RendererExposureMode::CameraCalibration;
    boundaryManifest.ColorPipelineSettings.CameraApertureFNumber = 64.0;
    boundaryManifest.ColorPipelineSettings.CameraShutterSeconds = 1.0 / 8000.0;
    boundaryManifest.ColorPipelineSettings.CameraISO = 50000.0;
    ProjectManifest boundaryRoundTripManifest;
    const bool boundaryRoundTrip = Engine::IsValidRendererColorPipelineSettings(boundaryManifest.ColorPipelineSettings)
        && std::abs(Engine::EffectiveExposureEV100(boundaryManifest.ColorPipelineSettings) - 16.0) < 0.0001
        && WriteProjectManifest(boundaryManifestPath, boundaryManifest)
        && ReadProjectManifest(boundaryManifestPath, boundaryRoundTripManifest)
        && boundaryRoundTripManifest.ColorPipelineSettings == boundaryManifest.ColorPipelineSettings;

    const ProjectManifest beforeInvalidRead {
        "unchanged.spiral",
        "unchanged.spiralassets",
        { Engine::FramePacingMode::SmoothFrametime, 123.0 },
        Engine::PresentationPolicy::TearingAllowed,
        { 1.0, 0.75, 1.25 }
    };
    const auto invalidReadPreserved = [&beforeInvalidRead](const ProjectManifest& target)
    {
        return target.ScenePath == beforeInvalidRead.ScenePath
            && target.AssetRegistryPath == beforeInvalidRead.AssetRegistryPath
            && target.FramePacingPolicy.Mode == beforeInvalidRead.FramePacingPolicy.Mode
            && target.FramePacingPolicy.SmoothTargetFramesPerSecond == beforeInvalidRead.FramePacingPolicy.SmoothTargetFramesPerSecond
            && target.PresentationPolicy == beforeInvalidRead.PresentationPolicy
            && target.ColorPipelineSettings == beforeInvalidRead.ColorPipelineSettings;
    };
    ProjectManifest invalidBoundsTarget = beforeInvalidRead;
    const bool invalidBoundsWritten = WriteTextFile(invalidBoundsPath,
        "SpiralProject 4\nScene \"invalid.spiral\"\nAssetRegistry \"invalid.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 17\n");
    const bool invalidBoundsRejected = invalidBoundsWritten
        && !ReadProjectManifest(invalidBoundsPath, invalidBoundsTarget)
        && invalidReadPreserved(invalidBoundsTarget);

    ProjectManifest invalidNonfiniteTarget = beforeInvalidRead;
    const bool invalidNonfiniteWritten = WriteTextFile(invalidNonfinitePath,
        "SpiralProject 4\nScene \"invalid.spiral\"\nAssetRegistry \"invalid.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 1e9999\n");
    const bool invalidNonfiniteRejected = invalidNonfiniteWritten
        && !ReadProjectManifest(invalidNonfinitePath, invalidNonfiniteTarget)
        && invalidReadPreserved(invalidNonfiniteTarget);

    ProjectManifest invalidSaturationTarget = beforeInvalidRead;
    const bool invalidSaturationWritten = WriteTextFile(invalidSaturationPath,
        "SpiralProject 5\nScene \"invalid.spiral\"\nAssetRegistry \"invalid.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 0\nPostToneMapSaturation 2.25\nPostToneMapContrast 1\n");
    const bool invalidSaturationRejected = invalidSaturationWritten
        && !ReadProjectManifest(invalidSaturationPath, invalidSaturationTarget)
        && invalidReadPreserved(invalidSaturationTarget);

    ProjectManifest invalidContrastTarget = beforeInvalidRead;
    const bool invalidContrastWritten = WriteTextFile(invalidContrastPath,
        "SpiralProject 5\nScene \"invalid.spiral\"\nAssetRegistry \"invalid.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1e9999\n");
    const bool invalidContrastRejected = invalidContrastWritten
        && !ReadProjectManifest(invalidContrastPath, invalidContrastTarget)
        && invalidReadPreserved(invalidContrastTarget);

    ProjectManifest invalidExposureModeTarget = beforeInvalidRead;
    const bool invalidExposureModeWritten = WriteTextFile(invalidExposureModePath,
        "SpiralProject 6\nScene \"invalid.spiral\"\nAssetRegistry \"invalid.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1\n"
        "ExposureMode AperturePriority\nCameraApertureFNumber 1\nCameraShutterSeconds 1\nCameraISO 100\n");
    const bool invalidExposureModeRejected = invalidExposureModeWritten
        && !ReadProjectManifest(invalidExposureModePath, invalidExposureModeTarget)
        && invalidReadPreserved(invalidExposureModeTarget);

    ProjectManifest invalidCameraExposureTarget = beforeInvalidRead;
    const bool invalidCameraExposureWritten = WriteTextFile(invalidCameraExposurePath,
        "SpiralProject 6\nScene \"invalid.spiral\"\nAssetRegistry \"invalid.spiralassets\"\n"
        "FramePacingMode Responsive\nFramePacingTargetFps 60\nPresentationPolicy Synchronized\n"
        "ManualExposureEV100 0\nPostToneMapSaturation 1\nPostToneMapContrast 1\n"
        "ExposureMode CameraCalibration\nCameraApertureFNumber 0.5\nCameraShutterSeconds 1\nCameraISO 100\n");
    const bool invalidCameraExposureRejected = invalidCameraExposureWritten
        && !ReadProjectManifest(invalidCameraExposurePath, invalidCameraExposureTarget)
        && invalidReadPreserved(invalidCameraExposureTarget);

    const Engine::RendererColorPipelineSettings previousSettings = m_ProjectColorPipelineSettings;
    const HistoryState historyBeforeSettingsChange = CaptureHistoryState();
    m_ProjectColorPipelineSettings = { 2.0, 0.5, 1.25 };
    m_ProjectColorPipelineSettings.ExposureMode = Engine::RendererExposureMode::CameraCalibration;
    m_ProjectColorPipelineSettings.CameraApertureFNumber = 2.0;
    m_ProjectColorPipelineSettings.CameraShutterSeconds = 0.25;
    m_ProjectColorPipelineSettings.CameraISO = 200.0;
    PublishColorPipelineSettings();
    const bool rendererPublished = Engine::Renderer::GetColorPipelineSettings().ManualExposureEV100 == 2.0
        && Engine::Renderer::GetColorPipelineSettings().PostToneMapSaturation == 0.5
        && Engine::Renderer::GetColorPipelineSettings().PostToneMapContrast == 1.25
        && Engine::Renderer::GetColorPipelineSettings().ExposureMode == Engine::RendererExposureMode::CameraCalibration
        && Engine::Renderer::GetColorPipelineSettings().CameraApertureFNumber == 2.0
        && Engine::Renderer::GetColorPipelineSettings().CameraShutterSeconds == 0.25
        && Engine::Renderer::GetColorPipelineSettings().CameraISO == 200.0
        && std::abs(Engine::EffectiveExposureEV100(Engine::Renderer::GetColorPipelineSettings()) - 3.0) < 0.0001;
    const bool savedAndReloaded = SaveProject() && LoadProject()
        && m_ProjectColorPipelineSettings.ManualExposureEV100 == 2.0
        && m_ProjectColorPipelineSettings.PostToneMapSaturation == 0.5
        && m_ProjectColorPipelineSettings.PostToneMapContrast == 1.25
        && m_ProjectColorPipelineSettings.ExposureMode == Engine::RendererExposureMode::CameraCalibration
        && m_ProjectColorPipelineSettings.CameraApertureFNumber == 2.0
        && m_ProjectColorPipelineSettings.CameraShutterSeconds == 0.25
        && m_ProjectColorPipelineSettings.CameraISO == 200.0
        && Engine::Renderer::GetColorPipelineSettings().ManualExposureEV100 == 2.0
        && Engine::Renderer::GetColorPipelineSettings().PostToneMapSaturation == 0.5
        && Engine::Renderer::GetColorPipelineSettings().PostToneMapContrast == 1.25
        && Engine::Renderer::GetColorPipelineSettings().ExposureMode == Engine::RendererExposureMode::CameraCalibration
        && Engine::Renderer::GetColorPipelineSettings().CameraApertureFNumber == 2.0
        && Engine::Renderer::GetColorPipelineSettings().CameraShutterSeconds == 0.25
        && Engine::Renderer::GetColorPipelineSettings().CameraISO == 200.0;
    const HistoryState historyAfterSettingsChange = CaptureHistoryState();
    const std::vector<HistoryEntry> priorUndoHistory = m_UndoHistory;
    const std::vector<HistoryEntry> priorRedoHistory = m_RedoHistory;
    const bool historyRestoredBefore = RestoreHistoryState(historyBeforeSettingsChange)
        && m_ProjectColorPipelineSettings == previousSettings
        && Engine::Renderer::GetColorPipelineSettings() == previousSettings;
    m_UndoHistory.clear();
    m_RedoHistory.clear();
    const bool historyApplied = historyRestoredBefore
        && ApplyProjectColorPipelineSettings(
            historyAfterSettingsChange.ProjectColorPipelineSettings,
            "Color pipeline settings smoke")
        && m_UndoHistory.size() == 1 && m_RedoHistory.empty()
        && m_ProjectColorPipelineSettings
            == historyAfterSettingsChange.ProjectColorPipelineSettings
        && Engine::Renderer::GetColorPipelineSettings()
            == historyAfterSettingsChange.ProjectColorPipelineSettings;
    const bool historyUndone = historyApplied && Undo()
        && m_UndoHistory.empty() && m_RedoHistory.size() == 1
        && m_ProjectColorPipelineSettings == previousSettings
        && Engine::Renderer::GetColorPipelineSettings() == previousSettings;
    const bool historyRedone = historyUndone && Redo()
        && m_UndoHistory.size() == 1 && m_RedoHistory.empty()
        && m_ProjectColorPipelineSettings
            == historyAfterSettingsChange.ProjectColorPipelineSettings
        && Engine::Renderer::GetColorPipelineSettings()
            == historyAfterSettingsChange.ProjectColorPipelineSettings;

    const auto transformsEqual = [](const Engine::TransformComponent& left,
                                     const Engine::TransformComponent& right)
    {
        return left.GetPosition().Sector.X == right.GetPosition().Sector.X
            && left.GetPosition().Sector.Y == right.GetPosition().Sector.Y
            && left.GetPosition().Sector.Z == right.GetPosition().Sector.Z
            && left.GetPosition().Local.X == right.GetPosition().Local.X
            && left.GetPosition().Local.Y == right.GetPosition().Local.Y
            && left.GetPosition().Local.Z == right.GetPosition().Local.Z
            && left.RotationDegrees.X == right.RotationDegrees.X
            && left.RotationDegrees.Y == right.RotationDegrees.Y
            && left.RotationDegrees.Z == right.RotationDegrees.Z
            && left.Scale.X == right.Scale.X
            && left.Scale.Y == right.Scale.Y
            && left.Scale.Z == right.Scale.Z;
    };
    const HistoryState liveBeforeInvalidUndo = CaptureHistoryState();
    const Engine::TransformComponent liveMainCameraTransform =
        m_ActiveScene.GetMainCameraTransform();
    HistoryState invalidHistoryState = liveBeforeInvalidUndo;
    Engine::Math::DVec3 invalidCameraPosition;
    const Engine::Entity invalidMainCamera = invalidHistoryState.Scene.GetMainCameraEntity();
    const bool invalidSceneMadeDistinct = invalidHistoryState.Scene
            .TryGetEntityApproximateWorldPosition(invalidMainCamera, invalidCameraPosition)
        && invalidHistoryState.Scene.SetEntityWorldPositionAxis(
            invalidMainCamera, 0, invalidCameraPosition.X + 123.0);
    invalidHistoryState.SelectedEntity = {};
    invalidHistoryState.CameraPosition[0] += 321.0;
    invalidHistoryState.CameraRotation[1] += 17.0f;
    const Engine::AssetHandle invalidOnlyAsset = invalidHistoryState.AssetRegistry.RegisterAsset(
        Engine::AssetType::Material,
        "output/assets/invalid-history-only.spiralmat", "Invalid history only");
    Engine::MaterialAsset invalidOnlyMaterial;
    invalidOnlyMaterial.Name = "Invalid history only";
    const bool invalidAssetsMadeDistinct = invalidOnlyAsset != Engine::kInvalidAssetHandle
        && invalidHistoryState.MaterialLibrary.Set(invalidOnlyAsset, invalidOnlyMaterial);
    invalidHistoryState.ProjectColorPipelineSettings.ManualExposureEV100 =
        std::numeric_limits<double>::quiet_NaN();
    m_UndoHistory.push_back({ "Invalid color history smoke",
        invalidHistoryState, liveBeforeInvalidUndo });
    const std::size_t invalidUndoDepth = m_UndoHistory.size();
    const std::size_t invalidRedoDepth = m_RedoHistory.size();
    const bool invalidUndoRejected = !Undo();
    const bool invalidStacksPreserved = m_UndoHistory.size() == invalidUndoDepth
        && m_RedoHistory.size() == invalidRedoDepth
        && !m_UndoHistory.empty()
        && m_UndoHistory.back().Label == "Invalid color history smoke";
    const bool invalidLiveStatePreserved =
        m_ActiveScene.GetName() == liveBeforeInvalidUndo.Scene.GetName()
        && m_ActiveScene.GetEntities().size()
            == liveBeforeInvalidUndo.Scene.GetEntities().size()
        && transformsEqual(m_ActiveScene.GetMainCameraTransform(), liveMainCameraTransform)
        && m_SelectedEntity == liveBeforeInvalidUndo.SelectedEntity
        && m_CameraPosition == liveBeforeInvalidUndo.CameraPosition
        && m_CameraRotation == liveBeforeInvalidUndo.CameraRotation
        && m_CameraFovDegrees == liveBeforeInvalidUndo.CameraFovDegrees
        && m_CameraNearClip == liveBeforeInvalidUndo.CameraNearClip
        && m_CameraFarClip == liveBeforeInvalidUndo.CameraFarClip
        && m_ProjectColorPipelineSettings
            == liveBeforeInvalidUndo.ProjectColorPipelineSettings
        && Engine::Renderer::GetColorPipelineSettings()
            == liveBeforeInvalidUndo.ProjectColorPipelineSettings
        && !m_AssetRegistry.GetAsset(invalidOnlyAsset)
        && !m_MaterialLibrary.Get(invalidOnlyAsset);
    const bool invalidHistoryRejected = invalidSceneMadeDistinct
        && invalidAssetsMadeDistinct && invalidUndoRejected
        && invalidStacksPreserved && invalidLiveStatePreserved;

    const bool historyRestoredAfter = RestoreHistoryState(historyBeforeSettingsChange)
        && m_ProjectColorPipelineSettings == previousSettings
        && Engine::Renderer::GetColorPipelineSettings() == previousSettings;
    m_UndoHistory = priorUndoHistory;
    m_RedoHistory = priorRedoHistory;
    const bool restoredAndSaved = historyRestoredAfter && SaveProject();

    m_ColorPipelineSettingsSmokeCompleted = true;
    if (!v3Migrated || !v4Loaded || !v5Loaded || !v6Loaded || !orderLoaded || !orderRoundTrip
        || !boundaryRoundTrip || !invalidBoundsRejected || !invalidNonfiniteRejected
        || !invalidSaturationRejected || !invalidContrastRejected
        || !invalidExposureModeRejected || !invalidCameraExposureRejected
        || !rendererPublished || !savedAndReloaded || !historyApplied
        || !historyUndone || !historyRedone || !invalidHistoryRejected
        || !historyRestoredAfter || !restoredAndSaved)
    {
        throw std::runtime_error("Color pipeline settings smoke failed");
    }

    const Engine::RendererColorPipelineSettings published = Engine::Renderer::GetColorPipelineSettings();
    Engine::Log::Info("ColorPipelineSettingsSmokeV1 default=pass bounds=pass nonfinite=pass v3Migration=pass v4GradingMigration=pass v5SaveReopen=pass historyUndoRedo=pass restoreFailureAtomic=pass rendererPublication=pass manualExposureEV100=",
        published.ManualExposureEV100, " postToneMapSaturation=", published.PostToneMapSaturation,
        " postToneMapContrast=", published.PostToneMapContrast,
        " exposureMode=", Engine::ToString(published.ExposureMode),
        " effectiveExposureEV100=", Engine::EffectiveExposureEV100(published),
        " v6Calibration=pass orderIndependent=pass roundTripPrecision=pass result=pass");
    m_ConsoleLines.emplace_back("Color pipeline settings smoke passed");
}

void EditorLayer::RunEditorSettingsSmoke()
{
    if (!m_EditorSettingsSmokeRequested || m_EditorSettingsSmokeCompleted || m_FrameCounter < 1)
        return;

    m_EditorSettingsSmokeCompleted = true;
    const std::filesystem::path smokeRoot = "output/editor-settings-smoke";
    const std::filesystem::path missingPath = smokeRoot / "missing.spiralsettings";
    const std::filesystem::path persistedPath = smokeRoot / "persisted.spiralsettings";
    const std::filesystem::path invalidPath = smokeRoot / "invalid.spiralsettings";
    const std::filesystem::path manifestPath = smokeRoot / "separation.spiralproject";
    std::error_code error;
    std::filesystem::remove(missingPath, error);

    EditorSettings missingSettings;
    const bool missingDefaultsToFusion = !ReadEditorSettings(missingPath, missingSettings)
        && missingSettings.ViewportNavigation == ViewportNavigationPreset::Fusion;
    const EditorSettings unrealSettings { ViewportNavigationPreset::Unreal };
    EditorSettings persistedSettings;
    const bool persisted = WriteEditorSettings(persistedPath, unrealSettings)
        && ReadEditorSettings(persistedPath, persistedSettings)
        && persistedSettings.ViewportNavigation == ViewportNavigationPreset::Unreal;
    const bool invalidWritten = WriteTextFile(invalidPath,
        "SpiralEditorSettings 1\nViewportNavigationPreset Unsupported\n");
    EditorSettings unchangedOnInvalidRead { ViewportNavigationPreset::Unreal };
    const bool invalidRejected = invalidWritten && !ReadEditorSettings(invalidPath, unchangedOnInvalidRead)
        && unchangedOnInvalidRead.ViewportNavigation == ViewportNavigationPreset::Unreal;

    const ProjectManifest manifest {
        "separation.spiral",
        "separation.spiralassets",
        {},
        Engine::PresentationPolicy::Synchronized,
        {}
    };
    const bool manifestSeparated = WriteProjectManifest(manifestPath, manifest);
    std::ifstream manifestInput(manifestPath);
    std::stringstream manifestContents;
    manifestContents << manifestInput.rdbuf();
    const bool projectManifestSeparated = manifestSeparated && manifestInput
        && manifestContents.str().find("ViewportNavigationPreset") == std::string::npos;

    if (!missingDefaultsToFusion || !persisted || !invalidRejected || !projectManifestSeparated)
        throw std::runtime_error("Editor settings smoke failed");

    Engine::Log::Info("EditorSettingsSmokeV1 missingFusion=pass persistence=pass invalidTransactional=pass projectManifestSeparate=pass result=pass");
    m_ConsoleLines.emplace_back("Editor settings smoke passed");
}

void EditorLayer::RunViewportNavigationSmoke()
{
    if (!m_ViewportNavigationSmokeRequested || m_ViewportNavigationSmokeCompleted || m_FrameCounter < 1)
        return;

    m_ViewportNavigationSmokeCompleted = true;
    const ViewportNavigationPreset previousPreset = m_ViewportNavigationPreset;
    m_ViewportNavigationInputEnabled = true;

    const auto dot = [] (const Engine::Math::DVec3& lhs, const Engine::Math::DVec3& rhs)
    {
        return lhs.X * rhs.X + lhs.Y * rhs.Y + lhs.Z * rhs.Z;
    };
    const auto distance = [&dot] (const Engine::Math::DVec3& lhs, const Engine::Math::DVec3& rhs)
    {
        const Engine::Math::DVec3 delta { lhs.X - rhs.X, lhs.Y - rhs.Y, lhs.Z - rhs.Z };
        return std::sqrt(dot(delta, delta));
    };
    const auto sameVector = [] (const Engine::Math::DVec3& lhs, const Engine::Math::DVec3& rhs, double tolerance = 0.0001)
    {
        return std::abs(lhs.X - rhs.X) < tolerance && std::abs(lhs.Y - rhs.Y) < tolerance && std::abs(lhs.Z - rhs.Z) < tolerance;
    };
    const auto cameraChanged = [this, &sameVector](const Engine::Math::DVec3& beforePosition, const Engine::Math::Vec3& beforeRotation)
    {
        return !sameVector(m_EditorCamera.GetPosition(), beforePosition)
            || m_EditorCamera.GetRotationDegrees().X != beforeRotation.X
            || m_EditorCamera.GetRotationDegrees().Y != beforeRotation.Y
            || m_EditorCamera.GetRotationDegrees().Z != beforeRotation.Z;
    };

    m_ViewportImageX = 0.0f;
    m_ViewportImageY = 0.0f;
    m_ViewportImageWidth = 1600.0f;
    m_ViewportImageHeight = 900.0f;
    m_ViewportNavigationPreset = ViewportNavigationPreset::Fusion;
    m_CameraRotation = { 22.0f, 37.0f, 0.0f };
    const float yaw = Engine::Math::DegreesToRadians(m_CameraRotation[1]);
    const float pitch = Engine::Math::DegreesToRadians(m_CameraRotation[0]);
    const Engine::Math::DVec3 forward { std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
    const Engine::Math::DVec3 right { std::cos(yaw), 0.0, -std::sin(yaw) };
    const Engine::Math::DVec3 up {
        forward.Y * right.Z - forward.Z * right.Y,
        forward.Z * right.X - forward.X * right.Z,
        forward.X * right.Y - forward.Y * right.X
    };
    const Engine::Math::DVec3 initialPivot { 4.0, -2.0, 7.0 };
    SetFusionNavigationPivot(initialPivot);
    m_CameraPosition = {
        initialPivot.X - forward.X * 12.0,
        initialPivot.Y - forward.Y * 12.0,
        initialPivot.Z - forward.Z * 12.0
    };
    m_EditorCamera.SetPosition({ m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] });
    m_EditorCamera.SetRotationDegrees({ m_CameraRotation[0], m_CameraRotation[1], m_CameraRotation[2] });
    ApplyEditorCameraStateToScene();

    m_SelectedEntity = m_PrototypeMeshEntity;
    Engine::Math::DVec3 selectedEntityPosition;
    const bool hasSelectedEntityPosition = m_ActiveScene.TryGetEntityApproximateWorldPosition(
        m_SelectedEntity, selectedEntityPosition);
    const Engine::Math::DVec3 cameraBeforeSelectionRetarget = m_EditorCamera.GetPosition();
    SetFusionNavigationPivot({ initialPivot.X + 0.75, initialPivot.Y, initialPivot.Z });
    ResetFusionNavigationPivotFromSelectionOrScene();
    const bool startupSelectionPivotRetargeted = hasSelectedEntityPosition
        && sameVector(m_FusionNavigationPivot, selectedEntityPosition)
        && sameVector(m_EditorCamera.GetPosition(), cameraBeforeSelectionRetarget);
    SetFusionNavigationPivot(initialPivot);

    const Engine::Math::DVec3 focusAcquireStart = m_EditorCamera.GetPosition();
    const Engine::Math::Vec3 focusAcquireStartRotation = m_EditorCamera.GetRotationDegrees();
    m_WindowFocused = true;
    m_ViewportHovered = true;
    m_ViewportFocused = false;
    m_ViewportNavigationFocusAvailable = true;
    m_ViewportNavigationInputEnabled = false;
    m_ViewportFocusRequested = false;
    Engine::MouseButtonPressedEvent unfocusedFusionMiddlePress(2);
    OnEvent(unfocusedFusionMiddlePress);
    const bool fusionMiddlePressAcquiredFocus = m_ViewportFocusRequested
        && m_ViewportNavigationInputEnabled && m_CursorCapturePending && m_MiddleMouseDown;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    Engine::MouseMovedEvent acquiredFocusMotion(311.0f, 227.0f);
    OnEvent(acquiredFocusMotion);
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const bool fusionFirstMiddleDragMoved = cameraChanged(focusAcquireStart, focusAcquireStartRotation);
    Engine::MouseButtonReleasedEvent unfocusedFusionMiddleRelease(2);
    OnEvent(unfocusedFusionMiddleRelease);
    const bool fusionMiddleFocusAcquire = fusionMiddlePressAcquiredFocus && fusionFirstMiddleDragMoved
        && !m_CursorCaptured && !m_CursorCapturePending;

    m_CameraPosition = { focusAcquireStart.X, focusAcquireStart.Y, focusAcquireStart.Z };
    m_CameraRotation = { focusAcquireStartRotation.X, focusAcquireStartRotation.Y, focusAcquireStartRotation.Z };
    SetFusionNavigationPivot(initialPivot);
    m_EditorCamera.SetPosition(focusAcquireStart);
    m_EditorCamera.SetRotationDegrees(focusAcquireStartRotation);
    ApplyEditorCameraStateToScene();
    m_ViewportFocused = false;
    m_ViewportNavigationFocusAvailable = true;
    m_ViewportNavigationInputEnabled = false;
    m_ViewportFocusRequested = false;
    Engine::MouseScrolledEvent unfocusedFusionScroll(0.0f, 1.0f);
    OnEvent(unfocusedFusionScroll);
    const bool fusionScrollAcquiredFocus = m_ViewportFocusRequested && m_ViewportNavigationInputEnabled;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const bool fusionFirstScrollZoomed = cameraChanged(focusAcquireStart, focusAcquireStartRotation);
    const bool fusionScrollFocusAcquire = fusionScrollAcquiredFocus && fusionFirstScrollZoomed;

    m_CameraPosition = { focusAcquireStart.X, focusAcquireStart.Y, focusAcquireStart.Z };
    m_CameraRotation = { focusAcquireStartRotation.X, focusAcquireStartRotation.Y, focusAcquireStartRotation.Z };
    SetFusionNavigationPivot(initialPivot);
    m_EditorCamera.SetPosition(focusAcquireStart);
    m_EditorCamera.SetRotationDegrees(focusAcquireStartRotation);
    ApplyEditorCameraStateToScene();
    m_ViewportFocused = true;
    m_ViewportNavigationFocusAvailable = true;
    m_ViewportNavigationInputEnabled = true;
    m_ViewportFocusRequested = false;

    const Engine::Math::DVec3 fusionBefore = m_EditorCamera.GetPosition();
    const Engine::Math::Vec3 fusionBeforeRotation = m_EditorCamera.GetRotationDegrees();
    m_LeftMouseDown = true;
    m_MouseDeltaX = 25.0f;
    m_MouseDeltaY = -17.0f;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    m_LeftMouseDown = false;
    const bool fusionLeftNoOp = !cameraChanged(fusionBefore, fusionBeforeRotation);
    m_RightMouseDown = true;
    m_MouseDeltaX = 25.0f;
    m_MouseDeltaY = -17.0f;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    m_RightMouseDown = false;
    const bool fusionRightNoOp = !cameraChanged(fusionBefore, fusionBeforeRotation);

    m_MouseX = 320.0;
    m_MouseY = 240.0;
    m_HasMousePosition = true;
    const Engine::Math::DVec3 beforeStaleCaptureMotion = m_EditorCamera.GetPosition();
    const Engine::Math::DVec3 beforeStaleCapturePivot = m_FusionNavigationPivot;
    m_MiddleMouseDown = true;
    BeginViewportCursorCapture();
    Engine::MouseMovedEvent staleCaptureMotion(1400.0f, 800.0f);
    OnEvent(staleCaptureMotion);
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const bool staleCaptureMotionIgnored = sameVector(m_EditorCamera.GetPosition(), beforeStaleCaptureMotion)
        && sameVector(m_FusionNavigationPivot, beforeStaleCapturePivot) && m_CursorCaptured && m_CursorCaptureBaselineArmed;
    m_MiddleMouseDown = false;
    EndViewportCursorCapture();

    m_MiddleMouseDown = true;
    BeginViewportCursorCapture();
    m_MiddleMouseDown = false;
    EndViewportCursorCapture();
    const bool quickReleaseBeforeCaptureArm = !m_CursorCaptured && !m_CursorCapturePending && !m_CursorCaptureBaselineArmed
        && m_MouseDeltaX == 0.0f && m_MouseDeltaY == 0.0f;

    m_MiddleMouseDown = true;
    BeginViewportCursorCapture();
    Engine::WindowFocusEvent pendingCaptureFocusLost(false);
    OnEvent(pendingCaptureFocusLost);
    const bool pendingCaptureFocusLossReleased = !m_CursorCaptured && !m_CursorCapturePending && !m_CursorCaptureBaselineArmed
        && !m_MiddleMouseDown;
    Engine::WindowFocusEvent focusRestored(true);
    OnEvent(focusRestored);
    m_ViewportNavigationInputEnabled = true;

    const double worldUnitsPerPixel = 2.0 * 12.0
        * std::tan(Engine::Math::DegreesToRadians(m_EditorCamera.GetProjection().VerticalFovDegrees) * 0.5)
        / m_ViewportImageHeight;
    const auto expectedPanTranslation = [&right, &up, worldUnitsPerPixel](float deltaX, float deltaY)
    {
        return Engine::Math::DVec3 {
            -right.X * deltaX * worldUnitsPerPixel + up.X * deltaY * worldUnitsPerPixel,
            -right.Y * deltaX * worldUnitsPerPixel + up.Y * deltaY * worldUnitsPerPixel,
            -right.Z * deltaX * worldUnitsPerPixel + up.Z * deltaY * worldUnitsPerPixel
        };
    };
    const Engine::Math::DVec3 beforeTransitionCaptureMotion = m_EditorCamera.GetPosition();
    const Engine::Math::DVec3 beforeTransitionCapturePivot = m_FusionNavigationPivot;
    m_MiddleMouseDown = true;
    BeginViewportCursorCapture();
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    m_CursorCaptureBaselineArmed = false;
    m_HasMousePosition = false;
    Engine::MouseMovedEvent transitionCaptureMotion(1200.0f, 50.0f);
    OnEvent(transitionCaptureMotion);
    m_CursorCaptureBaselineArmed = true;
    Engine::MouseMovedEvent firstRealCaptureMotion(1208.0f, 45.0f);
    OnEvent(firstRealCaptureMotion);
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const Engine::Math::DVec3 expectedFirstCaptureTranslation = expectedPanTranslation(8.0f, -5.0f);
    const Engine::Math::DVec3 firstCaptureTranslation {
        m_EditorCamera.GetPosition().X - beforeTransitionCaptureMotion.X,
        m_EditorCamera.GetPosition().Y - beforeTransitionCaptureMotion.Y,
        m_EditorCamera.GetPosition().Z - beforeTransitionCaptureMotion.Z
    };
    const Engine::Math::DVec3 firstCapturePivotTranslation {
        m_FusionNavigationPivot.X - beforeTransitionCapturePivot.X,
        m_FusionNavigationPivot.Y - beforeTransitionCapturePivot.Y,
        m_FusionNavigationPivot.Z - beforeTransitionCapturePivot.Z
    };
    const bool transitionCaptureMotionIgnored = sameVector(firstCaptureTranslation, expectedFirstCaptureTranslation)
        && sameVector(firstCapturePivotTranslation, expectedFirstCaptureTranslation);
    m_MiddleMouseDown = false;
    EndViewportCursorCapture();

    const Engine::Math::DVec3 beforeNormalCaptureMotion = m_EditorCamera.GetPosition();
    m_MiddleMouseDown = true;
    BeginViewportCursorCapture();
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    m_MouseX = 400.0;
    m_MouseY = 300.0;
    m_HasMousePosition = true;
    Engine::MouseMovedEvent normalCaptureMotion(406.0f, 297.0f);
    OnEvent(normalCaptureMotion);
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const Engine::Math::DVec3 normalCaptureTranslation {
        m_EditorCamera.GetPosition().X - beforeNormalCaptureMotion.X,
        m_EditorCamera.GetPosition().Y - beforeNormalCaptureMotion.Y,
        m_EditorCamera.GetPosition().Z - beforeNormalCaptureMotion.Z
    };
    const bool normalCaptureDrag = sameVector(normalCaptureTranslation, expectedPanTranslation(6.0f, -3.0f));
    m_MiddleMouseDown = false;
    EndViewportCursorCapture();

    const Engine::Math::DVec3 cameraBeforePan = m_EditorCamera.GetPosition();
    const Engine::Math::DVec3 pivotBeforePan = m_FusionNavigationPivot;
    m_MiddleMouseDown = true;
    m_MouseDeltaX = 80.0f;
    m_MouseDeltaY = -45.0f;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    m_MiddleMouseDown = false;
    const Engine::Math::DVec3 panTranslation {
        m_EditorCamera.GetPosition().X - cameraBeforePan.X,
        m_EditorCamera.GetPosition().Y - cameraBeforePan.Y,
        m_EditorCamera.GetPosition().Z - cameraBeforePan.Z
    };
    const Engine::Math::DVec3 pivotTranslation {
        m_FusionNavigationPivot.X - pivotBeforePan.X,
        m_FusionNavigationPivot.Y - pivotBeforePan.Y,
        m_FusionNavigationPivot.Z - pivotBeforePan.Z
    };
    const bool fusionCameraPlanePan = sameVector(panTranslation, pivotTranslation)
        && std::abs(dot(panTranslation, forward)) < 0.0001 && dot(panTranslation, right) < 0.0;

    m_MouseX = 1200.0f;
    m_MouseY = 300.0f;
    const Engine::Math::DVec3 cameraBeforeZoom = m_EditorCamera.GetPosition();
    const Engine::Math::DVec3 pivotOffset {
        m_FusionNavigationPivot.X - cameraBeforeZoom.X,
        m_FusionNavigationPivot.Y - cameraBeforeZoom.Y,
        m_FusionNavigationPivot.Z - cameraBeforeZoom.Z
    };
    const double navigationDepth = dot(pivotOffset, forward);
    const double tangent = std::tan(Engine::Math::DegreesToRadians(m_EditorCamera.GetProjection().VerticalFovDegrees) * 0.5);
    const double normalizedX = ((m_MouseX - m_ViewportImageX) / m_ViewportImageWidth - 0.5) * 2.0;
    const double normalizedY = (0.5 - (m_MouseY - m_ViewportImageY) / m_ViewportImageHeight) * 2.0;
    Engine::Math::DVec3 ray {
        forward.X + right.X * normalizedX * tangent * m_EditorCamera.GetAspectRatio() + up.X * normalizedY * tangent,
        forward.Y + right.Y * normalizedX * tangent * m_EditorCamera.GetAspectRatio() + up.Y * normalizedY * tangent,
        forward.Z + right.Z * normalizedX * tangent * m_EditorCamera.GetAspectRatio() + up.Z * normalizedY * tangent
    };
    const double rayLength = std::sqrt(dot(ray, ray));
    ray = { ray.X / rayLength, ray.Y / rayLength, ray.Z / rayLength };
    const Engine::Math::DVec3 zoomAnchor {
        cameraBeforeZoom.X + ray.X * navigationDepth / dot(ray, forward),
        cameraBeforeZoom.Y + ray.Y * navigationDepth / dot(ray, forward),
        cameraBeforeZoom.Z + ray.Z * navigationDepth / dot(ray, forward)
    };
    const double distanceBeforeZoom = distance(cameraBeforeZoom, zoomAnchor);
    m_MouseWheelDelta = 1.0f;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const Engine::Math::DVec3 cameraAfterZoom = m_EditorCamera.GetPosition();
    const double distanceAfterZoom = distance(cameraAfterZoom, zoomAnchor);
    const Engine::Math::DVec3 beforeAnchor { cameraBeforeZoom.X - zoomAnchor.X, cameraBeforeZoom.Y - zoomAnchor.Y, cameraBeforeZoom.Z - zoomAnchor.Z };
    const Engine::Math::DVec3 afterAnchor { cameraAfterZoom.X - zoomAnchor.X, cameraAfterZoom.Y - zoomAnchor.Y, cameraAfterZoom.Z - zoomAnchor.Z };
    const Engine::Math::DVec3 anchorAfterCamera { zoomAnchor.X - cameraAfterZoom.X, zoomAnchor.Y - cameraAfterZoom.Y, zoomAnchor.Z - cameraAfterZoom.Z };
    const double collinearity = std::sqrt(std::pow(beforeAnchor.Y * afterAnchor.Z - beforeAnchor.Z * afterAnchor.Y, 2.0)
        + std::pow(beforeAnchor.Z * afterAnchor.X - beforeAnchor.X * afterAnchor.Z, 2.0)
        + std::pow(beforeAnchor.X * afterAnchor.Y - beforeAnchor.Y * afterAnchor.X, 2.0));
    const bool fusionCursorZoom = std::abs(distanceAfterZoom / distanceBeforeZoom - 0.85) < 0.0001
        && collinearity < 0.0001 && dot(anchorAfterCamera, forward) > m_EditorCamera.GetProjection().NearClip;

    const Engine::Math::DVec3 orbitPivot = m_FusionNavigationPivot;
    const Engine::Math::DVec3 beforeZeroDeltaOrbit = m_EditorCamera.GetPosition();
    const Engine::Math::Vec3 beforeZeroDeltaOrbitRotation = m_EditorCamera.GetRotationDegrees();
    m_MiddleMouseDown = true;
    m_KeyDown[340] = true;
    m_MouseDeltaX = 0.0f;
    m_MouseDeltaY = 0.0f;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const Engine::Math::DVec3 afterZeroDeltaOrbit = m_EditorCamera.GetPosition();
    const Engine::Math::Vec3 afterZeroDeltaOrbitRotation = m_EditorCamera.GetRotationDegrees();
    const bool fusionZeroDeltaOrbit = afterZeroDeltaOrbit.X == beforeZeroDeltaOrbit.X
        && afterZeroDeltaOrbit.Y == beforeZeroDeltaOrbit.Y && afterZeroDeltaOrbit.Z == beforeZeroDeltaOrbit.Z
        && m_FusionNavigationPivot.X == orbitPivot.X && m_FusionNavigationPivot.Y == orbitPivot.Y && m_FusionNavigationPivot.Z == orbitPivot.Z
        && afterZeroDeltaOrbitRotation.X == beforeZeroDeltaOrbitRotation.X
        && afterZeroDeltaOrbitRotation.Y == beforeZeroDeltaOrbitRotation.Y && afterZeroDeltaOrbitRotation.Z == beforeZeroDeltaOrbitRotation.Z;

    constexpr float orbitDeltaX = 4.0f;
    constexpr float orbitDeltaY = -2.0f;
    const double orbitDistanceBefore = distance(afterZeroDeltaOrbit, orbitPivot);
    m_MouseDeltaX = orbitDeltaX;
    m_MouseDeltaY = orbitDeltaY;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const Engine::Math::DVec3 afterFirstOrbitDelta = m_EditorCamera.GetPosition();
    const double maximumContinuousDisplacement = 2.0 * orbitDistanceBefore
        * std::sin(Engine::Math::DegreesToRadians(std::sqrt(orbitDeltaX * orbitDeltaX + orbitDeltaY * orbitDeltaY) * 0.15f) * 0.5) * 1.1 + 0.001;
    const bool fusionFirstOrbitContinuous = distance(afterFirstOrbitDelta, afterZeroDeltaOrbit) > 0.0
        && distance(afterFirstOrbitDelta, afterZeroDeltaOrbit) <= maximumContinuousDisplacement
        && std::abs(distance(afterFirstOrbitDelta, orbitPivot) - orbitDistanceBefore) < 0.001
        && sameVector(m_FusionNavigationPivot, orbitPivot) && m_EditorCamera.GetRotationDegrees().Z == 0.0f;

    m_MouseDeltaX = -orbitDeltaX;
    m_MouseDeltaY = -orbitDeltaY;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    const bool fusionReverseOrbitRestored = sameVector(m_EditorCamera.GetPosition(), afterZeroDeltaOrbit, 0.001)
        && std::abs(m_EditorCamera.GetRotationDegrees().X - beforeZeroDeltaOrbitRotation.X) < 0.001f
        && std::abs(m_EditorCamera.GetRotationDegrees().Y - beforeZeroDeltaOrbitRotation.Y) < 0.001f
        && m_EditorCamera.GetRotationDegrees().Z == 0.0f && sameVector(m_FusionNavigationPivot, orbitPivot);
    m_MiddleMouseDown = false;
    m_KeyDown[340] = false;
    const bool fusionOrbitContinuous = fusionZeroDeltaOrbit && fusionFirstOrbitContinuous && fusionReverseOrbitRestored && m_FusionNavigationPivotValid;

    const Engine::Math::DVec3 beforeHeldWheel = m_EditorCamera.GetPosition();
    m_MiddleMouseDown = true;
    m_MouseWheelDelta = -1.0f;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    m_MiddleMouseDown = false;
    const bool fusionWheelWhileMiddleHeld = !sameVector(beforeHeldWheel, m_EditorCamera.GetPosition());

    m_ViewportNavigationPreset = ViewportNavigationPreset::Unreal;
    const Engine::Math::DVec3 before = m_EditorCamera.GetPosition();
    const Engine::Math::Vec3 beforeRotation = m_EditorCamera.GetRotationDegrees();
    m_LeftMouseDown = true;
    m_MouseDeltaX = 25.0f;
    m_MouseDeltaY = -17.0f;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    m_LeftMouseDown = false;
    const bool unrealLeftDragMoved = cameraChanged(before, beforeRotation);

    const Engine::Math::DVec3 beforeKeyboardNavigation = m_EditorCamera.GetPosition();
    m_RightMouseDown = true;
    m_KeyDown[static_cast<size_t>('W')] = true;
    UpdateViewportNavigation(Engine::Timestep(1.0f));
    m_KeyDown[static_cast<size_t>('W')] = false;
    m_RightMouseDown = false;

    Engine::Math::DVec3 scenePosition;
    const bool sceneMatches = m_ActiveScene.TryGetEntityApproximateWorldPosition(
        m_ActiveScene.GetMainCameraEntity(), scenePosition)
        && scenePosition.X == m_EditorCamera.GetPosition().X
        && scenePosition.Y == m_EditorCamera.GetPosition().Y
        && scenePosition.Z == m_EditorCamera.GetPosition().Z;
    const Engine::Math::DVec3 afterKeyboardNavigation = m_EditorCamera.GetPosition();
    const bool unrealFlyMoved = afterKeyboardNavigation.X != beforeKeyboardNavigation.X
        || afterKeyboardNavigation.Y != beforeKeyboardNavigation.Y
        || afterKeyboardNavigation.Z != beforeKeyboardNavigation.Z;
    m_SelectedEntity = m_PrototypeMeshEntity;
    FocusSelectedEntity();
    const bool focusDiscontinuous = m_ViewportDiscontinuousRelocationPending;
    m_RightMouseDown = true;
    m_KeyDown[static_cast<size_t>('W')] = true;
    BeginViewportCursorCapture();
    Engine::WindowFocusEvent focusLost(false);
    OnEvent(focusLost);
    const bool focusLossReleased = !m_CursorCaptured && !m_RightMouseDown
        && !m_KeyDown[static_cast<size_t>('W')] && m_MouseDeltaX == 0.0f && m_MouseDeltaY == 0.0f;
    m_ViewportNavigationPreset = previousPreset;
    if (!startupSelectionPivotRetargeted || !fusionMiddleFocusAcquire || !fusionScrollFocusAcquire
        || !fusionLeftNoOp || !fusionRightNoOp || !staleCaptureMotionIgnored || !quickReleaseBeforeCaptureArm
        || !pendingCaptureFocusLossReleased || !transitionCaptureMotionIgnored || !normalCaptureDrag
        || !fusionCameraPlanePan || !fusionCursorZoom || !fusionOrbitContinuous || !fusionWheelWhileMiddleHeld)
        throw std::runtime_error("Viewport navigation Fusion preset smoke failed");
    if (!unrealLeftDragMoved || !sceneMatches || !unrealFlyMoved || !focusDiscontinuous || !focusLossReleased)
        throw std::runtime_error("Viewport navigation smoke failed");

    Engine::Log::Info("ViewportNavigationSmokeV8 startupSelectionPivot=pass focusAcquire=pass deferredCapture=pass fusionPlanePan=pass cursorZoom=pass orbitContinuity=pass wheelHeld=pass stablePivot=pass unreal=pass dvec3=pass sceneAuthority=pass focusDiscontinuity=pass focusLossRelease=pass result=pass");
    m_ConsoleLines.emplace_back("Viewport navigation smoke passed");
}

bool EditorLayer::SaveProject()
{
    if (!Engine::IsValidFramePacingPolicy(m_ProjectFramePacingPolicy))
    {
        Engine::Log::Error("Project save rejected invalid frame-pacing policy");
        m_ConsoleLines.emplace_back("Project save rejected invalid frame-pacing policy");
        return false;
    }
    if (!Engine::IsValidRendererColorPipelineSettings(m_ProjectColorPipelineSettings))
    {
        Engine::Log::Error("Project save rejected invalid color pipeline settings");
        m_ConsoleLines.emplace_back("Project save rejected invalid color pipeline settings");
        return false;
    }

    if (!SaveActiveScene())
        return false;

    const ProjectManifest manifest {
        m_ScenePath,
        m_AssetRegistryPath,
        m_ProjectFramePacingPolicy,
        m_ProjectPresentationPolicy,
        m_ProjectColorPipelineSettings
    };
    if (!WriteProjectManifest(m_ProjectPath, manifest))
    {
        Engine::Log::Error("Project save failed: ", m_ProjectPath);
        m_ConsoleLines.emplace_back("Project save failed: " + m_ProjectPath);
        return false;
    }

    Engine::Log::Info("Project saved: ", m_ProjectPath);
    Engine::Log::Info("Frame pacing policy saved: ",
        Engine::DescribeFramePacingPolicy(m_GameFramePacingSettings.Resolve(m_ProjectFramePacingPolicy)));
    m_ConsoleLines.emplace_back("Project saved: " + m_ProjectPath);
    return true;
}

bool EditorLayer::CreateNewProject(std::string name, const std::filesystem::path& parentDirectory, bool overwriteExisting)
{
    const std::string fileStem = SanitizeFileStem(name);
    if (name.empty() || fileStem.empty() || parentDirectory.empty())
        return false;

    const std::filesystem::path projectRoot = parentDirectory / fileStem;
    const std::filesystem::path projectPath = projectRoot / (fileStem + ".spiralproject");
    std::error_code pathError;
    const bool projectExists = std::filesystem::exists(projectPath, pathError);
    if (pathError)
    {
        m_ConsoleLines.emplace_back("Could not inspect project path: " + pathError.message());
        return false;
    }
    if (!overwriteExisting && projectExists)
    {
        m_ConsoleLines.emplace_back("Project already exists: " + projectPath.string());
        return false;
    }

    const HistoryState previousState = CaptureHistoryState();
    const std::string previousProjectPath = m_ProjectPath;
    const std::string previousScenePath = m_ScenePath;
    const std::string previousAssetRegistryPath = m_AssetRegistryPath;
    const Engine::FramePacingPolicy previousFramePacingPolicy = m_ProjectFramePacingPolicy;
    const Engine::RendererColorPipelineSettings previousPublishedColorPipelineSettings =
        Engine::Renderer::GetColorPipelineSettings();
    const std::vector<HistoryEntry> previousUndoHistory = m_UndoHistory;
    const std::vector<HistoryEntry> previousRedoHistory = m_RedoHistory;

    m_ProjectPath = projectPath.string();
    m_ScenePath = (projectRoot / "Scenes" / "Main.spiral").string();
    m_AssetRegistryPath = (projectRoot / "Assets" / "assets.spiralassets").string();
    m_ProjectFramePacingPolicy = {};
    m_ProjectColorPipelineSettings = {};
    m_AssetRegistry = {};
    m_MaterialLibrary = {};
    m_ActiveScene = Engine::Scene(std::move(name));
    m_PrototypeMeshEntity = {};
    m_DirectionalLightEntity = {};
    m_PlayerStartEntity = {};
    m_SelectedEntity = {};
    m_SelectedAssetHandle = Engine::kInvalidAssetHandle;
    m_UndoHistory.clear();
    m_RedoHistory.clear();

    EnsureDefaultSceneEntities();
    SyncEditorCameraStateFromMainCamera(true);
    ResetFusionNavigationPivotFromSelectionOrScene();
    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    if (!SaveProject())
    {
        m_ProjectPath = previousProjectPath;
        m_ScenePath = previousScenePath;
        m_AssetRegistryPath = previousAssetRegistryPath;
        m_ProjectFramePacingPolicy = previousFramePacingPolicy;
        if (!RestoreHistoryState(previousState))
            Engine::Log::Error("Project-creation rollback could not restore history state");
        if (!Engine::Renderer::SetColorPipelineSettings(previousPublishedColorPipelineSettings))
            Engine::Log::Error("Project-creation rollback could not restore renderer color settings");
        m_UndoHistory = previousUndoHistory;
        m_RedoHistory = previousRedoHistory;
        return false;
    }

    if (!Engine::Renderer::SetColorPipelineSettings(m_ProjectColorPipelineSettings))
    {
        m_ProjectPath = previousProjectPath;
        m_ScenePath = previousScenePath;
        m_AssetRegistryPath = previousAssetRegistryPath;
        m_ProjectFramePacingPolicy = previousFramePacingPolicy;
        if (!RestoreHistoryState(previousState))
            Engine::Log::Error("Project-creation rollback could not restore history state");
        if (!Engine::Renderer::SetColorPipelineSettings(previousPublishedColorPipelineSettings))
            Engine::Log::Error("Project-creation rollback could not restore renderer color settings");
        m_UndoHistory = previousUndoHistory;
        m_RedoHistory = previousRedoHistory;
        return false;
    }

    Engine::Log::Info("Project created: ", m_ProjectPath);
    m_ConsoleLines.emplace_back("Project created: " + m_ProjectPath);
    m_EditorMaterialControl.EnsureProjectIdentity(m_ProjectPath);
    return true;
}

Engine::Entity EditorLayer::CreateSceneEntity(std::string name)
{
    const HistoryState before = CaptureHistoryState();
    if (name == "Entity")
    {
        unsigned int suffix = 1;
        while (m_ActiveScene.FindEntityByName(name))
            name = "Entity " + std::to_string(++suffix);
    }

    const Engine::Entity entity = m_ActiveScene.CreateEntity(std::move(name));
    if (!entity)
        return {};

    m_SelectedEntity = entity;
    RecordHistory("Create entity", before);
    return entity;
}

bool EditorLayer::DeleteSelectedEntity()
{
    if (!m_ActiveScene.IsEntityValid(m_SelectedEntity)
        || m_SelectedEntity == m_ActiveScene.GetMainCameraEntity())
        return false;

    const HistoryState before = CaptureHistoryState();
    const Engine::Entity deletedEntity = m_SelectedEntity;
    const Engine::SceneEntity* sceneEntity = m_ActiveScene.TryGetEntity(deletedEntity);
    const std::string name = sceneEntity ? sceneEntity->Name : "entity";
    if (!m_ActiveScene.DestroyEntity(deletedEntity))
        return false;

    if (deletedEntity == m_PrototypeMeshEntity)
        m_PrototypeMeshEntity = {};
    if (deletedEntity == m_DirectionalLightEntity)
        m_DirectionalLightEntity = {};
    if (deletedEntity == m_PlayerStartEntity)
        m_PlayerStartEntity = {};
    m_SelectedEntity = m_ActiveScene.GetMainCameraEntity();
    RecordHistory("Delete " + name, before);
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

    Engine::AssetHandle defaultMeshAsset = Engine::kInvalidAssetHandle;
    std::string defaultMeshError;
    if (!Engine::EnsureDefaultSceneMeshArtifact(loadedRegistry, defaultMeshAsset, defaultMeshError))
    {
        Engine::Log::Error("Could not prepare default scene mesh artifact: ", defaultMeshError);
        m_ConsoleLines.emplace_back("Project load failed: default scene mesh artifact");
        return false;
    }
    const bool defaultTextureWasMissing = loadedRegistry.FindAssetByPath(
        Engine::AssetType::Texture, Engine::GetDefaultSceneTextureSourcePath())
        == Engine::kInvalidAssetHandle;
    Engine::AssetHandle defaultTextureAsset = Engine::kInvalidAssetHandle;
    std::string defaultTextureError;
    if (!Engine::EnsureDefaultSceneTextureArtifact(
        loadedRegistry, defaultTextureAsset, defaultTextureError))
    {
        Engine::Log::Error("Could not prepare default scene texture artifact: ", defaultTextureError);
        m_ConsoleLines.emplace_back("Project load failed: default scene texture artifact");
        return false;
    }

    const std::filesystem::path prototypeMaterialPath =
        std::filesystem::path(manifest.AssetRegistryPath).parent_path() / "PrototypeDefault.spiralmat";
    const Engine::AssetHandle prototypeMaterialAsset = loadedRegistry.FindAssetByPath(
        Engine::AssetType::Material, prototypeMaterialPath.string());
    Engine::MaterialAsset* prototypeMaterial = loadedMaterials.Get(prototypeMaterialAsset);
    const bool upgradePrototypeMaterial = prototypeMaterial
        && prototypeMaterial->Textures.BaseColor == Engine::kInvalidAssetHandle;
    if (upgradePrototypeMaterial)
    {
        prototypeMaterial->Textures.BaseColor = defaultTextureAsset;
        prototypeMaterial->Samplers.BaseColor = Engine::MaterialTextureSampler::LinearWrap;
        if (!loadedMaterials.Save(prototypeMaterialAsset,
            Engine::AssetFileSystem::ResolvePath(prototypeMaterialPath.string())))
        {
            Engine::Log::Error("Could not upgrade the prototype material with its default texture");
            return false;
        }
    }
    if (defaultTextureWasMissing && !loadedRegistry.SaveToFile(manifest.AssetRegistryPath))
    {
        Engine::Log::Error("Could not persist the default scene texture registration");
        return false;
    }

    m_ScenePath = std::move(manifest.ScenePath);
    m_AssetRegistryPath = std::move(manifest.AssetRegistryPath);
    m_ProjectFramePacingPolicy = manifest.FramePacingPolicy;
    m_ProjectPresentationPolicy = manifest.PresentationPolicy;
    m_ProjectColorPipelineSettings = manifest.ColorPipelineSettings;
    PublishFramePacingPolicy();
    PublishPresentationPolicy();
    PublishColorPipelineSettings();
    m_AssetRegistry = std::move(loadedRegistry);
    m_MaterialLibrary = std::move(loadedMaterials);
    Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);
    m_ActiveScene = std::move(loadedScene);
    m_PrototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
    m_DirectionalLightEntity = m_ActiveScene.FindEntityByName("Directional Light");
    m_PlayerStartEntity = m_ActiveScene.FindEntityByName("Player Start");
    m_SelectedEntity = m_PrototypeMeshEntity ? m_PrototypeMeshEntity : m_ActiveScene.GetMainCameraEntity();
    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    SyncEditorCameraStateFromMainCamera(true);
    ResetFusionNavigationPivotFromSelectionOrScene();

    Engine::Log::Info("Project loaded: ", m_ProjectPath);
    Engine::Log::Info("Frame pacing policy loaded: ",
        Engine::DescribeFramePacingPolicy(m_GameFramePacingSettings.Resolve(m_ProjectFramePacingPolicy)));
    m_ConsoleLines.emplace_back("Project loaded: " + m_ProjectPath);
    m_EditorMaterialControl.EnsureProjectIdentity(m_ProjectPath);
    return true;
}

EditorLayer::HistoryState EditorLayer::CaptureHistoryState() const
{
    HistoryState state;
    state.Scene = m_ActiveScene;
    state.AssetRegistry = m_AssetRegistry;
    state.MaterialLibrary = m_MaterialLibrary;
    state.SelectedEntity = m_SelectedEntity;
    state.CameraPosition = m_CameraPosition;
    state.CameraRotation = m_CameraRotation;
    state.CameraFovDegrees = m_CameraFovDegrees;
    state.CameraNearClip = m_CameraNearClip;
    state.CameraFarClip = m_CameraFarClip;
    state.ProjectColorPipelineSettings = m_ProjectColorPipelineSettings;
    return state;
}

bool EditorLayer::RestoreHistoryState(const HistoryState& state)
{
    if (!Engine::IsValidRendererColorPipelineSettings(state.ProjectColorPipelineSettings)
        || !Engine::Renderer::SetColorPipelineSettings(state.ProjectColorPipelineSettings))
    {
        Engine::Log::Error("History restore rejected invalid project color pipeline settings");
        return false;
    }

    m_ActiveScene = state.Scene;
    m_AssetRegistry = state.AssetRegistry;
    m_MaterialLibrary = state.MaterialLibrary;
    Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);
    m_SelectedEntity = state.SelectedEntity;
    m_CameraPosition = state.CameraPosition;
    m_CameraRotation = state.CameraRotation;
    m_CameraFovDegrees = state.CameraFovDegrees;
    m_CameraNearClip = state.CameraNearClip;
    m_CameraFarClip = state.CameraFarClip;
    m_ProjectColorPipelineSettings = state.ProjectColorPipelineSettings;
    m_PrototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
    m_DirectionalLightEntity = m_ActiveScene.FindEntityByName("Directional Light");
    m_PlayerStartEntity = m_ActiveScene.FindEntityByName("Player Start");
    if (!m_ActiveScene.IsEntityValid(m_SelectedEntity))
        m_SelectedEntity = m_PrototypeMeshEntity ? m_PrototypeMeshEntity : m_ActiveScene.GetMainCameraEntity();

    SyncEditorCameraStateFromMainCamera(true);
    ResetFusionNavigationPivotFromSelectionOrScene();
    m_AssetWatcher.SyncRegistry(m_AssetRegistry);
    return true;
}

void EditorLayer::RecordHistory(std::string label, HistoryState before)
{
    m_RedoHistory.clear();
    m_UndoHistory.push_back({ std::move(label), std::move(before), CaptureHistoryState() });
    constexpr std::size_t maxHistoryEntries = 128;
    if (m_UndoHistory.size() > maxHistoryEntries)
        m_UndoHistory.erase(m_UndoHistory.begin());
}

bool EditorLayer::Undo()
{
    if (m_UndoHistory.empty())
        return false;

    HistoryEntry entry = m_UndoHistory.back();
    if (!RestoreHistoryState(entry.Before))
        return false;
    m_UndoHistory.pop_back();
    m_ConsoleLines.emplace_back("Undid: " + entry.Label);
    m_RedoHistory.push_back(std::move(entry));
    return true;
}

bool EditorLayer::Redo()
{
    if (m_RedoHistory.empty())
        return false;

    HistoryEntry entry = m_RedoHistory.back();
    if (!RestoreHistoryState(entry.After))
        return false;
    m_RedoHistory.pop_back();
    m_ConsoleLines.emplace_back("Redid: " + entry.Label);
    m_UndoHistory.push_back(std::move(entry));
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
        Engine::Renderer::PublishArtifactResolvers(m_AssetRegistry, m_MaterialLibrary);
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
    Engine::AssetHandle prototypeMeshAsset = Engine::kInvalidAssetHandle;
    std::string prototypeMeshError;
    if (!Engine::EnsureDefaultSceneMeshArtifact(m_AssetRegistry, prototypeMeshAsset, prototypeMeshError))
        throw std::runtime_error("could not publish the default scene mesh artifact: " + prototypeMeshError);
    Engine::AssetHandle prototypeTextureAsset = Engine::kInvalidAssetHandle;
    std::string prototypeTextureError;
    if (!Engine::EnsureDefaultSceneTextureArtifact(
        m_AssetRegistry, prototypeTextureAsset, prototypeTextureError))
        throw std::runtime_error("could not publish the default scene texture artifact: " + prototypeTextureError);
    const std::filesystem::path prototypeMaterialPath = std::filesystem::path(m_AssetRegistryPath).parent_path() / "PrototypeDefault.spiralmat";
    const Engine::AssetHandle prototypeMaterialAsset = m_AssetRegistry.RegisterAsset(
        Engine::AssetType::Material,
        prototypeMaterialPath.string(),
        "Prototype Default");
    if (!m_MaterialLibrary.Get(prototypeMaterialAsset))
    {
        const std::filesystem::path materialPath = Engine::AssetFileSystem::ResolvePath(prototypeMaterialPath.string());
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
    if (Engine::MaterialAsset* prototypeMaterial = m_MaterialLibrary.Get(prototypeMaterialAsset);
        prototypeMaterial && prototypeMaterial->Textures.BaseColor == Engine::kInvalidAssetHandle)
    {
        prototypeMaterial->Textures.BaseColor = prototypeTextureAsset;
        prototypeMaterial->Samplers.BaseColor = Engine::MaterialTextureSampler::LinearWrap;
        if (!SaveMaterialAsset(prototypeMaterialAsset))
            throw std::runtime_error("could not save the default scene material texture binding");
    }

    m_PrototypeMeshEntity = m_ActiveScene.FindEntityByName("Prototype Mesh");
    const bool createdPrototypeMesh = !m_PrototypeMeshEntity;
    if (!m_PrototypeMeshEntity)
        m_PrototypeMeshEntity = m_ActiveScene.CreateEntity("Prototype Mesh");
    if (createdPrototypeMesh)
    {
        if (Engine::TransformComponent* transform =
            m_ActiveScene.TryGetTransform(m_PrototypeMeshEntity))
            transform->RotationDegrees = { -18.0f, 28.0f, 0.0f };
    }
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
        light.PhotometricValue = 30000.0;
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
