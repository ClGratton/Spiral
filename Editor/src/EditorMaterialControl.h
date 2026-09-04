#pragma once

#include <Engine/Assets/MaterialAsset.h>
#include <Engine/Core/Base.h>
#include <Engine/Renderer/ColorPipelineSettings.h>
#include <Engine/Renderer/SceneDebugVisualization.h>
#include <Engine/Scene/Components.h>
#include <Engine/Scene/Entity.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

enum class EditorMaterialControlAction
{
    InspectMaterialSurface,
    SelectEntityPatchMaterialSurface,
    InspectEntity,
    SelectEntity,
    SetEntityTransform,
    SetTypedLight,
    SetProjectColorPipeline,
    SetViewportMainCameraPose,
    SetSceneDebugVisualization,
    SetMeshRendererFlags
};

struct EditorMaterialControlRequest
{
    std::string RequestId;
    std::string SessionId;
    std::string ProjectPath;
    EditorMaterialControlAction Action = EditorMaterialControlAction::InspectMaterialSurface;
    Engine::EntityId EntityId = Engine::kInvalidEntityId;
    std::string ExpectedEntityName;
    Engine::AssetHandle MaterialHandle = Engine::kInvalidAssetHandle;
    Engine::MaterialSurface ExpectedSurface;
    Engine::MaterialSurface NewSurface;
    bool HasExpectedSurface = false;
    bool HasNewSurface = false;
    bool SharedMaterialScope = false;
    Engine::TransformComponent ExpectedTransform;
    Engine::TransformComponent NewTransform;
    Engine::Math::SectorLocalPosition ExpectedTransformPosition;
    Engine::Math::SectorLocalPosition NewTransformPosition;
    bool HasExpectedTransform = false;
    bool HasNewTransform = false;
    Engine::LightComponent ExpectedLight;
    Engine::LightComponent NewLight;
    bool HasExpectedLight = false;
    bool HasNewLight = false;
    Engine::RendererColorPipelineSettings ExpectedColorPipeline;
    Engine::RendererColorPipelineSettings NewColorPipeline;
    bool HasExpectedColorPipeline = false;
    bool HasNewColorPipeline = false;
    Engine::SceneDebugView ExpectedDebugView = Engine::SceneDebugView::Lit;
    Engine::SceneDebugView NewDebugView = Engine::SceneDebugView::Lit;
    bool ExpectedShowSelectedBounds = true;
    bool NewShowSelectedBounds = true;
    bool HasExpectedDebugVisualization = false;
    bool HasNewDebugVisualization = false;
    bool ExpectedMeshVisible = true;
    bool ExpectedMeshCastsShadows = true;
    bool NewMeshVisible = true;
    bool NewMeshCastsShadows = true;
    bool HasExpectedMeshRendererFlags = false;
    bool HasNewMeshRendererFlags = false;
    Engine::EntityId ExpectedSelectedEntityId = Engine::kInvalidEntityId;
    bool HasExpectedSelectedEntityId = false;
};

struct EditorMaterialControlReceipt
{
    std::string RequestId;
    std::string SessionId;
    std::string ProjectPath;
    std::string RequestDigest;
    EditorMaterialControlAction Action = EditorMaterialControlAction::InspectMaterialSurface;
    bool ActionKnown = false;
    bool Succeeded = false;
    std::string Reason;
    Engine::u64 Frame = 0;
    std::string Effect = "None";
    std::string Recovery = "None";
    Engine::EntityId EntityId = Engine::kInvalidEntityId;
    std::string EntityName;
    Engine::EntityId MainCameraEntityId = Engine::kInvalidEntityId;
    bool IsMainCamera = false;
    Engine::EntityId SelectedEntityIdBefore = Engine::kInvalidEntityId;
    Engine::EntityId SelectedEntityIdAfter = Engine::kInvalidEntityId;
    Engine::AssetHandle MaterialHandle = Engine::kInvalidAssetHandle;
    Engine::MaterialSurface Before;
    Engine::MaterialSurface After;
    std::size_t AffectedEntityCount = 0;
    std::vector<Engine::EntityId> AffectedEntityIds;
    bool AffectedEntityIdsTruncated = false;
    Engine::u64 RendererGeneration = 0;
    std::size_t UndoDepthBefore = 0;
    std::size_t UndoDepthAfter = 0;
    std::size_t RedoDepthBefore = 0;
    std::size_t RedoDepthAfter = 0;
    bool SelectionCommitted = false;
    bool PivotRetargeted = false;
    bool RendererReadbackVerified = false;
    std::string Persistence = "SessionOnly";
    bool Saved = false;
    Engine::TransformComponent BeforeTransform;
    Engine::TransformComponent AfterTransform;
    bool BeforeCameraPresent = false;
    bool AfterCameraPresent = false;
    Engine::CameraComponent BeforeCamera;
    Engine::CameraComponent AfterCamera;
    bool BeforeLightPresent = false;
    bool AfterLightPresent = false;
    Engine::LightComponent BeforeLight;
    Engine::LightComponent AfterLight;
    bool BeforeMeshRendererPresent = false;
    bool AfterMeshRendererPresent = false;
    Engine::MeshRendererComponent BeforeMeshRenderer;
    Engine::MeshRendererComponent AfterMeshRenderer;
    Engine::RendererColorPipelineSettings BeforeColorPipeline;
    Engine::RendererColorPipelineSettings AfterColorPipeline;
    Engine::SceneDebugView BeforeDebugView = Engine::SceneDebugView::Lit;
    Engine::SceneDebugView AfterDebugView = Engine::SceneDebugView::Lit;
    bool BeforeShowSelectedBounds = true;
    bool AfterShowSelectedBounds = true;
    Engine::u64 DebugVisualizationGeneration = 0;
    bool PostconditionVerified = false;
    bool RollbackVerified = false;
    bool EditorCameraSynchronized = false;
};

