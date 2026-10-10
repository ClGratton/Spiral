#pragma once

#include "../Commands/CommandRegistry.h"
#include "../Gizmo/TransformGizmo.h"
#include "../History/EditGesture.h"
#include "../History/HistoryLabel.h"

#include "Engine/Scene/Camera.h"

#include <span>
#include <string>
#include <string_view>

// The Editor-independent session that the Scene Viewport hosts for the transform
// gizmo. It owns the viewport authoring state (tool, space, snap), runs the pure
// Gizmo::TransformGizmo state machine once per UI frame, turns the result into
// what the viewport needs (draw primitives, a pointer-ownership flag, a cursor
// hint, a status string) and writes through a narrow host interface that the
// Editor implements. No ImGui, GLFW, Scene or renderer include: it compiles into
// EngineTests and is exercised there against a fake host over a real Scene.
//
// Single selection only: the gizmo acts on the one entity the host reports.
//
// Frame protocol (all on the main thread, ImGui phase):
//   1. Build GizmoSessionInput (camera, viewport rectangle, pointer, Esc, flags).
//   2. result = session.Update(input, host).
//   3. Draw result.Primitives clipped to the viewport rectangle.
//   4. Latch result.OwnsPointer for the next GLFW event phase: while it is set a
//      press must neither pick nor start camera navigation.
//
// Camera conventions are those of Engine::BuildCameraView and
// SpiralEditor::Picking: row vectors, left-handed, +Z forward, depth 0..1, pixels
// with the origin at the top-left of the viewport image and y down.
namespace SpiralEditor
{
    // Command ids registered by GizmoSession::RegisterCommands.
    inline constexpr std::string_view kCommandViewportToolSelect = "viewport.tool.select";
    inline constexpr std::string_view kCommandViewportToolTranslate = "viewport.tool.translate";
    inline constexpr std::string_view kCommandViewportToolRotate = "viewport.tool.rotate";
    inline constexpr std::string_view kCommandViewportToolScale = "viewport.tool.scale";
    inline constexpr std::string_view kCommandViewportSpaceToggle = "viewport.space.toggle";
    inline constexpr std::string_view kCommandViewportSnapToggle = "viewport.snap.toggle";

    // Viewport authoring state. Workspace-global, never project data.
    struct ViewportToolState
    {
        Gizmo::TransformTool Tool = Gizmo::TransformTool::Translate;
        Gizmo::TransformSpace Space = Gizmo::TransformSpace::World;
        // Off by default; translate 1.0, rotate 15 degrees, scale 0.1.
        Gizmo::SnapSettings Snap;

        bool operator==(const ViewportToolState&) const = default;
    };

    enum class ToolStateStatus : Engine::u8
    {
        Applied,
        // A gizmo drag is open; state changes are refused until it ends.
        DragInProgress,
        // A snap step is non-finite or outside its documented range; see SnapError.
        InvalidSnapSettings
    };

    struct ToolStateResult
    {
        ToolStateStatus Status = ToolStateStatus::Applied;
        Gizmo::SnapSettingsError SnapError = Gizmo::SnapSettingsError::None;

        bool Ok() const { return Status == ToolStateStatus::Applied; }
    };

    // "grid restarts at sector boundaries" when the translate step is valid but
    // does not divide the sector extent (see Gizmo::IsSectorLatticeAligned),
    // otherwise empty. It does not depend on the snap toggle. The snap popover
    // shows it instead of claiming absolute-lattice alignment.
    std::string SnapLatticeNote(const Gizmo::SnapSettings& snap, const Engine::Math::WorldGridPolicy& policy);

    // The history identity of a gizmo drag. Item has bit 63 set so it can never
    // equal a 32-bit ImGui item id; Property is 1 Position, 2 Rotation, 3 Scale.
    EditorHistory::EditGestureKey ToEditGestureKey(const Gizmo::GizmoGestureKey& key);

    // The DESIGN.md gizmo colours: Hover is the Selection hover token and Active
    // the Docking preview colour at full alpha; the three axis hues are the only
    // non-blue accents in the Editor and are kept visibly apart from both
    // selection blues (the Z hue is violet, not blue, so an active or hovered
    // handle never looks like its idle self). Every handle also carries an X/Y/Z
    // letter, so no meaning rests on colour alone.
    Gizmo::GizmoPalette DesignGizmoPalette();

    // The entity the gizmo acts on, as the host holds it this frame.
    struct GizmoSelection
    {
        Engine::u64 EntityId = 0;
        // Shown in the history label ("Move Cube"); sanitised by MakeHistoryLabel.
        std::string Name;
        // Canonical sector-local position, Euler degrees and scale as stored.
        Gizmo::GizmoTransform Transform;
        // False for camera-bearing entities, which have no Scale tool.
        bool AllowScale = true;
    };

