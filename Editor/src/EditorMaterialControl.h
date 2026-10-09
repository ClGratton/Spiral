#pragma once

#include <Engine/Assets/FabImportReceipt.h>
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
    SetMeshRendererFlags,
    // Schema 5: viewport interaction. PickAtViewportPoint runs the same ray cast and
    // selection path as a viewport click; FocusSelection runs the same framing code
    // as the F key. Neither carries a path, a command string, or a pixel (the point is
    // normalized to the viewport image so it is independent of window size).
    PickAtViewportPoint,
    FocusSelection,
    // Schema 4: the Fab import workflow and project-level actions. Every one has a
    // fixed typed field set; none carries a filesystem path, URL, or credential.
    InspectFabImport,
    SelectFabPackage,
    SetFabProvenance,
    ConfirmFabProvenance,
    CommitFabImport,
    CancelFabImport,
    DismissFabImport,
    PlaceMeshAsset,
    SetEntityMeshRendererAssets,
    SaveProjectState,
    ValidateProject,
    SetFabPanelVisible,
    InspectFabPanel
};

inline bool IsFabControlAction(EditorMaterialControlAction action)
{
    return action >= EditorMaterialControlAction::InspectFabImport;
}

inline bool IsViewportControlAction(EditorMaterialControlAction action)
{
    return action == EditorMaterialControlAction::PickAtViewportPoint
        || action == EditorMaterialControlAction::FocusSelection;
}

// Attribution text is capped here so a provenance request fits the mailbox
// limit; the Editor UI keeps the receipt validator's full limit.
inline constexpr std::size_t kEditorFabControlMaximumAttributionBytes = 2 * 1024;

struct EditorFabControlRequest
{
    Engine::u64 ExpectedJobId = 0;
    bool HasExpectedJobId = false;
    // A single leaf inside the session's fab-inbox directory.
    std::string InboxName;
    // zip, glb, gltf, or directory.
    std::string ExpectedSourceKind;
    std::string ExpectedSourceSha256;

    std::string ProductIdentity;
    std::string ProductName;
    std::string Publisher;
    std::string VersionOrDownloadLabel;
    Engine::FabLicenseFamily LicenseFamily = Engine::FabLicenseFamily::Unknown;
    Engine::FabLicenseTier LicenseTier = Engine::FabLicenseTier::Unknown;
    std::string AttributionText;
    std::string AttributionLink;
    Engine::FabMetadataFlag NoAI = Engine::FabMetadataFlag::Unknown;
    Engine::FabMetadataFlag GeneratedWithAI = Engine::FabMetadataFlag::Unknown;
    Engine::FabRawSourcePolicy RawSourcePolicy = Engine::FabRawSourcePolicy::ExcludedFromProject;

    std::string ExpectedProvenanceDigest;
    std::string ExpectedGenerationId;
    std::string ExpectedRelation;

    // Compare-and-swap handles for SetEntityMeshRendererAssets and for the
    // optional CommitFabImport assignment (together with EntityId).
    Engine::AssetHandle ExpectedMeshAsset = Engine::kInvalidAssetHandle;
    Engine::AssetHandle ExpectedMaterialAsset = Engine::kInvalidAssetHandle;
    Engine::AssetHandle NewMeshAsset = Engine::kInvalidAssetHandle;
    Engine::AssetHandle NewMaterialAsset = Engine::kInvalidAssetHandle;
    Engine::AssetHandle MeshAsset = Engine::kInvalidAssetHandle;
    bool HasAssignment = false;

    std::string ExpectedManifestSha256;
    bool PanelVisible = false;
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
    // PickAtViewportPoint: a point of the viewport image, each coordinate in [0, 1],
    // origin top-left. FocusSelection: whether the camera eases or jumps.
    double ViewportNormalizedX = 0.0;
    double ViewportNormalizedY = 0.0;
    bool HasViewportPoint = false;
    bool FocusAnimate = false;
    bool HasFocusAnimation = false;
    EditorFabControlRequest Fab;
};

