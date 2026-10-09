// EditorLayer members for the selection path, viewport click-to-select, camera
// framing (F, Home, View > Frame), and the typed PickAtViewportPoint and
// FocusSelection mailbox actions. They share the pure math in
// Viewport/PickingMath so a click, a key press, and a typed request run one
// implementation. EditorLayer.cpp only carries the hook calls.
#include "EditorLayer.h"

#include "Viewport/PickingMath.h"

#include "Engine/Assets/MeshArtifact.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Picking = SpiralEditor::Picking;

namespace
{
    // A click is a press and release that stays within this movement and time.
    constexpr double kClickMaximumMovementPixels = 4.0;
    constexpr double kClickMaximumSeconds = 0.4;
    // F eases the camera over this long (ease-out cubic).
    constexpr double kFocusAnimationSeconds = 0.18;
    // The virtual viewport rectangle used when there is no live viewport image
    // (headless runs, or the Viewport panel is closed). 16:9, origin at the top-left.
    constexpr double kVirtualViewportWidth = 1280.0;
    constexpr double kVirtualViewportHeight = 720.0;
    // GLFW key codes the Engine key events carry.
    constexpr int kKeyEscape = 256;
    constexpr int kKeyHome = 268;
    constexpr size_t kMaximumCachedPickMeshes = 8;
    constexpr size_t kMaximumCachedPickTriangles = 2000000;

    Picking::ViewCamera MakeViewCamera(const Engine::Math::Vec3& rotation, double fovDegrees, double aspect)
    {
        Picking::ViewCamera camera;
        camera.RotationDegrees = rotation;
        camera.VerticalFovDegrees = fovDegrees;
        camera.AspectRatio = aspect;
        return camera;
    }
}

struct EditorLayer::ViewportPickGeometry
{
    Picking::ViewportRect Rect;
    double Aspect = 16.0 / 9.0;
    double FovDegrees = 60.0;
    bool Virtual = true;
};

struct EditorLayer::ViewportPickReport
{
    bool Valid = false;
    bool Hit = false;
    Engine::EntityId Entity = Engine::kInvalidEntityId;
    double Distance = 0.0;
    bool Refined = false;
    size_t Candidates = 0;
    size_t BoxHits = 0;
    size_t TrianglesTested = 0;
    double PixelX = 0.0;
    double PixelY = 0.0;
    ViewportPickGeometry Geometry;
};

struct EditorLayer::FocusPlan
{
    Engine::Math::DVec3 CameraTarget {};
    Engine::Math::DVec3 Center {};
    double Radius = 0.0;
    double Distance = 0.0;
    bool DefaultRadius = false;
};

// Immutable picking geometry of one cooked mesh: object-space bounds plus, when the
// mesh is within the triangle budget, its triangle list.
struct EditorLayer::PickMeshData
{
    Engine::AssetHandle Asset = Engine::kInvalidAssetHandle;
    std::string CookedRoot;
    Picking::LocalBox Bounds;
    size_t TriangleCount = 0;
    bool HasTriangles = false;
    std::vector<float> Positions;
    std::vector<Engine::u32> Indices;

    Picking::TriangleMeshView View() const
    {
        return { Positions.data(), Positions.size() / 3, Indices.data(), Indices.size() };
    }
};

std::shared_ptr<const EditorLayer::PickMeshData> EditorLayer::GetPickMesh(Engine::AssetHandle mesh)
{
    const Engine::AssetMetadata* metadata = m_AssetRegistry.GetAsset(mesh);
    if (!metadata || metadata->Type != Engine::AssetType::Mesh)
        return nullptr;
    // An empty cooked root is the legacy mutable location: never cache it across
    // picks, but share one load between the entities of a single pick.
    const bool cacheable = !metadata->CookedRoot.empty();
    for (const auto& entry : cacheable ? m_PickMeshCache : m_PickMeshScratch)
    {
        if (entry->Asset == mesh && entry->CookedRoot == metadata->CookedRoot)
            return entry;
    }

    Engine::MeshArtifact artifact;
    Engine::MeshArtifactBounds bounds;
    std::string error;
    if (!Engine::ResolveMeshArtifact(m_AssetRegistry, mesh, artifact, error)
        || !Engine::ComputeMeshArtifactBounds(artifact, bounds, error))
        return nullptr;

    auto data = std::make_shared<PickMeshData>();
    data->Asset = mesh;
    data->CookedRoot = metadata->CookedRoot;
    for (int axis = 0; axis < 3; ++axis)
    {
        data->Bounds.Min[axis] = bounds.Min[axis];
        data->Bounds.Max[axis] = bounds.Max[axis];
    }
    for (const Engine::MeshArtifactPrimitive& primitive : artifact.Primitives)
        data->TriangleCount += static_cast<size_t>(primitive.IndexByteSize / sizeof(Engine::u32)) / 3;
    if (data->TriangleCount > 0 && data->TriangleCount <= Picking::kDefaultTriangleBudget)
    {
        data->HasTriangles = true;
        data->Positions.reserve(artifact.Vertices.size() * 3);
        for (const Engine::MeshArtifactVertex& vertex : artifact.Vertices)
            data->Positions.insert(data->Positions.end(), vertex.Position, vertex.Position + 3);
        data->Indices.reserve(data->TriangleCount * 3);
        for (const Engine::MeshArtifactPrimitive& primitive : artifact.Primitives)
        {
            const size_t first = static_cast<size_t>(primitive.IndexByteOffset / sizeof(Engine::u32));
            const size_t count = static_cast<size_t>(primitive.IndexByteSize / sizeof(Engine::u32)) / 3 * 3;
            data->Indices.insert(data->Indices.end(), artifact.Indices.begin() + static_cast<std::ptrdiff_t>(first),
                artifact.Indices.begin() + static_cast<std::ptrdiff_t>(first + count));
        }
    }

    std::shared_ptr<const PickMeshData> result = data;
    if (cacheable)
    {
        m_PickMeshCache.push_back(result);
        size_t triangles = 0;
        for (const auto& entry : m_PickMeshCache)
            triangles += entry->HasTriangles ? entry->TriangleCount : 0;
        while (m_PickMeshCache.size() > 1
            && (m_PickMeshCache.size() > kMaximumCachedPickMeshes || triangles > kMaximumCachedPickTriangles))
        {
            triangles -= m_PickMeshCache.front()->HasTriangles ? m_PickMeshCache.front()->TriangleCount : 0;
            m_PickMeshCache.erase(m_PickMeshCache.begin());
        }
    }
    else
    {
        m_PickMeshScratch.push_back(result);
    }
    return result;
}

bool EditorLayer::SetSelectedEntity(Engine::Entity entity, SelectionSource source)
{
    if (entity && !m_ActiveScene.IsEntityValid(entity))
        return false;

    const bool changed = entity != m_SelectedEntity;
    m_SelectedEntity = entity;
    if (entity)
    {
        // A transform or pivot edit is not an undo entry and neither is selection.
        RetargetFusionNavigationPivotToSelectedEntity();
        if (source != SelectionSource::Hierarchy)
            m_HierarchyScrollRequest = true;
    }
    else
    {
        m_HierarchyScrollRequest = false;
    }
    return changed;
}