struct EditorMaterialControlTransaction
{
    EditorMaterialControlReceipt Receipt;
    bool Mutating = false;
    std::function<bool(std::string&)> Commit;
    std::function<bool(EditorMaterialControlReceipt&)> Rollback;
};

class EditorMaterialControlMailbox
{
public:
    static constexpr std::size_t MaximumRequestBytes = 16 * 1024;
    static constexpr std::size_t MaximumRequestsPerFrame = 4;
    static constexpr std::size_t MaximumEntriesScannedPerFrame = 32;
    static constexpr std::size_t MaximumTerminalRequests = 256;
    static constexpr std::size_t MaximumAffectedEntityIds = 32;
    static constexpr std::size_t MaximumResponseBytes = 64 * 1024;

    using Handler = std::function<EditorMaterialControlTransaction(
        const EditorMaterialControlRequest&, Engine::u64)>;

    bool Initialize(const std::filesystem::path& root,
        const std::filesystem::path& projectPath, std::string& error);
    void Close();
    void Drain(Engine::u64 frame, const Handler& handler);

    bool IsOpen() const { return !m_Root.empty(); }
    const std::filesystem::path& GetRoot() const { return m_Root; }
    const std::string& GetSessionId() const { return m_SessionId; }
    std::size_t GetResponseCollisionCount() const { return m_ResponseCollisionCount; }
    Engine::u64 GetDirectoryPollCount() const { return m_DirectoryPollCount; }
    Engine::u64 GetCadenceSkipCount() const { return m_CadenceSkipCount; }
    Engine::u64 GetDurabilityDegradationCount() const
    {
        return m_DurabilityDegradationCount;
    }
    std::size_t GetTerminalCount() const { return m_Terminals.size(); }
    bool IsAcceptingRequests() const { return m_AcceptingRequests; }
    bool EnsureProjectIdentity(const std::filesystem::path& projectPath);
    const EditorMaterialControlReceipt* FindTerminalReceipt(std::string_view requestId) const;
    const std::string* FindTerminalText(std::string_view requestId) const;

    bool PublishRequestForSmoke(
        std::string_view requestId, std::string_view contents, std::string& error);
    bool PublishRawResponseForSmoke(
        std::string_view requestId, std::string_view contents, std::string& error);
    bool PublishLiveTargetForSmoke(Engine::EntityId entityId,
        std::string_view entityName, Engine::AssetHandle materialHandle,
        const Engine::MaterialSurface& before,
        const Engine::MaterialSurface& after, std::string& error);
    bool PublishSceneControlTargetForSmoke(std::string_view contents, std::string& error);
    void InjectParentDirectorySyncFailureForSmoke()
    {
        m_ForceParentDirectorySyncFailureOnce = true;
    }

    static std::string FormatInspectRequest(std::string_view requestId,
        std::string_view sessionId, std::string_view projectPath,
        Engine::EntityId entityId,
        std::string_view expectedEntityName, Engine::AssetHandle materialHandle);
    static std::string FormatInspectEntityRequest(std::string_view requestId,
        std::string_view sessionId, std::string_view projectPath,
        Engine::EntityId entityId, std::string_view expectedEntityName);
    static std::string FormatPatchRequest(std::string_view requestId,
        std::string_view sessionId, std::string_view projectPath,
        Engine::EntityId entityId,
        std::string_view expectedEntityName, Engine::AssetHandle materialHandle,
        const Engine::MaterialSurface& expectedSurface,
        const Engine::MaterialSurface& newSurface);

private:
    struct TerminalEntry
    {
        std::string RequestId;
        std::string RequestDigest;
        std::string RequestBytes;
        EditorMaterialControlReceipt Receipt;
        std::string Text;
        bool RequestReplayable = true;
    };

    bool PublishFileNoReplace(const std::filesystem::path& temporary,
        const std::filesystem::path& destination, std::string_view purpose,
        bool closeOnDurabilityDegradation, std::string& error);
    bool PublishSessionFile(std::string_view state, std::string& error);
    bool PublishResponse(const TerminalEntry& terminal, bool allowExisting, std::string& error);
    bool PublishRecoveryResponse(const TerminalEntry& terminal, std::string& error);
    bool PublishCollisionReceipt(std::string_view requestId, std::string& error);
    bool StageResponse(std::string_view requestId, std::string_view text,
        std::filesystem::path& temporary, std::string& error);
    bool RequeueClaimedRequest(const std::filesystem::path& claimed,
        std::string_view requestId, std::string_view requestBytes, std::string& error);
    void TransitionToClosed(std::string_view reason);
    void ProcessRequest(const std::filesystem::path& path,
        Engine::u64 frame, const Handler& handler);

private:
    std::filesystem::path m_Root;
    std::filesystem::path m_Requests;
    std::filesystem::path m_Responses;
    std::string m_SessionId;
    std::string m_ProjectPath;
    Engine::u64 m_ProcessId = 0;
    std::vector<TerminalEntry> m_Terminals;
    std::size_t m_ResponseCollisionCount = 0;
    Engine::u64 m_TemporarySequence = 0;
    Engine::u64 m_DirectoryPollCount = 0;
    Engine::u64 m_CadenceSkipCount = 0;
    Engine::u64 m_DurabilityDegradationCount = 0;
    bool m_AcceptingRequests = false;
    bool m_ClosedPublished = false;
    bool m_ImmediatePoll = true;
    bool m_ForceParentDirectorySyncFailureOnce = false;
    std::chrono::steady_clock::time_point m_NextDirectoryPoll {};
};
