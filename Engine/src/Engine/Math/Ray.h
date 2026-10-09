#pragma once

#include "Engine/Math/Aabb.h"
#include "Engine/Math/Math.h"

namespace Engine::Math
{
    inline constexpr double kDefaultParallelEpsilon = 1e-9;

    // Parameters are in units of |Direction|; Direction need not be unit length.
    struct Ray
    {
        DVec3 Origin;
        DVec3 Direction;
    };

    // Finite origin and a finite, non-zero direction.
    bool IsValid(const Ray& ray);
    DVec3 PointAt(const Ray& ray, double t);

    // Intersects the ray's supporting line with a plane. t is unclamped, so a
    // plane behind the origin yields t < 0 and the caller decides whether that
    // is a hit. Fails when the line is parallel to the plane
    // (|n.d| <= epsilon * |n||d|) or any input is invalid.
    bool IntersectRayPlane(
        const Ray& ray,
        const DVec3& planePoint,
        const DVec3& planeNormal,
        double& outT,
        double parallelEpsilon = kDefaultParallelEpsilon);

    // Slab test against the forward half-line. TEnter is 0 when the origin is
    // inside or on the box; TExit >= TEnter. Boundary contact counts as a hit.
    struct RayAabbHit
    {
        double TEnter = 0.0;
        double TExit = 0.0;
    };

    bool IntersectRayAabb(const Ray& ray, const Aabb& box, RayAabbHit& outHit);

    // Closest points between two infinite lines. Both parameters are unclamped.
    // Fails when the directions are parallel (|cross| <= epsilon * |d1||d2|).
    struct LineClosestPoints
    {
        double FirstT = 0.0;
        double SecondT = 0.0;
        double Distance = 0.0;
    };

    bool ClosestPointsBetweenLines(
        const Ray& first,
        const Ray& second,
        LineClosestPoints& outClosest,
        double parallelEpsilon = kDefaultParallelEpsilon);

    // Distance between the forward half-line (t >= 0) and the segment [a, b].
    // outRayT and outSegmentT (in [0, 1]) locate the closest pair; a == b is a point.
    double DistanceRayToSegment(
        const Ray& ray,
        const DVec3& segmentA,
        const DVec3& segmentB,
        double& outRayT,
        double& outSegmentT);
}
