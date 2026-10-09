#pragma once

#include "GizmoProjection.h"

namespace Gizmo
{
    // Sine of the smallest angle between the pointer ray and the axis (or the
    // plane) the solvers accept. Below it the closest-point parameter is
    // dominated by noise, so the caller keeps its last value.
    inline constexpr double kMinimumSolverSine = 0.02;

    // Parameter along axisDirection (unit) of the point on the axis line
    // closest to the pointer ray. Applying (t - tGrab) * axisDirection to the
    // entity keeps the grabbed point at its closest approach to the cursor.
    bool SolveAxisParameter(
        const Engine::Math::Ray& pointerRay,
        const Engine::Math::DVec3& axisOrigin,
        const Engine::Math::DVec3& axisDirection,
        double& outT);

    // Intersection of the pointer ray with the plane through planePoint. Fails
    // for planes seen edge-on and for planes behind the eye. Applying
    // (hit - grabHit) moves the grabbed point exactly under the cursor.
    bool SolvePlanePoint(
        const Engine::Math::Ray& pointerRay,
        const Engine::Math::DVec3& planePoint,
        const Engine::Math::DVec3& planeNormal,
        Engine::Math::DVec3& outPoint);

    // Direction from the pivot to the pointer's intersection with the plane
    // through the pivot normal to the unit axis, as a unit vector in that
    // plane. Fails for an edge-on plane or a hit on the pivot.
    bool SolveRotationArm(
        const Engine::Math::Ray& pointerRay,
        const Engine::Math::DVec3& pivot,
        const Engine::Math::DVec3& unitAxis,
        Engine::Math::DVec3& outArm);

    // Signed angle (radians, right-hand about unitAxis) that rotates startArm
    // onto arm, in (-pi, pi].
    double RotationAngleBetween(
        const Engine::Math::DVec3& unitAxis,
        const Engine::Math::DVec3& startArm,
        const Engine::Math::DVec3& arm);

    // Turns per-frame wrapped angles into a continuous total, so a drag can
    // pass +-180 degrees and keep accumulating through several turns.
    class AngleAccumulator
    {
    public:
        void Reset();
        // rawAngle is RotationAngleBetween against the fixed start arm.
        double Update(double rawAngle);
        double Total() const { return m_Total; }

    private:
        double m_Last = 0.0;
        double m_Total = 0.0;
    };

    // Unit screen direction in which the angle of a ring grows at the grab
    // point, used when the ring plane is edge-on to the camera.
    bool SolveRingScreenTangent(
        const GizmoView& view,
        const Engine::Math::DVec3& pivot,
        const Engine::Math::DVec3& unitAxis,
        const Engine::Math::DVec3& startArm,
        double radius,
        ScreenPoint& outTangent);

    // Edge-on fallback: pointer travel along the screen tangent, with one
    // radian per pixelsPerRadian pixels.
    double RotationAngleFromScreenDrag(
        const ScreenPoint& cursor,
        const ScreenPoint& startCursor,
        const ScreenPoint& tangent,
        double pixelsPerRadian);

    // Axis scale factor 1 + (tCurrent - tGrab) / reference, which equals
    // tCurrent / tGrab when the grab is at least minimumReference from the
    // pivot. A closer grab uses minimumReference (keeping the sign) so a press
    // near the centre neither jumps nor explodes, and zero travel is exactly 1.
    // The result is clamped to [kMinimumScaleFactor, inf).
    inline constexpr double kMinimumScaleFactor = 0.001;
    double SolveAxisScaleFactor(double tCurrent, double tGrab, double minimumReference);

    // Uniform scale from the Center handle: right and up growth, one factor
    // per referencePixels of pointer travel. Zero travel returns exactly 1.
    double SolveUniformScaleFactor(const ScreenPoint& cursor, const ScreenPoint& startCursor, double referencePixels);
}
