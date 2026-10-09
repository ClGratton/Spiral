#include "GizmoSolvers.h"

#include "Engine/Math/DVec3Ops.h"

#include <algorithm>
#include <cmath>

namespace Gizmo
{
    namespace
    {
        using Engine::Math::DVec3;
        using Engine::Math::Ray;

        constexpr double kPi = 3.14159265358979323846;
        constexpr double kMinimumArmLength = 1e-9;

        bool SolveForwardPlaneHit(const Ray& ray, const DVec3& point, const DVec3& normal, DVec3& outHit)
        {
            double rayLength = 0.0;
            DVec3 unitNormal;
            DVec3 unitDirection;
            if (!Engine::Math::TryNormalize(normal, unitNormal)
                || !Engine::Math::TryNormalize(ray.Direction, unitDirection)
                || std::abs(Engine::Math::Dot(unitNormal, unitDirection)) < kMinimumSolverSine
                || !Engine::Math::IntersectRayPlane(ray, point, unitNormal, rayLength)
                || rayLength <= 0.0)
            {
                return false;
            }

            outHit = Engine::Math::PointAt(ray, rayLength);
            return true;
        }
    }

    bool SolveAxisParameter(const Ray& pointerRay, const DVec3& axisOrigin, const DVec3& axisDirection, double& outT)
    {
        Engine::Math::LineClosestPoints closest;
        if (!Engine::Math::ClosestPointsBetweenLines(
                { axisOrigin, axisDirection }, pointerRay, closest, kMinimumSolverSine))
        {
            return false;
        }

        outT = closest.FirstT;
        return true;
    }

    bool SolvePlanePoint(const Ray& pointerRay, const DVec3& planePoint, const DVec3& planeNormal, DVec3& outPoint)
    {
        return SolveForwardPlaneHit(pointerRay, planePoint, planeNormal, outPoint);
    }

    bool SolveRotationArm(const Ray& pointerRay, const DVec3& pivot, const DVec3& unitAxis, DVec3& outArm)
    {
        DVec3 hit;
        DVec3 arm;
        if (!SolveForwardPlaneHit(pointerRay, pivot, unitAxis, hit)
            || Engine::Math::Length(hit - pivot) <= kMinimumArmLength
            || !Engine::Math::TryNormalize(hit - pivot, arm))
        {
            return false;
        }

        outArm = arm;
        return true;
    }

    double RotationAngleBetween(const DVec3& unitAxis, const DVec3& startArm, const DVec3& arm)
    {
        return std::atan2(
            Engine::Math::Dot(unitAxis, Engine::Math::Cross(startArm, arm)),
            Engine::Math::Dot(startArm, arm));
    }

    void AngleAccumulator::Reset()
    {
        m_Last = 0.0;
        m_Total = 0.0;
    }

    double AngleAccumulator::Update(double rawAngle)
    {
        double delta = rawAngle - m_Last;
        delta -= 2.0 * kPi * std::round(delta / (2.0 * kPi));
        m_Total += delta;
        m_Last = rawAngle;
        return m_Total;
    }

    bool SolveRingScreenTangent(
        const GizmoView& view,
        const DVec3& pivot,
        const DVec3& unitAxis,
        const DVec3& startArm,
        double radius,
        ScreenPoint& outTangent)
    {
        const DVec3 grab = pivot + startArm * radius;
        const DVec3 tangent3D = Engine::Math::Cross(unitAxis, startArm);
        ProjectedPoint from;
        ProjectedPoint to;
        if (!ProjectToScreen(view, grab, from) || !ProjectToScreen(view, grab + tangent3D * (radius * 0.01), to))
            return false;

        const double dx = to.Screen.X - from.Screen.X;
        const double dy = to.Screen.Y - from.Screen.Y;
        const double length = std::hypot(dx, dy);
        if (!(length > 1e-9))
            return false;

        outTangent = { dx / length, dy / length };
        return true;
    }

    double RotationAngleFromScreenDrag(
        const ScreenPoint& cursor,
        const ScreenPoint& startCursor,
        const ScreenPoint& tangent,
        double pixelsPerRadian)
    {
        return ((cursor.X - startCursor.X) * tangent.X + (cursor.Y - startCursor.Y) * tangent.Y) / pixelsPerRadian;
    }

    double SolveAxisScaleFactor(double tCurrent, double tGrab, double minimumReference)
    {
        double reference = tGrab;
        if (std::abs(reference) < minimumReference)
            reference = reference < 0.0 ? -minimumReference : minimumReference;

        return std::max(kMinimumScaleFactor, 1.0 + (tCurrent - tGrab) / reference);
    }

    double SolveUniformScaleFactor(const ScreenPoint& cursor, const ScreenPoint& startCursor, double referencePixels)
    {
        const double travel = (cursor.X - startCursor.X) - (cursor.Y - startCursor.Y);
        return std::max(kMinimumScaleFactor, 1.0 + travel / referencePixels);
    }
}
