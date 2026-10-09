#include "GizmoHandles.h"

#include "Engine/Math/DVec3Ops.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Gizmo
{
    namespace
    {
        using Engine::Math::DVec3;

        constexpr double kPi = 3.14159265358979323846;
        constexpr double kFaceOnTolerance = 1e-6;

        double Distance(const ScreenPoint& a, const ScreenPoint& b)
        {
            return std::hypot(a.X - b.X, a.Y - b.Y);
        }

        double DistanceToSegment(const ScreenPoint& point, const ScreenPoint& a, const ScreenPoint& b)
        {
            const double dx = b.X - a.X;
            const double dy = b.Y - a.Y;
            const double lengthSquared = dx * dx + dy * dy;
            if (lengthSquared < 1e-12)
                return Distance(point, a);

            const double t = std::clamp(((point.X - a.X) * dx + (point.Y - a.Y) * dy) / lengthSquared, 0.0, 1.0);
            return Distance(point, { a.X + dx * t, a.Y + dy * t });
        }

        double SignedArea(const std::vector<ScreenPoint>& points)
        {
            double area = 0.0;
            for (size_t index = 0; index < points.size(); ++index)
            {
                const ScreenPoint& a = points[index];
                const ScreenPoint& b = points[(index + 1) % points.size()];
                area += a.X * b.Y - b.X * a.Y;
            }

            return area * 0.5;
        }

        bool InsideConvex(const std::vector<ScreenPoint>& polygon, const ScreenPoint& point)
        {
            const double orientation = SignedArea(polygon) < 0.0 ? -1.0 : 1.0;
            for (size_t index = 0; index < polygon.size(); ++index)
            {
                const ScreenPoint& a = polygon[index];
                const ScreenPoint& b = polygon[(index + 1) % polygon.size()];
                const double cross = (b.X - a.X) * (point.Y - a.Y) - (b.Y - a.Y) * (point.X - a.X);
                if (cross * orientation < 0.0)
                    return false;
            }

            return true;
        }

        // Handle priority for equal distance and depth.
        int Priority(GizmoHandle handle)
        {
            switch (handle)
            {
            case GizmoHandle::Center: return 0;
            case GizmoHandle::PlaneYZ:
            case GizmoHandle::PlaneXZ:
            case GizmoHandle::PlaneXY: return 1;
            default: return 2;
            }
        }

        struct Hit
        {
            GizmoHandle Handle = GizmoHandle::None;
            double Distance = std::numeric_limits<double>::infinity();
            double Depth = 0.0;
        };

        bool Better(const Hit& candidate, const Hit& best)
        {
            if (best.Handle == GizmoHandle::None)
                return true;
            if (candidate.Distance != best.Distance)
                return candidate.Distance < best.Distance;
            if (candidate.Depth != best.Depth)
                return candidate.Depth < best.Depth;
            if (Priority(candidate.Handle) != Priority(best.Handle))
                return Priority(candidate.Handle) < Priority(best.Handle);
            return static_cast<u8>(candidate.Handle) < static_cast<u8>(best.Handle);
        }

        GizmoColor AxisColor(GizmoHandle handle, const GizmoPalette& palette)
        {
            switch (handle)
            {
            case GizmoHandle::AxisX:
            case GizmoHandle::PlaneYZ: return palette.AxisX;
            case GizmoHandle::AxisY:
            case GizmoHandle::PlaneXZ: return palette.AxisY;
            case GizmoHandle::AxisZ:
            case GizmoHandle::PlaneXY: return palette.AxisZ;
            default: return palette.Center;
            }
        }

        GizmoDrawPrimitive MakeLine(const ScreenPoint& a, const ScreenPoint& b, GizmoColor color, float thickness)
        {
            GizmoDrawPrimitive primitive;
            primitive.Kind = GizmoPrimitiveKind::Line;
            primitive.Points = { a, b };
            primitive.Color = color;
            primitive.Thickness = thickness;
            return primitive;
        }

        ScreenPoint Add(const ScreenPoint& point, double dx, double dy)
        {
            return { point.X + dx, point.Y + dy };
        }

        // Cap polygon at the tip of a screen-space axis: a triangle for an
        // arrow, a square for a scale box.
        std::vector<ScreenPoint> CapPolygon(const ScreenPoint& origin, const ScreenPoint& tip, bool square)
        {
            const double dx = tip.X - origin.X;
            const double dy = tip.Y - origin.Y;
            const double length = std::hypot(dx, dy);
            const double ux = length > 1e-9 ? dx / length : 1.0;
            const double uy = length > 1e-9 ? dy / length : 0.0;
            const double nx = -uy;
            const double ny = ux;
            if (square)
            {
                const double h = 5.0;
                return {
                    Add(tip, -ux * h * 2.0 + nx * h, -uy * h * 2.0 + ny * h),
                    Add(tip, nx * h, ny * h),
                    Add(tip, -nx * h, -ny * h),
                    Add(tip, -ux * h * 2.0 - nx * h, -uy * h * 2.0 - ny * h)
                };
            }

            return {
                Add(tip, ux * 8.0, uy * 8.0),
                Add(tip, -ux * 6.0 + nx * 5.0, -uy * 6.0 + ny * 5.0),
                Add(tip, -ux * 6.0 - nx * 5.0, -uy * 6.0 - ny * 5.0)
            };
        }

        const char* AxisLetter(GizmoHandle handle)
        {
            switch (handle)
            {
            case GizmoHandle::AxisX: return "X";
            case GizmoHandle::AxisY: return "Y";
            case GizmoHandle::AxisZ: return "Z";
            default: return "";
            }
        }

        void PushOutlined(
            std::vector<GizmoDrawPrimitive>& out,
            GizmoDrawPrimitive primitive,
            const GizmoPalette& palette,
            GizmoColor color,
            float thickness)
        {
            GizmoDrawPrimitive outline = primitive;
            outline.Color = palette.Outline;
            outline.Thickness = thickness + 2.0f;
            if (primitive.Kind != GizmoPrimitiveKind::Polygon)
                out.push_back(std::move(outline));

            primitive.Color = color;
            primitive.Thickness = thickness;
            out.push_back(std::move(primitive));
        }
    }

    bool BuildGizmoGeometry(
        const GizmoView& view,
        TransformTool tool,
        const DVec3& origin,
        const DVec3 basis[3],
        bool orientTowardEye,
        const GizmoStyle& style,
        GizmoGeometry& outGeometry)
    {
        GizmoGeometry geometry;
        geometry.Tool = tool;
        geometry.Origin = origin;

        ProjectedPoint originProjected;
        double length = 0.0;
        if (!ProjectToScreen(view, origin, originProjected)
            || !TryComputeGizmoLength(view, origin, style.TargetPixels, length))
        {
            return false;
        }

        geometry.Length = length;
        geometry.OriginScreen = originProjected.Screen;
        geometry.OriginDepth = originProjected.Depth;
        const DVec3 toEye = view.Eye - origin;
        const double toEyeLength = Engine::Math::Length(toEye);
        for (u32 axis = 0; axis < 3; ++axis)
        {
            geometry.Axes[axis] = basis[axis];
            const bool flips = orientTowardEye && (tool == TransformTool::Translate || tool == TransformTool::Scale);
            if (flips && Engine::Math::Dot(basis[axis], toEye) < 0.0)
                geometry.Axes[axis] = -basis[axis];
        }

        geometry.Valid = true;
        if (tool == TransformTool::Select)
        {
            outGeometry = std::move(geometry);
            return true;
        }

        constexpr GizmoHandle kAxisHandles[3] = { GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ };
        if (tool == TransformTool::Rotate)
        {
            for (u32 axis = 0; axis < 3; ++axis)
            {
                const DVec3 u = geometry.Axes[(axis + 1) % 3];
                const DVec3 v = geometry.Axes[(axis + 2) % 3];
                HandleShape ring;
                ring.Handle = kAxisHandles[axis];
                ring.Kind = HandleShapeKind::Ring;
                ring.Depth = geometry.OriginDepth;
                bool complete = true;
                std::vector<DVec3> worldPoints;
                for (u32 segment = 0; segment < style.RingSegments; ++segment)
                {
                    const double angle = 2.0 * kPi * segment / style.RingSegments;
                    const DVec3 point = origin + (u * std::cos(angle) + v * std::sin(angle)) * (length * style.RingRadius);
                    ProjectedPoint projected;
                    if (!ProjectToScreen(view, point, projected))
                    {
                        complete = false;
                        break;
                    }

                    worldPoints.push_back(point);
                    ring.Points.push_back(projected.Screen);
                }

                if (complete)
                {
                    for (u32 segment = 0; segment < style.RingSegments; ++segment)
                    {
                        const DVec3& a = worldPoints[segment];
                        const DVec3& b = worldPoints[(segment + 1) % style.RingSegments];
                        const DVec3 middle = (a + b) * 0.5;
                        // A ring seen exactly face-on lies on the silhouette of its sphere;
                        // the tolerance keeps all of it pickable instead of none.
                        const double facing = Engine::Math::Dot(middle - origin, toEye);
                        ring.Facing.push_back(facing >= -kFaceOnTolerance * length * style.RingRadius * toEyeLength ? 1 : 0);
                    }
                }

                ring.Enabled = complete;
                geometry.Shapes.push_back(std::move(ring));
            }

            outGeometry = std::move(geometry);
            return true;
        }

        for (u32 axis = 0; axis < 3; ++axis)
        {
            HandleShape shape;
            shape.Handle = kAxisHandles[axis];
            shape.Kind = HandleShapeKind::Segment;
            shape.Depth = geometry.OriginDepth;
            ProjectedPoint tip;
            if (ProjectToScreen(view, origin + geometry.Axes[axis] * length, tip))
            {
                shape.Points = { geometry.OriginScreen, tip.Screen };
                shape.Enabled = Distance(geometry.OriginScreen, tip.Screen) >= style.MinimumAxisPixels;
            }

            geometry.Shapes.push_back(std::move(shape));
        }

        if (tool == TransformTool::Translate)
        {
            constexpr GizmoHandle kPlaneHandles[3] = { GizmoHandle::PlaneYZ, GizmoHandle::PlaneXZ, GizmoHandle::PlaneXY };
            constexpr u32 kPlaneAxes[3][2] = { { 1, 2 }, { 0, 2 }, { 0, 1 } };
            for (u32 plane = 0; plane < 3; ++plane)
            {
                const DVec3& a = geometry.Axes[kPlaneAxes[plane][0]];
                const DVec3& b = geometry.Axes[kPlaneAxes[plane][1]];
                const double near = length * style.PlaneStart;
                const double far = length * style.PlaneEnd;
                const DVec3 corners[4] = {
                    origin + a * near + b * near,
                    origin + a * far + b * near,
                    origin + a * far + b * far,
                    origin + a * near + b * far
                };

                HandleShape shape;
                shape.Handle = kPlaneHandles[plane];
                shape.Kind = HandleShapeKind::Quad;
                bool complete = true;
                double depthSum = 0.0;
                for (const DVec3& corner : corners)
                {
                    ProjectedPoint projected;
                    if (!ProjectToScreen(view, corner, projected))
                    {
                        complete = false;
                        break;
                    }

                    shape.Points.push_back(projected.Screen);
                    depthSum += projected.Depth;
                }

                if (complete)
                {
                    shape.Depth = depthSum * 0.25;
                    shape.Enabled = std::abs(SignedArea(shape.Points)) >= style.MinimumPlaneAreaPixels;
                }

                geometry.Shapes.push_back(std::move(shape));
            }
        }

        HandleShape center;
        center.Handle = GizmoHandle::Center;
        center.Kind = HandleShapeKind::Disc;
        center.Enabled = true;
        center.Depth = geometry.OriginDepth;
        center.Radius = style.CenterRadiusPixels;
        center.Points = { geometry.OriginScreen };
        geometry.Shapes.push_back(std::move(center));

        outGeometry = std::move(geometry);
        return true;
    }

    GizmoHandle HitTestHandles(const GizmoGeometry& geometry, const ScreenPoint& cursor, const GizmoStyle& style)
    {
        Hit best;
        for (const HandleShape& shape : geometry.Shapes)
        {
            if (!shape.Enabled)
                continue;

            Hit candidate;
            candidate.Handle = shape.Handle;
            candidate.Depth = shape.Depth;
            bool accepted = false;
            switch (shape.Kind)
            {
            case HandleShapeKind::Segment:
            {
                const double line = DistanceToSegment(cursor, shape.Points[0], shape.Points[1]);
                const double cap = Distance(cursor, shape.Points[1]);
                // The distance to the segment never exceeds the distance to its tip,
                // so the cap radius only widens what is accepted, not the ranking.
                accepted = line <= style.LineHitPixels || cap <= style.CapHitPixels;
                candidate.Distance = line;
                break;
            }
            case HandleShapeKind::Quad:
                accepted = InsideConvex(shape.Points, cursor);
                candidate.Distance = 0.0;
                break;
            case HandleShapeKind::Disc:
            {
                const double distance = Distance(cursor, shape.Points[0]);
                accepted = distance <= shape.Radius;
                candidate.Distance = std::max(0.0, distance - shape.Radius);
                break;
            }
            case HandleShapeKind::Ring:
            {
                double nearest = std::numeric_limits<double>::infinity();
                for (size_t index = 0; index < shape.Points.size(); ++index)
                {
                    if (!shape.Facing[index])
                        continue;

                    nearest = std::min(nearest, DistanceToSegment(
                        cursor, shape.Points[index], shape.Points[(index + 1) % shape.Points.size()]));
                }

                accepted = nearest <= style.LineHitPixels;
                candidate.Distance = nearest;
                break;
            }
            }

            if (accepted && Better(candidate, best))
                best = candidate;
        }

        return best.Handle;
    }

    void AppendGizmoDrawList(
        const GizmoGeometry& geometry,
        GizmoHandle hover,
        GizmoHandle active,
        const GizmoPalette& palette,
        std::vector<GizmoDrawPrimitive>& outPrimitives)
    {
        if (!geometry.Valid)
            return;

        for (const HandleShape& shape : geometry.Shapes)
        {
            if (!shape.Enabled)
                continue;

            const bool isActive = shape.Handle == active;
            const bool isHover = !isActive && shape.Handle == hover && active == GizmoHandle::None;
            GizmoColor color = AxisColor(shape.Handle, palette);
            float thickness = 2.0f;
            if (isActive)
            {
                color = palette.Active;
                thickness = 4.0f;
            }
            else if (isHover)
            {
                color = palette.Hover;
                thickness = 3.0f;
            }

            switch (shape.Kind)
            {
            case HandleShapeKind::Segment:
            {
                PushOutlined(outPrimitives, MakeLine(shape.Points[0], shape.Points[1], color, thickness), palette, color, thickness);
                GizmoDrawPrimitive cap;
                cap.Kind = GizmoPrimitiveKind::Polygon;
                cap.Points = CapPolygon(shape.Points[0], shape.Points[1], geometry.Tool == TransformTool::Scale);
                cap.Color = color;
                cap.Filled = true;
                outPrimitives.push_back(std::move(cap));

                const double dx = shape.Points[1].X - shape.Points[0].X;
                const double dy = shape.Points[1].Y - shape.Points[0].Y;
                const double length = std::max(1e-9, std::hypot(dx, dy));
                GizmoDrawPrimitive label;
                label.Kind = GizmoPrimitiveKind::Text;
                label.Points = { Add(shape.Points[1], dx / length * 14.0 - 4.0, dy / length * 14.0 - 7.0) };
                label.Color = palette.Label;
                label.Text = AxisLetter(shape.Handle);
                outPrimitives.push_back(std::move(label));
                break;
            }
            case HandleShapeKind::Quad:
            {
                GizmoDrawPrimitive fill;
                fill.Kind = GizmoPrimitiveKind::Polygon;
                fill.Points = shape.Points;
                fill.Color = color;
                fill.Color.A = isActive || isHover ? 190 : 110;
                fill.Filled = true;
                outPrimitives.push_back(std::move(fill));

                GizmoDrawPrimitive border;
                border.Kind = GizmoPrimitiveKind::Polyline;
                border.Points = shape.Points;
                border.Closed = true;
                PushOutlined(outPrimitives, std::move(border), palette, color, 1.5f);
                break;
            }
            case HandleShapeKind::Disc:
            {
                GizmoDrawPrimitive circle;
                circle.Kind = GizmoPrimitiveKind::Circle;
                circle.Points = shape.Points;
                circle.Radius = shape.Radius;
                circle.Filled = isActive || isHover;
                PushOutlined(outPrimitives, std::move(circle), palette, isActive || isHover ? color : palette.Center, thickness);
                break;
            }
            case HandleShapeKind::Ring:
            {
                GizmoDrawPrimitive back;
                back.Kind = GizmoPrimitiveKind::Polyline;
                back.Points = shape.Points;
                back.Closed = true;
                back.Color = color;
                back.Color.A = 60;
                back.Thickness = 1.0f;
                outPrimitives.push_back(std::move(back));

                size_t firstFacing = shape.Points.size();
                for (size_t index = 0; index < shape.Points.size(); ++index)
                {
                    if (!shape.Facing[index])
                        continue;

                    if (firstFacing == shape.Points.size())
                        firstFacing = index;
                    GizmoDrawPrimitive segment = MakeLine(
                        shape.Points[index], shape.Points[(index + 1) % shape.Points.size()], color, thickness);
                    PushOutlined(outPrimitives, std::move(segment), palette, color, thickness);
                }

                if (firstFacing != shape.Points.size())
                {
                    const ScreenPoint& anchor = shape.Points[(firstFacing + shape.Points.size() / 8) % shape.Points.size()];
                    GizmoDrawPrimitive label;
                    label.Kind = GizmoPrimitiveKind::Text;
                    label.Points = { Add(anchor, -4.0, -7.0) };
                    label.Color = palette.Label;
                    label.Text = AxisLetter(shape.Handle);
                    outPrimitives.push_back(std::move(label));
                }
                break;
            }
            }
        }
    }
}