// Always present in a receipt. Fields default to the "unused" spelling that the
// formatter prints as none/0, so a non-Fab action carries an inert block.
struct EditorFabControlReceipt
{
    std::string State = "none";
    Engine::u64 JobId = 0;
    bool CancelRequested = false;
    Engine::u64 FilesCompleted = 0;
    Engine::u64 FileCount = 0;
    Engine::u64 BytesCompleted = 0;
    Engine::u64 BytesTotal = 0;
    std::string SourceKind = "none";
    std::string SourceOrigin = "none";
    std::string SourceName;
    std::string ErrorCode = "None";
    std::string Message;
    std::string LastRejection;
    std::string Note;
    std::string Format = "none";
    std::string SourceSha256;
    std::string ExpandedTreeSha256;
    Engine::u64 SummaryVertices = 0;
    Engine::u64 SummaryTriangles = 0;
    Engine::u64 SummaryPrimitives = 0;
    Engine::u64 SummaryTextures = 0;
    Engine::u64 SummaryFiles = 0;
    Engine::u64 SummaryBytes = 0;
    std::string SummaryMaterial;
    bool ProvenanceValid = false;
    bool ProvenanceConfirmed = false;
    std::string ProvenanceDigest;
    std::string ProvenanceError;
    std::string Relation = "none";
    std::string StreamId;
    std::string GenerationId;
    bool ProjectChanged = false;
    bool AssignmentApplied = false;
    std::string CommitOutcome = "none";
    Engine::u64 ManifestRevision = 0;
    std::string ManifestSha256;
    Engine::AssetHandle MeshAsset = Engine::kInvalidAssetHandle;
    Engine::AssetHandle MaterialAsset = Engine::kInvalidAssetHandle;
    Engine::u64 ResultHandleCount = 0;
    std::vector<Engine::AssetHandle> ResultHandles;
    Engine::u64 ProjectReceiptCount = 0;
    std::vector<Engine::AssetHandle> ProjectMeshAssets;
    std::vector<Engine::AssetHandle> ProjectMaterialAssets;
    std::string ProjectStructural = "none";
    std::string ProjectStructuralMessage;
    std::string ProjectValidation = "none";
    std::string ProjectValidationMessage;
    std::string PanelState = "none";
    bool PanelInitialized = false;
    bool PanelFailed = false;
    bool PanelVisible = false;
    bool PanelKeyboardOwnedByPage = false;
    bool PanelTextureValid = false;
    bool PanelLoading = false;
    Engine::u64 PanelFramesReceived = 0;
    Engine::u64 PanelFrameWidth = 0;
    Engine::u64 PanelFrameHeight = 0;
    Engine::u64 PanelNavigationDenials = 0;
    Engine::u64 PanelDownloadsCompleted = 0;
    std::string PanelHost;
    std::string PanelError;
};

// Always present in a receipt, like the Fab block: the schema-5 viewport block holds
// the "unused" spelling for every other action.
struct EditorViewportControlReceipt
{
    // PickAtViewportPoint
    std::string PickState = "none"; // none | hit | miss
    Engine::EntityId PickEntityId = Engine::kInvalidEntityId;
    double PickDistance = 0.0;
    std::string PickRefinement = "none"; // none | box | triangles
    Engine::u64 PickCandidates = 0;
    Engine::u64 PickBoxHits = 0;
    Engine::u64 PickTrianglesTested = 0;
    double PickNormalizedX = 0.0;
    double PickNormalizedY = 0.0;
    double PickPixelX = 0.0;
    double PickPixelY = 0.0;
    bool PickRectVirtual = false;
    // The viewport image rectangle and camera the pick used (virtual when headless).
    double RectX = 0.0;
    double RectY = 0.0;
    double RectWidth = 0.0;
    double RectHeight = 0.0;
    double RectAspect = 0.0;
    double RectFovDegrees = 0.0;
    // FocusSelection
    std::string FocusState = "none"; // none | framed
    std::string FocusSubject = "none"; // none | bounds | default-radius
    bool FocusAnimated = false;
    double FocusMargin = 0.0;
    double FocusBefore[6] {}; // camera position xyz, rotation pitch yaw roll
    double FocusAfter[6] {};
    double FocusCenter[3] {};
    double FocusRadius = 0.0;
    double FocusDistance = 0.0;
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
    EditorFabControlReceipt Fab;
    EditorViewportControlReceipt Viewport;
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
    static constexpr std::size_t MaximumFabResultHandles = 32;

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
    // The per-session directory the operator places packages in. A
    // SelectFabPackage request names one leaf of it, never a path.
    const std::filesystem::path& GetFabInboxPath() const { return m_FabInbox; }
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
    bool PublishFabControlTargetForSmoke(std::string_view contents, std::string& error);
    bool PublishViewportPickingTargetForSmoke(std::string_view contents, std::string& error);
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
    std::filesystem::path m_FabInbox;
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