    // What the Editor implements. Calls arrive synchronously inside
    // GizmoSession::Update, for one drag in this order:
    //   BeginGesture, ApplyTransform*, then EndGesture or CancelGesture.
    class IGizmoHost
    {
    public:
        virtual ~IGizmoHost() = default;

        // The primary selected entity, or false when there is none, it is the
        // main camera (the viewport eye), it is locked, or it was deleted.
        virtual bool TryGetSelection(GizmoSelection& outSelection) = 0;

        // Nothing has been written yet: capture the history "Before" state and
        // open the history gesture under ToEditGestureKey(key).
        virtual void BeginGesture(const EditorHistory::HistoryLabel& label, const Gizmo::GizmoGestureKey& key) = 0;

        // Writes the complete transform (the drag-start transform plus the total
        // solved delta, never accumulated) through the Scene's canonical
        // validated write. Returns false when the document rejects it; the drag
        // then keeps the last accepted transform and continues.
        virtual bool ApplyTransform(const Gizmo::GizmoGestureKey& key, const Gizmo::GizmoTransform& transform) = 0;

        // Pointer released: the last accepted transform stays. Close the history
        // gesture; an unchanged gesture records nothing.
        virtual void EndGesture(const Gizmo::GizmoGestureKey& key) = 0;

        // Esc, focus loss, a changed or lost target, a changed view, navigation
        // or a blocking UI state. When any update was accepted the start
        // transform was just restored through ApplyTransform (bit-exact; a
        // deleted entity rejects it). Discard the history gesture, record nothing.
        virtual void CancelGesture(const Gizmo::GizmoGestureKey& key) = 0;
    };

    struct GizmoSessionPointer
    {
        // Window pixels, origin top-left, y down.
        Gizmo::ScreenPoint Position;
        // Hover-eligible: over the viewport image with no toolbar widget, popup
        // or other UI item claiming the pointer.
        bool Over = false;
        // Button levels sampled once per frame. Only the primary button drives
        // the gizmo; a held secondary or middle button means camera navigation
        // and keeps the gizmo from hovering or starting.
        bool PrimaryDown = false;
        bool SecondaryDown = false;
        bool MiddleDown = false;
        // Ctrl inverts the snap toggle for the frames it is held during a drag.
        bool CtrlDown = false;
    };

    struct GizmoSessionInput
    {
        // The view the viewport renders with this frame, or null without one.
        // Its translation origin must stay fixed during a drag: a changed view
        // cancels the gesture.
        const Engine::CameraView* Camera = nullptr;
        // The viewport image rectangle in window pixels.
        Gizmo::ViewportRect Viewport;
        Engine::Math::WorldGridPolicy Policy;
        GizmoSessionPointer Pointer;
        // Esc went down this frame and no text field is active.
        bool EscapePressed = false;
        bool WindowFocused = true;
        // Camera navigation is captured (RMB fly, MMB orbit, Alt chords, ...).
        // It blocks new gizmo interaction and cancels an open drag.
        bool NavigationActive = false;
        // A modal or popup is open, a text field is active, or another history
        // gesture (an Inspector drag) is open. Same effect as NavigationActive.
        bool InputBlocked = false;
    };

    enum class GizmoCursor : Engine::u8
    {
        Default,
        // Over a handle.
        Hand,
        // Dragging a handle.
        Grabbing
    };

    // Why no handles are shown while the tool would normally show some.
    enum class GizmoUnavailable : Engine::u8
    {
        None,
        // The Select tool has no handles by design.
        SelectTool,
        NoSelection,
        // No valid camera, a zero-sized viewport or a singular projection.
        NoView,
        // Camera-bearing entities have no Scale tool.
        ScaleNotAllowed,
        // The pivot is behind the camera or too close to it.
        NotVisible
    };

    const char* DescribeGizmoUnavailable(GizmoUnavailable reason);

    struct GizmoSessionResult
    {
        Gizmo::GizmoPhase Phase = Gizmo::GizmoPhase::Idle;
        Gizmo::GizmoHandle Hover = Gizmo::GizmoHandle::None;
        Gizmo::GizmoHandle Active = Gizmo::GizmoHandle::None;

        // The pointer belongs to the gizmo: a handle is hovered, a drag is open,
        // or the current press started on a handle (including the frame the
        // button is released and after an Esc cancel). While set, a press must
        // not pick and must not start camera navigation, and a click that began
        // on a handle must not pick when released. The Editor latches this for
        // the next GLFW event phase.
        bool OwnsPointer = false;
        // The primary button went down on a handle and has not yet been released
        // as of this frame (true on the release frame, false afterwards).
        bool PressStartedOnGizmo = false;