bool EditorLayer::ClearSelection(SelectionSource source)
{
    return SetSelectedEntity(Engine::Entity {}, source);
}

EditorLayer::ViewportPickGeometry EditorLayer::GetViewportPickGeometry() const
{
    ViewportPickGeometry geometry;
    geometry.FovDegrees = m_EditorCamera.GetProjection().VerticalFovDegrees;
    if (m_ViewportImageValid)
    {
        geometry.Rect = { m_ViewportImageX, m_ViewportImageY, m_ViewportImageWidth, m_ViewportImageHeight };
        geometry.Aspect = m_EditorCamera.GetAspectRatio();
        geometry.Virtual = false;
    }
    else
    {
        geometry.Rect = { 0.0, 0.0, kVirtualViewportWidth, kVirtualViewportHeight };
        geometry.Aspect = kVirtualViewportWidth / kVirtualViewportHeight;
        geometry.Virtual = true;
    }
    return geometry;
}

EditorLayer::ViewportPickReport EditorLayer::PickAtViewportPixel(double pixelX, double pixelY)
{
    ViewportPickReport report;
    report.Geometry = GetViewportPickGeometry();
    report.PixelX = pixelX;
    report.PixelY = pixelY;

    const Picking::ViewCamera camera = MakeViewCamera(
        m_EditorCamera.GetRotationDegrees(), report.Geometry.FovDegrees, report.Geometry.Aspect);
    Engine::Math::DVec3 direction;
    if (!Picking::ViewportPixelToRayDirection(report.Geometry.Rect, pixelX, pixelY, camera, direction))
        return report;
    report.Valid = true;

    m_PickMeshScratch.clear();
    std::vector<Picking::PickCandidate> candidates;
    std::vector<Picking::TriangleMeshView> views;
    const std::vector<Engine::SceneEntity>& entities = m_ActiveScene.GetEntities();
    candidates.reserve(entities.size());
    views.reserve(entities.size());
    for (size_t index = 0; index < entities.size(); ++index)
    {
        const Engine::SceneEntity& entity = entities[index];
        if (!entity.MeshRenderer || !entity.MeshRenderer->Visible
            || entity.MeshRenderer->MeshAsset == Engine::kInvalidAssetHandle)
            continue;
        const std::shared_ptr<const PickMeshData> mesh = GetPickMesh(entity.MeshRenderer->MeshAsset);
        if (!mesh)
            continue;

        Picking::PickCandidate candidate;
        candidate.EntityId = entity.EntityHandle.Id;
        candidate.Order = static_cast<Engine::u32>(index);
        candidate.Position = entity.Transform.GetPosition();
        candidate.RotationDegrees = entity.Transform.RotationDegrees;
        candidate.Scale = entity.Transform.Scale;
        candidate.Box = mesh->Bounds;
        if (mesh->HasTriangles)
        {
            views.push_back(mesh->View());
            candidate.Triangles = &views.back();
        }
        candidates.push_back(candidate);
    }

    const Picking::PickResult result = Picking::PickNearest(
        m_ActiveScene.GetMainCameraTransform().GetPosition(), direction,
        m_ActiveScene.GetWorldGridPolicy(), candidates, {});
    report.Hit = result.Hit;
    report.Entity = result.Hit ? result.EntityId : Engine::kInvalidEntityId;
    report.Distance = result.Distance;
    report.Refined = result.Refined;
    report.Candidates = result.Candidates;
    report.BoxHits = result.BoxHits;
    report.TrianglesTested = result.TrianglesTested;
    m_PickMeshScratch.clear();
    return report;
}

EditorLayer::ViewportPickReport EditorLayer::PickAtViewportNormalized(double normalizedX, double normalizedY)
{
    const ViewportPickGeometry geometry = GetViewportPickGeometry();
    double pixelX = 0.0;
    double pixelY = 0.0;
    if (!Picking::NormalizedToViewportPixel(geometry.Rect, normalizedX, normalizedY, pixelX, pixelY))
    {
        ViewportPickReport report;
        report.Geometry = geometry;
        return report;
    }
    return PickAtViewportPixel(pixelX, pixelY);
}

void EditorLayer::BeginViewportClick()
{
    m_ViewportClick = {};
    if (!m_ViewportPickAvailable || !m_WindowFocused || m_RightMouseDown || m_MiddleMouseDown
        || m_CursorCaptured)
        return;

    if (m_ViewportClickCursorOverride)
    {
        m_ViewportClick.PressX = (*m_ViewportClickCursorOverride)[0];
        m_ViewportClick.PressY = (*m_ViewportClickCursorOverride)[1];
    }
    else
    {
        Engine::Window& window = Engine::Application::Get().GetWindow();
        window.GetCursorPosition(m_ViewportClick.PressX, m_ViewportClick.PressY);
    }
    m_ViewportClick.LastX = m_ViewportClick.PressX;
    m_ViewportClick.LastY = m_ViewportClick.PressY;
    m_ViewportClick.PressTime = std::chrono::steady_clock::now();
    m_ViewportClick.Armed = true;
}

void EditorLayer::TrackViewportClickMotion(double x, double y)
{
    if (!m_ViewportClick.Armed)
        return;

    // While the cursor is captured the visible cursor is parked, so only the virtual
    // motion the navigation code accepts counts; otherwise follow the visible cursor.
    if (m_CursorCaptured)
    {
        if (m_HasMousePosition && m_CursorCaptureBaselineArmed)
            m_ViewportClick.Movement += std::hypot(x - m_MouseX, y - m_MouseY);
    }
    else
    {
        m_ViewportClick.Movement += std::hypot(x - m_ViewportClick.LastX, y - m_ViewportClick.LastY);
        m_ViewportClick.LastX = x;
        m_ViewportClick.LastY = y;
    }
    if (m_ViewportClick.Movement >= kClickMaximumMovementPixels)
        m_ViewportClick.Armed = false;
}

void EditorLayer::CancelViewportClick()
{
    m_ViewportClick.Armed = false;
}

void EditorLayer::FinishViewportClick()
{
    if (!m_ViewportClick.Armed)
        return;
    m_ViewportClick.Armed = false;

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_ViewportClick.PressTime).count();
    if (elapsed >= kClickMaximumSeconds || m_ViewportClick.Movement >= kClickMaximumMovementPixels
        || m_RightMouseDown || m_MiddleMouseDown || !m_WindowFocused)
        return;

    const ViewportPickReport report = PickAtViewportPixel(m_ViewportClick.PressX, m_ViewportClick.PressY);
    if (!report.Valid)
        return;
    if (report.Hit)
        SetSelectedEntity(Engine::Entity { report.Entity }, SelectionSource::Viewport);
    else
        ClearSelection(SelectionSource::Viewport);
}

