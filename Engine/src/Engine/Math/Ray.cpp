#include "Engine/Math/Ray.h"

#include "Engine/Math/DVec3Ops.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine::Math
{
    namespace
    {
        bool SlabIntersect(double origin, double direction, double minimum, double maximum, double& tNear, double& tFar)
        {
            if (direction == 0.0)
                return origin >= minimum && origin <= maximum;

            double t1 = (minimum - origin) / direction;
            double t2 = (maximum - origin) / direction;
            if (t1 > t2)
                std::swap(t1, t2);

            tNear = std::max(tNear, t1);
            tFar = std::min(tFar, t2);
            return tNear <= tFar;
        }

        struct SegmentCandidate
        {
            double Distance = std::numeric_limits<double>::infinity();
            double RayT = 0.0;
            double SegmentT = 0.0;
        };

        SegmentCandidate Evaluate(const Ray& ray, const DVec3& a, const DVec3& edge, double rayT, double segmentT)
        {
            const DVec3 rayPoint = PointAt(ray, rayT);
            const DVec3 segmentPoint = a + edge * segmentT;
            return { Length(rayPoint - segmentPoint), rayT, segmentT };
        }

        SegmentCandidate ClosestForSegmentParameter(const Ray& ray, const DVec3& a, const DVec3& edge, double segmentT)
        {
            const DVec3 point = a + edge * segmentT;
            const double rayT = std::max(0.0, Dot(point - ray.Origin, ray.Direction) / LengthSquared(ray.Direction));
            return Evaluate(ray, a, edge, rayT, segmentT);
        }
    }

    bool IsValid(const Ray& ray)
    {
        return AllFinite(ray.Origin) && AllFinite(ray.Direction) && LengthSquared(ray.Direction) > 1e-300;
    }

    DVec3 PointAt(const Ray& ray, double t)
    {
        return ray.Origin + ray.Direction * t;
    }

    bool IntersectRayPlane(
        const Ray& ray,
        const DVec3& planePoint,
        const DVec3& planeNormal,
        double& outT,
        double parallelEpsilon)
    {
        if (!IsValid(ray) || !AllFinite(planePoint) || !AllFinite(planeNormal)
            || !std::isfinite(parallelEpsilon) || LengthSquared(planeNormal) < 1e-300)
        {
            return false;
        }

        const double denominator = Dot(planeNormal, ray.Direction);
        if (std::abs(denominator) <= parallelEpsilon * Length(planeNormal) * Length(ray.Direction))
            return false;

        const double t = Dot(planeNormal, planePoint - ray.Origin) / denominator;
        if (!std::isfinite(t))
            return false;

        outT = t;
        return true;
    }

    bool IntersectRayAabb(const Ray& ray, const Aabb& box, RayAabbHit& outHit)
    {
        if (!IsValid(ray) || !IsValid(box))
            return false;

        double tNear = -std::numeric_limits<double>::infinity();
        double tFar = std::numeric_limits<double>::infinity();
        if (!SlabIntersect(ray.Origin.X, ray.Direction.X, box.Min.X, box.Max.X, tNear, tFar)
            || !SlabIntersect(ray.Origin.Y, ray.Direction.Y, box.Min.Y, box.Max.Y, tNear, tFar)
            || !SlabIntersect(ray.Origin.Z, ray.Direction.Z, box.Min.Z, box.Max.Z, tNear, tFar)
            || tFar < 0.0)
        {
            return false;
        }

        outHit.TEnter = std::max(tNear, 0.0);
        outHit.TExit = tFar;
        return true;
    }

    bool ClosestPointsBetweenLines(
        const Ray& first,
        const Ray& second,
        LineClosestPoints& outClosest,
        double parallelEpsilon)
    {
        if (!IsValid(first) || !IsValid(second) || !std::isfinite(parallelEpsilon))
            return false;

        const double firstLength = Length(first.Direction);
        const double secondLength = Length(second.Direction);
        const double crossLength = Length(Cross(first.Direction, second.Direction));
        if (crossLength <= parallelEpsilon * firstLength * secondLength)
            return false;

        const DVec3 offset = first.Origin - second.Origin;
        const double a = Dot(first.Direction, first.Direction);
        const double b = Dot(first.Direction, second.Direction);
        const double c = Dot(second.Direction, second.Direction);
        const double d = Dot(first.Direction, offset);
        const double e = Dot(second.Direction, offset);
        const double denominator = a * c - b * b;
        const double firstT = (b * e - c * d) / denominator;
        const double secondT = (a * e - b * d) / denominator;
        if (!std::isfinite(firstT) || !std::isfinite(secondT))
            return false;

        outClosest.FirstT = firstT;
        outClosest.SecondT = secondT;
        outClosest.Distance = Length(PointAt(first, firstT) - PointAt(second, secondT));
        return true;
    }

    double DistanceRayToSegment(
        const Ray& ray,
        const DVec3& segmentA,
        const DVec3& segmentB,
        double& outRayT,
        double& outSegmentT)
    {
        const DVec3 edge = segmentB - segmentA;
        const double edgeLengthSquared = LengthSquared(edge);
        SegmentCandidate best;

        // Degenerate segment: the closest point of the half-line to one point.
        if (edgeLengthSquared < 1e-300)
        {
            best = ClosestForSegmentParameter(ray, segmentA, DVec3 {}, 0.0);
        }
        else
        {
            // The squared distance is convex in (t, s), so its minimum over
            // t >= 0, s in [0, 1] is interior or on one of three boundary edges.
            const double directionLengthSquared = LengthSquared(ray.Direction);
            const double b = Dot(ray.Direction, edge);
            const DVec3 offset = ray.Origin - segmentA;
            const double d = Dot(ray.Direction, offset);
            const double e = Dot(edge, offset);
            const double denominator = directionLengthSquared * edgeLengthSquared - b * b;
            const auto consider = [&best](const SegmentCandidate& candidate)
            {
                if (candidate.Distance < best.Distance)
                    best = candidate;
            };

            if (denominator > 1e-12 * directionLengthSquared * edgeLengthSquared)
            {
                const double t = (b * e - edgeLengthSquared * d) / denominator;
                const double s = (directionLengthSquared * e - b * d) / denominator;
                if (t >= 0.0 && s >= 0.0 && s <= 1.0)
                    consider(Evaluate(ray, segmentA, edge, t, s));
            }

            const double originSegmentT = std::clamp(e / edgeLengthSquared, 0.0, 1.0);
            consider(Evaluate(ray, segmentA, edge, 0.0, originSegmentT));
            consider(ClosestForSegmentParameter(ray, segmentA, edge, 0.0));
            consider(ClosestForSegmentParameter(ray, segmentA, edge, 1.0));
        }

        outRayT = best.RayT;
        outSegmentT = best.SegmentT;
        return best.Distance;
    }
}
