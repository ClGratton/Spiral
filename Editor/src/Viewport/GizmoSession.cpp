#include "GizmoSession.h"

#include "../Gizmo/SnapMath.h"

#include "Engine/Math/DVec3Ops.h"

#include <cmath>
#include <cstdio>

namespace SpiralEditor
{
    namespace
    {
        using Engine::Math::DVec3;

        constexpr Engine::u64 kGizmoItemTag = 1ull << 63;
        // GLFW key codes (a stable ABI, see ShortcutMap.h).
        constexpr int kKeyE = 69;
        constexpr int kKeyQ = 81;
        constexpr int kKeyR = 82;
        constexpr int kKeyW = 87;
        constexpr int kKeyX = 88;

        const char* VerbFor(Gizmo::GizmoProperty property)
        {
            switch (property)
            {
            case Gizmo::GizmoProperty::Rotation: return "Rotate";
            case Gizmo::GizmoProperty::Scale: return "Scale";
            default: return "Move";
            }
        }

        std::string Format(const char* format, double value)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), format, value + 0.0);
            return buffer;
        }

        // Two decimals, a sign only when the rounded value is not zero, never "-0.00".
        std::string FormatDelta(double value)
        {
            const double rounded = std::round(value * 100.0) / 100.0;
            return rounded == 0.0 ? "0.00" : Format("%+.2f", rounded);
        }

        std::string FormatStep(double value)
        {
            return Format("%.6g", value);
        }

        const char* AxisLetter(Gizmo::GizmoHandle handle)
        {
            switch (handle)
            {
            case Gizmo::GizmoHandle::AxisX: return "X";
            case Gizmo::GizmoHandle::AxisY: return "Y";
            case Gizmo::GizmoHandle::AxisZ: return "Z";
            default: return "";
            }
        }

        const char* PlaneLetters(Gizmo::GizmoHandle handle)
        {
            switch (handle)
            {
            case Gizmo::GizmoHandle::PlaneYZ: return "YZ";
            case Gizmo::GizmoHandle::PlaneXZ: return "XZ";
            case Gizmo::GizmoHandle::PlaneXY: return "XY";
            default: return "";
            }
        }

        std::string HoverText(Gizmo::TransformTool tool, Gizmo::GizmoHandle handle)
        {
            const char* verb = tool == Gizmo::TransformTool::Rotate ? "Rotate"
                : tool == Gizmo::TransformTool::Scale ? "Scale" : "Move";
            switch (handle)
            {
            case Gizmo::GizmoHandle::AxisX:
            case Gizmo::GizmoHandle::AxisY:
            case Gizmo::GizmoHandle::AxisZ:
                return std::string(verb) + " " + AxisLetter(handle) + (tool == Gizmo::TransformTool::Rotate ? " ring" : " axis");
            case Gizmo::GizmoHandle::PlaneYZ:
            case Gizmo::GizmoHandle::PlaneXZ:
            case Gizmo::GizmoHandle::PlaneXY:
                return std::string(verb) + " " + PlaneLetters(handle) + " plane";
            case Gizmo::GizmoHandle::Center:
                return tool == Gizmo::TransformTool::Scale ? "Scale uniformly" : "Move in the screen plane";
            default:
                return {};
            }
        }

        // The snap note of a drag: the active step, or that Ctrl changed the toggle.
        std::string SnapNote(const Gizmo::SnapSettings& snap, Gizmo::TransformTool tool, bool active)
        {
            std::string step;
            switch (tool)
            {
            case Gizmo::TransformTool::Rotate: step = FormatStep(snap.RotateStepDegrees) + "\xC2\xB0"; break;
            case Gizmo::TransformTool::Scale: step = FormatStep(snap.ScaleStep); break;
            default: step = FormatStep(snap.TranslateStep) + " m"; break;
            }

            if (active)
                return snap.Enabled ? " (snap " + step + ")" : " (snap " + step + ", Ctrl)";
            return snap.Enabled ? " (snap suspended, Ctrl)" : std::string();
        }

        bool TryRelativePosition(
            const GizmoSessionInput& input,
            const Gizmo::GizmoTransform& transform,
            Engine::Math::DVec3& outRelative)
        {
            const Engine::CameraView& camera = *input.Camera;
            if (camera.HasCanonicalTranslationOrigin)
            {
                return Engine::Math::TryGetSectorLocalRelativePosition(
                    transform.Position, camera.TranslationOriginPosition, input.Policy, outRelative);
            }

            Engine::Math::DVec3 world;
            if (!Engine::Math::TryComposeApproximateWorldPosition(transform.Position, input.Policy, world))
                return false;

            outRelative = world - camera.TranslationOrigin;
            return Engine::Math::AllFinite(outRelative);
        }
    }

    std::string SnapLatticeNote(const Gizmo::SnapSettings& snap, const Engine::Math::WorldGridPolicy& policy)
    {
        if (Gizmo::ValidateSnapSettings(snap) != Gizmo::SnapSettingsError::None
            || !Engine::Math::IsWorldGridPolicyValid(policy)
            || Gizmo::IsSectorLatticeAligned(snap.TranslateStep, policy))
        {
            return {};
        }

        return "grid restarts at sector boundaries";
    }

    Gizmo::GizmoPalette DesignGizmoPalette()
    {
        Gizmo::GizmoPalette palette;
        palette.AxisX = { 214, 86, 86, 255 };
        palette.AxisY = { 102, 184, 102, 255 };
        palette.AxisZ = { 178, 118, 214, 255 };
        palette.Center = { 210, 214, 218, 255 };
        palette.Outline = { 12, 14, 16, 230 };
        palette.Hover = { 61, 97, 128, 255 };
        palette.Active = { 69, 133, 179, 255 };
        palette.Label = { 235, 238, 240, 255 };
        return palette;
    }

    EditorHistory::EditGestureKey ToEditGestureKey(const Gizmo::GizmoGestureKey& key)
    {
        return { kGizmoItemTag | key.GestureId, key.EntityId, static_cast<Engine::u32>(key.Property) + 1u };
    }

    const char* DescribeGizmoUnavailable(GizmoUnavailable reason)
    {
        switch (reason)
        {
        case GizmoUnavailable::None: return "";
        case GizmoUnavailable::SelectTool: return "The Select tool has no handles";
        case GizmoUnavailable::NoSelection: return "Select an entity to use the transform tools";
        case GizmoUnavailable::NoView: return "The viewport has no valid camera";
        case GizmoUnavailable::ScaleNotAllowed: return "Camera entities have no Scale tool";
        case GizmoUnavailable::NotVisible: return "The selected entity is behind or too close to the camera";
        }
        return "";
    }

    // Forwards the pure gizmo's edit calls to the Editor's host and records the
    // open gesture, so the session can describe it and never loses a call.
    class GizmoSession::HostAdapter final : public Gizmo::IGizmoEditHost
    {
    public:
        HostAdapter(GizmoSession& session, IGizmoHost& host)
            : m_Session(session)
            , m_Host(host)
        {
        }

        void BeginGesture(const Gizmo::GizmoGestureKey& key) override
        {
            m_Session.m_Open = {};
            m_Session.m_Open.Key = key;
            m_Session.m_Open.Tool = m_Session.m_State.Tool;
            m_Session.m_Open.Space = m_Session.m_State.Space;
            m_Session.m_Open.Start = m_Session.m_Selection.Transform;
            m_Session.m_Open.Current = m_Session.m_Selection.Transform;
            m_Session.m_OpenGestureValid = true;
            m_Session.m_GestureBegan = true;
            m_Host.BeginGesture(
                EditorHistory::MakeHistoryLabel(VerbFor(key.Property), m_Session.m_Selection.Name), key);
        }

        bool ApplyTransform(const Gizmo::GizmoGestureKey& key, const Gizmo::GizmoTransform& transform) override
        {
            const bool accepted = m_Host.ApplyTransform(key, transform);
            if (accepted && m_Session.m_OpenGestureValid)
                m_Session.m_Open.Current = transform;
            return accepted;
        }

        void EndGesture(const Gizmo::GizmoGestureKey& key) override
        {
            m_Session.m_OpenGestureValid = false;
            m_Session.m_GestureEnded = true;
            m_Host.EndGesture(key);
        }

        void CancelGesture(const Gizmo::GizmoGestureKey& key) override
        {
            m_Session.m_OpenGestureValid = false;
            m_Session.m_GestureCancelled = true;
            m_Host.CancelGesture(key);
        }

    private:
        GizmoSession& m_Session;
        IGizmoHost& m_Host;
    };

    std::string GizmoSession::DescribeDrag(
        const Gizmo::SnapSettings& snap,
        bool snapActive,
        const Engine::Math::WorldGridPolicy& policy) const
    {
        const OpenGesture& open = m_Open;
        const std::string note = SnapNote(snap, open.Tool, snapActive);
        const std::string local = open.Space == Gizmo::TransformSpace::Local ? " (local)" : "";
        switch (open.Tool)
        {
        case Gizmo::TransformTool::Translate:
        {
            DVec3 moved;
            if (!Engine::Math::TryGetSectorLocalRelativePosition(open.Current.Position, open.Start.Position, policy, moved))
                return "Move" + local;

            DVec3 basis[3];
            Gizmo::BuildToolBasis(Gizmo::TransformTool::Translate, open.Space, open.Start.RotationDegrees, basis);
            return "Move" + local + ": " + FormatDelta(Engine::Math::Dot(moved, basis[0])) + ", "
                + FormatDelta(Engine::Math::Dot(moved, basis[1])) + ", " + FormatDelta(Engine::Math::Dot(moved, basis[2])) + note;
        }
        case Gizmo::TransformTool::Rotate:
            return std::string("Rotate ") + AxisLetter(open.Handle) + local + ": "
                + (open.Readout.empty() ? "+0.0\xC2\xB0" : open.Readout) + note;
        case Gizmo::TransformTool::Scale:
            return std::string("Scale") + (open.Handle == Gizmo::GizmoHandle::Center ? "" : std::string(" ") + AxisLetter(open.Handle))
                + ": " + (open.Readout.empty() ? "x1.00" : open.Readout) + note;
        default:
            return {};
        }
    }

    GizmoSession::GizmoSession() = default;

    ToolStateResult GizmoSession::Refuse() const
    {
        return { ToolStateStatus::DragInProgress, Gizmo::SnapSettingsError::None };
    }

    ToolStateResult GizmoSession::SetState(const ViewportToolState& state)
    {
        if (DragActive())
            return Refuse();

        const Gizmo::SnapSettingsError error = Gizmo::ValidateSnapSettings(state.Snap);
        if (error != Gizmo::SnapSettingsError::None)
            return { ToolStateStatus::InvalidSnapSettings, error };

        m_State = state;
        return {};
    }

    ToolStateResult GizmoSession::SetTool(Gizmo::TransformTool tool)
    {
        ViewportToolState next = m_State;
        next.Tool = tool;
        return SetState(next);
    }

    ToolStateResult GizmoSession::SetSpace(Gizmo::TransformSpace space)
    {
        ViewportToolState next = m_State;
        next.Space = space;
        return SetState(next);
    }

    ToolStateResult GizmoSession::ToggleSpace()
    {
        return SetSpace(m_State.Space == Gizmo::TransformSpace::World ? Gizmo::TransformSpace::Local : Gizmo::TransformSpace::World);
    }

    ToolStateResult GizmoSession::SetSnapEnabled(bool enabled)
    {
        ViewportToolState next = m_State;
        next.Snap.Enabled = enabled;
        return SetState(next);
    }

    ToolStateResult GizmoSession::ToggleSnap()
    {
        return SetSnapEnabled(!m_State.Snap.Enabled);
    }

    ToolStateResult GizmoSession::SetSnapSettings(const Gizmo::SnapSettings& snap)
    {
        ViewportToolState next = m_State;
        next.Snap = snap;
        return SetState(next);
    }

    RegisterResult GizmoSession::RegisterCommands(CommandRegistry& registry)
    {
        struct Entry
        {
            std::string_view Id;
            const char* Title;
            int Key;
            const char* Announcement;
        };
        const Entry entries[] = {
            { kCommandViewportToolSelect, "Select Tool", kKeyQ, "Select tool" },
            { kCommandViewportToolTranslate, "Translate Tool", kKeyW, "Translate tool" },
            { kCommandViewportToolRotate, "Rotate Tool", kKeyE, "Rotate tool" },
            { kCommandViewportToolScale, "Scale Tool", kKeyR, "Scale tool" },
            { kCommandViewportSpaceToggle, "Toggle Local/World Space", kKeyX, "" },
            { kCommandViewportSnapToggle, "Toggle Snapping", 0, "" }
        };

        for (const Entry& entry : entries)
        {
            CommandDescriptor descriptor;
            descriptor.Id = std::string(entry.Id);
            descriptor.Title = entry.Title;
            descriptor.Category = "Viewport";
            if (entry.Key != 0)
            {
                descriptor.DefaultShortcuts.push_back(
                    { { entry.Key, Engine::InputModifierNone }, { ShortcutScopeKind::Viewport, {} } });
            }

            descriptor.IsEnabled = [this]()
            {
                return DragActive() ? CommandAvailability::Disabled("A gizmo drag is in progress") : CommandAvailability {};
            };

            const std::string id = descriptor.Id;
            const std::string announcement = entry.Announcement;
            descriptor.Execute = [this, id, announcement](const CommandInvocation&) -> CommandOutcome
            {
                ToolStateResult result;
                std::string message = announcement;
                if (id == kCommandViewportToolSelect)
                    result = SetTool(Gizmo::TransformTool::Select);
                else if (id == kCommandViewportToolTranslate)
                    result = SetTool(Gizmo::TransformTool::Translate);
                else if (id == kCommandViewportToolRotate)
                    result = SetTool(Gizmo::TransformTool::Rotate);
                else if (id == kCommandViewportToolScale)
                    result = SetTool(Gizmo::TransformTool::Scale);
                else if (id == kCommandViewportSpaceToggle)
                {
                    result = ToggleSpace();
                    message = m_State.Space == Gizmo::TransformSpace::Local ? "Local space" : "World space";
                }
                else
                {
                    result = ToggleSnap();
                    message = m_State.Snap.Enabled ? "Snapping on" : "Snapping off";
                }

                if (!result.Ok())
                    return { false, "A gizmo drag is in progress" };
                return { true, message };
            };

            const RegisterResult registered = registry.Register(std::move(descriptor));
            if (!registered.Ok())
                return registered;
        }

        return {};
    }

    GizmoSessionResult GizmoSession::Update(const GizmoSessionInput& input, IGizmoHost& host)
    {
        m_GestureBegan = false;
        m_GestureEnded = false;
        m_GestureCancelled = false;

        // The view and the target are rebuilt every frame from the host's truth.
        Gizmo::GizmoView view;
        const bool haveView = input.Camera && input.Camera->Valid
            && Gizmo::TryBuildGizmoView(input.Camera->View, input.Camera->Projection, input.Viewport, view);

        m_Selection = {};
        const bool haveSelection = host.TryGetSelection(m_Selection);
        Gizmo::GizmoTarget target;
        bool haveTarget = false;
        if (haveSelection && haveView)
        {
            target.EntityId = m_Selection.EntityId;
            target.Transform = m_Selection.Transform;
            target.AllowScale = m_Selection.AllowScale;
            haveTarget = TryRelativePosition(input, m_Selection.Transform, target.RelativePosition);
        }

        // Navigation, a blocking UI state, a held secondary or middle button and an
        // ineligible pointer keep the gizmo from hovering or starting. Navigation
        // or a blocking state also ends an open drag the way Esc does (restore, no entry).
        const GizmoSessionPointer& pointer = input.Pointer;
        // A non-finite pointer (a stale or unavailable position) can neither hover
        // nor press, and an open drag keeps the last finite position.
        const bool pointerFinite = std::isfinite(pointer.Position.X) && std::isfinite(pointer.Position.Y);
        if (pointerFinite)
            m_LastFinitePointer = pointer.Position;
        const bool blocked = input.NavigationActive || input.InputBlocked;
        const bool wasDragging = DragActive();

        Gizmo::GizmoFrameInput frame;
        frame.View = haveView ? &view : nullptr;
        frame.Target = haveTarget ? &target : nullptr;
        frame.Tool = m_State.Tool;
        frame.Space = m_State.Space;
        frame.Snap = m_State.Snap;
        frame.Policy = input.Policy;
        frame.Style = m_Style;
        frame.Palette = m_Palette;
        frame.Pointer.Position = pointerFinite ? pointer.Position : m_LastFinitePointer;
        frame.Pointer.Over = pointer.Over && pointerFinite && !blocked && !pointer.SecondaryDown && !pointer.MiddleDown;
        frame.Pointer.PrimaryDown = pointer.PrimaryDown;
        frame.Pointer.CtrlDown = pointer.CtrlDown;
        frame.Pointer.EscapePressed = input.EscapePressed || (wasDragging && blocked);
        frame.WindowFocused = input.WindowFocused;

        HostAdapter adapter(*this, host);
        const Gizmo::GizmoFrameResult drawn = m_Gizmo.Update(frame, adapter);

        // A press that started on a handle owns the pointer until it is released,
        // even after an Esc cancel and on the release frame itself, so the click
        // can neither pick nor start navigation.
        const bool pressed = pointer.PrimaryDown && !m_PreviousPrimaryDown;
        if (pressed)
            m_PressStartedOnGizmo = drawn.WantsPointer;
        const bool latched = m_PressStartedOnGizmo;
        if (!pointer.PrimaryDown)
            m_PressStartedOnGizmo = false;
        m_PreviousPrimaryDown = pointer.PrimaryDown;

        GizmoSessionResult result;
        result.Phase = drawn.Phase;
        result.Hover = drawn.Hover;
        result.Active = drawn.Active;
        result.OwnsPointer = drawn.WantsPointer || latched;
        result.PressStartedOnGizmo = latched;
        result.SnapActive = drawn.SnapActive;
        result.GestureBegan = m_GestureBegan;
        result.GestureEnded = m_GestureEnded;
        result.GestureCancelled = m_GestureCancelled;
        result.GestureId = drawn.GestureId;
        result.Primitives = m_Gizmo.DrawList();

        if (m_GestureBegan)
            m_Open.Handle = drawn.Active;
        if (m_OpenGestureValid && !drawn.Readout.empty())
            m_Open.Readout = drawn.Readout;

        if (drawn.Phase == Gizmo::GizmoPhase::Active && m_OpenGestureValid)
        {
            result.Cursor = GizmoCursor::Grabbing;
            result.Status = DescribeDrag(m_State.Snap, drawn.SnapActive, input.Policy);
        }
        else if (drawn.Hover != Gizmo::GizmoHandle::None)
        {
            result.Cursor = GizmoCursor::Hand;
            result.Status = HoverText(m_State.Tool, drawn.Hover);
        }

        if (m_State.Tool == Gizmo::TransformTool::Select)
            result.Unavailable = GizmoUnavailable::SelectTool;
        else if (!haveSelection)
            result.Unavailable = GizmoUnavailable::NoSelection;
        else if (!haveView)
            result.Unavailable = GizmoUnavailable::NoView;
        else if (m_State.Tool == Gizmo::TransformTool::Scale && !m_Selection.AllowScale)
            result.Unavailable = GizmoUnavailable::ScaleNotAllowed;
        else if (result.Primitives.empty() && drawn.Phase != Gizmo::GizmoPhase::Active)
            result.Unavailable = GizmoUnavailable::NotVisible;

        return result;
    }
}