bool EditorLayer::CollectFramePoints(const Engine::SceneEntity& entity, bool visibleMeshesOnly,
    std::vector<Engine::Math::DVec3>& points, bool& usedBounds)
{
    usedBounds = false;
    const Engine::Math::SectorLocalPosition cameraPosition = m_ActiveScene.GetMainCameraTransform().GetPosition();
    Engine::Math::DVec3 relative;
    if (!Engine::Math::TryGetSectorLocalRelativePosition(
            entity.Transform.GetPosition(), cameraPosition, m_ActiveScene.GetWorldGridPolicy(), relative))
        return false;

    if (entity.MeshRenderer && entity.MeshRenderer->MeshAsset != Engine::kInvalidAssetHandle
        && (!visibleMeshesOnly || entity.MeshRenderer->Visible))
    {
        if (const std::shared_ptr<const PickMeshData> mesh = GetPickMesh(entity.MeshRenderer->MeshAsset))
        {
            Picking::LinearMap map;
            if (Picking::MakeLinearMap(entity.Transform.RotationDegrees, entity.Transform.Scale, map))
            {
                Engine::Math::DVec3 corners[8];
                Picking::OrientedBoxCorners(relative, map, mesh->Bounds, corners);
                points.insert(points.end(), corners, corners + 8);
                usedBounds = true;
                return true;
            }
        }
    }
    if (visibleMeshesOnly)
        return false;

    // No mesh bounds: a sphere of the default radius around the entity position.
    const double radius = Picking::kDefaultFramingRadius;
    points.push_back({ relative.X - radius, relative.Y, relative.Z });
    points.push_back({ relative.X + radius, relative.Y, relative.Z });
    points.push_back({ relative.X, relative.Y - radius, relative.Z });
    points.push_back({ relative.X, relative.Y + radius, relative.Z });
    points.push_back({ relative.X, relative.Y, relative.Z - radius });
    points.push_back({ relative.X, relative.Y, relative.Z + radius });
    return true;
}

namespace
{
    // Solves the framing for points relative to the current camera position.
    bool SolveFocusPlan(const std::vector<Engine::Math::DVec3>& points,
        const std::array<double, 3>& cameraPosition, const Engine::Math::Vec3& rotation,
        double fovDegrees, double aspect, double nearClip, Picking::FramingSolution& outSolution)
    {
        Picking::BoundingSphere sphere;
        Picking::CameraBasis basis;
        return Picking::EnclosingSphere(points, sphere) && Picking::ComputeCameraBasis(rotation, basis)
            && Picking::SolveFraming(sphere, basis, fovDegrees, aspect, nearClip,
                Picking::kFramingMargin, outSolution)
            && std::isfinite(cameraPosition[0]) && std::isfinite(cameraPosition[1])
            && std::isfinite(cameraPosition[2]);
    }
}

bool EditorLayer::PlanFocusForEntity(Engine::Entity entity, FocusPlan& outPlan, std::string& outError)
{
    const Engine::SceneEntity* target = m_ActiveScene.TryGetEntity(entity);
    if (!target)
    {
        outError = "entity_not_found";
        return false;
    }
    if (entity == m_ActiveScene.GetMainCameraEntity())
    {
        outError = "main_camera_cannot_be_framed";
        return false;
    }

    std::vector<Engine::Math::DVec3> points;
    bool usedBounds = false;
    if (!CollectFramePoints(*target, false, points, usedBounds))
    {
        outError = "entity_has_no_finite_world_position";
        return false;
    }
    const ViewportPickGeometry geometry = GetViewportPickGeometry();
    Picking::FramingSolution solution;
    if (!SolveFocusPlan(points, m_CameraPosition, m_EditorCamera.GetRotationDegrees(),
            geometry.FovDegrees, geometry.Aspect, m_EditorCamera.GetProjection().NearClip, solution))
    {
        outError = "framing_failed";
        return false;
    }

    outPlan.CameraTarget = { m_CameraPosition[0] + solution.CameraOffset.X,
        m_CameraPosition[1] + solution.CameraOffset.Y, m_CameraPosition[2] + solution.CameraOffset.Z };
    outPlan.Center = { m_CameraPosition[0] + solution.Subject.Center.X,
        m_CameraPosition[1] + solution.Subject.Center.Y, m_CameraPosition[2] + solution.Subject.Center.Z };
    outPlan.Radius = solution.Subject.Radius;
    outPlan.Distance = solution.Distance;
    outPlan.DefaultRadius = !usedBounds;
    return true;
}

bool EditorLayer::PlanFocusForAllMeshes(FocusPlan& outPlan, std::string& outError)
{
    m_PickMeshScratch.clear();
    std::vector<Engine::Math::DVec3> points;
    for (const Engine::SceneEntity& entity : m_ActiveScene.GetEntities())
    {
        bool usedBounds = false;
        if (entity.MeshRenderer && entity.MeshRenderer->Visible)
            CollectFramePoints(entity, true, points, usedBounds);
    }
    m_PickMeshScratch.clear();
    if (points.empty())
    {
        outError = "no_visible_meshes";
        return false;
    }
    const ViewportPickGeometry geometry = GetViewportPickGeometry();
    Picking::FramingSolution solution;
    if (!SolveFocusPlan(points, m_CameraPosition, m_EditorCamera.GetRotationDegrees(),
            geometry.FovDegrees, geometry.Aspect, m_EditorCamera.GetProjection().NearClip, solution))
    {
        outError = "framing_failed";
        return false;
    }
    outPlan.CameraTarget = { m_CameraPosition[0] + solution.CameraOffset.X,
        m_CameraPosition[1] + solution.CameraOffset.Y, m_CameraPosition[2] + solution.CameraOffset.Z };
    outPlan.Center = { m_CameraPosition[0] + solution.Subject.Center.X,
        m_CameraPosition[1] + solution.Subject.Center.Y, m_CameraPosition[2] + solution.Subject.Center.Z };
    outPlan.Radius = solution.Subject.Radius;
    outPlan.Distance = solution.Distance;
    outPlan.DefaultRadius = false;
    return true;
}

void EditorLayer::StartFocus(const FocusPlan& plan, bool animate)
{
    // The pivot moves to the framed center at once so a wheel or orbit gesture that
    // cancels the animation continues around the subject.
    SetFusionNavigationPivot(plan.Center);
    if (animate)
    {
        m_FocusAnimation.Active = true;
        m_FocusAnimation.Start = m_CameraPosition;
        m_FocusAnimation.Target = { plan.CameraTarget.X, plan.CameraTarget.Y, plan.CameraTarget.Z };
        m_FocusAnimation.Elapsed = 0.0;
        return;
    }

    CancelFocusAnimation();
    m_CameraPosition = { plan.CameraTarget.X, plan.CameraTarget.Y, plan.CameraTarget.Z };
    m_EditorCamera.SetPosition({ m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] });
    ApplyEditorCameraStateToScene();
    m_ViewportDiscontinuousRelocationPending = true;
}

void EditorLayer::CancelFocusAnimation()
{
    m_FocusAnimation = {};
}

