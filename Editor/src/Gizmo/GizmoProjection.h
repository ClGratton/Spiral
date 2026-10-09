#pragma once

#include "GizmoTypes.h"

#include "Engine/Math/Ray.h"

namespace Gizmo
{
    // The viewport image rectangle in window pixels (top-left origin).
    struct ViewportRect
    {
        double X = 0.0;
        double Y = 0.0;
        double Width = 0.0;
        double Height = 0.0;

        bool operator==(const ViewportRect&) const = default;
    };

    // Per-frame projection state built from the CameraView matrices the
    // viewport already renders with. All positions are in the same
    // camera-relative space as the view (the view's translation origin), so a
    // far-sector scene keeps float-sized coordinates. Only perspective
    // left-handed row-vector projections with depth 0..1 are supported.
    struct GizmoView
    {
        Engine::Math::Mat4 View;
        Engine::Math::Mat4 Projection;
        ViewportRect Viewport;
        double ViewProjection[16] {};
        double InverseViewProjection[16] {};
        Engine::Math::DVec3 Eye;
        Engine::Math::DVec3 Forward;
        double NearDepth = 0.0;
        double YScale = 0.0;
    };

    // Fails for non-finite or singular matrices, a non-perspective projection,
    // or an empty rectangle.
    bool TryBuildGizmoView(
        const Engine::Math::Mat4& view,
        const Engine::Math::Mat4& projection,
        const ViewportRect& viewport,
        GizmoView& outView);

    // True when two views would map every point and pixel identically.
    bool SameView(const GizmoView& lhs, const GizmoView& rhs);

    struct ProjectedPoint
    {
        ScreenPoint Screen;
        // View-space depth (clip w).
        double Depth = 0.0;
        // NDC z in 0..1 between the near and far planes when in front of the camera.
        double NdcZ = 0.0;
    };

    // Fails for points at or behind the eye plane.
    bool ProjectToScreen(const GizmoView& view, const Engine::Math::DVec3& point, ProjectedPoint& outProjected);

    // Ray from the eye through the pixel, in camera-relative space.
    bool TryScreenToRay(const GizmoView& view, const ScreenPoint& pixel, Engine::Math::Ray& outRay);

    // World length of targetPixels pixels measured perpendicular to the view
    // direction at the point's depth (clamped to the near plane), so a handle
    // that long keeps a constant on-screen size. Fails behind the camera.
    bool TryComputeGizmoLength(
        const GizmoView& view,
        const Engine::Math::DVec3& origin,
        double targetPixels,
        double& outLength);
}
