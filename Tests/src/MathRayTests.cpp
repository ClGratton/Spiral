#include "MathRayTests.h"

#include "TestSupport/PropertyRunner.h"

#include "Engine/Math/Aabb.h"
#include "Engine/Math/DVec3Ops.h"
#include "Engine/Math/Ray.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace SpiralTests
{
    namespace
    {
        using namespace Engine::Math;

        // Failure hypotheses, oracles, and non-claims for the whole file:
        // - Hand-computed tables pin plane, slab, line and segment behaviour at
        //   the documented edges (parallel, behind, inside, boundary contact,
        //   zero thickness).
        // - Ray-AABB is checked against a 12-triangle Moller-Trumbore model, which
        //   shares no code with the slab test; ray-segment distance against a
        //   ternary search over the segment parameter with an analytic half-line
        //   distance; line closest points against the closed-form skew-line
        //   distance and the perpendicularity conditions.
        // - Properties run 500 iterations from a replayable seed
        //   (SPIRAL_MATH_RAY_SEED / SPIRAL_MATH_RAY_REPLAY).
        // - Tier: fast, in-process. Not claimed: float-precision behaviour (all
        //   inputs are doubles), SIMD/GPU variants, or camera conventions.

        struct Checker
        {
            const char* Suite;
            bool Ok = true;

            void Expect(bool condition, const std::string& message)
            {
                if (!condition)
                {
                    std::cerr << "Math ray test failed [" << Suite << "]: " << message << '\n';
                    Ok = false;
                }
            }

            void ExpectNear(double actual, double expected, double tolerance, const std::string& message)
            {
                Expect(std::abs(actual - expected) <= tolerance,
                    message + " expected " + std::to_string(expected) + " got " + std::to_string(actual));
            }
        };

        bool RunProperty(std::string_view name, const Spiral::Tests::Property& property)
        {
            return Spiral::Tests::RunNamedProperty("math-ray", name, "SPIRAL_MATH_RAY", 500, property);
        }

        DVec3 RandomPoint(Spiral::Tests::ChoiceStream& stream, double extent)
        {
            return {
                Spiral::Tests::RangeDouble(stream, -extent, extent),
                Spiral::Tests::RangeDouble(stream, -extent, extent),
                Spiral::Tests::RangeDouble(stream, -extent, extent)
            };
        }

        // Moller-Trumbore, inclusive edges, forward and backward hits reported.
        bool TriangleHit(const Ray& ray, const DVec3& a, const DVec3& b, const DVec3& c, double& outT)
        {
            const DVec3 edge1 = b - a;
            const DVec3 edge2 = c - a;
            const DVec3 p = Cross(ray.Direction, edge2);
            const double determinant = Dot(edge1, p);
            if (std::abs(determinant) < 1e-300)
                return false;

            const double inverse = 1.0 / determinant;
            const DVec3 toOrigin = ray.Origin - a;
            const double u = Dot(toOrigin, p) * inverse;
            const DVec3 q = Cross(toOrigin, edge1);
            const double v = Dot(ray.Direction, q) * inverse;
            constexpr double kEdge = 1e-12;
            if (u < -kEdge || v < -kEdge || u + v > 1.0 + kEdge)
                return false;

            outT = Dot(edge2, q) * inverse;
            return true;
        }

        struct OracleHit
        {
            bool Hit = false;
            double Enter = 0.0;
            double Exit = 0.0;
        };

        OracleHit TriangleOracle(const Ray& ray, const Aabb& box)
        {
            const DVec3 lo = box.Min;
            const DVec3 hi = box.Max;
            const DVec3 c[8] = {
                { lo.X, lo.Y, lo.Z }, { hi.X, lo.Y, lo.Z }, { hi.X, hi.Y, lo.Z }, { lo.X, hi.Y, lo.Z },
                { lo.X, lo.Y, hi.Z }, { hi.X, lo.Y, hi.Z }, { hi.X, hi.Y, hi.Z }, { lo.X, hi.Y, hi.Z }
            };
            constexpr int kQuads[6][4] = {
                { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 3, 2, 6, 7 }, { 0, 3, 7, 4 }, { 1, 2, 6, 5 }
            };

            double nearest = std::numeric_limits<double>::infinity();
            double farthest = -std::numeric_limits<double>::infinity();
            for (const auto& quad : kQuads)
            {
                const int order[2][3] = { { quad[0], quad[1], quad[2] }, { quad[0], quad[2], quad[3] } };
                for (const auto& triangle : order)
                {
                    double t = 0.0;
                    if (TriangleHit(ray, c[triangle[0]], c[triangle[1]], c[triangle[2]], t) && t >= 0.0)
                    {
                        nearest = std::min(nearest, t);
                        farthest = std::max(farthest, t);
                    }
                }
            }

            OracleHit result;
            if (Contains(box, ray.Origin))
            {
                result.Hit = true;
                result.Enter = 0.0;
                result.Exit = std::isfinite(farthest) ? farthest : 0.0;
                return result;
            }

            if (std::isfinite(nearest))
            {
                result.Hit = true;
                result.Enter = nearest;
                result.Exit = farthest;
            }

            return result;
        }

        double PointToHalfLineDistance(const Ray& ray, const DVec3& point)
        {
            const double t = std::max(0.0, Dot(point - ray.Origin, ray.Direction) / LengthSquared(ray.Direction));
            return Length(point - PointAt(ray, t));
        }

        // Convex in s, so a ternary search finds the exact minimum.
        double TernarySegmentDistance(const Ray& ray, const DVec3& a, const DVec3& b)
        {
            double low = 0.0;
            double high = 1.0;
            const auto f = [&](double s) { return PointToHalfLineDistance(ray, a + (b - a) * s); };
            for (int iteration = 0; iteration < 200; ++iteration)
            {
                const double m1 = low + (high - low) / 3.0;
                const double m2 = high - (high - low) / 3.0;
                if (f(m1) < f(m2))
                    high = m2;
                else
                    low = m1;
            }

            return f((low + high) * 0.5);
        }
    }

    bool TestMathRayPlaneAndLineHandCases()
    {
        Checker check { "plane-line" };

        double t = 0.0;
        check.Expect(IntersectRayPlane({ { 0, 0, 0 }, { 0, 0, 1 } }, { 0, 0, 5 }, { 0, 0, 2 }, t), "z plane hit");
        check.ExpectNear(t, 5.0, 1e-12, "unit direction t");
        check.Expect(IntersectRayPlane({ { 0, 0, 0 }, { 0, 0, 2 } }, { 0, 0, 5 }, { 0, 0, 1 }, t), "scaled direction hit");
        check.ExpectNear(t, 2.5, 1e-12, "t is in units of the direction");
        check.Expect(IntersectRayPlane({ { 0, 0, 0 }, { 0, 0, 1 } }, { 0, 0, -3 }, { 0, 0, 1 }, t), "plane behind reports negative t");
        check.ExpectNear(t, -3.0, 1e-12, "behind t");

        const Ray oblique { { 1, 2, 3 }, { 1, 1, 1 } };
        check.Expect(IntersectRayPlane(oblique, { 0, 0, 0 }, { 1, 0, 0 }, t), "oblique hit");
        check.ExpectNear(t, -1.0, 1e-12, "oblique t");
        const DVec3 hit = PointAt(oblique, t);
        check.Expect(hit.X == 0.0 && hit.Y == 1.0 && hit.Z == 2.0, "oblique point");

        check.Expect(!IntersectRayPlane({ { 0, 0, 0 }, { 1, 0, 0 } }, { 0, 0, 5 }, { 0, 0, 1 }, t), "parallel rejected");
        check.Expect(!IntersectRayPlane({ { 0, 0, 0 }, { 1, 0, 1e-10 } }, { 0, 0, 5 }, { 0, 0, 1 }, t), "1e-10 sine rejected at default epsilon");
        check.Expect(IntersectRayPlane({ { 0, 0, 0 }, { 1, 0, 1e-8 } }, { 0, 0, 5 }, { 0, 0, 1 }, t), "1e-8 sine accepted at default epsilon");
        check.Expect(!IntersectRayPlane({ { 0, 0, 0 }, { 0, 0, 0 } }, { 0, 0, 5 }, { 0, 0, 1 }, t), "zero direction rejected");
        check.Expect(!IntersectRayPlane({ { 0, 0, 0 }, { 0, 0, 1 } }, { 0, 0, 5 }, { 0, 0, 0 }, t), "zero normal rejected");
        check.Expect(!IntersectRayPlane({ { std::nan(""), 0, 0 }, { 0, 0, 1 } }, { 0, 0, 5 }, { 0, 0, 1 }, t), "NaN origin rejected");

        LineClosestPoints closest;
        check.Expect(ClosestPointsBetweenLines({ { 0, 0, 0 }, { 1, 0, 0 } }, { { 0, 1, 1 }, { 0, 1, 0 } }, closest), "skew lines");
        check.ExpectNear(closest.FirstT, 0.0, 1e-12, "first parameter");
        check.ExpectNear(closest.SecondT, -1.0, 1e-12, "second parameter");
        check.ExpectNear(closest.Distance, 1.0, 1e-12, "skew distance");
        check.Expect(ClosestPointsBetweenLines({ { 0, 0, 0 }, { 2, 0, 0 } }, { { 3, -4, 0 }, { 0, 3, 0 } }, closest), "intersecting lines");
        check.ExpectNear(closest.FirstT, 1.5, 1e-12, "scaled first parameter");
        check.ExpectNear(closest.SecondT, 4.0 / 3.0, 1e-12, "scaled second parameter");
        check.ExpectNear(closest.Distance, 0.0, 1e-12, "intersection distance");
        check.Expect(!ClosestPointsBetweenLines({ { 0, 0, 0 }, { 1, 0, 0 } }, { { 0, 1, 0 }, { -2, 0, 0 } }, closest), "parallel lines rejected");
        check.Expect(!ClosestPointsBetweenLines({ { 0, 0, 0 }, { 1, 0, 0 } }, { { 0, 1, 0 }, { 1, 1e-10, 0 } }, closest), "nearly parallel rejected");
        check.Expect(ClosestPointsBetweenLines({ { 0, 0, 0 }, { 1, 0, 0 } }, { { 0, 1, 0 }, { 1, 0.1, 0 } }, closest, 0.01), "custom epsilon accepts a 5.7 degree angle");
        check.Expect(!ClosestPointsBetweenLines({ { 0, 0, 0 }, { 1, 0, 0 } }, { { 0, 1, 0 }, { 1, 0.1, 0 } }, closest, 0.2), "custom epsilon rejects a 5.7 degree angle");
        return check.Ok;
    }

    bool TestMathRayAabbHandCasesAndEdges()
    {
        Checker check { "aabb-hand" };
        const Aabb box { { -1, -1, -1 }, { 1, 1, 1 } };
        RayAabbHit hit;

        check.Expect(IsValid(box) && Contains(box, { 1, 1, 1 }) && !Contains(box, { 1.0001, 0, 0 }), "box helpers");
        check.Expect(Center(box).X == 0.0 && HalfExtents(box).Z == 1.0, "center and half extents");
        check.Expect(!IsValid(Aabb { { 1, 0, 0 }, { 0, 0, 0 } }), "inverted box invalid");
        check.Expect(!IsValid(Aabb { { std::nan(""), 0, 0 }, { 1, 1, 1 } }), "NaN box invalid");

        check.Expect(IntersectRayAabb({ { -5, 0, 0 }, { 1, 0, 0 } }, box, hit), "axis ray hits");
        check.ExpectNear(hit.TEnter, 4.0, 1e-12, "axis enter");
        check.ExpectNear(hit.TExit, 6.0, 1e-12, "axis exit");
        check.Expect(IntersectRayAabb({ { -5, 0, 0 }, { 2, 0, 0 } }, box, hit), "scaled direction hits");
        check.ExpectNear(hit.TEnter, 2.0, 1e-12, "scaled enter");
        check.ExpectNear(hit.TExit, 3.0, 1e-12, "scaled exit");
        check.Expect(IntersectRayAabb({ { 0, 0, 0 }, { 0, 0, 1 } }, box, hit), "origin inside hits");
        check.ExpectNear(hit.TEnter, 0.0, 0.0, "inside enter is zero");
        check.ExpectNear(hit.TExit, 1.0, 1e-12, "inside exit");
        check.Expect(!IntersectRayAabb({ { 5, 0, 0 }, { 1, 0, 0 } }, box, hit), "box behind the origin misses");
        check.Expect(!IntersectRayAabb({ { 0, 2, 0 }, { 1, 0, 0 } }, box, hit), "parallel slab outside misses");
        check.Expect(IntersectRayAabb({ { -5, 1, 0 }, { 1, 0, 0 } }, box, hit), "parallel slab on the boundary touches");
        check.ExpectNear(hit.TEnter, 4.0, 1e-12, "boundary touch enter");
        check.Expect(IntersectRayAabb({ { -2, -2, -2 }, { 1, 1, 1 } }, box, hit), "diagonal through two corners");
        check.ExpectNear(hit.TEnter, 1.0, 1e-12, "diagonal enter");
        check.ExpectNear(hit.TExit, 3.0, 1e-12, "diagonal exit");
        check.Expect(IntersectRayAabb({ { 1, 0, 0 }, { 1, 0, 0 } }, box, hit), "origin on the face leaving counts as contact");
        check.ExpectNear(hit.TEnter, 0.0, 0.0, "face contact enter");
        check.ExpectNear(hit.TExit, 0.0, 1e-12, "face contact exit");
        check.Expect(!IntersectRayAabb({ { 1.0001, 0, 0 }, { 1, 0, 0 } }, box, hit), "just past the face misses");
        check.Expect(IntersectRayAabb({ { 0.999, 5, 0 }, { 0, -1, 0 } }, box, hit), "near the face edge hits");
        check.ExpectNear(hit.TEnter, 4.0, 1e-12, "near-edge enter");
        check.Expect(!IntersectRayAabb({ { 1.001, 5, 0 }, { 0, -1, 0 } }, box, hit), "just outside the face edge misses");

        const Aabb flat { { 1, 1, 1 }, { 1, 1, 1 } };
        check.Expect(IntersectRayAabb({ { 1, 1, -5 }, { 0, 0, 1 } }, flat, hit), "zero-extent box hit by a centred ray");
        check.ExpectNear(hit.TEnter, 6.0, 1e-12, "point box enter");
        check.ExpectNear(hit.TExit, 6.0, 1e-12, "point box exit");
        check.Expect(!IntersectRayAabb({ { 1.0001, 1, -5 }, { 0, 0, 1 } }, flat, hit), "zero-extent box missed by a nearby ray");
        const Aabb sheet { { -1, -1, 0 }, { 1, 1, 0 } };
        check.Expect(IntersectRayAabb({ { 0.5, 0.5, 3 }, { 0, 0, -1 } }, sheet, hit), "zero-thickness sheet hit");
        check.ExpectNear(hit.TEnter, 3.0, 1e-12, "sheet enter");
        check.Expect(!IntersectRayAabb({ { 0.5, 0.5, 3 }, { 1, 0, 0 } }, sheet, hit), "ray parallel to the sheet misses");

        check.Expect(!IntersectRayAabb({ { 0, 0, 0 }, { 0, 0, 0 } }, box, hit), "zero direction rejected");
        check.Expect(!IntersectRayAabb({ { 0, 0, 0 }, { std::nan(""), 0, 1 } }, box, hit), "NaN direction rejected");
        check.Expect(!IntersectRayAabb({ { -5, 0, 0 }, { 1, 0, 0 } }, Aabb { { 1, 0, 0 }, { -1, 0, 0 } }, hit), "invalid box rejected");
        const double infinity = std::numeric_limits<double>::infinity();
        check.Expect(!IntersectRayAabb({ { -5, 0, 0 }, { infinity, 0, 0 } }, box, hit), "infinite direction rejected");

        RayAabbHit untouched { 7.0, 9.0 };
        check.Expect(!IntersectRayAabb({ { 5, 0, 0 }, { 1, 0, 0 } }, box, untouched)
                && untouched.TEnter == 7.0 && untouched.TExit == 9.0,
            "a miss leaves the output untouched");
        return check.Ok;
    }

    bool TestMathRayAabbMatchesTriangleOracle()
    {
        Checker check { "aabb-oracle" };
        const bool ok = RunProperty("TestMathRayAabbMatchesTriangleOracle",
            [](Spiral::Tests::ChoiceStream& stream, std::string& message)
            {
                using Spiral::Tests::RangeDouble;
                const DVec3 center = RandomPoint(stream, 50.0);
                const DVec3 half { RangeDouble(stream, 0.01, 20.0), RangeDouble(stream, 0.01, 20.0), RangeDouble(stream, 0.01, 20.0) };
                const Aabb box { center - half, center + half };

                const DVec3 target = center + DVec3 {
                    RangeDouble(stream, -1.2, 1.2) * half.X,
                    RangeDouble(stream, -1.2, 1.2) * half.Y,
                    RangeDouble(stream, -1.2, 1.2) * half.Z
                };
                DVec3 direction = RandomPoint(stream, 1.0);
                direction = direction + DVec3 { 0.05, 0.05, 0.05 };
                const double directionScale = RangeDouble(stream, 0.1, 10.0);
                const double distance = RangeDouble(stream, 0.0, 200.0);
                const bool inside = stream.Next() % 5 == 0;

                DVec3 unit;
                if (!TryNormalize(direction, unit))
                    return true;

                Ray ray { inside ? target : target - unit * distance, unit * directionScale };
                // Axis-parallel rays keep the parallel coordinate strictly inside or outside its slab.
                const double* minimum[3] = { &box.Min.X, &box.Min.Y, &box.Min.Z };
                const double* maximum[3] = { &box.Max.X, &box.Max.Y, &box.Max.Z };
                double* origin[3] = { &ray.Origin.X, &ray.Origin.Y, &ray.Origin.Z };
                double* component[3] = { &ray.Direction.X, &ray.Direction.Y, &ray.Direction.Z };
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (stream.Next() % 4 != 0)
                        continue;

                    *component[axis] = 0.0;
                    const double span = *maximum[axis] - *minimum[axis];
                    if (stream.NextBool())
                        *origin[axis] = *minimum[axis] + span * RangeDouble(stream, 0.05, 0.95);
                    else if (stream.NextBool())
                        *origin[axis] = *maximum[axis] + span * RangeDouble(stream, 0.05, 1.0);
                    else
                        *origin[axis] = *minimum[axis] - span * RangeDouble(stream, 0.05, 1.0);
                }

                if (LengthSquared(ray.Direction) < 1e-12)
                    return true;

                const OracleHit expected = TriangleOracle(ray, box);
                RayAabbHit actual;
                const bool hit = IntersectRayAabb(ray, box, actual);
                if (hit != expected.Hit)
                {
                    message = "hit " + std::to_string(hit) + " but the triangle model says " + std::to_string(expected.Hit);
                    return false;
                }

                if (hit)
                {
                    const double tolerance = 1e-8 * (1.0 + std::abs(expected.Exit));
                    if (std::abs(actual.TEnter - expected.Enter) > tolerance || std::abs(actual.TExit - expected.Exit) > tolerance)
                    {
                        message = "enter/exit " + std::to_string(actual.TEnter) + "/" + std::to_string(actual.TExit)
                            + " vs model " + std::to_string(expected.Enter) + "/" + std::to_string(expected.Exit);
                        return false;
                    }
                }

                return true;
            });
        check.Expect(ok, "generated ray-AABB property");
        return check.Ok;
    }

    bool TestMathRaySegmentDistanceMatchesOracle()
    {
        Checker check { "segment-oracle" };

        double rayT = 0.0;
        double segmentT = 0.0;
        const Ray zRay { { 0, 0, 0 }, { 0, 0, 1 } };
        check.ExpectNear(DistanceRayToSegment(zRay, { 1, 0, 5 }, { 1, 0, 10 }, rayT, segmentT), 1.0, 1e-12, "offset segment");
        check.ExpectNear(rayT, 5.0, 1e-12, "offset segment ray parameter");
        check.ExpectNear(segmentT, 0.0, 1e-12, "offset segment parameter");
        check.ExpectNear(DistanceRayToSegment(zRay, { 1, 0, -5 }, { 1, 0, -1 }, rayT, segmentT), std::sqrt(2.0), 1e-12, "segment behind the origin");
        check.ExpectNear(rayT, 0.0, 0.0, "behind clamps the ray to the origin");
        check.ExpectNear(segmentT, 1.0, 1e-12, "behind uses the near end");
        check.ExpectNear(DistanceRayToSegment(zRay, { -1, 0, 3 }, { 1, 0, 3 }, rayT, segmentT), 0.0, 1e-12, "crossing segment");
        check.ExpectNear(rayT, 3.0, 1e-12, "crossing ray parameter");
        check.ExpectNear(segmentT, 0.5, 1e-12, "crossing segment parameter");
        check.ExpectNear(DistanceRayToSegment(zRay, { 2, 0, 1 }, { 2, 0, 4 }, rayT, segmentT), 2.0, 1e-12, "parallel segment");
        check.ExpectNear(DistanceRayToSegment(zRay, { 3, 4, 2 }, { 3, 4, 2 }, rayT, segmentT), 5.0, 1e-12, "degenerate segment is a point");
        check.ExpectNear(rayT, 2.0, 1e-12, "degenerate point ray parameter");
        check.ExpectNear(DistanceRayToSegment({ { 0, 0, 0 }, { 0, 0, 4 } }, { 1, 0, 8 }, { 1, 0, 9 }, rayT, segmentT), 1.0, 1e-12, "non-unit direction");
        check.ExpectNear(rayT, 2.0, 1e-12, "ray parameter is in units of the direction");

        const bool ok = RunProperty("TestMathRaySegmentDistanceMatchesOracle",
            [](Spiral::Tests::ChoiceStream& stream, std::string& message)
            {
                using Spiral::Tests::RangeDouble;
                const DVec3 direction = RandomPoint(stream, 1.0) + DVec3 { 0.0, 0.0, 0.1 };
                if (LengthSquared(direction) < 1e-6)
                    return true;

                const Ray ray { RandomPoint(stream, 20.0), direction * RangeDouble(stream, 0.2, 5.0) };
                const DVec3 a = RandomPoint(stream, 20.0);
                const DVec3 b = stream.Next() % 8 == 0 ? a : RandomPoint(stream, 20.0);

                double t = 0.0;
                double s = 0.0;
                const double actual = DistanceRayToSegment(ray, a, b, t, s);
                const double expected = TernarySegmentDistance(ray, a, b);
                if (std::abs(actual - expected) > 1e-7 * (1.0 + expected))
                {
                    message = "distance " + std::to_string(actual) + " vs ternary oracle " + std::to_string(expected);
                    return false;
                }

                const double reconstructed = Length(PointAt(ray, t) - (a + (b - a) * s));
                if (t < 0.0 || s < 0.0 || s > 1.0 || std::abs(reconstructed - actual) > 1e-9 * (1.0 + actual))
                {
                    message = "reported parameters t=" + std::to_string(t) + " s=" + std::to_string(s)
                        + " do not reproduce the distance " + std::to_string(actual);
                    return false;
                }

                // Closest points between lines: perpendicular to both and equal to the closed form.
                const Ray second { RandomPoint(stream, 20.0), RandomPoint(stream, 1.0) + DVec3 { 0.2, 0.0, 0.0 } };
                LineClosestPoints closest;
                const double crossLength = Length(Cross(ray.Direction, second.Direction));
                if (crossLength <= 1e-6 * Length(ray.Direction) * Length(second.Direction) || LengthSquared(second.Direction) < 1e-6)
                    return true;

                if (!ClosestPointsBetweenLines(ray, second, closest))
                {
                    message = "non-parallel lines rejected";
                    return false;
                }

                const DVec3 gap = PointAt(ray, closest.FirstT) - PointAt(second, closest.SecondT);
                const double closedForm = std::abs(Dot(second.Origin - ray.Origin, Cross(ray.Direction, second.Direction))) / crossLength;
                const double scale = Length(ray.Direction) * Length(second.Direction);
                if (std::abs(Dot(gap, ray.Direction)) > 1e-8 * scale * (1.0 + closedForm)
                    || std::abs(Dot(gap, second.Direction)) > 1e-8 * scale * (1.0 + closedForm)
                    || std::abs(closest.Distance - closedForm) > 1e-8 * (1.0 + closedForm))
                {
                    message = "line closest points are not perpendicular or distance " + std::to_string(closest.Distance)
                        + " differs from the closed form " + std::to_string(closedForm);
                    return false;
                }

                return true;
            });
        check.Expect(ok, "generated ray-segment and line property");
        return check.Ok;
    }
}