void EditorLayer::AdvanceFocusAnimation(Engine::Timestep timestep)
{
    if (!m_FocusAnimation.Active)
        return;
    // Any navigation input wins immediately: a captured or pending drag, or a wheel
    // event, means the user took the camera.
    if (m_CursorCaptured || m_CursorCapturePending || m_MouseWheelDelta != 0.0f)
    {
        CancelFocusAnimation();
        return;
    }

    m_FocusAnimation.Elapsed += static_cast<double>(timestep.GetSeconds());
    const double t = std::min(1.0, m_FocusAnimation.Elapsed / kFocusAnimationSeconds);
    const double eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    for (size_t axis = 0; axis < 3; ++axis)
    {
        m_CameraPosition[axis] = t >= 1.0
            ? m_FocusAnimation.Target[axis]
            : m_FocusAnimation.Start[axis] + (m_FocusAnimation.Target[axis] - m_FocusAnimation.Start[axis]) * eased;
    }
    m_EditorCamera.SetPosition({ m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] });
    ApplyEditorCameraStateToScene();
    if (t >= 1.0)
        CancelFocusAnimation();
}

bool EditorLayer::FocusEntity(Engine::Entity entity, bool animate)
{
    FocusPlan plan;
    std::string error;
    if (!PlanFocusForEntity(entity, plan, error))
    {
        std::string text = error;
        std::replace(text.begin(), text.end(), '_', ' ');
        m_ConsoleLines.emplace_back("Cannot frame the entity: " + text);
        return false;
    }
    StartFocus(plan, animate);
    return true;
}

bool EditorLayer::FocusSelectedEntity(bool animate)
{
    if (!m_ActiveScene.IsEntityValid(m_SelectedEntity))
    {
        m_ConsoleLines.emplace_back("Nothing is selected: select an entity, then press F to frame it");
        return false;
    }
    return FocusEntity(m_SelectedEntity, animate);
}

bool EditorLayer::FrameAllVisibleMeshes(bool animate)
{
    FocusPlan plan;
    std::string error;
    if (!PlanFocusForAllMeshes(plan, error))
    {
        std::string text = error;
        std::replace(text.begin(), text.end(), '_', ' ');
        m_ConsoleLines.emplace_back("Cannot frame the scene: " + text);
        return false;
    }
    StartFocus(plan, animate);
    return true;
}

void EditorLayer::DrawViewportOverlays(const ImVec2& imageMin, const ImVec2& imageMax, bool hasNativeViewportTexture)
{
    (void)imageMax;
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Selected entity name, small and out of the way, top-left of the image.
    float developerTextTop = imageMin.y + 18.0f;
    if (const Engine::SceneEntity* selected = m_ActiveScene.TryGetEntity(m_SelectedEntity))
    {
        std::string name = selected->Name.empty() ? std::string("(unnamed)") : selected->Name;
        if (name.size() > 48)
            name = name.substr(0, 45) + "...";
        const ImVec2 textSize = ImGui::CalcTextSize(name.c_str());
        const ImVec2 chipMin(imageMin.x + 10.0f, imageMin.y + 10.0f);
        const ImVec2 chipMax(chipMin.x + textSize.x + 22.0f, chipMin.y + textSize.y + 8.0f);
        drawList->AddRectFilled(chipMin, chipMax, IM_COL32(20, 23, 26, 210), 3.0f);
        drawList->AddRect(chipMin, chipMax, IM_COL32(61, 69, 77, 255), 3.0f);
        // Selection hover color accent (DESIGN.md token).
        drawList->AddRectFilled(ImVec2(chipMin.x + 5.0f, chipMin.y + 5.0f),
            ImVec2(chipMin.x + 9.0f, chipMax.y - 5.0f), IM_COL32(61, 97, 128, 255));
        drawList->AddText(ImVec2(chipMin.x + 14.0f, chipMin.y + 4.0f), IM_COL32(230, 235, 240, 255), name.c_str());
        developerTextTop = chipMax.y + 12.0f;
    }

    // Developer renderer status keeps its place below the label.
    const char* title = hasNativeViewportTexture ? "Renderer target" : "Renderer preview";
    const std::string subtitle = hasNativeViewportTexture
        ? std::string("Active backend: ") + Engine::Renderer::GetActiveBackendName() + "; native Scene mesh pass"
        : std::string("Active backend: ") + Engine::Renderer::GetActiveBackendName() + "; native viewport unavailable";
    drawList->AddText(ImVec2(imageMin.x + 18.0f, developerTextTop), IM_COL32(230, 235, 240, 255), title);
    drawList->AddText(ImVec2(imageMin.x + 18.0f, developerTextTop + 22.0f), IM_COL32(150, 160, 170, 255), subtitle.c_str());

    const Engine::RendererBuildInfo& buildInfo = Engine::Renderer::GetBuildInfo();
    if (!hasNativeViewportTexture && !buildInfo.HasNVRHID3D12)
        drawList->AddText(ImVec2(imageMin.x + 18.0f, developerTextTop + 44.0f), IM_COL32(120, 130, 140, 255),
            "Open the VS2022 build for D3D12 rendering.");
}

// ---------------------------------------------------------------------------
// Typed viewport actions (control mailbox schema 5)
// ---------------------------------------------------------------------------

