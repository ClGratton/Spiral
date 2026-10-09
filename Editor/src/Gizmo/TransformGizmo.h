#pragma once

#include "EulerRotation.h"
#include "GizmoApply.h"
#include "GizmoHandles.h"
#include "GizmoProjection.h"
#include "GizmoSolvers.h"
#include "SnapSettings.h"

#include <string>
#include <vector>

namespace Gizmo
{
    enum class GizmoProperty : u8
    {
        Position,
        Rotation,
        Scale
    };

    // One drag is one gesture: a stable id plus the entity and property being
    // edited, the key an editor history gesture tracker coalesces on.
    struct GizmoGestureKey
    {
        u64 GestureId = 0;
        u64 EntityId = 0;
        GizmoProperty Property = GizmoProperty::Position;
    };

    // What the gizmo needs from its owner. Calls happen synchronously inside
    // TransformGizmo::Update in this order for one drag:
    //   BeginGesture, ApplyTransform*, then EndGesture or CancelGesture.
    class IGizmoEditHost
    {
    public:
        virtual ~IGizmoEditHost() = default;

        // Nothing has been written yet: capture the "before" state here.
        virtual void BeginGesture(const GizmoGestureKey& key) = 0;
        // Writes the transform. Returns false when the document rejects it
        // (non-canonical, invalid scale, ...); the gizmo then keeps the last
        // accepted transform and continues the drag.
        virtual bool ApplyTransform(const GizmoGestureKey& key, const GizmoTransform& transform) = 0;
        // Pointer released: the last accepted transform stays. The owner closes
        // its history gesture (an unchanged gesture records nothing).
        virtual void EndGesture(const GizmoGestureKey& key) = 0;
        // Escape, focus loss, a lost or changed target or a changed view. If any
        // update was accepted, the start transform was just restored through
        // ApplyTransform (a deleted entity rejects it). The owner discards its
        // history gesture.
        virtual void CancelGesture(const GizmoGestureKey& key) = 0;
    };

    // The entity being edited, as the owner holds it this frame.
    struct GizmoTarget
    {
        u64 EntityId = 0;
        GizmoTransform Transform;
        // Transform.Position relative to the view's translation origin
        // (TryGetSectorLocalRelativePosition on the canonical origin when the
        // view has one), refreshed every frame.
        Engine::Math::DVec3 RelativePosition;
        // False for camera-bearing entities, which have no Scale tool.
        bool AllowScale = true;
    };

    struct GizmoPointer
    {
        ScreenPoint Position;
        // Over the viewport image with no UI item or popup claiming the pointer.
        bool Over = false;
        // Primary button level, sampled once per frame.
        bool PrimaryDown = false;
        bool CtrlDown = false;
        // Escape went down this frame and no text field is active.
        bool EscapePressed = false;
    };

    // Everything the owner feeds each frame. The view's translation origin
    // must stay fixed during a drag: a changed view cancels the gesture.
    struct GizmoFrameInput
    {
        // Null when the viewport has no valid camera this frame.
        const GizmoView* View = nullptr;
        const GizmoTarget* Target = nullptr;
        TransformTool Tool = TransformTool::Select;
        TransformSpace Space = TransformSpace::World;
        SnapSettings Snap;
        Engine::Math::WorldGridPolicy Policy;
        GizmoStyle Style;
        GizmoPalette Palette;
        GizmoPointer Pointer;
        bool WindowFocused = true;
    };

    enum class GizmoPhase : u8
    {
        Idle,
        Hover,
        Active,
        // Escape (or another cancel) ended a drag; the next drag needs a new press.
        Cancelled
    };

    struct GizmoFrameResult
    {
        GizmoPhase Phase = GizmoPhase::Idle;
        GizmoHandle Hover = GizmoHandle::None;
        GizmoHandle Active = GizmoHandle::None;
        // The pointer belongs to the gizmo (hovering a handle or dragging), so
        // viewport navigation and picking must not start from it.
        bool WantsPointer = false;
        // Snapping applied to this frame's drag (toggle xor Ctrl).
        bool SnapActive = false;
        u64 GestureId = 0;
        // Numeric readout of the active drag, e.g. "+1.50 m", "+15.0\xC2\xB0", "x1.20".
        std::string Readout;
    };

    // Interaction state machine for one transform gizmo. Pure: the owner feeds
    // frame input, draws DrawList() and writes transforms through the host.
    // Every transform written during a drag is the start transform plus the
    // total solved delta, never accumulated per frame.
    class TransformGizmo
    {
    public:
        GizmoFrameResult Update(const GizmoFrameInput& input, IGizmoEditHost& host);

        const std::vector<GizmoDrawPrimitive>& DrawList() const { return m_DrawList; }
        GizmoPhase Phase() const { return m_Phase; }
        u64 RejectedUpdateCount() const { return m_RejectedUpdates; }

    private:
        struct Session
        {
            GizmoGestureKey Key;
            TransformTool Tool = TransformTool::Select;
            TransformSpace Space = TransformSpace::World;
            GizmoHandle Handle = GizmoHandle::None;
            GizmoTransform Start;
            GizmoTransform LastApplied;
            Engine::Math::DVec3 Pivot;
            Engine::Math::DVec3 Axes[3];
            double Length = 0.0;
            GizmoView View;
            ScreenPoint StartCursor;
            double GrabT = 0.0;
            Engine::Math::DVec3 GrabPoint;
            Engine::Math::DVec3 StartArm;
            AngleAccumulator Angle;
            bool ScreenFallback = false;
            ScreenPoint Tangent;
        };

        bool TryBegin(const GizmoFrameInput& input, const GizmoGeometry& geometry, GizmoHandle handle, IGizmoEditHost& host);
        void StepActive(const GizmoFrameInput& input, bool primaryDown, IGizmoEditHost& host, GizmoFrameResult& result);
        void CancelSession(bool primaryDown, IGizmoEditHost& host);
        bool Solve(const GizmoFrameInput& input, bool snapActive, GizmoTransform& outTransform, std::string& outReadout);
        void BuildDrawList(const GizmoFrameInput& input, const GizmoGeometry& geometry, GizmoHandle hover, GizmoHandle active);

        GizmoPhase m_Phase = GizmoPhase::Idle;
        bool m_PreviousDown = false;
        bool m_HasSession = false;
        Session m_Session;
        u64 m_NextGestureId = 1;
        u64 m_RejectedUpdates = 0;
        std::vector<GizmoDrawPrimitive> m_DrawList;
    };
}
