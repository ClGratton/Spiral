#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"

namespace Gizmo
{
    using Engine::i64;
    using Engine::u32;
    using Engine::u64;
    using Engine::u8;

    // Inspector range for Transform Scale; the gizmo never writes outside it.
    inline constexpr float kMinimumScale = 0.01f;
    inline constexpr float kMaximumScale = 100.0f;

    // Window pixels, origin at the top-left, y down.
    struct ScreenPoint
    {
        double X = 0.0;
        double Y = 0.0;
    };

    // Select is the Q tool: no handles are shown.
    enum class TransformTool : u8
    {
        Select,
        Translate,
        Rotate,
        Scale
    };

    enum class TransformSpace : u8
    {
        World,
        Local
    };

    // AxisX/Y/Z are arrows (Translate), rings (Rotate) or boxes (Scale).
    // PlaneYZ/XZ/XY are named for the plane they span, so PlaneXY moves along
    // X and Y. Center is screen-space translate or uniform scale.
    enum class GizmoHandle : u8
    {
        None,
        AxisX,
        AxisY,
        AxisZ,
        PlaneYZ,
        PlaneXZ,
        PlaneXY,
        Center
    };

    struct AxisMask
    {
        bool X = false;
        bool Y = false;
        bool Z = false;

        bool operator==(const AxisMask&) const = default;
    };

    struct GizmoTransform
    {
        Engine::Math::SectorLocalPosition Position;
        Engine::Math::Vec3 RotationDegrees;
        Engine::Math::Vec3 Scale { 1.0f, 1.0f, 1.0f };
    };

    // Index 0..2 for AxisX/Y/Z, otherwise -1.
    inline int AxisIndex(GizmoHandle handle)
    {
        switch (handle)
        {
        case GizmoHandle::AxisX: return 0;
        case GizmoHandle::AxisY: return 1;
        case GizmoHandle::AxisZ: return 2;
        default: return -1;
        }
    }

    // Axes a handle constrains: one for an axis handle, two for a plane
    // handle, all three for Center, none for None.
    inline AxisMask ConstrainedAxes(GizmoHandle handle)
    {
        switch (handle)
        {
        case GizmoHandle::AxisX: return { true, false, false };
        case GizmoHandle::AxisY: return { false, true, false };
        case GizmoHandle::AxisZ: return { false, false, true };
        case GizmoHandle::PlaneYZ: return { false, true, true };
        case GizmoHandle::PlaneXZ: return { true, false, true };
        case GizmoHandle::PlaneXY: return { true, true, false };
        case GizmoHandle::Center: return { true, true, true };
        default: return {};
        }
    }
}