EditorMaterialControlTransaction EditorLayer::ExecuteViewportControlRequest(
    const EditorMaterialControlRequest& request, EditorMaterialControlTransaction transaction)
{
    EditorMaterialControlReceipt& receipt = transaction.Receipt;
    EditorViewportControlReceipt& viewport = receipt.Viewport;
    const auto reject = [&transaction, &receipt](std::string reason)
    {
        receipt.Succeeded = false;
        receipt.Reason = std::move(reason);
        return transaction;
    };

    const Engine::EntityId selectedId = GetSceneDebugSelectedEntityId();
    if (!request.HasExpectedSelectedEntityId || request.ExpectedSelectedEntityId != selectedId)
        return reject("compare_and_swap_state_mismatch");

    // Everything a rollback needs: viewport interaction is session state only.
    struct ViewportRollbackState
    {
        Engine::Entity Selection;
        bool FusionPivotValid = false;
        Engine::Math::DVec3 FusionPivot;
        bool HierarchyScrollRequest = false;
        std::array<double, 3> CameraPosition {};
        bool DiscontinuousRelocationPending = false;
        FocusAnimationState Animation;
    };
    auto rollback = std::make_shared<ViewportRollbackState>();
    rollback->Selection = m_SelectedEntity;
    rollback->FusionPivotValid = m_FusionNavigationPivotValid;
    rollback->FusionPivot = m_FusionNavigationPivot;
    rollback->HierarchyScrollRequest = m_HierarchyScrollRequest;
    rollback->CameraPosition = m_CameraPosition;
    rollback->DiscontinuousRelocationPending = m_ViewportDiscontinuousRelocationPending;
    rollback->Animation = m_FocusAnimation;
    const auto restore = [this, rollback](EditorMaterialControlReceipt& rolledBack)
    {
        CancelFocusAnimation();
        m_SelectedEntity = rollback->Selection;
        m_FusionNavigationPivotValid = rollback->FusionPivotValid;
        m_FusionNavigationPivot = rollback->FusionPivot;
        m_HierarchyScrollRequest = rollback->HierarchyScrollRequest;
        m_CameraPosition = rollback->CameraPosition;
        m_EditorCamera.SetPosition({ m_CameraPosition[0], m_CameraPosition[1], m_CameraPosition[2] });
        ApplyEditorCameraStateToScene();
        m_ViewportDiscontinuousRelocationPending = rollback->DiscontinuousRelocationPending;
        m_FocusAnimation = rollback->Animation;
        rolledBack.SelectedEntityIdAfter = m_SelectedEntity.Id;
        return m_SelectedEntity == rollback->Selection
            && m_CameraPosition == rollback->CameraPosition
            && m_FusionNavigationPivotValid == rollback->FusionPivotValid;
    };

    const ViewportPickGeometry geometry = GetViewportPickGeometry();
    viewport.RectX = geometry.Rect.X;
    viewport.RectY = geometry.Rect.Y;
    viewport.RectWidth = geometry.Rect.Width;
    viewport.RectHeight = geometry.Rect.Height;
    viewport.RectAspect = geometry.Aspect;
    viewport.RectFovDegrees = geometry.FovDegrees;
    viewport.PickRectVirtual = geometry.Virtual;

    if (request.Action == EditorMaterialControlAction::PickAtViewportPoint)
    {
        if (!request.HasViewportPoint)
            return reject("missing_viewport_point");
        viewport.PickNormalizedX = request.ViewportNormalizedX;
        viewport.PickNormalizedY = request.ViewportNormalizedY;
        const ViewportPickReport report = PickAtViewportNormalized(
            request.ViewportNormalizedX, request.ViewportNormalizedY);
        if (!report.Valid)
            return reject("viewport_ray_unavailable");

        viewport.PickPixelX = report.PixelX;
        viewport.PickPixelY = report.PixelY;
        viewport.PickState = report.Hit ? "hit" : "miss";
        viewport.PickEntityId = report.Entity;
        viewport.PickDistance = report.Hit ? report.Distance : 0.0;
        viewport.PickRefinement = !report.Hit ? "none" : (report.Refined ? "triangles" : "box");
        viewport.PickCandidates = report.Candidates;
        viewport.PickBoxHits = report.BoxHits;
        viewport.PickTrianglesTested = report.TrianglesTested;

        const Engine::Entity picked { report.Entity };
        if (report.Hit)
        {
            const Engine::SceneEntity* pickedEntity = m_ActiveScene.TryGetEntity(picked);
            if (!pickedEntity)
                return reject("picked_entity_disappeared");
            receipt.EntityId = picked.Id;
            receipt.EntityName = pickedEntity->Name;
            receipt.IsMainCamera = picked == m_ActiveScene.GetMainCameraEntity();
            receipt.AffectedEntityCount = 1;
            receipt.AffectedEntityIds.push_back(picked.Id);
        }
        const bool changesSelection = picked != m_SelectedEntity;
        const bool retargetsPivot = report.Hit && !receipt.IsMainCamera;
        receipt.Succeeded = true;
        receipt.Reason = "ok";
        receipt.Effect = report.Hit ? "ViewportPickHit" : "ViewportPickMiss";
        receipt.Recovery = changesSelection ? "SelectPreviousEntity" : "None";
        receipt.SelectedEntityIdAfter = picked.Id;
        receipt.SelectionCommitted = changesSelection;
        receipt.PivotRetargeted = changesSelection && retargetsPivot;
        receipt.PostconditionVerified = true;
        if (!changesSelection)
            return transaction;

        transaction.Mutating = true;
        transaction.Commit = [this, picked, expected = request.ExpectedSelectedEntityId](std::string& error)
        {
            if (GetSceneDebugSelectedEntityId() != expected || (picked && !m_ActiveScene.IsEntityValid(picked)))
            {
                error = "state_changed_before_commit";
                return false;
            }
            SetSelectedEntity(picked, SelectionSource::Viewport);
            if (m_SelectedEntity != picked)
            {
                error = "selection_postcondition_mismatch";
                return false;
            }
            return true;
        };
        transaction.Rollback = restore;
        return transaction;
    }

    // FocusSelection
    if (!request.HasFocusAnimation)
        return reject("missing_focus_animation");
    if (!m_ActiveScene.IsEntityValid(m_SelectedEntity))
        return reject("nothing_selected");
    FocusPlan plan;
    std::string planError;
    if (!PlanFocusForEntity(m_SelectedEntity, plan, planError))
        return reject(planError);

    const Engine::SceneEntity* selectedEntity = m_ActiveScene.TryGetEntity(m_SelectedEntity);
    receipt.EntityId = m_SelectedEntity.Id;
    receipt.EntityName = selectedEntity ? selectedEntity->Name : std::string();
    receipt.AffectedEntityCount = 1;
    receipt.AffectedEntityIds.push_back(m_SelectedEntity.Id);
    viewport.FocusState = "framed";
    viewport.FocusSubject = plan.DefaultRadius ? "default-radius" : "bounds";
    viewport.FocusAnimated = request.FocusAnimate;
    viewport.FocusMargin = Picking::kFramingMargin;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        viewport.FocusBefore[axis] = m_CameraPosition[axis];
        viewport.FocusBefore[3 + axis] = m_CameraRotation[axis];
        viewport.FocusAfter[3 + axis] = m_CameraRotation[axis];
    }
    viewport.FocusAfter[0] = plan.CameraTarget.X;
    viewport.FocusAfter[1] = plan.CameraTarget.Y;
    viewport.FocusAfter[2] = plan.CameraTarget.Z;
    viewport.FocusCenter[0] = plan.Center.X;
    viewport.FocusCenter[1] = plan.Center.Y;
    viewport.FocusCenter[2] = plan.Center.Z;
    viewport.FocusRadius = plan.Radius;
    viewport.FocusDistance = plan.Distance;

    receipt.Succeeded = true;
    receipt.Reason = "ok";
    receipt.Effect = "SelectionFocused";
    receipt.Recovery = "RestoreCameraPose";
    receipt.PivotRetargeted = true;
    receipt.PostconditionVerified = true;
    receipt.EditorCameraSynchronized = !request.FocusAnimate;
    transaction.Mutating = true;
    transaction.Commit = [this, plan, animate = request.FocusAnimate, selected = m_SelectedEntity,
                             expected = request.ExpectedSelectedEntityId](std::string& error)
    {
        if (m_SelectedEntity != selected || GetSceneDebugSelectedEntityId() != expected)
        {
            error = "state_changed_before_commit";
            return false;
        }
        StartFocus(plan, animate);
        if (animate)
        {
            if (!m_FocusAnimation.Active)
            {
                error = "animation_postcondition_mismatch";
                return false;
            }
            return true;
        }
        if (m_CameraPosition[0] != plan.CameraTarget.X || m_CameraPosition[1] != plan.CameraTarget.Y
            || m_CameraPosition[2] != plan.CameraTarget.Z || m_FocusAnimation.Active)
        {
            error = "camera_postcondition_mismatch";
            return false;
        }
        return true;
    };
    transaction.Rollback = restore;
    return transaction;
}

// ---------------------------------------------------------------------------
// In-process half of --editor-control-viewport-picking-helper-smoke
// ---------------------------------------------------------------------------

