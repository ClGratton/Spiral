#include "TransformGizmo.h"

#include "Engine/Math/DVec3Ops.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace Gizmo
{
    namespace
    {
        using Engine::Math::DVec3;

        constexpr double kPi = 3.14159265358979323846;
        // Below this angle (sine) between the pointer ray and a ring plane the
        // plane intersection is too unstable and a screen-space drag is used.
        constexpr double kRotationEdgeOnSine = 0.15;
        constexpr double kScaleReferenceFraction = 0.25;

        bool SameTransform(const GizmoTransform& lhs, const GizmoTransform& rhs)
        {
            return lhs.Position.Sector == rhs.Position.Sector
                && lhs.Position.Local.X == rhs.Position.Local.X
                && lhs.Position.Local.Y == rhs.Position.Local.Y
                && lhs.Position.Local.Z == rhs.Position.Local.Z
                && std::memcmp(&lhs.RotationDegrees, &rhs.RotationDegrees, sizeof(lhs.RotationDegrees)) == 0
                && std::memcmp(&lhs.Scale, &rhs.Scale, sizeof(lhs.Scale)) == 0;
        }

        GizmoProperty PropertyFor(TransformTool tool)
        {
            switch (tool)
            {
            case TransformTool::Rotate: return GizmoProperty::Rotation;
            case TransformTool::Scale: return GizmoProperty::Scale;
            default: return GizmoProperty::Position;
            }
        }

        // Plane handles are named for the plane; the normal is the third axis.
        int PlaneNormalAxis(GizmoHandle handle)
        {
            switch (handle)
            {
            case GizmoHandle::PlaneYZ: return 0;
            case GizmoHandle::PlaneXZ: return 1;
            case GizmoHandle::PlaneXY: return 2;
            default: return -1;
            }
        }

        std::string Format(const char* format, double value)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), format, value + 0.0);
            return buffer;
        }

        // Rounds before formatting so a tiny negative never prints "-0.00".
        double Tidy(double value, double scale)
        {
            return std::round(value * scale) / scale;
        }

        std::string TranslationReadout(const GizmoTransform& start, const GizmoTransform& now, GizmoHandle handle,
            const DVec3 axes[3], const Engine::Math::WorldGridPolicy& policy)
        {
            DVec3 moved;
            if (!Engine::Math::TryGetSectorLocalRelativePosition(now.Position, start.Position, policy, moved))
                return {};

            const AxisMask mask = ConstrainedAxes(handle);
            const bool constrained[3] = { mask.X, mask.Y, mask.Z };
            const char* names[3] = { "X", "Y", "Z" };
            std::string text;
            int count = 0;
            for (int axis = 0; axis < 3; ++axis)
                count += constrained[axis] ? 1 : 0;

            for (int axis = 0; axis < 3; ++axis)
            {
                if (!constrained[axis])
                    continue;

                if (!text.empty())
                    text += ", ";
                if (count > 1)
                    text += std::string(names[axis]) + " ";
                text += Format("%+.2f", Tidy(Engine::Math::Dot(moved, axes[axis]), 100.0));
            }

            return text + " m";
        }
    }

    GizmoFrameResult TransformGizmo::Update(const GizmoFrameInput& input, IGizmoEditHost& host)
    {
        GizmoFrameResult result;
        const bool primaryDown = input.Pointer.PrimaryDown;
        const bool pressed = primaryDown && !m_PreviousDown;
        m_PreviousDown = primaryDown;
        m_DrawList.clear();

        if (m_HasSession)
        {
            StepActive(input, primaryDown, host, result);
            if (m_HasSession)
                return result;
        }

        if (m_Phase == GizmoPhase::Cancelled)
        {
            if (primaryDown)
            {
                result.Phase = GizmoPhase::Cancelled;
                return result;
            }

            m_Phase = GizmoPhase::Idle;
        }

        m_Phase = GizmoPhase::Idle;
        result.Phase = GizmoPhase::Idle;
        result.SnapActive = IsSnapActive(input.Snap, input.Pointer.CtrlDown);
        if (!input.View || !input.Target || input.Tool == TransformTool::Select
            || (input.Tool == TransformTool::Scale && !input.Target->AllowScale))
        {
            return result;
        }

        DVec3 basis[3];
        BuildToolBasis(input.Tool, input.Space, input.Target->Transform.RotationDegrees, basis);
        GizmoGeometry geometry;
        if (!BuildGizmoGeometry(*input.View, input.Tool, input.Target->RelativePosition, basis, true, input.Style, geometry))
            return result;

        GizmoHandle hover = GizmoHandle::None;
        if (input.Pointer.Over && input.WindowFocused)
            hover = HitTestHandles(geometry, input.Pointer.Position, input.Style);

        if (pressed && hover != GizmoHandle::None && TryBegin(input, geometry, hover, host))
        {
            result.Phase = GizmoPhase::Active;
            result.Active = hover;
            result.WantsPointer = true;
            result.GestureId = m_Session.Key.GestureId;
            BuildDrawList(input, geometry, GizmoHandle::None, hover);
            return result;
        }

        if (hover != GizmoHandle::None)
        {
            m_Phase = GizmoPhase::Hover;
            result.Phase = GizmoPhase::Hover;
            result.Hover = hover;
            result.WantsPointer = true;
        }

        BuildDrawList(input, geometry, hover, GizmoHandle::None);
        return result;
    }

    bool TransformGizmo::TryBegin(
        const GizmoFrameInput& input,
        const GizmoGeometry& geometry,
        GizmoHandle handle,
        IGizmoEditHost& host)
    {
        Engine::Math::Ray ray;
        if (!TryScreenToRay(*input.View, input.Pointer.Position, ray))
            return false;

        Session session;
        session.Tool = input.Tool;
        session.Space = input.Space;
        session.Handle = handle;
        session.Start = input.Target->Transform;
        session.LastApplied = session.Start;
        session.Pivot = input.Target->RelativePosition;
        for (int axis = 0; axis < 3; ++axis)
            session.Axes[axis] = geometry.Axes[axis];
        session.Length = geometry.Length;
        session.View = *input.View;
        session.StartCursor = input.Pointer.Position;
        const int axisIndex = AxisIndex(handle);
        if (input.Tool == TransformTool::Rotate)
        {
            const DVec3 axis = session.Axes[axisIndex];
            if (std::abs(Engine::Math::Dot(axis, ray.Direction)) >= kRotationEdgeOnSine
                && SolveRotationArm(ray, session.Pivot, axis, session.StartArm))
            {
                session.Angle.Reset();
            }
            else
            {
                // Edge-on ring: start from the facing ring point nearest the cursor.
                const HandleShape* ring = nullptr;
                for (const HandleShape& shape : geometry.Shapes)
                {
                    if (shape.Handle == handle)
                        ring = &shape;
                }

                if (!ring || ring->Points.empty())
                    return false;

                size_t nearest = ring->Points.size();
                double nearestDistance = 0.0;
                for (size_t index = 0; index < ring->Points.size(); ++index)
                {
                    const double distance = std::hypot(
                        ring->Points[index].X - input.Pointer.Position.X,
                        ring->Points[index].Y - input.Pointer.Position.Y);
                    if (ring->Facing[index] && (nearest == ring->Points.size() || distance < nearestDistance))
                    {
                        nearest = index;
                        nearestDistance = distance;
                    }
                }

                if (nearest == ring->Points.size())
                    return false;

                const double angle = 2.0 * kPi * static_cast<double>(nearest) / static_cast<double>(input.Style.RingSegments);
                const DVec3 u = session.Axes[(axisIndex + 1) % 3];
                const DVec3 v = session.Axes[(axisIndex + 2) % 3];
                session.StartArm = u * std::cos(angle) + v * std::sin(angle);
                session.ScreenFallback = true;
                if (!SolveRingScreenTangent(*input.View, session.Pivot, axis, session.StartArm,
                        session.Length * input.Style.RingRadius, session.Tangent))
                {
                    return false;
                }
            }
        }
        else if (axisIndex >= 0)
        {
            if (!SolveAxisParameter(ray, session.Pivot, session.Axes[axisIndex], session.GrabT))
                return false;
        }
        else if (handle == GizmoHandle::Center && input.Tool == TransformTool::Translate)
        {
            if (!SolvePlanePoint(ray, session.Pivot, input.View->Forward, session.GrabPoint))
                return false;
        }
        else if (PlaneNormalAxis(handle) >= 0)
        {
            if (!SolvePlanePoint(ray, session.Pivot, session.Axes[PlaneNormalAxis(handle)], session.GrabPoint))
                return false;
        }

        session.Key = { m_NextGestureId++, input.Target->EntityId, PropertyFor(input.Tool) };
        m_Session = std::move(session);
        m_HasSession = true;
        m_Phase = GizmoPhase::Active;
        host.BeginGesture(m_Session.Key);
        return true;
    }

    bool TransformGizmo::Solve(
        const GizmoFrameInput& input,
        bool snapActive,
        GizmoTransform& outTransform,
        std::string& outReadout)
    {
        Session& session = m_Session;
        Engine::Math::Ray ray;
        if (!TryScreenToRay(session.View, input.Pointer.Position, ray))
            return false;

        const int axisIndex = AxisIndex(session.Handle);
        switch (session.Tool)
        {
        case TransformTool::Translate:
        {
            DVec3 raw;
            if (axisIndex >= 0)
            {
                double t = 0.0;
                if (!SolveAxisParameter(ray, session.Pivot, session.Axes[axisIndex], t))
                    return false;

                raw = session.Axes[axisIndex] * (t - session.GrabT);
            }
            else
            {
                DVec3 hit;
                const DVec3 normal = session.Handle == GizmoHandle::Center
                    ? session.View.Forward
                    : session.Axes[PlaneNormalAxis(session.Handle)];
                if (!SolvePlanePoint(ray, session.Pivot, normal, hit))
                    return false;

                raw = hit - session.GrabPoint;
            }

            if (!ApplyTranslation(session.Start, session.Axes, session.Handle, session.Space, raw,
                    snapActive, input.Snap, input.Policy, outTransform))
            {
                return false;
            }

            outReadout = TranslationReadout(session.Start, outTransform, session.Handle, session.Axes, input.Policy);
            return true;
        }
        case TransformTool::Rotate:
        {
            double total = 0.0;
            if (session.ScreenFallback)
            {
                total = RotationAngleFromScreenDrag(input.Pointer.Position, session.StartCursor, session.Tangent,
                    input.Style.TargetPixels);
            }
            else
            {
                DVec3 arm;
                if (!SolveRotationArm(ray, session.Pivot, session.Axes[axisIndex], arm))
                    return false;

                total = session.Angle.Update(RotationAngleBetween(session.Axes[axisIndex], session.StartArm, arm));
            }

            double appliedDegrees = 0.0;
            if (!ApplyRotation(session.Start, session.Axes[axisIndex], total, snapActive, input.Snap, outTransform, appliedDegrees))
                return false;

            outReadout = Format("%+.1f", Tidy(appliedDegrees, 10.0)) + "\xC2\xB0";
            return true;
        }
        case TransformTool::Scale:
        {
            double factor = 1.0;
            if (axisIndex >= 0)
            {
                double t = 0.0;
                if (!SolveAxisParameter(ray, session.Pivot, session.Axes[axisIndex], t))
                    return false;

                factor = SolveAxisScaleFactor(t, session.GrabT, session.Length * kScaleReferenceFraction);
            }
            else
            {
                factor = SolveUniformScaleFactor(input.Pointer.Position, session.StartCursor, input.Style.TargetPixels);
            }

            double applied = 1.0;
            if (!ApplyScale(session.Start, session.Handle, factor, snapActive, input.Snap, outTransform, applied))
                return false;

            outReadout = "x" + Format("%.2f", Tidy(applied, 100.0));
            return true;
        }
        case TransformTool::Select:
            break;
        }

        return false;
    }

    void TransformGizmo::CancelSession(bool primaryDown, IGizmoEditHost& host)
    {
        const GizmoGestureKey key = m_Session.Key;
        // The host resolves the entity by key, so the restore is attempted even
        // when this frame's target is another or no entity; a deleted entity
        // simply rejects it.
        if (!SameTransform(m_Session.LastApplied, m_Session.Start))
            host.ApplyTransform(key, m_Session.Start);

        host.CancelGesture(key);
        m_HasSession = false;
        m_Phase = primaryDown ? GizmoPhase::Cancelled : GizmoPhase::Idle;
    }

    void TransformGizmo::StepActive(
        const GizmoFrameInput& input,
        bool primaryDown,
        IGizmoEditHost& host,
        GizmoFrameResult& result)
    {
        const bool lost = !input.WindowFocused
            || input.Pointer.EscapePressed
            || !input.Target || input.Target->EntityId != m_Session.Key.EntityId
            || !input.View || !SameView(*input.View, m_Session.View);
        if (lost)
        {
            CancelSession(primaryDown, host);
            result.Phase = m_Phase;
            return;
        }

        const bool snapActive = IsSnapActive(input.Snap, input.Pointer.CtrlDown);
        GizmoTransform transform;
        std::string readout;
        if (Solve(input, snapActive, transform, readout) && !SameTransform(transform, m_Session.LastApplied))
        {
            if (host.ApplyTransform(m_Session.Key, transform))
                m_Session.LastApplied = transform;
            else
                ++m_RejectedUpdates;
        }

        if (!primaryDown)
        {
            host.EndGesture(m_Session.Key);
            m_HasSession = false;
            m_Phase = GizmoPhase::Idle;
            return;
        }

        result.Phase = GizmoPhase::Active;
        result.Active = m_Session.Handle;
        result.WantsPointer = true;
        result.SnapActive = snapActive;
        result.GestureId = m_Session.Key.GestureId;
        result.Readout = readout;

        GizmoGeometry geometry;
        if (BuildGizmoGeometry(*input.View, m_Session.Tool, input.Target->RelativePosition, m_Session.Axes, false,
                input.Style, geometry))
        {
            BuildDrawList(input, geometry, GizmoHandle::None, m_Session.Handle);
            if (!readout.empty())
            {
                GizmoDrawPrimitive text;
                text.Kind = GizmoPrimitiveKind::Text;
                text.Points = { { input.Pointer.Position.X + 16.0, input.Pointer.Position.Y + 16.0 } };
                text.Color = input.Palette.Label;
                text.Text = readout;
                m_DrawList.push_back(std::move(text));
            }
        }
    }

    void TransformGizmo::BuildDrawList(
        const GizmoFrameInput& input,
        const GizmoGeometry& geometry,
        GizmoHandle hover,
        GizmoHandle active)
    {
        AppendGizmoDrawList(geometry, hover, active, input.Palette, m_DrawList);
    }
}