        // Snapping applied to this frame's drag (toggle xor Ctrl).
        bool SnapActive = false;
        // Events of this frame, for the Editor's pivot sync and announcements.
        bool GestureBegan = false;
        bool GestureEnded = false;
        bool GestureCancelled = false;
        Engine::u64 GestureId = 0;

        GizmoCursor Cursor = GizmoCursor::Default;
        GizmoUnavailable Unavailable = GizmoUnavailable::None;

        // Status-bar text. While dragging, e.g. "Move: +1.00, 0.00, 0.00 (snap 1 m)",
        // "Rotate X: +15.0\xC2\xB0 (snap 15\xC2\xB0)" or "Scale X: x1.60"; while hovering,
        // e.g. "Move X axis"; otherwise empty.
        std::string Status;

        // Screen-space draw commands, outlines first, valid until the next Update
        // or the destruction of the session.
        std::span<const Gizmo::GizmoDrawPrimitive> Primitives;
    };

    class GizmoSession
    {
    public:
        GizmoSession();
        GizmoSession(const GizmoSession&) = delete;
        GizmoSession& operator=(const GizmoSession&) = delete;

        // ---- Authoring state --------------------------------------------------

        const ViewportToolState& State() const { return m_State; }
        // True from the press on a handle until the release or cancel.
        bool DragActive() const { return m_Gizmo.Phase() == Gizmo::GizmoPhase::Active; }

        // Every mutator below is refused while a drag is open and, for snap
        // settings, atomic: an invalid step changes nothing.
        ToolStateResult SetState(const ViewportToolState& state);
        ToolStateResult SetTool(Gizmo::TransformTool tool);
        ToolStateResult SetSpace(Gizmo::TransformSpace space);
        ToolStateResult ToggleSpace();
        ToolStateResult SetSnapEnabled(bool enabled);
        ToolStateResult ToggleSnap();
        ToolStateResult SetSnapSettings(const Gizmo::SnapSettings& snap);

        // ---- Presentation -----------------------------------------------------

        // Colours are injected so this code carries no theme. Axis colours belong
        // on handles only; Hover and Active are the Selection tokens.
        void SetPalette(const Gizmo::GizmoPalette& palette) { m_Palette = palette; }
        void SetStyle(const Gizmo::GizmoStyle& style) { m_Style = style; }
        const Gizmo::GizmoPalette& Palette() const { return m_Palette; }
        const Gizmo::GizmoStyle& Style() const { return m_Style; }

        // ---- Commands ---------------------------------------------------------

        // Registers viewport.tool.select/translate/rotate/scale (Q W E R, Viewport
        // scope), viewport.space.toggle (X, Viewport scope) and viewport.snap.toggle
        // (no default chord). Every command is disabled with a reason while a drag
        // is open. Returns the first registration failure, or Registered. The
        // session must outlive the registry's use of these commands: their
        // callbacks capture this.
        RegisterResult RegisterCommands(CommandRegistry& registry);

        // ---- Frame ------------------------------------------------------------

        GizmoSessionResult Update(const GizmoSessionInput& input, IGizmoHost& host);

        // Updates the host refused since the session was created.
        Engine::u64 RejectedUpdateCount() const { return m_Gizmo.RejectedUpdateCount(); }

    private:
        class HostAdapter;

        struct OpenGesture
        {
            Gizmo::GizmoGestureKey Key;
            Gizmo::TransformTool Tool = Gizmo::TransformTool::Translate;
            Gizmo::TransformSpace Space = Gizmo::TransformSpace::World;
            Gizmo::GizmoHandle Handle = Gizmo::GizmoHandle::None;
            Gizmo::GizmoTransform Start;
            Gizmo::GizmoTransform Current;
            // The last non-empty numeric readout of the pure gizmo.
            std::string Readout;
        };

        ToolStateResult Refuse() const;
        std::string DescribeDrag(const Gizmo::SnapSettings& snap, bool snapActive, const Engine::Math::WorldGridPolicy& policy) const;

        ViewportToolState m_State;
        Gizmo::GizmoPalette m_Palette = DesignGizmoPalette();
        Gizmo::GizmoStyle m_Style;
        Gizmo::TransformGizmo m_Gizmo;

        // Per-frame scratch shared with the host adapter.
        GizmoSelection m_Selection;
        bool m_GestureBegan = false;
        bool m_GestureEnded = false;
        bool m_GestureCancelled = false;
        bool m_OpenGestureValid = false;
        OpenGesture m_Open;

        Gizmo::ScreenPoint m_LastFinitePointer;
        bool m_PreviousPrimaryDown = false;
        bool m_PressStartedOnGizmo = false;
    };
}