void EditorLayer::PublishEditorViewportPickingTarget()
{
    const Engine::SceneEntity* prototype = m_ActiveScene.TryGetEntity(m_PrototypeMeshEntity);
    const Engine::SceneEntity* light = m_ActiveScene.TryGetEntity(m_DirectionalLightEntity);
    const Engine::Entity mainCamera = m_ActiveScene.GetMainCameraEntity();
    const Engine::SceneEntity* camera = m_ActiveScene.TryGetEntity(mainCamera);
    if (!prototype || !prototype->MeshRenderer || !light || !camera)
        throw std::runtime_error("viewport picking smoke requires prototype mesh, light, and main camera");
    m_PickMeshScratch.clear();
    const std::shared_ptr<const PickMeshData> mesh = GetPickMesh(prototype->MeshRenderer->MeshAsset);
    if (!mesh)
        throw std::runtime_error("viewport picking smoke could not resolve the prototype mesh bounds");
    m_PickMeshScratch.clear();

    // The smoke starts with the main camera selected (the shipped startup state) and
    // nothing animating, so every receipt pose is deterministic.
    m_SelectedEntity = mainCamera;
    ResetFusionNavigationPivotFromSelectionOrScene();
    m_EditorViewportPickingInitialSelection = m_SelectedEntity;
    m_EditorViewportPickingBaseUndoDepth = static_cast<Engine::u32>(m_UndoHistory.size());

    const ViewportPickGeometry geometry = GetViewportPickGeometry();
    const auto writeTransform = [](std::ostringstream& stream, std::string_view label, const Engine::TransformComponent& transform)
    {
        const Engine::Math::SectorLocalPosition& position = transform.GetPosition();
        stream << label << ' ' << position.Sector.X << ' ' << position.Sector.Y << ' ' << position.Sector.Z << ' '
               << position.Local.X << ' ' << position.Local.Y << ' ' << position.Local.Z << ' '
               << transform.RotationDegrees.X << ' ' << transform.RotationDegrees.Y << ' '
               << transform.RotationDegrees.Z << ' ' << transform.Scale.X << ' ' << transform.Scale.Y << ' '
               << transform.Scale.Z << '\n';
    };
    std::ostringstream target;
    target << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "SpiralEditorViewportPickingTarget 1\n"
           << "SessionId " << std::quoted(m_EditorMaterialControl.GetSessionId()) << '\n'
           << "InitialSelectedEntityId " << m_EditorViewportPickingInitialSelection.Id << '\n'
           << "PrototypeEntityId " << m_PrototypeMeshEntity.Id << '\n'
           << "PrototypeEntityName " << std::quoted(prototype->Name) << '\n'
           << "PrototypeLocalBounds " << mesh->Bounds.Min[0] << ' ' << mesh->Bounds.Min[1] << ' '
           << mesh->Bounds.Min[2] << ' ' << mesh->Bounds.Max[0] << ' ' << mesh->Bounds.Max[1] << ' '
           << mesh->Bounds.Max[2] << '\n'
           << "PrototypeTriangles " << mesh->TriangleCount << '\n';
    writeTransform(target, "PrototypeTransform", prototype->Transform);
    target << "LightEntityId " << m_DirectionalLightEntity.Id << '\n'
           << "LightEntityName " << std::quoted(light->Name) << '\n';
    writeTransform(target, "LightTransform", light->Transform);
    target << "MainCameraEntityId " << mainCamera.Id << '\n'
           << "MainCameraEntityName " << std::quoted(camera->Name) << '\n'
           << "CameraPose " << m_CameraPosition[0] << ' ' << m_CameraPosition[1] << ' ' << m_CameraPosition[2]
           << ' ' << m_CameraRotation[0] << ' ' << m_CameraRotation[1] << ' ' << m_CameraRotation[2] << '\n'
           << "CameraProjection " << m_EditorCamera.GetProjection().VerticalFovDegrees << ' '
           << m_EditorCamera.GetProjection().NearClip << ' ' << m_EditorCamera.GetProjection().FarClip << '\n'
           << "VirtualViewport " << geometry.Rect.X << ' ' << geometry.Rect.Y << ' ' << geometry.Rect.Width << ' '
           << geometry.Rect.Height << ' ' << geometry.Aspect << ' ' << (geometry.Virtual ? "yes" : "no") << '\n'
           << "FramingMargin " << Picking::kFramingMargin << '\n'
           << "DefaultFramingRadius " << Picking::kDefaultFramingRadius << '\n'
           << "ExpectedTypedRequests 16\n"
           << "ExpectedServerRejectedFixtures 2\n";
    std::string error;
    if (!m_EditorMaterialControl.PublishViewportPickingTargetForSmoke(target.str(), error))
        throw std::runtime_error("could not publish viewport picking smoke target: " + error);
}

