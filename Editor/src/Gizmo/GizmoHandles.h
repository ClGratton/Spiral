#pragma once

#include "GizmoProjection.h"

#include <string>
#include <vector>

namespace Gizmo
{
    struct GizmoStyle
    {
        // Gizmo length in pixels; the Editor scales it with its UI scale.
        double TargetPixels = 96.0;
        double LineHitPixels = 6.0;
        double CapHitPixels = 8.0;
        double CenterRadiusPixels = 7.0;
        // Fractions of the gizmo length.
        double CapStart = 0.8;
        double PlaneStart = 0.3;
        double PlaneEnd = 0.6;
        double RingRadius = 1.0;
        // Axis handles shorter than this on screen (axis pointing at the
        // camera) and plane handles smaller than this area are hidden and
        // cannot be picked.
        double MinimumAxisPixels = 12.0;
        double MinimumPlaneAreaPixels = 36.0;
        u32 RingSegments = 64;
    };

    enum class HandleShapeKind : u8
    {
        Segment,
        Quad,
        Disc,
        Ring
    };

    struct HandleShape
    {
        GizmoHandle Handle = GizmoHandle::None;
        HandleShapeKind Kind = HandleShapeKind::Segment;
        bool Enabled = false;
        // View depth used to break equal-distance hit ties.
        double Depth = 0.0;
        double Radius = 0.0;
        // Segment: origin, tip. Quad: four corners. Disc: centre. Ring: closed polyline.
        std::vector<ScreenPoint> Points;
        // Ring only: whether the segment starting at Points[i] faces the camera.
        std::vector<u8> Facing;
    };

    struct GizmoGeometry
    {
        bool Valid = false;
        TransformTool Tool = TransformTool::Select;
        Engine::Math::DVec3 Origin;
        // Oriented basis. Translate and Scale axes are flipped toward the
        // camera when requested; the basis is otherwise passed through.
        Engine::Math::DVec3 Axes[3];
        double Length = 0.0;
        ScreenPoint OriginScreen;
        double OriginDepth = 0.0;
        std::vector<HandleShape> Shapes;
    };

    // Builds the pickable and drawable handle set for a tool. basis must be
    // three orthonormal vectors (world axes or the entity's local axes).
    // Select produces no shapes. Fails when the origin is behind the camera.
    bool BuildGizmoGeometry(
        const GizmoView& view,
        TransformTool tool,
        const Engine::Math::DVec3& origin,
        const Engine::Math::DVec3 basis[3],
        bool orientTowardEye,
        const GizmoStyle& style,
        GizmoGeometry& outGeometry);

    // Hit test in pixels: the smallest screen distance wins, ties go to the
    // smaller view depth, then to Center, planes and finally axes.
    GizmoHandle HitTestHandles(const GizmoGeometry& geometry, const ScreenPoint& cursor, const GizmoStyle& style);

    struct GizmoColor
    {
        u8 R = 255;
        u8 G = 255;
        u8 B = 255;
        u8 A = 255;

        bool operator==(const GizmoColor&) const = default;
    };

    // Colours are injected so the pure code carries no theme. Axis colours are
    // allowed on gizmo handles only; hover and active use the Selection tokens.
    struct GizmoPalette
    {
        GizmoColor AxisX { 214, 86, 86, 255 };
        GizmoColor AxisY { 102, 184, 102, 255 };
        GizmoColor AxisZ { 86, 140, 214, 255 };
        GizmoColor Center { 210, 214, 218, 255 };
        GizmoColor Outline { 12, 14, 16, 230 };
        GizmoColor Hover { 61, 97, 128, 255 };
        GizmoColor Active { 69, 133, 179, 255 };
        GizmoColor Label { 235, 238, 240, 255 };
    };

    enum class GizmoPrimitiveKind : u8
    {
        Line,
        Polyline,
        Polygon,
        Circle,
        Text
    };

    // Screen-space draw command. Line: Points[0..1]. Polyline: open or Closed
    // stroke. Polygon: filled convex. Circle: Points[0] with Radius, filled
    // when Filled. Text: Points[0] is the top-left anchor.
    struct GizmoDrawPrimitive
    {
        GizmoPrimitiveKind Kind = GizmoPrimitiveKind::Line;
        std::vector<ScreenPoint> Points;
        GizmoColor Color;
        float Thickness = 1.0f;
        bool Closed = false;
        bool Filled = false;
        double Radius = 0.0;
        std::string Text;
    };

    // Appends the primitives for the geometry; outlines first so handles stay
    // legible on any scene. Letters X/Y/Z mark the axes without relying on colour.
    void AppendGizmoDrawList(
        const GizmoGeometry& geometry,
        GizmoHandle hover,
        GizmoHandle active,
        const GizmoPalette& palette,
        std::vector<GizmoDrawPrimitive>& outPrimitives);
}