void EditorLayer::RunEditorViewportPickingHelperSmokeAfterDrain()
{
    if (!m_EditorViewportPickingHelperSmokeRequested || m_EditorViewportPickingHelperSmokeCompleted)
        return;

    const auto receipt = [this](std::string_view id) { return m_EditorMaterialControl.FindTerminalReceipt(id); };
    const EditorMaterialControlReceipt* finalInspect = receipt("vp-17-final-inspect");
    if (!finalInspect)
        return;

    struct Expectation
    {
        const char* Id;
        EditorMaterialControlAction Action;
        bool Succeeded;
        const char* Reason; // "ok" for a success
    };
    constexpr Expectation expectations[] = {
        { "vp-01-pick-sky", EditorMaterialControlAction::PickAtViewportPoint, true, "ok" },
        { "vp-02-pick-cube", EditorMaterialControlAction::PickAtViewportPoint, true, "ok" },
        { "vp-03-stale-pick", EditorMaterialControlAction::PickAtViewportPoint, false, "compare_and_swap_state_mismatch" },
        { "vp-04-pick-cube-again", EditorMaterialControlAction::PickAtViewportPoint, true, "ok" },
        { "vp-05-focus-cube", EditorMaterialControlAction::FocusSelection, true, "ok" },
        { "vp-06-focus-again", EditorMaterialControlAction::FocusSelection, true, "ok" },
        { "vp-07-pick-after-focus", EditorMaterialControlAction::PickAtViewportPoint, true, "ok" },
        { "vp-08-pick-inside-a", EditorMaterialControlAction::PickAtViewportPoint, true, "ok" },
        { "vp-09-pick-inside-b", EditorMaterialControlAction::PickAtViewportPoint, true, "ok" },
        { "vp-10-pick-outside", EditorMaterialControlAction::PickAtViewportPoint, true, "ok" },
        { "vp-11-select-light", EditorMaterialControlAction::SelectEntity, true, "ok" },
        { "vp-12-focus-light", EditorMaterialControlAction::FocusSelection, true, "ok" },
        { "vp-13-pick-clear", EditorMaterialControlAction::PickAtViewportPoint, true, "ok" },
        { "vp-14-focus-nothing", EditorMaterialControlAction::FocusSelection, false, "nothing_selected" },
        { "vp-15-focus-stale", EditorMaterialControlAction::FocusSelection, false, "compare_and_swap_state_mismatch" },
        { "vp-16-pick-out-of-range", EditorMaterialControlAction::PickAtViewportPoint, false, "invalid_or_duplicate_viewport_point" },
    };
    bool valid = true;
    for (const Expectation& expected : expectations)
    {
        const EditorMaterialControlReceipt* actual = receipt(expected.Id);
        // vp-16 is a server-side parse rejection: the receipt carries the parse reason
        // and no action, so only the outcome is checked for it.
        valid = valid && actual && actual->Succeeded == expected.Succeeded && actual->Reason == expected.Reason
            && (std::string_view(expected.Id) == "vp-16-pick-out-of-range" || actual->Action == expected.Action);
        if (!valid)
        {
            Engine::Log::Error("Viewport picking smoke receipt mismatch: ", expected.Id);
            break;
        }
    }
    const EditorMaterialControlReceipt* staleSchema = receipt("vp-schema-stale");
    valid = valid && staleSchema && !staleSchema->Succeeded && staleSchema->Reason == "unsupported_schema_expected_v5";

    // The last framing (the light) is the live camera: the receipt pose is not just text.
    const EditorMaterialControlReceipt* lastFocus = receipt("vp-12-focus-light");
    valid = valid && lastFocus && m_CameraPosition[0] == lastFocus->Viewport.FocusAfter[0]
        && m_CameraPosition[1] == lastFocus->Viewport.FocusAfter[1]
        && m_CameraPosition[2] == lastFocus->Viewport.FocusAfter[2]
        && m_CameraRotation[0] == static_cast<float>(lastFocus->Viewport.FocusAfter[3])
        && m_CameraRotation[1] == static_cast<float>(lastFocus->Viewport.FocusAfter[4]);

    // None of these actions is a document edit: history is exactly where it started.
    valid = valid && finalInspect->Succeeded && finalInspect->UndoDepthBefore == m_EditorViewportPickingBaseUndoDepth
        && m_UndoHistory.size() == m_EditorViewportPickingBaseUndoDepth && m_RedoHistory.empty()
        && m_SelectedEntity.Id == Engine::kInvalidEntityId && !m_FocusAnimation.Active;
    if (!valid)
        throw std::runtime_error("external editor viewport picking helper smoke failed");

    Engine::Log::Info(
        "EditorViewportPickingV5 producer=external-python actions=PickAtViewportPoint,FocusSelection schema=5 "
        "stale=rejected selection=session-only history=unchanged persistence=SessionOnly saved=no "
        "input=typed-mailbox-no-ui-synthesis viewport=",
        GetViewportPickGeometry().Virtual ? "virtual-1280x720" : "live-image", " backend=",
        Engine::Renderer::GetActiveBackendName(), " result=pass");
    m_EditorViewportPickingHelperSmokeCompleted = true;
    Engine::Application::Get().Close();
}

// ---------------------------------------------------------------------------
// --editor-viewport-click-smoke (headless): the event-driven half of
// click-to-select, Esc, Home, and the F animation, driven through OnEvent.
// ---------------------------------------------------------------------------

void EditorLayer::RunViewportClickSmoke()
{
    if (!m_ViewportClickSmokeRequested || m_ViewportClickSmokeCompleted || m_FrameCounter < 2)
        return;
    m_ViewportClickSmokeCompleted = true;

    const Engine::Entity prototype = m_PrototypeMeshEntity;
    const Engine::Entity mainCamera = m_ActiveScene.GetMainCameraEntity();
    const size_t baseUndo = m_UndoHistory.size();
    const size_t baseRedo = m_RedoHistory.size();
    unsigned int checks = 0;
    const auto expect = [&checks](bool condition, const char* what)
    {
        ++checks;
        if (!condition)
            throw std::runtime_error(std::string("viewport click smoke failed: ") + what);
    };
    const auto press = [this](int button) { Engine::MouseButtonPressedEvent event(button); OnEvent(event); };
    const auto release = [this](int button) { Engine::MouseButtonReleasedEvent event(button); OnEvent(event); };
    const auto move = [this](float x, float y) { Engine::MouseMovedEvent event(x, y); OnEvent(event); };
    const auto key = [this](int code) { Engine::KeyPressedEvent event(code, false); OnEvent(event); };

    // Headless state the UI phase would normally latch: pointer over the image, window focused.
    m_WindowFocused = true;
    m_ViewportPickAvailable = true;
    m_ViewportFocused = true;
    m_ViewportImageValid = false; // the documented virtual 1280x720 rectangle
    CancelFocusAnimation();
    m_ViewportClickCursorOverride = std::array<double, 2> { 640.0, 360.0 };
    const ViewportNavigationPreset previousPreset = m_ViewportNavigationPreset;
    m_ViewportNavigationPreset = ViewportNavigationPreset::Fusion;
    m_SelectedEntity = mainCamera;

    // 1. A still press and release at the center picks the cube.
    press(0);
    release(0);
    expect(m_SelectedEntity == prototype, "center click selects the cube");
    expect(m_HierarchyScrollRequest, "a viewport selection asks the hierarchy to scroll once");
    m_HierarchyScrollRequest = false;

    // 2. A click on empty sky clears the selection.
    m_ViewportClickCursorOverride = std::array<double, 2> { 12.0, 12.0 };
    press(0);
    release(0);
    expect(!m_SelectedEntity, "sky click clears the selection");
    expect(!m_HierarchyScrollRequest, "clearing never schedules a scroll");

    // 3. Two pixels of motion is still a click; ten is a drag and picks nothing.
    m_ViewportClickCursorOverride = std::array<double, 2> { 640.0, 360.0 };
    press(0);
    move(641.0f, 360.0f);
    move(642.0f, 360.0f);
    release(0);
    expect(m_SelectedEntity == prototype, "a 2 px jitter is still a click");
    ClearSelection(SelectionSource::Keyboard);
    press(0);
    move(650.0f, 360.0f);
    release(0);
    expect(!m_SelectedEntity, "a 10 px drag is not a click");

    // 4. A long press is not a click.
    press(0);
    m_ViewportClick.PressTime -= std::chrono::milliseconds(500);
    release(0);
    expect(!m_SelectedEntity, "a 500 ms press is not a click");

    // 5. Gate: another button held, a second button pressed, or no hovered viewport image.
    m_RightMouseDown = true;
    press(0);
    release(0);
    m_RightMouseDown = false;
    expect(!m_SelectedEntity, "no pick while the right button is held");
    press(0);
    press(1);
    release(1);
    release(0);
    expect(!m_SelectedEntity, "a second button press cancels the click");
    m_ViewportPickAvailable = false;
    press(0);
    release(0);
    m_ViewportPickAvailable = true;
    expect(!m_SelectedEntity, "no pick when the pointer is not over the image");
    m_WindowFocused = false;
    press(0);
    release(0);
    m_WindowFocused = true;
    expect(!m_SelectedEntity, "no pick without window focus");

    // 6. The Unreal preset navigates with the left button, but a still click still picks.
    m_ViewportNavigationPreset = ViewportNavigationPreset::Unreal;
    const std::array<double, 3> cameraBeforeUnrealClick = m_CameraPosition;
    press(0);
    release(0);
    expect(m_SelectedEntity == prototype, "a still click picks in the Unreal preset");
    expect(m_CameraPosition == cameraBeforeUnrealClick && !m_CursorCaptured && !m_CursorCapturePending,
        "the Unreal click leaves no capture and moves nothing");
    m_ViewportNavigationPreset = ViewportNavigationPreset::Fusion;

    // 7. Esc clears the selection only while the viewport is focused.
    m_ViewportFocused = false;
    key(kKeyEscape);
    expect(m_SelectedEntity == prototype, "Esc without viewport focus keeps the selection");
    m_ViewportFocused = true;
    key(kKeyEscape);
    expect(!m_SelectedEntity, "Esc with viewport focus clears the selection");

    // 8. F with nothing selected does nothing and says so; with a selection it starts an animation.
    m_ViewportNavigationInputEnabled = true;
    const size_t consoleLines = m_ConsoleLines.size();
    const std::array<double, 3> cameraBeforeNothing = m_CameraPosition;
    key('F');
    expect(m_CameraPosition == cameraBeforeNothing && !m_FocusAnimation.Active, "F with no selection moves nothing");
    expect(m_ConsoleLines.size() == consoleLines + 1
            && m_ConsoleLines.back().find("Nothing is selected") != std::string::npos,
        "F with no selection says so in the Console");
    SetSelectedEntity(prototype, SelectionSource::Viewport);
    key('F');
    expect(m_FocusAnimation.Active, "F starts the framing animation");
    const std::array<double, 3> start = m_FocusAnimation.Start;
    const std::array<double, 3> target = m_FocusAnimation.Target;
    // Ease-out cubic: after half the duration 87.5 percent of the way (1 - 0.5^3).
    AdvanceFocusAnimation(Engine::Timestep(0.09f));
    bool halfway = m_FocusAnimation.Active;
    for (size_t axis = 0; axis < 3; ++axis)
        halfway = halfway && std::abs(m_CameraPosition[axis] - (start[axis] + (target[axis] - start[axis]) * 0.875))
            < 1e-6 * std::max(1.0, std::abs(target[axis] - start[axis]));
    expect(halfway, "the animation is ease-out cubic (87.5 percent at half time)");
    AdvanceFocusAnimation(Engine::Timestep(0.2f));
    expect(!m_FocusAnimation.Active && m_CameraPosition == target, "the animation ends exactly on the target");
    Engine::Math::DVec3 sceneCameraPosition;
    expect(m_ActiveScene.TryGetEntityApproximateWorldPosition(mainCamera, sceneCameraPosition)
            && std::abs(sceneCameraPosition.X - target[0]) < 1e-9 && std::abs(sceneCameraPosition.Z - target[2]) < 1e-9,
        "the scene camera follows the animation");

    // 9. Navigation input cancels an animation immediately and leaves the camera where it is.
    m_CameraPosition = start;
    m_EditorCamera.SetPosition({ start[0], start[1], start[2] });
    ApplyEditorCameraStateToScene();
    key('F');
    expect(m_FocusAnimation.Active, "F restarts the animation");
    AdvanceFocusAnimation(Engine::Timestep(0.05f));
    const std::array<double, 3> midFlight = m_CameraPosition;
    m_MouseWheelDelta = 1.0f;
    AdvanceFocusAnimation(Engine::Timestep(0.05f));
    expect(!m_FocusAnimation.Active && m_CameraPosition == midFlight, "a wheel event cancels the animation in place");
    m_MouseWheelDelta = 0.0f;

    // 10. Home frames every visible mesh: the cube's bounds end inside the viewport.
    m_CameraPosition = start;
    m_EditorCamera.SetPosition({ start[0], start[1], start[2] });
    ApplyEditorCameraStateToScene();
    FrameAllVisibleMeshes(false);
    expect(m_CameraPosition != start && !m_FocusAnimation.Active, "Frame All jumps when animation is disabled");
    expect(m_ViewportDiscontinuousRelocationPending, "an instant frame flags a discontinuous relocation");
    const std::shared_ptr<const PickMeshData> mesh = GetPickMesh(
        m_ActiveScene.TryGetMeshRendererComponent(prototype)->MeshAsset);
    expect(mesh != nullptr, "prototype mesh resolves");
    {
        Picking::LinearMap map;
        const Engine::SceneEntity* entity = m_ActiveScene.TryGetEntity(prototype);
        expect(Picking::MakeLinearMap(entity->Transform.RotationDegrees, entity->Transform.Scale, map), "map");
        Engine::Math::DVec3 relative;
        expect(Engine::Math::TryGetSectorLocalRelativePosition(entity->Transform.GetPosition(),
                   m_ActiveScene.GetMainCameraTransform().GetPosition(), m_ActiveScene.GetWorldGridPolicy(), relative),
            "relative position");
        Engine::Math::DVec3 corners[8];
        Picking::OrientedBoxCorners(relative, map, mesh->Bounds, corners);
        const ViewportPickGeometry geometry = GetViewportPickGeometry();
        const Picking::ViewCamera camera = MakeViewCamera(
            m_EditorCamera.GetRotationDegrees(), geometry.FovDegrees, geometry.Aspect);
        for (const Engine::Math::DVec3& corner : corners)
        {
            double px = 0.0, py = 0.0, depth = 0.0;
            expect(Picking::ProjectToViewport(geometry.Rect, camera, corner, px, py, depth)
                    && px > 0.0 && px < geometry.Rect.Width && py > 0.0 && py < geometry.Rect.Height,
                "every framed bound corner is inside the viewport");
        }
    }
    // Home from the keyboard needs the viewport focused and starts the animation.
    m_CameraPosition = start;
    m_EditorCamera.SetPosition({ start[0], start[1], start[2] });
    ApplyEditorCameraStateToScene();
    key(kKeyHome);
    expect(m_FocusAnimation.Active, "Home starts the Frame All animation");
    CancelFocusAnimation();

    // 11. The main camera is not frameable; a hidden-only scene has nothing to frame.
    m_SelectedEntity = mainCamera;
    const size_t linesBefore = m_ConsoleLines.size();
    expect(!FocusSelectedEntity(false) && m_ConsoleLines.size() == linesBefore + 1, "the viewport camera cannot be framed");
    m_SelectedEntity = prototype;

    // Selection is not an undo entry.
    expect(m_UndoHistory.size() == baseUndo && m_RedoHistory.size() == baseRedo,
        "none of this touched the undo history");

    m_ViewportClickCursorOverride.reset();
    m_ViewportNavigationPreset = previousPreset;
    m_ViewportNavigationInputEnabled = false;
    ClearViewportNavigationInput();
    Engine::Log::Info("ViewportClickSmokeV1 checks=", checks,
        " click=center-hit,sky-clear,jitter-2px-ok,drag-10px-ignored,long-press-ignored,gated unreal=click-picks"
        " keys=esc-needs-viewport-focus,f-nothing-selected-message,home-frames-all"
        " animation=ease-out-cubic-exact-end,wheel-cancels,main-camera-rejected history=unchanged result=pass");
    m_ConsoleLines.emplace_back("Viewport click smoke passed");
    Engine::Application::Get().Close();
}
