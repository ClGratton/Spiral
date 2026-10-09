#include "GizmoInteractionTests.h"

#include "TestSupport/PropertyRunner.h"

#include "EulerRotation.h"
#include "GizmoApply.h"
#include "GizmoHandles.h"
#include "GizmoProjection.h"
#include "GizmoSolvers.h"
#include "SnapMath.h"
#include "TransformGizmo.h"

#include "Engine/Math/DVec3Ops.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/Ray.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/Scene/Camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace SpiralTests
{
    namespace
    {
        using namespace Gizmo;
        using Engine::i64;
        using Engine::Math::Cross;
        using Engine::Math::DVec3;
        using Engine::Math::Dot;
        using Engine::Math::Length;
        using Engine::Math::Ray;
        using Engine::Math::SectorLocalPosition;
        using Engine::Math::Vec3;
        using Engine::Math::WorldGridPolicy;
        using Spiral::Tests::ChoiceStream;
        using Spiral::Tests::RangeDouble;

        // Failure hypotheses, oracles, and non-claims for the whole file:
        // - Projection, picking rays and constant size are checked against a
        //   pinhole camera written from the intended eye and the rows of the
        //   engine's own float view matrix (no matrix inverse, no shared
        //   code with GizmoProjection), built by Engine::BuildCameraView, the
        //   same path the viewport renders with.
        // - Solvers are checked by inverse construction (choose the true
        //   displacement, build the pointer ray that realises it, require the
        //   solver to recover it), by a golden-section minimiser, and by the
        //   Rodrigues rotation formula.
        // - Hit testing is checked against hand-computed pixels in a camera
        //   whose geometry is exact, and by a differential naive reference
        //   that samples the screen shapes densely.
        // - The state machine is checked by scripted gestures with hand
        //   expectations and by a generated pointer-trace property over a
        //   call-log grammar plus an independent reference of the phase rules.
        // - Properties run 500 iterations from a replayable seed
        //   (SPIRAL_GIZMO_INTERACTION_SEED / SPIRAL_GIZMO_INTERACTION_REPLAY).
        // - Tier: fast, in-process. Not claimed: ImGui drawing, real input
        //   delivery, a renderer, or any hardware path.

        constexpr double kPi = 3.14159265358979323846;

        struct Checker
        {
            const char* Suite;
            bool Ok = true;

            void Expect(bool condition, const std::string& message)
            {
                if (!condition)
                {
                    std::cerr << "Gizmo interaction test failed [" << Suite << "]: " << message << '\n';
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
            return Spiral::Tests::RunNamedProperty("gizmo-interaction", name, "SPIRAL_GIZMO_INTERACTION", 500, property);
        }

        double Float(double value)
        {
            return static_cast<double>(static_cast<float>(value));
        }

        double WrapAngle(double radians)
        {
            return radians - 2.0 * kPi * std::round(radians / (2.0 * kPi));
        }

        bool SameBits(const Vec3& a, const Vec3& b)
        {
            return std::memcmp(&a, &b, sizeof(Vec3)) == 0;
        }

        bool SameTransformBits(const GizmoTransform& a, const GizmoTransform& b)
        {
            return a.Position.Sector == b.Position.Sector
                && std::memcmp(&a.Position.Local, &b.Position.Local, sizeof(a.Position.Local)) == 0
                && SameBits(a.RotationDegrees, b.RotationDegrees) && SameBits(a.Scale, b.Scale);
        }

        // ---- random construction ------------------------------------------

        DVec3 RandomVector(ChoiceStream& stream, double extent)
        {
            return { RangeDouble(stream, -extent, extent), RangeDouble(stream, -extent, extent), RangeDouble(stream, -extent, extent) };
        }

        DVec3 RandomUnit(ChoiceStream& stream)
        {
            DVec3 unit;
            return Engine::Math::TryNormalize(RandomVector(stream, 1.0), unit) ? unit : DVec3 { 0.0, 1.0, 0.0 };
        }

        DVec3 RandomPerpendicular(ChoiceStream& stream, const DVec3& unitAxis)
        {
            const DVec3 candidate = RandomVector(stream, 1.0);
            DVec3 perpendicular;
            if (Engine::Math::TryNormalize(candidate - unitAxis * Dot(candidate, unitAxis), perpendicular)
                && std::abs(Dot(perpendicular, unitAxis)) < 1e-9)
            {
                return perpendicular;
            }

            DVec3 fallback;
            Engine::Math::TryNormalize(Cross(unitAxis, std::abs(unitAxis.X) < 0.9 ? DVec3 { 1.0, 0.0, 0.0 } : DVec3 { 0.0, 1.0, 0.0 }), fallback);
            return fallback;
        }

        // Rodrigues rotation of a vector about a unit axis (right-hand rule).
        DVec3 Rodrigues(const DVec3& v, const DVec3& axis, double radians)
        {
            return v * std::cos(radians) + Cross(axis, v) * std::sin(radians)
                + axis * (Dot(axis, v) * (1.0 - std::cos(radians)));
        }

        // ---- pinhole camera oracle ----------------------------------------

        struct TestScene
        {
            Engine::CameraView Camera;
            GizmoView View;
            DVec3 Eye;
            // v = (world - eye) * R, so view axis k in world space is column k.
            double R[3][3] {};

            DVec3 ViewAxis(int k) const { return { R[0][k], R[1][k], R[2][k] }; }
            double P(int index) const { return Camera.Projection.Values[index]; }
        };

        bool BuildScene(
            const DVec3& eye,
            const Vec3& rotationDegrees,
            float fovDegrees,
            float aspect,
            float nearClip,
            float farClip,
            const ViewportRect& rect,
            TestScene& out)
        {
            Engine::CameraProjection projection { fovDegrees, nearClip, farClip };
            out.Camera = Engine::BuildCameraView(eye, rotationDegrees, projection, aspect, {});
            out.Eye = eye;
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                    out.R[row][column] = out.Camera.View.Values[row * 4 + column];
            }

            return out.Camera.Valid && TryBuildGizmoView(out.Camera.View, out.Camera.Projection, rect, out.View);
        }

        bool MakeRandomScene(ChoiceStream& stream, TestScene& scene, bool stretched = false)
        {
            const DVec3 eye { Float(RangeDouble(stream, -60, 60)), Float(RangeDouble(stream, -60, 60)), Float(RangeDouble(stream, -60, 60)) };
            const Vec3 rotation {
                static_cast<float>(RangeDouble(stream, -75, 75)),
                static_cast<float>(RangeDouble(stream, -180, 180)),
                static_cast<float>(RangeDouble(stream, -25, 25))
            };
            const float aspect = static_cast<float>(RangeDouble(stream, 0.6, 2.5));
            const double height = RangeDouble(stream, 240, 1400);
            const double width = height * aspect * (stretched ? RangeDouble(stream, 0.8, 1.25) : 1.0);
            const ViewportRect rect { RangeDouble(stream, 0, 400), RangeDouble(stream, 0, 400), width, height };
            return BuildScene(eye, rotation, static_cast<float>(RangeDouble(stream, 25, 100)), aspect,
                static_cast<float>(RangeDouble(stream, 0.05, 0.5)), static_cast<float>(RangeDouble(stream, 200, 20000)), rect, scene);
        }

        // View-space depth (z) of a world point.
        double OracleDepth(const TestScene& scene, const DVec3& world)
        {
            return Dot(world - scene.Eye, scene.ViewAxis(2));
        }

        bool OracleProject(const TestScene& scene, const DVec3& world, ScreenPoint& pixel, double& depth)
        {
            const DVec3 relative = world - scene.Eye;
            const double vx = Dot(relative, scene.ViewAxis(0));
            const double vy = Dot(relative, scene.ViewAxis(1));
            depth = Dot(relative, scene.ViewAxis(2));
            if (!(depth > 1e-6))
                return false;

            const ViewportRect& rect = scene.View.Viewport;
            const double ndcX = vx * scene.P(0) / depth;
            const double ndcY = vy * scene.P(5) / depth;
            pixel = { rect.X + (ndcX * 0.5 + 0.5) * rect.Width, rect.Y + (0.5 - ndcY * 0.5) * rect.Height };
            return true;
        }

        ScreenPoint OraclePixel(const TestScene& scene, const DVec3& world)
        {
            ScreenPoint pixel;
            double depth = 0.0;
            OracleProject(scene, world, pixel, depth);
            return pixel;
        }

        DVec3 OracleWorldFromPixel(const TestScene& scene, const ScreenPoint& pixel, double depth)
        {
            const ViewportRect& rect = scene.View.Viewport;
            const double ndcX = ((pixel.X - rect.X) / rect.Width - 0.5) * 2.0;
            const double ndcY = (0.5 - (pixel.Y - rect.Y) / rect.Height) * 2.0;
            return scene.Eye + scene.ViewAxis(0) * (ndcX * depth / scene.P(0))
                + scene.ViewAxis(1) * (ndcY * depth / scene.P(5)) + scene.ViewAxis(2) * depth;
        }

        // Pointer ray from the intended eye through a pixel, built without GizmoProjection.
        Ray OracleRay(const TestScene& scene, const ScreenPoint& pixel)
        {
            const DVec3 far = OracleWorldFromPixel(scene, pixel, 1.0);
            return { scene.Eye, far - scene.Eye };
        }

        double Distance(const ScreenPoint& a, const ScreenPoint& b)
        {
            return std::hypot(a.X - b.X, a.Y - b.Y);
        }

        double PointToLineDistance(const DVec3& point, const Ray& ray)
        {
            DVec3 unit;
            Engine::Math::TryNormalize(ray.Direction, unit);
            const DVec3 offset = point - ray.Origin;
            return Length(offset - unit * Dot(offset, unit));
        }

        // Golden-section minimiser of a unimodal function.
        template <typename F>
        double MinimizeGolden(F&& f, double low, double high)
        {
            const double ratio = (std::sqrt(5.0) - 1.0) / 2.0;
            double a = low;
            double b = high;
            double c = b - ratio * (b - a);
            double d = a + ratio * (b - a);
            double fc = f(c);
            double fd = f(d);
            for (int iteration = 0; iteration < 200; ++iteration)
            {
                if (fc < fd)
                {
                    b = d;
                    d = c;
                    fd = fc;
                    c = b - ratio * (b - a);
                    fc = f(c);
                }
                else
                {
                    a = c;
                    c = d;
                    fc = fd;
                    d = a + ratio * (b - a);
                    fd = f(d);
                }
            }

            return 0.5 * (a + b);
        }

        // Parameter on the axis line O + u t closest to the pointer line.
        double OracleAxisParameter(const DVec3& origin, const DVec3& unitAxis, const Ray& pointer, double span)
        {
            return MinimizeGolden(
                [&](double t)
                {
                    const double distance = PointToLineDistance(origin + unitAxis * t, pointer);
                    return distance * distance;
                },
                -span, span);
        }
    }

    bool TestGizmoProjectionRoundTripsAndConstantSize()
    {
        Checker check { "projection" };

        const bool ok = RunProperty("TestGizmoProjectionRoundTripsAndConstantSize",
            [](ChoiceStream& stream, std::string& message)
            {
                TestScene scene;
                if (!MakeRandomScene(stream, scene, true))
                {
                    message = "random scene did not build";
                    return false;
                }

                const ViewportRect& rect = scene.View.Viewport;
                const double nearClip = scene.View.NearDepth;
                const ScreenPoint pixel { rect.X + RangeDouble(stream, 0.0, 1.0) * rect.Width, rect.Y + RangeDouble(stream, 0.0, 1.0) * rect.Height };
                const double depth = RangeDouble(stream, nearClip * 2.0, 2000.0);
                const DVec3 world = OracleWorldFromPixel(scene, pixel, depth);

                ProjectedPoint projected;
                if (!ProjectToScreen(scene.View, world, projected))
                {
                    message = "a point in front of the camera did not project";
                    return false;
                }

                if (Distance(projected.Screen, pixel) > 0.05)
                {
                    message = "ProjectToScreen differs from the pinhole oracle by " + std::to_string(Distance(projected.Screen, pixel)) + " px";
                    return false;
                }

                if (std::abs(projected.Depth - depth) > 1e-5 * depth + 1e-4)
                {
                    message = "depth " + std::to_string(projected.Depth) + " expected " + std::to_string(depth);
                    return false;
                }

                const double expectedNdcZ = (depth * scene.P(10) + scene.P(14)) / depth;
                if (std::abs(projected.NdcZ - expectedNdcZ) > 1e-4 || projected.NdcZ < -1e-4)
                {
                    message = "NdcZ " + std::to_string(projected.NdcZ) + " expected " + std::to_string(expectedNdcZ);
                    return false;
                }

                // Unprojection: the ray starts at the eye and passes through the world point.
                Ray ray;
                if (!TryScreenToRay(scene.View, pixel, ray) || !Engine::Math::IsValid(ray))
                {
                    message = "TryScreenToRay failed inside the viewport";
                    return false;
                }

                if (Length(ray.Origin - scene.Eye) > 2e-4 || std::abs(Length(ray.Direction) - 1.0) > 1e-12)
                {
                    message = "ray origin is not the eye or the direction is not unit";
                    return false;
                }

                if (PointToLineDistance(world, ray) > 2e-4 + depth * 2e-6 || Dot(ray.Direction, scene.ViewAxis(2)) <= 0.0)
                {
                    message = "the ray misses the projected world point by " + std::to_string(PointToLineDistance(world, ray));
                    return false;
                }

                // Round trip at a different depth along the same ray.
                const double otherDepth = RangeDouble(stream, nearClip * 2.0, 500.0);
                const double along = otherDepth / Dot(ray.Direction, scene.ViewAxis(2));
                ProjectedPoint again;
                if (!ProjectToScreen(scene.View, Engine::Math::PointAt(ray, along), again) || Distance(again.Screen, pixel) > 0.05)
                {
                    message = "unproject then project is not the identity";
                    return false;
                }

                // Constant on-screen size: a handle of the returned length spans the target pixels.
                double length = 0.0;
                if (!TryComputeGizmoLength(scene.View, world, 96.0, length))
                {
                    message = "TryComputeGizmoLength failed in front of the camera";
                    return false;
                }

                const ScreenPoint originPixel = OraclePixel(scene, world);
                const double up = Distance(originPixel, OraclePixel(scene, world + scene.ViewAxis(1) * length));
                const double right = Distance(originPixel, OraclePixel(scene, world + scene.ViewAxis(0) * length));
                const double rightExpected = 96.0 * (rect.Width / rect.Height) / (static_cast<double>(scene.P(5)) / scene.P(0));
                if (std::abs(up - 96.0) > 0.5 || std::abs(right - rightExpected) > 0.5)
                {
                    message = "gizmo spans " + std::to_string(up) + " / " + std::to_string(right) + " px, expected 96 / " + std::to_string(rightExpected);
                    return false;
                }

                double doubled = 0.0;
                const DVec3 farther = scene.View.Eye + (world - scene.View.Eye) * 2.0;
                if (!TryComputeGizmoLength(scene.View, farther, 96.0, doubled) || std::abs(doubled - 2.0 * length) > 1e-9 * length)
                {
                    message = "length does not scale linearly with depth";
                    return false;
                }

                // Between the eye plane and the near plane the depth is clamped to the near plane.
                const double shallow = nearClip * 0.3;
                const DVec3 nearPoint = scene.View.Eye + scene.View.Forward * shallow;
                double clamped = 0.0;
                const double expectedClamped = 96.0 * nearClip * 2.0 / (static_cast<double>(scene.P(5)) * rect.Height);
                if (!TryComputeGizmoLength(scene.View, nearPoint, 96.0, clamped) || std::abs(clamped - expectedClamped) > 1e-9 * expectedClamped)
                {
                    message = "length inside the near plane is not clamped to the near depth";
                    return false;
                }

                // Behind or on the eye plane nothing projects and nothing has a length.
                const DVec3 behind = scene.View.Eye - scene.View.Forward * RangeDouble(stream, 0.0, 50.0);
                ProjectedPoint untouched;
                untouched.Depth = 123.0;
                double untouchedLength = 7.0;
                if (ProjectToScreen(scene.View, behind, untouched) || untouched.Depth != 123.0
                    || TryComputeGizmoLength(scene.View, behind, 96.0, untouchedLength) || untouchedLength != 7.0)
                {
                    message = "a point at or behind the eye plane projected or wrote an output";
                    return false;
                }

                return true;
            });
        check.Expect(ok, "generated projection property");

        // Hand case: a 60 degree camera at the origin looking down +Z, object at depth 10.
        {
            TestScene scene;
            check.Expect(BuildScene({ 0, 0, 0 }, { 0, 0, 0 }, 60.0f, 4.0f / 3.0f, 0.1f, 100.0f, { 0, 0, 800, 600 }, scene), "hand scene builds");
            ProjectedPoint centre;
            check.Expect(ProjectToScreen(scene.View, { 0, 0, 10 }, centre) && Distance(centre.Screen, { 400, 300 }) < 1e-6, "the optical axis projects to the viewport centre");
            ProjectedPoint corner;
            // Up by 10*tan(30deg) is the top edge; right by 10*tan(30deg)*4/3 is the right edge.
            const double top = 10.0 * std::tan(30.0 * kPi / 180.0);
            check.Expect(ProjectToScreen(scene.View, { top * 4.0 / 3.0, top, 10 }, corner)
                    && Distance(corner.Screen, { 800, 0 }) < 0.01,
                "the frustum corner projects to the top-right pixel, y down");
            double length = 0.0;
            check.Expect(TryComputeGizmoLength(scene.View, { 0, 0, 10 }, 96.0, length), "length at depth 10");
            check.ExpectNear(length, 96.0 * 10.0 * 2.0 / (1.0 / std::tan(30.0 * kPi / 180.0) * 600.0), 1e-4, "96 px at depth 10, 600 px tall, 60 degrees");
            Ray ray;
            check.Expect(TryScreenToRay(scene.View, { 400, 300 }, ray) && std::abs(ray.Direction.Z - 1.0) < 1e-9 && std::abs(ray.Direction.X) < 1e-9,
                "the centre pixel looks straight ahead");
            check.Expect(TryScreenToRay(scene.View, { -500, 2000 }, ray), "a pixel outside the viewport still has a ray (drags continue off the image)");
            check.Expect(!TryScreenToRay(scene.View, { std::nan(""), 0 }, ray) && !TryScreenToRay(scene.View, { 0, std::numeric_limits<double>::infinity() }, ray), "non-finite pixels have no ray");
            check.Expect(!ProjectToScreen(scene.View, { std::nan(""), 0, 10 }, centre), "a non-finite point does not project");
        }

        return check.Ok;
    }

    bool TestGizmoViewValidationAndAtomicity()
    {
        Checker check { "view" };
        TestScene scene;
        check.Expect(BuildScene({ 3, 4, 5 }, { 10, 20, 0 }, 60.0f, 1.5f, 0.1f, 500.0f, { 10, 20, 600, 400 }, scene), "scene builds");
        const Engine::Math::Mat4& view = scene.Camera.View;
        const Engine::Math::Mat4& projection = scene.Camera.Projection;
        const ViewportRect rect { 10, 20, 600, 400 };

        GizmoView sentinel = scene.View;
        sentinel.NearDepth = 777.0;
        GizmoView out = sentinel;
        const auto untouched = [&]() { return std::memcmp(&out, &sentinel, sizeof(out)) == 0; };

        Engine::Math::Mat4 bad = projection;
        bad.Values[0] = std::nanf("");
        check.Expect(!TryBuildGizmoView(view, bad, rect, out) && untouched(), "NaN projection rejected without writing");
        bad = view;
        bad.Values[13] = std::numeric_limits<float>::infinity();
        check.Expect(!TryBuildGizmoView(bad, projection, rect, out) && untouched(), "infinite view rejected without writing");
        bad = Engine::Math::Mat4 {};
        check.Expect(!TryBuildGizmoView(bad, projection, rect, out) && untouched(), "singular view rejected");
        check.Expect(!TryBuildGizmoView(view, Engine::Math::Mat4::Identity(), rect, out) && untouched(), "identity projection is not perspective");
        bad = projection;
        bad.Values[15] = 1.0f;
        check.Expect(!TryBuildGizmoView(view, bad, rect, out) && untouched(), "an orthographic-style projection is rejected");
        bad = projection;
        bad.Values[11] = -1.0f;
        check.Expect(!TryBuildGizmoView(view, bad, rect, out) && untouched(), "a right-handed projection is rejected");
        bad = projection;
        bad.Values[5] = -bad.Values[5];
        check.Expect(!TryBuildGizmoView(view, bad, rect, out) && untouched(), "a flipped Y scale is rejected");
        check.Expect(!TryBuildGizmoView(view, projection, { 0, 0, 0, 100 }, out) && untouched(), "zero width rejected");
        check.Expect(!TryBuildGizmoView(view, projection, { 0, 0, 100, -1 }, out) && untouched(), "negative height rejected");
        check.Expect(!TryBuildGizmoView(view, projection, { std::nan(""), 0, 100, 100 }, out) && untouched(), "NaN origin rejected");
        check.Expect(TryBuildGizmoView(view, projection, rect, out) && !untouched(), "the valid inputs still build");

        GizmoView same = out;
        check.Expect(SameView(out, same), "a view equals itself");
        same.Viewport.Width += 1.0;
        check.Expect(!SameView(out, same), "a resized viewport is a different view");
        same = out;
        same.View.Values[12] += 0.001f;
        check.Expect(!SameView(out, same), "a moved camera is a different view");
        same = out;
        same.Projection.Values[5] += 0.001f;
        check.Expect(!SameView(out, same), "a changed lens is a different view");

        // The eye and forward recovered from the matrices are the camera's.
        check.ExpectNear(Length(out.Eye - scene.Eye), 0.0, 1e-4, "eye recovered from the view matrix");
        check.ExpectNear(Length(out.Forward - scene.ViewAxis(2)), 0.0, 1e-6, "forward recovered from the view matrix");
        check.ExpectNear(out.NearDepth, 0.1, 1e-6, "near depth recovered from the projection");
        return check.Ok;
    }

    namespace
    {
        // ---- handle geometry reference ------------------------------------

        const GizmoStyle& DefaultStyle()
        {
            static const GizmoStyle style;
            return style;
        }

        const HandleShape* FindShape(const GizmoGeometry& geometry, GizmoHandle handle)
        {
            for (const HandleShape& shape : geometry.Shapes)
            {
                if (shape.Handle == handle)
                    return &shape;
            }

            return nullptr;
        }

        TestScene& HandScene()
        {
            static TestScene scene;
            static const bool built = BuildScene({ 0, 0, 0 }, { 0, 0, 0 }, 60.0f, 4.0f / 3.0f, 0.1f, 100.0f, { 0, 0, 800, 600 }, scene);
            if (!built)
                std::cerr << "hand scene failed to build\n";
            return scene;
        }

        const DVec3 kWorldBasis[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

        // Minimum distance from a cursor to a polyline by dense sampling (0.1 px).
        double SampledDistance(const std::vector<ScreenPoint>& points, bool closed, const std::vector<u8>* facing, const ScreenPoint& cursor)
        {
            double best = std::numeric_limits<double>::infinity();
            const size_t count = closed ? points.size() : points.size() - 1;
            for (size_t index = 0; index < count; ++index)
            {
                if (facing && !(*facing)[index])
                    continue;

                const ScreenPoint& a = points[index];
                const ScreenPoint& b = points[(index + 1) % points.size()];
                const int samples = static_cast<int>(std::ceil(Distance(a, b) / 0.1)) + 1;
                for (int step = 0; step <= samples; ++step)
                {
                    const double t = static_cast<double>(step) / samples;
                    best = std::min(best, Distance(cursor, { a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t }));
                }
            }

            return best;
        }

        bool InsideTriangle(const ScreenPoint& p, const ScreenPoint& a, const ScreenPoint& b, const ScreenPoint& c)
        {
            const auto sign = [](const ScreenPoint& p1, const ScreenPoint& p2, const ScreenPoint& p3)
            {
                return (p1.X - p3.X) * (p2.Y - p3.Y) - (p2.X - p3.X) * (p1.Y - p3.Y);
            };
            const double d1 = sign(p, a, b);
            const double d2 = sign(p, b, c);
            const double d3 = sign(p, c, a);
            const bool negative = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
            const bool positive = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
            return !(negative && positive);
        }

        struct ReferenceCandidate
        {
            GizmoHandle Handle = GizmoHandle::None;
            double Distance = 0.0;
            double Depth = 0.0;
            bool ExactZero = false;
        };

        int ReferencePriority(GizmoHandle handle)
        {
            if (handle == GizmoHandle::Center)
                return 0;
            return handle == GizmoHandle::PlaneYZ || handle == GizmoHandle::PlaneXZ || handle == GizmoHandle::PlaneXY ? 1 : 2;
        }

        enum class ReferenceOutcome
        {
            Decided,
            Ambiguous
        };

        // Naive reference hit test. Ambiguous when a decision depends on a
        // sub-0.1 px difference the sampling cannot resolve.
        ReferenceOutcome ReferenceHit(const GizmoGeometry& geometry, const ScreenPoint& cursor, const GizmoStyle& style, GizmoHandle& outHandle)
        {
            constexpr double kMargin = 0.12;
            std::vector<ReferenceCandidate> accepted;
            for (const HandleShape& shape : geometry.Shapes)
            {
                if (!shape.Enabled)
                    continue;

                ReferenceCandidate candidate;
                candidate.Handle = shape.Handle;
                candidate.Depth = shape.Depth;
                bool hit = false;
                switch (shape.Kind)
                {
                case HandleShapeKind::Segment:
                {
                    const double line = SampledDistance(shape.Points, false, nullptr, cursor);
                    const double cap = Distance(cursor, shape.Points[1]);
                    if (std::abs(line - style.LineHitPixels) < kMargin || std::abs(cap - style.CapHitPixels) < kMargin)
                        return ReferenceOutcome::Ambiguous;

                    hit = line <= style.LineHitPixels || cap <= style.CapHitPixels;
                    candidate.Distance = std::min(line, cap);
                    break;
                }
                case HandleShapeKind::Quad:
                {
                    const double edge = SampledDistance(shape.Points, true, nullptr, cursor);
                    if (edge < kMargin)
                        return ReferenceOutcome::Ambiguous;

                    hit = InsideTriangle(cursor, shape.Points[0], shape.Points[1], shape.Points[2])
                        || InsideTriangle(cursor, shape.Points[0], shape.Points[2], shape.Points[3]);
                    candidate.ExactZero = true;
                    break;
                }
                case HandleShapeKind::Disc:
                {
                    const double distance = Distance(cursor, shape.Points[0]);
                    if (std::abs(distance - shape.Radius) < kMargin)
                        return ReferenceOutcome::Ambiguous;

                    hit = distance <= shape.Radius;
                    candidate.ExactZero = hit;
                    candidate.Distance = std::max(0.0, distance - shape.Radius);
                    break;
                }
                case HandleShapeKind::Ring:
                {
                    const double distance = SampledDistance(shape.Points, true, &shape.Facing, cursor);
                    if (std::abs(distance - style.LineHitPixels) < kMargin)
                        return ReferenceOutcome::Ambiguous;

                    hit = distance <= style.LineHitPixels;
                    candidate.Distance = distance;
                    break;
                }
                }

                if (hit)
                    accepted.push_back(candidate);
            }

            if (accepted.empty())
            {
                outHandle = GizmoHandle::None;
                return ReferenceOutcome::Decided;
            }

            const auto better = [](const ReferenceCandidate& a, const ReferenceCandidate& b)
            {
                if (a.Distance != b.Distance)
                    return a.Distance < b.Distance;
                if (a.Depth != b.Depth)
                    return a.Depth < b.Depth;
                if (ReferencePriority(a.Handle) != ReferencePriority(b.Handle))
                    return ReferencePriority(a.Handle) < ReferencePriority(b.Handle);
                return static_cast<u8>(a.Handle) < static_cast<u8>(b.Handle);
            };
            std::sort(accepted.begin(), accepted.end(), better);
            for (size_t index = 1; index < accepted.size(); ++index)
            {
                const bool bothExact = accepted[0].ExactZero && accepted[index].ExactZero;
                if (!bothExact && std::abs(accepted[index].Distance - accepted[0].Distance) < kMargin)
                    return ReferenceOutcome::Ambiguous;
            }

            outHandle = accepted.front().Handle;
            return ReferenceOutcome::Decided;
        }

        ScreenPoint PointOnShape(const HandleShape& shape, ChoiceStream& stream)
        {
            const double t = RangeDouble(stream, 0.0, 1.0);
            switch (shape.Kind)
            {
            case HandleShapeKind::Segment:
                return { shape.Points[0].X + (shape.Points[1].X - shape.Points[0].X) * t, shape.Points[0].Y + (shape.Points[1].Y - shape.Points[0].Y) * t };
            case HandleShapeKind::Quad:
            {
                const double u = RangeDouble(stream, 0.0, 1.0);
                const ScreenPoint top { shape.Points[0].X + (shape.Points[1].X - shape.Points[0].X) * t, shape.Points[0].Y + (shape.Points[1].Y - shape.Points[0].Y) * t };
                const ScreenPoint bottom { shape.Points[3].X + (shape.Points[2].X - shape.Points[3].X) * t, shape.Points[3].Y + (shape.Points[2].Y - shape.Points[3].Y) * t };
                return { top.X + (bottom.X - top.X) * u, top.Y + (bottom.Y - top.Y) * u };
            }
            case HandleShapeKind::Disc:
                return shape.Points[0];
            case HandleShapeKind::Ring:
            {
                const size_t index = static_cast<size_t>(t * static_cast<double>(shape.Points.size() - 1));
                const ScreenPoint& a = shape.Points[index];
                const ScreenPoint& b = shape.Points[(index + 1) % shape.Points.size()];
                const double u = RangeDouble(stream, 0.0, 1.0);
                return { a.X + (b.X - a.X) * u, a.Y + (b.Y - a.Y) * u };
            }
            }

            return {};
        }

        struct HitCounters
        {
            size_t Compared = 0;
            size_t Hits = 0;
            size_t Ambiguous = 0;
        };

        // Points closer to the eye than this are dominated by float noise in the view
        // matrix, so the pinhole oracle and the matrix path legitimately disagree there.
        constexpr double kOracleMinimumDepth = 1.0;

        // Compares one built geometry against the independent expectations.
        bool CheckGeometryAgainstOracle(
            const TestScene& scene,
            const GizmoGeometry& geometry,
            TransformTool tool,
            const DVec3& origin,
            const DVec3 basis[3],
            bool orientTowardEye,
            const GizmoStyle& style,
            std::string& message)
        {
            const double depth = OracleDepth(scene, origin);
            const double expectedLength = style.TargetPixels * std::max(depth, scene.View.NearDepth) * 2.0
                / (static_cast<double>(scene.P(5)) * scene.View.Viewport.Height);
            if (!geometry.Valid || std::abs(geometry.Length - expectedLength) > 1e-4 * expectedLength)
            {
                message = "gizmo length " + std::to_string(geometry.Length) + " expected " + std::to_string(expectedLength);
                return false;
            }

            if (Distance(geometry.OriginScreen, OraclePixel(scene, origin)) > 0.05 || std::abs(geometry.OriginDepth - depth) > 1e-4 * depth)
            {
                message = "origin projection differs from the pinhole oracle";
                return false;
            }

            const DVec3 toEye = scene.View.Eye - origin;
            DVec3 axes[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                const double facing = Dot(basis[axis], toEye);
                const bool flips = orientTowardEye && (tool == TransformTool::Translate || tool == TransformTool::Scale) && facing < 0.0;
                axes[axis] = flips ? -basis[axis] : basis[axis];
                if (std::abs(facing) > 1e-6 && Length(geometry.Axes[axis] - axes[axis]) > 1e-12)
                {
                    message = "axis " + std::to_string(axis) + " is not oriented toward the eye as specified";
                    return false;
                }
            }

            const double length = geometry.Length;
            for (const HandleShape& shape : geometry.Shapes)
            {
                const int axisIndex = AxisIndex(shape.Handle);
                if (shape.Kind == HandleShapeKind::Segment)
                {
                    const DVec3 tip3 = origin + geometry.Axes[axisIndex] * length;
                    ScreenPoint tip;
                    double tipDepth = 0.0;
                    if (!OracleProject(scene, tip3, tip, tipDepth) || tipDepth < kOracleMinimumDepth)
                        continue;

                    const double pixels = Distance(tip, OraclePixel(scene, origin));
                    if (Distance(shape.Points[1], tip) > 0.05 || Distance(shape.Points[0], geometry.OriginScreen) > 1e-9)
                    {
                        message = "axis segment endpoints differ from the oracle by " + std::to_string(Distance(shape.Points[1], tip)) + " px at depth " + std::to_string(tipDepth);
                        return false;
                    }

                    if (std::abs(pixels - style.MinimumAxisPixels) > 0.1 && shape.Enabled != (pixels >= style.MinimumAxisPixels))
                    {
                        message = "axis enabled flag disagrees with the minimum screen length";
                        return false;
                    }
                }
                else if (shape.Kind == HandleShapeKind::Quad)
                {
                    static constexpr int kPlaneAxes[8][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 2 }, { 0, 2 }, { 0, 1 }, { 0, 0 } };
                    const int* planeAxes = kPlaneAxes[static_cast<int>(shape.Handle)];
                    const DVec3& a = geometry.Axes[planeAxes[0]];
                    const DVec3& b = geometry.Axes[planeAxes[1]];
                    const double nearOffset = length * style.PlaneStart;
                    const double farOffset = length * style.PlaneEnd;
                    const DVec3 corners[4] = {
                        origin + a * nearOffset + b * nearOffset, origin + a * farOffset + b * nearOffset,
                        origin + a * farOffset + b * farOffset, origin + a * nearOffset + b * farOffset
                    };
                    double depthSum = 0.0;
                    bool comparable = shape.Points.size() == 4;
                    for (int corner = 0; corner < 4 && comparable; ++corner)
                    {
                        ScreenPoint pixel;
                        double cornerDepth = 0.0;
                        if (!OracleProject(scene, corners[corner], pixel, cornerDepth) || cornerDepth < kOracleMinimumDepth)
                        {
                            comparable = false;
                            break;
                        }

                        if (Distance(pixel, shape.Points[corner]) > 0.06)
                        {
                            message = "plane corner differs from the oracle by " + std::to_string(Distance(pixel, shape.Points[corner])) + " px";
                            return false;
                        }

                        depthSum += cornerDepth;
                    }

                    if (comparable && std::abs(shape.Depth - depthSum * 0.25) > 1e-4 * std::max(depth, depthSum * 0.25))
                    {
                        message = "plane depth is not the mean corner depth";
                        return false;
                    }
                }
                else if (shape.Kind == HandleShapeKind::Ring)
                {
                    const DVec3& u = geometry.Axes[(axisIndex + 1) % 3];
                    const DVec3& v = geometry.Axes[(axisIndex + 2) % 3];
                    const double radius = length * style.RingRadius;
                    bool complete = true;
                    bool comparable = true;
                    std::vector<DVec3> points;
                    std::vector<ScreenPoint> pixels;
                    std::vector<double> depths;
                    for (u32 segment = 0; segment < style.RingSegments; ++segment)
                    {
                        const double angle = 2.0 * kPi * segment / style.RingSegments;
                        const DVec3 point = origin + (u * std::cos(angle) + v * std::sin(angle)) * radius;
                        ScreenPoint pixel;
                        double pointDepth = 0.0;
                        const double signedDepth = OracleDepth(scene, point);
                        comparable &= std::abs(signedDepth) > 0.05;
                        if (!OracleProject(scene, point, pixel, pointDepth))
                            complete = false;
                        points.push_back(point);
                        pixels.push_back(pixel);
                        depths.push_back(pointDepth);
                    }

                    if (!comparable)
                        continue;

                    if (shape.Enabled != complete)
                    {
                        message = "a ring with a point behind the eye must be disabled, and only then";
                        return false;
                    }

                    for (size_t segment = 0; complete && segment < points.size(); ++segment)
                    {
                        if (depths[segment] >= kOracleMinimumDepth && Distance(pixels[segment], shape.Points[segment]) > 0.06)
                        {
                            message = "ring point differs from the oracle by " + std::to_string(Distance(pixels[segment], shape.Points[segment])) + " px at depth " + std::to_string(depths[segment]);
                            return false;
                        }
                    }

                    for (size_t segment = 0; complete && segment < points.size(); ++segment)
                    {
                        const DVec3 middle = (points[segment] + points[(segment + 1) % points.size()]) * 0.5;
                        const double facing = Dot(middle - origin, toEye);
                        const double scale = radius * Length(toEye);
                        if (std::abs(facing) > 1e-3 * scale && shape.Facing[segment] != (facing > 0.0 ? 1 : 0))
                        {
                            message = "ring facing flag disagrees with the eye side of the ring plane";
                            return false;
                        }
                    }
                }
            }

            return true;
        }
    }

    bool TestGizmoHandleGeometryAndHitTesting()
    {
        Checker check { "handles" };
        const GizmoStyle style = DefaultStyle();
        TestScene& hand = HandScene();
        const DVec3 origin { 0, 0, 10 };

        // Exact camera: the X axis is 96 px long, the view is down +Z, so Z-based handles degenerate.
        {
            GizmoGeometry geometry;
            check.Expect(BuildGizmoGeometry(hand.View, TransformTool::Translate, origin, kWorldBasis, true, style, geometry), "translate geometry");
            check.ExpectNear(geometry.OriginScreen.X, 400.0, 0.01, "origin x");
            check.ExpectNear(geometry.OriginScreen.Y, 300.0, 0.01, "origin y");
            check.ExpectNear(geometry.OriginDepth, 10.0, 1e-4, "origin depth");
            check.Expect(geometry.Axes[2].Z == -1.0 && geometry.Axes[0].X == 1.0 && geometry.Axes[1].Y == 1.0, "only Z points back toward the camera");
            check.Expect(geometry.Shapes.size() == 7, "three arrows, three planes and the centre");
            const auto enabled = [&](GizmoHandle handle)
            {
                const HandleShape* shape = FindShape(geometry, handle);
                return shape && shape->Enabled;
            };
            check.Expect(enabled(GizmoHandle::AxisX) && enabled(GizmoHandle::AxisY) && enabled(GizmoHandle::PlaneXY) && enabled(GizmoHandle::Center),
                "X, Y, XY plane and the centre are visible");
            check.Expect(!enabled(GizmoHandle::AxisZ) && !enabled(GizmoHandle::PlaneYZ) && !enabled(GizmoHandle::PlaneXZ),
                "the axis pointing along the view and the planes seen edge-on are hidden");
            const HandleShape* x = FindShape(geometry, GizmoHandle::AxisX);
            check.ExpectNear(x->Points[1].X, 496.0, 0.05, "X tip is 96 px to the right");
            check.ExpectNear(x->Points[1].Y, 300.0, 0.01, "X tip stays on the horizon");
            const HandleShape* xy = FindShape(geometry, GizmoHandle::PlaneXY);
            check.ExpectNear(xy->Points[0].X, 400.0 + 96.0 * 0.3, 0.05, "plane near corner x at 30 percent");
            check.ExpectNear(xy->Points[2].Y, 300.0 - 96.0 * 0.6, 0.05, "plane far corner y at 60 percent, up is smaller y");

            GizmoGeometry unflipped;
            check.Expect(BuildGizmoGeometry(hand.View, TransformTool::Translate, origin, kWorldBasis, false, style, unflipped) && unflipped.Axes[2].Z == 1.0,
                "orientTowardEye=false keeps the basis as given");

            const auto hit = [&](double x2, double y2) { return HitTestHandles(geometry, { x2, y2 }, style); };
            check.Expect(hit(496.0, 300.0) == GizmoHandle::AxisX, "arrow tip");
            check.Expect(hit(448.0, 300.0) == GizmoHandle::AxisX, "arrow shaft");
            check.Expect(hit(448.0, 305.95) == GizmoHandle::AxisX && hit(448.0, 294.05) == GizmoHandle::AxisX, "5.95 px from the shaft still hits");
            check.Expect(hit(448.0, 306.05) == GizmoHandle::None && hit(448.0, 293.95) == GizmoHandle::None, "6.05 px from the shaft misses");
            check.Expect(hit(503.9, 300.0) == GizmoHandle::AxisX, "7.9 px beyond the tip hits through the cap radius");
            check.Expect(hit(504.1, 300.0) == GizmoHandle::None, "8.1 px beyond the tip misses");
            check.Expect(hit(400.0, 252.0) == GizmoHandle::AxisY, "Y shaft");
            check.Expect(hit(440.0, 270.0) == GizmoHandle::PlaneXY, "inside the XY square wins over the shafts 30 px away");
            check.Expect(hit(404.0, 300.0) == GizmoHandle::Center, "centre disc ties with the X shaft at distance 0 and wins by priority");
            check.Expect(hit(406.9, 300.0) == GizmoHandle::Center && hit(407.1, 300.0) == GizmoHandle::AxisX,
                "just outside the 7 px disc the shaft, now closer, wins");
            check.Expect(hit(700.0, 100.0) == GizmoHandle::None, "far from everything");
            check.Expect(hit(400.0, 330.0) == GizmoHandle::None, "the hidden Z arrow is not pickable below the origin");

            // An empty or invalid geometry never hits.
            GizmoGeometry empty;
            check.Expect(HitTestHandles(empty, { 400, 300 }, style) == GizmoHandle::None, "empty geometry");
        }

        // Scale shares the arrows and centre; Select has no handles.
        {
            GizmoGeometry geometry;
            check.Expect(BuildGizmoGeometry(hand.View, TransformTool::Scale, origin, kWorldBasis, true, style, geometry) && geometry.Shapes.size() == 4,
                "scale has three boxes and the uniform centre");
            check.Expect(HitTestHandles(geometry, { 470.0, 300.0 }, style) == GizmoHandle::AxisX, "scale X box");
            GizmoGeometry select;
            check.Expect(BuildGizmoGeometry(hand.View, TransformTool::Select, origin, kWorldBasis, true, style, select) && select.Valid && select.Shapes.empty(),
                "select has no shapes");
            check.Expect(HitTestHandles(select, { 400, 300 }, style) == GizmoHandle::None, "select never hits");
        }

        // Rotation rings, including the ring seen exactly face-on whose points all lie on the silhouette.
        {
            GizmoGeometry geometry;
            check.Expect(BuildGizmoGeometry(hand.View, TransformTool::Rotate, origin, kWorldBasis, true, style, geometry) && geometry.Shapes.size() == 3,
                "three rings");
            check.Expect(geometry.Axes[2].Z == 1.0, "rings are never flipped toward the eye");
            const HandleShape* z = FindShape(geometry, GizmoHandle::AxisZ);
            check.Expect(z && z->Enabled && z->Points.size() == style.RingSegments, "the Z ring exists");
            size_t facing = 0;
            for (u8 flag : z->Facing)
                facing += flag;
            check.Expect(facing == z->Points.size(), "a ring seen exactly face-on stays entirely pickable");
            const double radius = 96.0;
            const auto onRing = [&](double degrees, double extra)
            {
                const double a = degrees * kPi / 180.0;
                return ScreenPoint { 400.0 + (radius + extra) * std::cos(a), 300.0 - (radius + extra) * std::sin(a) };
            };
            check.Expect(HitTestHandles(geometry, onRing(60.0, 0.0), style) == GizmoHandle::AxisZ, "on the face-on ring");
            check.Expect(HitTestHandles(geometry, onRing(60.0, 5.7), style) == GizmoHandle::AxisZ, "5.7 px outside the ring still hits");
            check.Expect(HitTestHandles(geometry, onRing(60.0, 6.3), style) == GizmoHandle::None, "6.3 px outside the ring misses");
            check.Expect(HitTestHandles(geometry, onRing(60.0, -5.7), style) == GizmoHandle::AxisZ, "5.7 px inside the ring still hits");
            check.Expect(HitTestHandles(geometry, onRing(250.0, 0.0), style) == GizmoHandle::AxisZ, "the whole face-on ring is pickable, not half");
        }

        // Tie-breaking on a hand-built geometry: distance, then depth, then kind, then handle order.
        {
            const auto square = [](GizmoHandle handle, double depth, double dx)
            {
                HandleShape shape;
                shape.Handle = handle;
                shape.Kind = HandleShapeKind::Quad;
                shape.Enabled = true;
                shape.Depth = depth;
                shape.Points = { { 0 + dx, 0 }, { 10 + dx, 0 }, { 10 + dx, 10 }, { 0 + dx, 10 } };
                return shape;
            };
            GizmoGeometry geometry;
            geometry.Valid = true;
            geometry.Shapes = { square(GizmoHandle::PlaneYZ, 5.0, 0), square(GizmoHandle::PlaneXZ, 3.0, 0), square(GizmoHandle::PlaneXY, 3.0, 0) };
            check.Expect(HitTestHandles(geometry, { 5, 5 }, style) == GizmoHandle::PlaneXZ, "overlapping squares: nearer depth wins, then the lower handle");
            geometry.Shapes[1].Enabled = false;
            check.Expect(HitTestHandles(geometry, { 5, 5 }, style) == GizmoHandle::PlaneXY, "a disabled shape is never picked");
            geometry.Shapes[2].Depth = 6.0;
            check.Expect(HitTestHandles(geometry, { 5, 5 }, style) == GizmoHandle::PlaneYZ, "depth decides before handle order");
            check.Expect(HitTestHandles(geometry, { 10.0, 10.0 }, style) != GizmoHandle::None, "a square includes its boundary");
            check.Expect(HitTestHandles(geometry, { 10.1, 10.0 }, style) == GizmoHandle::None, "outside the square");

            // Winding does not matter.
            std::reverse(geometry.Shapes[0].Points.begin(), geometry.Shapes[0].Points.end());
            check.Expect(HitTestHandles(geometry, { 5, 5 }, style) == GizmoHandle::PlaneYZ, "a reversed quad is still hit");

            HandleShape disc;
            disc.Handle = GizmoHandle::Center;
            disc.Kind = HandleShapeKind::Disc;
            disc.Enabled = true;
            disc.Depth = 5.0;
            disc.Radius = 7.0;
            disc.Points = { { 5, 5 } };
            geometry.Shapes[0].Depth = 5.0;
            geometry.Shapes.push_back(disc);
            check.Expect(HitTestHandles(geometry, { 5, 5 }, style) == GizmoHandle::Center, "centre beats a plane at equal distance and depth");

            HandleShape ring;
            ring.Handle = GizmoHandle::AxisX;
            ring.Kind = HandleShapeKind::Ring;
            ring.Enabled = true;
            ring.Points = { { 100, 100 }, { 200, 100 }, { 200, 200 }, { 100, 200 } };
            ring.Facing = { 0, 1, 0, 0 };
            GizmoGeometry rings;
            rings.Valid = true;
            rings.Shapes = { ring };
            check.Expect(HitTestHandles(rings, { 200, 150 }, style) == GizmoHandle::AxisX, "the facing ring segment is pickable");
            check.Expect(HitTestHandles(rings, { 100, 150 }, style) == GizmoHandle::None, "a back-facing ring segment is not");
            check.Expect(HitTestHandles(rings, { 150, 100 }, style) == GizmoHandle::None, "a back-facing segment's own midpoint is not");
        }

        // Generated: geometry against the pinhole oracle and hit testing against the sampled reference.
        HitCounters counters;
        const bool ok = RunProperty("TestGizmoHandleGeometryAndHitTesting",
            [&counters](ChoiceStream& stream, std::string& message)
            {
                TestScene scene;
                if (!MakeRandomScene(stream, scene))
                {
                    message = "random scene did not build";
                    return false;
                }

                const ViewportRect& rect = scene.View.Viewport;
                const ScreenPoint centre { rect.X + RangeDouble(stream, 0.25, 0.75) * rect.Width, rect.Y + RangeDouble(stream, 0.25, 0.75) * rect.Height };
                const DVec3 origin3 = OracleWorldFromPixel(scene, centre, RangeDouble(stream, 2.0, 60.0));

                DVec3 basis[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                if (stream.NextBool())
                {
                    const Rotation3 rotation = RotationFromEulerDegrees({
                        static_cast<float>(RangeDouble(stream, -80, 80)), static_cast<float>(RangeDouble(stream, -180, 180)), static_cast<float>(RangeDouble(stream, -180, 180)) });
                    for (u32 axis = 0; axis < 3; ++axis)
                        basis[axis] = Row(rotation, axis);
                }

                GizmoStyle style;
                style.TargetPixels = RangeDouble(stream, 64.0, 160.0);
                const TransformTool tools[3] = { TransformTool::Translate, TransformTool::Rotate, TransformTool::Scale };
                const TransformTool tool = tools[stream.NextSize(0, 2)];
                const bool orient = stream.NextBool();
                GizmoGeometry geometry;
                if (!BuildGizmoGeometry(scene.View, tool, origin3, basis, orient, style, geometry))
                {
                    message = "geometry did not build for an origin in front of the camera";
                    return false;
                }

                if (!CheckGeometryAgainstOracle(scene, geometry, tool, origin3, basis, orient, style, message))
                    return false;

                for (int sample = 0; sample < 5; ++sample)
                {
                    ScreenPoint cursor;
                    if (sample < 4 && !geometry.Shapes.empty())
                    {
                        const HandleShape& shape = geometry.Shapes[stream.NextSize(0, geometry.Shapes.size() - 1)];
                        if (shape.Points.empty())
                            continue;

                        cursor = PointOnShape(shape, stream);
                        const double angle = RangeDouble(stream, 0.0, 2.0 * kPi);
                        const double offset = RangeDouble(stream, 0.0, 1.0) < 0.3 ? 0.0 : RangeDouble(stream, 0.0, 14.0);
                        cursor.X += offset * std::cos(angle);
                        cursor.Y += offset * std::sin(angle);
                    }
                    else
                    {
                        cursor = { rect.X + RangeDouble(stream, -0.1, 1.1) * rect.Width, rect.Y + RangeDouble(stream, -0.1, 1.1) * rect.Height };
                    }

                    GizmoHandle expected = GizmoHandle::None;
                    if (ReferenceHit(geometry, cursor, style, expected) == ReferenceOutcome::Ambiguous)
                    {
                        ++counters.Ambiguous;
                        continue;
                    }

                    const GizmoHandle actual = HitTestHandles(geometry, cursor, style);
                    ++counters.Compared;
                    counters.Hits += expected != GizmoHandle::None ? 1 : 0;
                    if (actual != expected)
                    {
                        message = "hit test returned handle " + std::to_string(static_cast<int>(actual)) + " expected "
                            + std::to_string(static_cast<int>(expected)) + " at (" + std::to_string(cursor.X) + ", " + std::to_string(cursor.Y)
                            + ") for tool " + std::to_string(static_cast<int>(tool));
                        return false;
                    }
                }

                return true;
            });
        check.Expect(ok, "generated geometry and hit-test property");
        check.Expect(counters.Compared > 1000 && counters.Hits > 400 && counters.Hits < counters.Compared,
            "the generator exercised hits and misses: compared " + std::to_string(counters.Compared) + ", hits "
                + std::to_string(counters.Hits) + ", ambiguous " + std::to_string(counters.Ambiguous));
        return check.Ok;
    }

    namespace
    {
        Rotation3 EngineRotationRows(const Vec3& degrees)
        {
            const Engine::Math::Mat4 matrix = Engine::Math::RotationYawPitchRoll(
                Engine::Math::DegreesToRadians(degrees.Y), Engine::Math::DegreesToRadians(degrees.X), Engine::Math::DegreesToRadians(degrees.Z));
            Rotation3 result;
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                    result.M[row][column] = matrix.Values[row * 4 + column];
            }

            return result;
        }

        bool SamePosition(const SectorLocalPosition& a, const SectorLocalPosition& b)
        {
            return a.Sector == b.Sector && std::memcmp(&a.Local, &b.Local, sizeof(a.Local)) == 0;
        }

        // A scene plus an on-screen pivot at a comfortable depth.
        bool MakePivotScene(ChoiceStream& stream, TestScene& scene, DVec3& pivot, double& length)
        {
            if (!MakeRandomScene(stream, scene))
                return false;

            const ViewportRect& rect = scene.View.Viewport;
            const ScreenPoint centre { rect.X + RangeDouble(stream, 0.3, 0.7) * rect.Width, rect.Y + RangeDouble(stream, 0.3, 0.7) * rect.Height };
            pivot = OracleWorldFromPixel(scene, centre, RangeDouble(stream, 3.0, 40.0));
            return TryComputeGizmoLength(scene.View, pivot, 96.0, length);
        }

        Ray PointerRay(const TestScene& scene, const ScreenPoint& pixel)
        {
            Ray ray;
            TryScreenToRay(scene.View, pixel, ray);
            return ray;
        }

        double SineBetween(const DVec3& a, const DVec3& b)
        {
            return Length(Cross(a, b)) / (Length(a) * Length(b));
        }
    }

    bool TestGizmoSolversRecoverTrueDisplacements()
    {
        Checker check { "solvers" };

        // Hand cases for handedness and the accumulator boundary.
        {
            check.ExpectNear(RotationAngleBetween({ 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 }), kPi / 2.0, 1e-12,
                "+90 degrees about +Y takes +X to -Z, as Math::RotationY does for row vectors");
            const Engine::Math::Mat4 ry = Engine::Math::RotationY(Engine::Math::DegreesToRadians(90.0f));
            check.ExpectNear(ry.Values[2], -1.0, 1e-6, "the engine's RotationY agrees: row 0 is (0, 0, -1)");
            check.ExpectNear(RotationAngleBetween({ 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }), kPi / 2.0, 1e-12, "+90 about +Z takes +X to +Y");
            check.ExpectNear(std::abs(RotationAngleBetween({ 0, 1, 0 }, { 1, 0, 0 }, { -1, 0, 0 })), kPi, 1e-12, "opposite arms are half a turn");
            check.Expect(RotationAngleBetween({ 0, 1, 0 }, { 1, 0, 0 }, { 1, 0, 0 }) == 0.0, "equal arms are exactly zero");

            // A grab point that moves straight toward the camera has no screen tangent.
            TestScene& hand = HandScene();
            ScreenPoint tangent { 5.0, 6.0 };
            check.Expect(!SolveRingScreenTangent(hand.View, { -1.8, 0.0, 10.0 }, { 0, 1, 0 }, { 1, 0, 0 }, 1.8, tangent) && tangent.X == 5.0 && tangent.Y == 6.0,
                "a tangent along the view ray fails without writing");
            check.Expect(SolveRingScreenTangent(hand.View, { 0.0, 0.0, 10.0 }, { 0, 0, 1 }, { 1, 0, 0 }, 1.8, tangent)
                    && std::abs(tangent.X) < 1e-9 && std::abs(tangent.Y + 1.0) < 1e-9,
                "a +90 degree turn of a ring about +Z at its +X point heads up the screen (y down is negative)");

            AngleAccumulator accumulator;
            check.Expect(accumulator.Total() == 0.0, "starts at zero");
            double total = 0.0;
            for (double raw : { 1.0, 2.0, 3.0, -3.0, -2.0, -1.0, 0.5 })
                total = accumulator.Update(raw);
            check.ExpectNear(total, 2.0 * kPi + 0.5, 1e-12, "a full turn through +-pi accumulates to 2*pi + 0.5");
            accumulator.Reset();
            check.Expect(accumulator.Total() == 0.0 && accumulator.Update(0.25) == 0.25, "Reset starts over");
        }

        const bool axisOk = RunProperty("TestGizmoSolversRecoverTrueDisplacements.Axis",
            [](ChoiceStream& stream, std::string& message)
            {
                const DVec3 origin = RandomVector(stream, 30.0);
                const DVec3 axis = RandomUnit(stream);
                const double tTrue = RangeDouble(stream, -20.0, 20.0);
                const double offset = stream.NextBool() ? 0.0 : RangeDouble(stream, 0.0, 6.0);
                const DVec3 target = origin + axis * tTrue + RandomPerpendicular(stream, axis) * offset;
                const DVec3 eye = target + RandomUnit(stream) * RangeDouble(stream, 4.0, 80.0);
                const Ray ray { eye, (target - eye) * RangeDouble(stream, 0.2, 5.0) };
                const double sine = SineBetween(axis, ray.Direction);

                double t = 12345.0;
                const bool solved = SolveAxisParameter(ray, origin, axis, t);
                if (sine < kMinimumSolverSine)
                {
                    if (solved || t != 12345.0)
                    {
                        message = "a nearly parallel pointer ray must fail without writing";
                        return false;
                    }

                    return true;
                }

                if (sine < 2.0 * kMinimumSolverSine)
                    return true;

                if (!solved)
                {
                    message = "a well-conditioned pointer ray failed";
                    return false;
                }

                // Optimality: the pointer-perpendicular part of (P(t) - eye) is orthogonal to the axis.
                DVec3 unit;
                Engine::Math::TryNormalize(ray.Direction, unit);
                const DVec3 point = origin + axis * t;
                const DVec3 separation = (point - eye) - unit * Dot(point - eye, unit);
                if (std::abs(Dot(separation, axis)) > 1e-7 * (1.0 + Length(point - eye)))
                {
                    message = "the solved point is not the closest approach, residual " + std::to_string(Dot(separation, axis));
                    return false;
                }

                const double golden = OracleAxisParameter(origin, axis, ray, 400.0);
                if (std::abs(t - golden) > 1e-3 * (1.0 + std::abs(golden)) / std::max(sine, 0.05) * 0.05)
                {
                    message = "solved t " + std::to_string(t) + " differs from the golden-section minimum " + std::to_string(golden);
                    return false;
                }

                if (offset == 0.0 && std::abs(t - tTrue) > 1e-8 * (1.0 + std::abs(tTrue)) / sine)
                {
                    message = "a ray through the axis must recover the exact parameter";
                    return false;
                }

                return true;
            });
        check.Expect(axisOk, "axis parameter property");

        const bool planeOk = RunProperty("TestGizmoSolversRecoverTrueDisplacements.Plane",
            [](ChoiceStream& stream, std::string& message)
            {
                const DVec3 normal = RandomUnit(stream);
                const DVec3 planePoint = RandomVector(stream, 30.0);
                const DVec3 a = RandomPerpendicular(stream, normal);
                const DVec3 b = Cross(normal, a);
                const DVec3 hit = planePoint + a * RangeDouble(stream, -20.0, 20.0) + b * RangeDouble(stream, -20.0, 20.0);
                const double distance = RangeDouble(stream, 3.0, 50.0);
                const DVec3 toEye = RandomUnit(stream);
                const DVec3 eye = hit + toEye * distance;
                const double cosine = std::abs(Dot(normal, toEye));
                const double scale = stream.NextBool() ? 1.0 : RangeDouble(stream, 0.1, 9.0);
                const Ray ray { eye, (hit - eye) * scale };

                DVec3 point { 5.0, 6.0, 7.0 };
                const bool solved = SolvePlanePoint(ray, planePoint, normal * RangeDouble(stream, 0.5, 4.0), point);
                if (cosine < kMinimumSolverSine)
                {
                    if (solved || point.X != 5.0 || point.Y != 6.0 || point.Z != 7.0)
                    {
                        message = "an edge-on plane must fail without writing";
                        return false;
                    }

                    return true;
                }

                if (cosine < 2.0 * kMinimumSolverSine)
                    return true;

                if (!solved || Length(point - hit) > 1e-6 * distance / cosine)
                {
                    message = "the plane point was not recovered";
                    return false;
                }

                // A plane behind the pointer origin is not a hit.
                DVec3 behind { 1.0, 2.0, 3.0 };
                if (SolvePlanePoint({ eye, (eye - hit) * scale }, planePoint, normal, behind) || behind.X != 1.0 || behind.Y != 2.0)
                {
                    message = "a plane behind the eye must fail without writing";
                    return false;
                }

                return true;
            });
        check.Expect(planeOk, "plane point property");

        const bool rotationOk = RunProperty("TestGizmoSolversRecoverTrueDisplacements.Rotation",
            [](ChoiceStream& stream, std::string& message)
            {
                const DVec3 axis = RandomUnit(stream);
                const DVec3 pivot = RandomVector(stream, 30.0);
                const DVec3 startArm = RandomPerpendicular(stream, axis);
                const double picks[4] = { 0.0, kPi - 1e-9, -kPi + 1e-9, RangeDouble(stream, -kPi + 0.001, kPi) };
                const double theta = stream.NextBool() ? picks[stream.NextSize(0, 3)] : picks[3];
                const DVec3 arm = Rodrigues(startArm, axis, theta);
                const double radius = RangeDouble(stream, 0.1, 5.0);
                const DVec3 hit = pivot + arm * radius;
                const DVec3 toEye = RandomUnit(stream);
                const DVec3 eye = hit + toEye * RangeDouble(stream, 3.0, 60.0);
                const Ray ray { eye, (hit - eye) * RangeDouble(stream, 0.3, 4.0) };
                const double cosine = std::abs(Dot(axis, toEye));

                DVec3 solvedArm { 9.0, 9.0, 9.0 };
                const bool solved = SolveRotationArm(ray, pivot, axis, solvedArm);
                if (cosine < kMinimumSolverSine)
                {
                    if (solved || solvedArm.X != 9.0)
                    {
                        message = "an edge-on ring plane must fail without writing";
                        return false;
                    }

                    return true;
                }

                if (cosine < 2.0 * kMinimumSolverSine)
                    return true;

                if (!solved || std::abs(Length(solvedArm) - 1.0) > 1e-12 || Length(solvedArm - arm) > 1e-6 / cosine)
                {
                    message = "the arm was not recovered";
                    return false;
                }

                if (std::abs(Dot(solvedArm, axis)) > 1e-9)
                {
                    message = "the arm must lie in the ring plane";
                    return false;
                }

                const double angle = RotationAngleBetween(axis, startArm, solvedArm);
                if (std::abs(WrapAngle(angle - theta)) > 1e-6 / cosine || angle <= -kPi - 1e-12 || angle > kPi + 1e-12)
                {
                    message = "angle " + std::to_string(angle) + " expected " + std::to_string(theta);
                    return false;
                }

                // A pointer ray through the pivot has no direction.
                DVec3 centreArm { 4.0, 4.0, 4.0 };
                if (SolveRotationArm({ eye, pivot - eye }, pivot, axis, centreArm) || centreArm.X != 4.0)
                {
                    message = "a pointer on the pivot must fail without writing";
                    return false;
                }

                return true;
            });
        check.Expect(rotationOk, "rotation arm and angle property");

        const bool accumulatorOk = RunProperty("TestGizmoSolversRecoverTrueDisplacements.Accumulator",
            [](ChoiceStream& stream, std::string& message)
            {
                AngleAccumulator accumulator;
                double truth = 0.0;
                for (int frame = 0; frame < 60; ++frame)
                {
                    truth += RangeDouble(stream, -3.1, 3.1);
                    const double total = accumulator.Update(WrapAngle(truth));
                    if (std::abs(total - truth) > 1e-9 || accumulator.Total() != total)
                    {
                        message = "frame " + std::to_string(frame) + " accumulated " + std::to_string(total) + " expected " + std::to_string(truth);
                        return false;
                    }
                }

                return true;
            });
        check.Expect(accumulatorOk, "angle accumulator property");

        const bool tangentOk = RunProperty("TestGizmoSolversRecoverTrueDisplacements.Tangent",
            [](ChoiceStream& stream, std::string& message)
            {
                TestScene scene;
                DVec3 pivot;
                double length = 0.0;
                if (!MakePivotScene(stream, scene, pivot, length))
                {
                    message = "scene did not build";
                    return false;
                }

                const DVec3 axis = RandomUnit(stream);
                const DVec3 startArm = RandomPerpendicular(stream, axis);
                ScreenPoint tangent;
                const bool solved = SolveRingScreenTangent(scene.View, pivot, axis, startArm, length, tangent);
                const DVec3 grab = pivot + startArm * length;
                ScreenPoint from;
                ScreenPoint to;
                double d0 = 0.0;
                double d1 = 0.0;
                const bool visible = OracleProject(scene, grab, from, d0)
                    && OracleProject(scene, pivot + Rodrigues(startArm, axis, 1e-4) * length, to, d1);
                if (!visible)
                    return true;

                const double travel = Distance(from, to);
                if (travel < 1e-3)
                    return true;

                if (!solved)
                {
                    message = "the tangent failed although the grab point moves on screen";
                    return false;
                }

                const double dot = (tangent.X * (to.X - from.X) + tangent.Y * (to.Y - from.Y)) / travel;
                if (std::abs(std::hypot(tangent.X, tangent.Y) - 1.0) > 1e-9 || dot < 0.9999)
                {
                    message = "tangent disagrees with the finite-difference screen motion, cosine " + std::to_string(dot);
                    return false;
                }

                const ScreenPoint start { 300.0, 200.0 };
                const double pixels = RangeDouble(stream, -200.0, 200.0);
                const ScreenPoint along { start.X + tangent.X * pixels, start.Y + tangent.Y * pixels };
                const ScreenPoint across { start.X - tangent.Y * pixels, start.Y + tangent.X * pixels };
                if (std::abs(RotationAngleFromScreenDrag(along, start, tangent, 96.0) - pixels / 96.0) > 1e-12
                    || std::abs(RotationAngleFromScreenDrag(across, start, tangent, 96.0)) > 1e-12
                    || RotationAngleFromScreenDrag(start, start, tangent, 96.0) != 0.0)
                {
                    message = "screen drag angle is not linear along the tangent, zero across it";
                    return false;
                }

                return true;
            });
        check.Expect(tangentOk, "ring screen tangent property");

        const bool scaleOk = RunProperty("TestGizmoSolversRecoverTrueDisplacements.Scale",
            [](ChoiceStream& stream, std::string& message)
            {
                const double minimum = RangeDouble(stream, 0.05, 1.0);
                const double grab = (stream.NextBool() ? 1.0 : -1.0) * RangeDouble(stream, 0.0, 3.0);
                const double current = RangeDouble(stream, -6.0, 6.0);
                const double factor = SolveAxisScaleFactor(current, grab, minimum);
                if (factor < kMinimumScaleFactor || !std::isfinite(factor))
                {
                    message = "factor below the minimum or non-finite";
                    return false;
                }

                if (SolveAxisScaleFactor(grab, grab, minimum) != 1.0)
                {
                    message = "zero travel must be exactly 1";
                    return false;
                }

                if (std::abs(grab) >= minimum)
                {
                    const double expected = std::max(kMinimumScaleFactor, current / grab);
                    if (std::abs(factor - expected) > 1e-12 * std::max(1.0, std::abs(expected)))
                    {
                        message = "far grabs scale by the ratio of distances from the pivot";
                        return false;
                    }

                    if (std::abs(SolveAxisScaleFactor(-current, -grab, minimum) - factor) > 1e-12 * std::max(1.0, factor))
                    {
                        message = "mirroring both distances must give the same factor";
                        return false;
                    }
                }
                else
                {
                    const double reference = grab < 0.0 ? -minimum : minimum;
                    const double expected = std::max(kMinimumScaleFactor, 1.0 + (current - grab) / reference);
                    if (std::abs(factor - expected) > 1e-12 * std::max(1.0, std::abs(expected)))
                    {
                        message = "a grab near the pivot uses the minimum reference with its sign";
                        return false;
                    }
                }

                // Monotonic in the pointer travel along the outward direction.
                const double outward = grab < 0.0 ? -1.0 : 1.0;
                if (SolveAxisScaleFactor(current + outward * 0.5, grab, minimum) < factor)
                {
                    message = "moving the pointer away from the pivot must not shrink the object";
                    return false;
                }

                const ScreenPoint start { RangeDouble(stream, 0.0, 800.0), RangeDouble(stream, 0.0, 600.0) };
                const ScreenPoint moved { start.X + RangeDouble(stream, -300.0, 300.0), start.Y + RangeDouble(stream, -300.0, 300.0) };
                const double reference = RangeDouble(stream, 30.0, 200.0);
                const double uniform = SolveUniformScaleFactor(moved, start, reference);
                const double expected = std::max(kMinimumScaleFactor, 1.0 + ((moved.X - start.X) - (moved.Y - start.Y)) / reference);
                if (std::abs(uniform - expected) > 1e-12 * std::max(1.0, expected) || SolveUniformScaleFactor(start, start, reference) != 1.0)
                {
                    message = "uniform scale is right-and-up growth, exactly 1 at zero travel";
                    return false;
                }

                const double twice = SolveUniformScaleFactor({ start.X + reference, start.Y }, start, reference);
                const double twiceUp = SolveUniformScaleFactor({ start.X, start.Y - reference }, start, reference);
                if (std::abs(twice - 2.0) > 1e-12 || std::abs(twiceUp - 2.0) > 1e-12
                    || std::abs(SolveUniformScaleFactor({ start.X + reference, start.Y + reference }, start, reference) - 1.0) > 1e-12)
                {
                    message = "right and up both double, right+down cancels";
                    return false;
                }

                return true;
            });
        check.Expect(scaleOk, "scale factor property");
        return check.Ok;
    }

    bool TestGizmoDragRoundTripsThroughApply()
    {
        Checker check { "drag-roundtrip" };
        WorldGridPolicy policy;
        policy.SectorExtent = 4096.0;
        policy.OriginHysteresis = 0.0;
        SnapSettings off;

        const bool axisOk = RunProperty("TestGizmoDragRoundTripsThroughApply.Axis",
            [&policy, &off](ChoiceStream& stream, std::string& message)
            {
                TestScene scene;
                DVec3 pivot;
                double length = 0.0;
                if (!MakePivotScene(stream, scene, pivot, length))
                {
                    message = "scene did not build";
                    return false;
                }

                const DVec3 u = RandomUnit(stream);
                const DVec3 v = RandomPerpendicular(stream, u);
                const DVec3 basis[3] = { u, v, Cross(u, v) };
                const double t0 = length * RangeDouble(stream, 0.2, 1.0);
                const DVec3 grabPoint = pivot + u * t0;
                const double delta = RangeDouble(stream, -2.0 * length, 2.0 * length);
                const DVec3 target = grabPoint + u * delta + v * (stream.NextBool() ? 0.0 : RangeDouble(stream, -0.3 * length, 0.3 * length));
                ScreenPoint c0;
                ScreenPoint c1;
                double depth0 = 0.0;
                double depth1 = 0.0;
                if (!OracleProject(scene, grabPoint, c0, depth0) || !OracleProject(scene, target, c1, depth1)
                    || depth0 < kOracleMinimumDepth || depth1 < kOracleMinimumDepth)
                {
                    return true;
                }

                const Ray ray0 = PointerRay(scene, c0);
                const Ray ray1 = PointerRay(scene, c1);
                if (SineBetween(u, ray0.Direction) < 0.05 || SineBetween(u, ray1.Direction) < 0.05)
                    return true;

                double tg = 0.0;
                double t1 = 0.0;
                if (!SolveAxisParameter(ray0, pivot, u, tg) || !SolveAxisParameter(ray1, pivot, u, t1))
                {
                    message = "axis solve failed";
                    return false;
                }

                if (std::abs(tg - t0) > 1e-4 * (1.0 + length))
                {
                    message = "the grab parameter is not where the cursor pressed on the axis";
                    return false;
                }

                GizmoTransform start;
                start.Position.Sector = { stream.NextI64(-1000000, 1000000), stream.NextI64(-5, 5), 3 };
                const double edge = stream.NextBool() ? 2048.0 - RangeDouble(stream, 0.0, 10.0) : RangeDouble(stream, -2000.0, 2000.0);
                start.Position.Local = { edge, RangeDouble(stream, -2000.0, 2000.0), RangeDouble(stream, -2000.0, 2000.0) };
                start.RotationDegrees = { 10.0f, 20.0f, 30.0f };
                const DVec3 raw = u * (t1 - tg);
                GizmoTransform moved;
                if (!ApplyTranslation(start, basis, GizmoHandle::AxisX, TransformSpace::World, raw, false, off, policy, moved))
                {
                    message = "apply rejected a representable move";
                    return false;
                }

                DVec3 actual;
                if (!Engine::Math::TryGetSectorLocalRelativePosition(moved.Position, start.Position, policy, actual)
                    || Length(actual - raw) > 1e-9 * (1.0 + Length(raw)))
                {
                    message = "the applied displacement differs from the solved one";
                    return false;
                }

                // Re-solving from the moved pivot yields the same grab parameter: the grabbed point stayed under the cursor.
                double again = 0.0;
                if (!SolveAxisParameter(ray1, pivot + actual, u, again) || std::abs(again - tg) > 1e-6 * (1.0 + length))
                {
                    message = "after the move the grabbed point is no longer at the cursor's closest approach";
                    return false;
                }

                // Zero is an exact no-op and the move is reversible across sector edges.
                GizmoTransform same;
                if (!ApplyTranslation(start, basis, GizmoHandle::AxisX, TransformSpace::World, {}, false, off, policy, same) || !SamePosition(same.Position, start.Position))
                {
                    message = "a zero delta changed the position bits";
                    return false;
                }

                GizmoTransform back;
                DVec3 residual;
                if (!ApplyTranslation(moved, basis, GizmoHandle::AxisX, TransformSpace::World, raw * -1.0, false, off, policy, back)
                    || !Engine::Math::TryGetSectorLocalRelativePosition(back.Position, start.Position, policy, residual)
                    || Length(residual) > 1e-9 * (1.0 + Length(raw)))
                {
                    message = "applying the negated delta did not return to the start";
                    return false;
                }

                return true;
            });
        check.Expect(axisOk, "axis translate round trip");

        const bool planeOk = RunProperty("TestGizmoDragRoundTripsThroughApply.Plane",
            [&policy, &off](ChoiceStream& stream, std::string& message)
            {
                TestScene scene;
                DVec3 pivot;
                double length = 0.0;
                if (!MakePivotScene(stream, scene, pivot, length))
                {
                    message = "scene did not build";
                    return false;
                }

                const DVec3 normal = RandomUnit(stream);
                const DVec3 a = RandomPerpendicular(stream, normal);
                const DVec3 b = Cross(normal, a);
                const DVec3 basis[3] = { a, b, normal };
                const DVec3 grabPoint = pivot + a * RangeDouble(stream, -length, length) + b * RangeDouble(stream, -length, length);
                const DVec3 target = grabPoint + a * RangeDouble(stream, -2.0 * length, 2.0 * length) + b * RangeDouble(stream, -2.0 * length, 2.0 * length);
                ScreenPoint c0;
                ScreenPoint c1;
                double depth0 = 0.0;
                double depth1 = 0.0;
                if (!OracleProject(scene, grabPoint, c0, depth0) || !OracleProject(scene, target, c1, depth1)
                    || depth0 < kOracleMinimumDepth || depth1 < kOracleMinimumDepth)
                {
                    return true;
                }

                const Ray ray0 = PointerRay(scene, c0);
                const Ray ray1 = PointerRay(scene, c1);
                const double cosine0 = std::abs(Dot(ray0.Direction, normal));
                const double cosine1 = std::abs(Dot(ray1.Direction, normal));
                DVec3 g0;
                DVec3 g1;
                const bool solved0 = SolvePlanePoint(ray0, pivot, normal, g0);
                const bool solved1 = SolvePlanePoint(ray1, pivot, normal, g1);
                if (cosine0 < 0.15 || cosine1 < 0.15)
                    return true;

                if (!solved0 || !solved1)
                {
                    message = "plane solve failed on a well-conditioned ray";
                    return false;
                }

                const double tolerance = 1e-4 * (1.0 + length) / std::min(cosine0, cosine1);
                if (Length(g0 - grabPoint) > tolerance || Length(g1 - target) > tolerance)
                {
                    message = "the plane solver did not recover the pressed and dragged points";
                    return false;
                }

                GizmoTransform start;
                start.Position = { { 7, -2, 9 }, { 100.0, 200.0, -300.0 } };
                GizmoTransform moved;
                const DVec3 raw = g1 - g0;
                if (!ApplyTranslation(start, basis, GizmoHandle::PlaneXY, TransformSpace::World, raw, false, off, policy, moved))
                {
                    message = "plane apply rejected a representable move";
                    return false;
                }

                DVec3 actual;
                Engine::Math::TryGetSectorLocalRelativePosition(moved.Position, start.Position, policy, actual);
                if (std::abs(Dot(actual, normal)) > 1e-9 * (1.0 + Length(raw)))
                {
                    message = "a plane handle moved the entity out of its plane";
                    return false;
                }

                ScreenPoint reprojected;
                double reprojectedDepth = 0.0;
                if (!OracleProject(scene, g0 + actual, reprojected, reprojectedDepth) || Distance(reprojected, c1) > 0.05)
                {
                    message = "the grabbed point does not land under the cursor after the move, off by " + std::to_string(Distance(reprojected, c1)) + " px";
                    return false;
                }

                return true;
            });
        check.Expect(planeOk, "plane translate round trip");

        const bool rotationOk = RunProperty("TestGizmoDragRoundTripsThroughApply.Rotation",
            [&off](ChoiceStream& stream, std::string& message)
            {
                TestScene scene;
                DVec3 pivot;
                double length = 0.0;
                if (!MakePivotScene(stream, scene, pivot, length))
                {
                    message = "scene did not build";
                    return false;
                }

                GizmoTransform start;
                start.RotationDegrees = { static_cast<float>(RangeDouble(stream, -80, 80)), static_cast<float>(RangeDouble(stream, -180, 180)), static_cast<float>(RangeDouble(stream, -180, 180)) };
                const Rotation3 before = EngineRotationRows(start.RotationDegrees);
                const DVec3 axis = stream.NextBool() ? RandomUnit(stream) : Row(before, static_cast<u32>(stream.NextSize(0, 2)));
                const DVec3 arm0 = RandomPerpendicular(stream, axis);
                const double theta = RangeDouble(stream, -kPi + 0.01, kPi - 0.01);
                const DVec3 arm1 = Rodrigues(arm0, axis, theta);
                ScreenPoint c0;
                ScreenPoint c1;
                double depth0 = 0.0;
                double depth1 = 0.0;
                if (!OracleProject(scene, pivot + arm0 * length, c0, depth0) || !OracleProject(scene, pivot + arm1 * length, c1, depth1)
                    || depth0 < kOracleMinimumDepth || depth1 < kOracleMinimumDepth)
                {
                    return true;
                }

                const Ray ray0 = PointerRay(scene, c0);
                const Ray ray1 = PointerRay(scene, c1);
                if (std::abs(Dot(ray0.Direction, axis)) < 0.3 || std::abs(Dot(ray1.Direction, axis)) < 0.3)
                    return true;

                DVec3 solved0;
                DVec3 solved1;
                if (!SolveRotationArm(ray0, pivot, axis, solved0) || !SolveRotationArm(ray1, pivot, axis, solved1))
                {
                    message = "rotation arm solve failed";
                    return false;
                }

                AngleAccumulator accumulator;
                const double total = accumulator.Update(RotationAngleBetween(axis, solved0, solved1));
                if (std::abs(WrapAngle(total - theta)) > 1e-4)
                {
                    message = "recovered angle " + std::to_string(total) + " expected " + std::to_string(theta);
                    return false;
                }

                GizmoTransform out;
                double applied = 0.0;
                if (!ApplyRotation(start, axis, total, false, off, out, applied) || std::abs(applied - total * 180.0 / kPi) > 1e-9)
                {
                    message = "rotation apply failed or misreported the applied angle";
                    return false;
                }

                // The object-local direction that pointed at the pressed ring point now points at the cursor.
                const Rotation3 after = EngineRotationRows(out.RotationDegrees);
                DVec3 local;
                local.X = Dot(Row(before, 0), solved0);
                local.Y = Dot(Row(before, 1), solved0);
                local.Z = Dot(Row(before, 2), solved0);
                const DVec3 world = Row(after, 0) * local.X + Row(after, 1) * local.Y + Row(after, 2) * local.Z;
                if (Length(world - solved1) > 1e-4)
                {
                    message = "the grabbed object direction does not follow the cursor, off by " + std::to_string(Length(world - solved1));
                    return false;
                }

                return true;
            });
        check.Expect(rotationOk, "rotation round trip");

        const bool scaleOk = RunProperty("TestGizmoDragRoundTripsThroughApply.Scale",
            [&off](ChoiceStream& stream, std::string& message)
            {
                TestScene scene;
                DVec3 pivot;
                double length = 0.0;
                if (!MakePivotScene(stream, scene, pivot, length))
                {
                    message = "scene did not build";
                    return false;
                }

                const DVec3 u = RandomUnit(stream);
                const double t0 = length * RangeDouble(stream, 0.3, 1.0);
                const double t1 = length * RangeDouble(stream, 0.05, 3.0);
                ScreenPoint c0;
                ScreenPoint c1;
                double depth0 = 0.0;
                double depth1 = 0.0;
                if (!OracleProject(scene, pivot + u * t0, c0, depth0) || !OracleProject(scene, pivot + u * t1, c1, depth1)
                    || depth0 < kOracleMinimumDepth || depth1 < kOracleMinimumDepth)
                {
                    return true;
                }

                const Ray ray0 = PointerRay(scene, c0);
                const Ray ray1 = PointerRay(scene, c1);
                if (SineBetween(u, ray0.Direction) < 0.1 || SineBetween(u, ray1.Direction) < 0.1)
                    return true;

                double tg = 0.0;
                double tc = 0.0;
                if (!SolveAxisParameter(ray0, pivot, u, tg) || !SolveAxisParameter(ray1, pivot, u, tc))
                {
                    message = "axis solve failed";
                    return false;
                }

                const double minimum = length * 0.25;
                const double factor = SolveAxisScaleFactor(tc, tg, minimum);
                GizmoTransform start;
                start.Scale = { 1.0f, 2.0f, 3.0f };
                const GizmoHandle handles[3] = { GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ };
                const size_t axis = stream.NextSize(0, 2);
                GizmoTransform out;
                double appliedFactor = 0.0;
                if (!ApplyScale(start, handles[axis], factor, false, off, out, appliedFactor) || appliedFactor != factor)
                {
                    message = "scale apply failed";
                    return false;
                }

                const float startValues[3] = { start.Scale.X, start.Scale.Y, start.Scale.Z };
                const float outValues[3] = { out.Scale.X, out.Scale.Y, out.Scale.Z };
                for (size_t index = 0; index < 3; ++index)
                {
                    const double expected = index == axis
                        ? std::clamp(static_cast<double>(startValues[index]) * factor, static_cast<double>(kMinimumScale), static_cast<double>(kMaximumScale))
                        : static_cast<double>(startValues[index]);
                    if (std::abs(static_cast<double>(outValues[index]) - expected) > 1e-5 * expected)
                    {
                        message = "only the dragged axis may scale";
                        return false;
                    }
                }

                // The handle point tg * factor sits where the cursor's closest approach is (for far grabs).
                if (std::abs(tg) >= minimum && factor > kMinimumScaleFactor && std::abs(tg * factor - tc) > 1e-9 * (1.0 + std::abs(tc)))
                {
                    message = "the scaled handle point is not at the cursor's closest approach";
                    return false;
                }

                // Uniform scale multiplies all three; reversing the drag returns to exactly 1.
                const ScreenPoint pressed { 100.0, 100.0 };
                const ScreenPoint dragged { pressed.X + RangeDouble(stream, -120.0, 120.0), pressed.Y + RangeDouble(stream, -120.0, 120.0) };
                const double uniform = SolveUniformScaleFactor(dragged, pressed, 96.0);
                if (!ApplyScale(start, GizmoHandle::Center, uniform, false, off, out, appliedFactor))
                {
                    message = "uniform scale apply failed";
                    return false;
                }

                const double clampedX = std::clamp(static_cast<double>(start.Scale.X) * uniform, static_cast<double>(kMinimumScale), static_cast<double>(kMaximumScale));
                if (std::abs(static_cast<double>(out.Scale.X) - clampedX) > 1e-5 * clampedX
                    || SolveUniformScaleFactor(pressed, pressed, 96.0) != 1.0)
                {
                    message = "uniform scale mismatch";
                    return false;
                }

                return true;
            });
        check.Expect(scaleOk, "scale round trip");
        return check.Ok;
    }

    namespace
    {
        // ---- interaction rig ----------------------------------------------

        class RecordingHost final : public IGizmoEditHost
        {
        public:
            enum class Kind
            {
                Begin,
                Apply,
                End,
                Cancel
            };

            struct Call
            {
                Kind What = Kind::Begin;
                GizmoGestureKey Key;
                GizmoTransform Transform;
                bool Accepted = true;
            };

            std::vector<Call> Calls;
            GizmoTransform Document;
            bool Reject = false;

            void BeginGesture(const GizmoGestureKey& key) override
            {
                Calls.push_back({ Kind::Begin, key, {}, true });
            }

            bool ApplyTransform(const GizmoGestureKey& key, const GizmoTransform& transform) override
            {
                const bool accepted = !Reject;
                if (accepted)
                    Document = transform;
                Calls.push_back({ Kind::Apply, key, transform, accepted });
                return accepted;
            }

            void EndGesture(const GizmoGestureKey& key) override
            {
                Calls.push_back({ Kind::End, key, {}, true });
            }

            void CancelGesture(const GizmoGestureKey& key) override
            {
                Calls.push_back({ Kind::Cancel, key, {}, true });
            }

            size_t Count(Kind what) const
            {
                return static_cast<size_t>(std::count_if(Calls.begin(), Calls.end(), [what](const Call& call) { return call.What == what; }));
            }
        };

        // One entity in front of an oblique camera, the way the Editor feeds the gizmo each frame.
        struct Rig
        {
            TestScene Scene;
            WorldGridPolicy Policy;
            SectorLocalPosition ViewOrigin;
            GizmoTarget Target;
            RecordingHost Host;
            TransformGizmo Gizmo;
            GizmoFrameInput Input;
            GizmoFrameResult Last;
            bool HasTarget = true;
            bool HasView = true;
            double Distance = 9.6;

            explicit Rig(const SectorLocalPosition& viewOrigin = {})
                : ViewOrigin(viewOrigin)
            {
                Policy.SectorExtent = 4096.0;
                Policy.OriginHysteresis = 0.0;
                if (!BuildScene({ 3.0, 2.5, -9.0 }, { 12.0f, -18.0f, 0.0f }, 60.0f, 4.0f / 3.0f, 0.1f, 500.0f, { 0, 0, 800, 600 }, Scene))
                    std::cerr << "rig scene failed to build\n";

                const DVec3 relative = Scene.Eye + Scene.ViewAxis(2) * Distance;
                const SectorLocalPosition moved { viewOrigin.Sector, { viewOrigin.Local.X + relative.X, viewOrigin.Local.Y + relative.Y, viewOrigin.Local.Z + relative.Z } };
                Engine::Math::TryNormalizeSectorLocal(moved, Policy, Target.Transform.Position);
                Target.EntityId = 42;
                Target.Transform.Scale = { 1.0f, 2.0f, 3.0f };
                Host.Document = Target.Transform;
                Input.Policy = Policy;
            }

            DVec3 Relative(const SectorLocalPosition& position) const
            {
                DVec3 relative;
                Engine::Math::TryGetSectorLocalRelativePosition(position, ViewOrigin, Policy, relative);
                return relative;
            }

            GizmoFrameResult Frame(const ScreenPoint& cursor, bool down, bool ctrl = false, bool escape = false, bool over = true)
            {
                Target.Transform = Host.Document;
                Target.RelativePosition = Relative(Host.Document.Position);
                Input.View = HasView ? &Scene.View : nullptr;
                Input.Target = HasTarget ? &Target : nullptr;
                Input.Policy = Policy;
                Input.Pointer = { cursor, over, down, ctrl, escape };
                Last = Gizmo.Update(Input, Host);
                return Last;
            }

            GizmoGeometry Geometry(TransformTool tool) const
            {
                DVec3 basis[3];
                BuildToolBasis(tool, Input.Space, Host.Document.RotationDegrees, basis);
                GizmoGeometry geometry;
                BuildGizmoGeometry(Scene.View, tool, Relative(Host.Document.Position), basis, true, Input.Style, geometry);
                return geometry;
            }

            ScreenPoint OnHandle(TransformTool tool, GizmoHandle handle, double fraction) const
            {
                const GizmoGeometry geometry = Geometry(tool);
                const HandleShape* shape = FindShape(geometry, handle);
                if (!shape || shape->Points.size() < 2)
                    return {};

                return { shape->Points[0].X + (shape->Points[1].X - shape->Points[0].X) * fraction,
                    shape->Points[0].Y + (shape->Points[1].Y - shape->Points[0].Y) * fraction };
            }

            // World direction of a handle's axis as the gizmo shows it (flipped toward the eye for arrows).
            DVec3 AxisDirection(TransformTool tool, GizmoHandle handle) const
            {
                return Geometry(tool).Axes[AxisIndex(handle)];
            }
        };

        ScreenPoint Lerp(const ScreenPoint& a, const ScreenPoint& b, double t)
        {
            return { a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t };
        }

        // Axis parameter the pointer reaches, from the pinhole oracle and the golden-section minimiser.
        double OracleParameter(const Rig& rig, const DVec3& pivot, const DVec3& axis, const ScreenPoint& cursor)
        {
            return OracleAxisParameter(pivot, axis, OracleRay(rig.Scene, cursor), 200.0);
        }

        bool BitsEqualExceptPosition(const GizmoTransform& a, const GizmoTransform& b)
        {
            return SameBits(a.RotationDegrees, b.RotationDegrees) && SameBits(a.Scale, b.Scale);
        }

        const char* kDegree = "\xC2\xB0";
    }

    bool TestGizmoStateMachineScriptedGestures()
    {
        Checker check { "scripted" };
        using Kind = RecordingHost::Kind;

        // Hover, idle and the conditions that suppress handles.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Translate;
            const ScreenPoint onX = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 0.6);
            GizmoFrameResult result = rig.Frame({ 700.0, 60.0 }, false);
            check.Expect(result.Phase == GizmoPhase::Idle && !result.WantsPointer && result.Hover == GizmoHandle::None, "idle far from every handle");
            check.Expect(!rig.Gizmo.DrawList().empty(), "handles are drawn while idle");
            result = rig.Frame(onX, false);
            check.Expect(result.Phase == GizmoPhase::Hover && result.Hover == GizmoHandle::AxisX && result.WantsPointer, "hovering the X arrow");
            check.Expect(rig.Host.Calls.empty(), "hover never calls the host");
            bool hoverColoured = false;
            for (const GizmoDrawPrimitive& primitive : rig.Gizmo.DrawList())
                hoverColoured |= primitive.Kind == GizmoPrimitiveKind::Line && primitive.Color == rig.Input.Palette.Hover;
            check.Expect(hoverColoured, "the hovered handle is drawn in the hover colour");

            result = rig.Frame(onX, false, false, false, false);
            check.Expect(result.Phase == GizmoPhase::Idle && result.Hover == GizmoHandle::None && !result.WantsPointer,
                "a pointer that is not over the viewport image hovers nothing");
            rig.Input.WindowFocused = false;
            result = rig.Frame(onX, false);
            check.Expect(result.Hover == GizmoHandle::None, "an unfocused window hovers nothing");
            rig.Input.WindowFocused = true;

            rig.Input.Tool = TransformTool::Select;
            result = rig.Frame(onX, true);
            check.Expect(result.Phase == GizmoPhase::Idle && rig.Gizmo.DrawList().empty() && rig.Host.Calls.empty(), "the Select tool shows and picks nothing");
            rig.Frame(onX, false);
            rig.Input.Tool = TransformTool::Scale;
            rig.Target.AllowScale = false;
            const GizmoTarget noScale = [&] { GizmoTarget t = rig.Target; t.AllowScale = false; return t; }();
            rig.Input.Target = &noScale;
            result = rig.Gizmo.Update(rig.Input, rig.Host);
            check.Expect(result.Phase == GizmoPhase::Idle && rig.Gizmo.DrawList().empty(), "a camera-bearing target has no Scale tool");
            rig.Input.Target = nullptr;
            rig.Input.Tool = TransformTool::Translate;
            result = rig.Gizmo.Update(rig.Input, rig.Host);
            check.Expect(result.Phase == GizmoPhase::Idle && rig.Gizmo.DrawList().empty(), "no target, no gizmo");
            rig.Input.Target = &rig.Target;
            rig.Input.View = nullptr;
            result = rig.Gizmo.Update(rig.Input, rig.Host);
            check.Expect(result.Phase == GizmoPhase::Idle && rig.Gizmo.DrawList().empty(), "no view, no gizmo");
            check.Expect(rig.Host.Calls.empty(), "none of that touched the host");

            // A press that does not start on a handle never begins; a button already held when the pointer arrives does not either.
            rig.Input.Tool = TransformTool::Translate;
            rig.Frame({ 700.0, 60.0 }, true);
            rig.Frame(onX, true);
            check.Expect(rig.Host.Calls.empty() && rig.Last.Phase == GizmoPhase::Hover, "dragging onto a handle with the button held is not a press");
        }

        // A world-space X drag follows the pointer's closest approach to the axis, never accumulating.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Translate;
            const DVec3 pivot = rig.Relative(rig.Target.Transform.Position);
            const DVec3 axis = rig.AxisDirection(TransformTool::Translate, GizmoHandle::AxisX);
            const GizmoTransform start = rig.Host.Document;
            const ScreenPoint grab = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 0.55);
            const ScreenPoint away = Lerp(grab, rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 1.0), 2.0);
            rig.Frame(grab, false);
            GizmoFrameResult result = rig.Frame(grab, true);
            check.Expect(result.Phase == GizmoPhase::Active && result.Active == GizmoHandle::AxisX && result.WantsPointer && result.GestureId == 1, "press begins gesture 1");
            check.Expect(rig.Host.Calls.size() == 1 && rig.Host.Calls[0].What == Kind::Begin, "Begin before any write");
            check.Expect(rig.Host.Calls[0].Key.EntityId == 42 && rig.Host.Calls[0].Key.Property == GizmoProperty::Position, "the key names the entity and the Position property");
            check.Expect(rig.Host.Document.Position.Local.X == start.Position.Local.X, "nothing written by the press itself");

            const double t0 = OracleParameter(rig, pivot, axis, grab);
            double previous = start.Position.Local.X;
            for (int step = 1; step <= 6; ++step)
            {
                const ScreenPoint cursor = Lerp(grab, away, step / 6.0);
                result = rig.Frame(cursor, true);
                const double expected = start.Position.Local.X + (OracleParameter(rig, pivot, axis, cursor) - t0) * Dot(axis, DVec3 { 1, 0, 0 });
                check.ExpectNear(rig.Host.Document.Position.Local.X, expected, 3e-4, "frame " + std::to_string(step) + " is start + total solved delta");
                check.Expect(rig.Host.Document.Position.Local.X != previous, "the entity moves each frame");
                previous = rig.Host.Document.Position.Local.X;
                check.Expect(result.Phase == GizmoPhase::Active && !result.Readout.empty(), "readout while dragging");
            }

            check.Expect(rig.Host.Document.Position.Local.Y == start.Position.Local.Y && rig.Host.Document.Position.Local.Z == start.Position.Local.Z
                    && rig.Host.Document.Position.Sector == start.Position.Sector && BitsEqualExceptPosition(rig.Host.Document, start),
                "a world X drag leaves Y, Z, the sector, rotation and scale bit-identical");
            const double finalX = rig.Host.Document.Position.Local.X;

            // Returning the pointer to the grab point returns the entity: no drift.
            rig.Frame(grab, true);
            check.ExpectNear(rig.Host.Document.Position.Local.X, start.Position.Local.X, 3e-4, "moving back to the grab point restores the start (derived from the start, not accumulated)");
            rig.Frame(away, true);
            check.ExpectNear(rig.Host.Document.Position.Local.X, finalX, 3e-4, "and forward again returns to the same value");

            result = rig.Frame(away, false);
            check.Expect(rig.Host.Count(Kind::Begin) == 1 && rig.Host.Count(Kind::End) == 1 && rig.Host.Count(Kind::Cancel) == 0, "one drag is exactly one Begin and one End");
            check.Expect(rig.Host.Calls.back().What == Kind::End && rig.Host.Calls.back().Key.GestureId == 1, "End closes gesture 1");
            check.Expect(rig.Gizmo.Phase() != GizmoPhase::Active && result.Phase != GizmoPhase::Active, "released");
            check.ExpectNear(rig.Host.Document.Position.Local.X, finalX, 3e-4, "release keeps the last accepted transform");
            result = rig.Frame(grab, false);
            check.Expect(result.Readout.empty() && result.GestureId == 0, "no readout and no gesture id after release");
        }

        // The numeric readout, snapping and the Ctrl inversion.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Translate;
            rig.Target.Transform.Position.Local.X += 0.3;
            rig.Host.Document = rig.Target.Transform;
            const GizmoTransform start = rig.Host.Document;
            const ScreenPoint grab = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 0.55);
            const ScreenPoint to = Lerp(grab, rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 1.0), 1.3);

            // Snapping off by default: Ctrl enables it for that frame only.
            rig.Input.Snap.Enabled = false;
            rig.Frame(grab, false);
            rig.Frame(grab, true);
            GizmoFrameResult result = rig.Frame(to, true);
            const double plainX = rig.Host.Document.Position.Local.X;
            check.Expect(!result.SnapActive && plainX != std::round(plainX), "snap off: the position is not on the lattice");
            result = rig.Frame(to, true, true);
            check.Expect(result.SnapActive && rig.Host.Document.Position.Local.X == std::round(rig.Host.Document.Position.Local.X),
                "Ctrl with snap off snaps to the absolute lattice (step 1)");
            check.Expect(rig.Host.Document.Position.Local.Y == start.Position.Local.Y && rig.Host.Document.Position.Local.Z == start.Position.Local.Z,
                "snapping leaves the unconstrained axes bit-identical");
            result = rig.Frame(to, true, false);
            check.ExpectNear(rig.Host.Document.Position.Local.X, plainX, 1e-9, "releasing Ctrl mid-drag returns to the unsnapped position");
            check.Expect(result.Readout.find(" m") != std::string::npos && (result.Readout[0] == '+' || result.Readout[0] == '-'), "translation readout is signed metres: " + result.Readout);
            rig.Frame(to, false);

            // Snapping on: Ctrl suspends it.
            rig.Host.Calls.clear();
            rig.Host.Document = start;
            rig.Input.Snap.Enabled = true;
            rig.Input.Snap.TranslateStep = 0.5;
            rig.Frame(grab, false);
            rig.Frame(grab, true);
            result = rig.Frame(to, true);
            const double snapped = rig.Host.Document.Position.Local.X;
            check.Expect(result.SnapActive && std::abs(snapped / 0.5 - std::round(snapped / 0.5)) < 1e-12, "snap on, step 0.5: on the half-metre lattice");
            result = rig.Frame(to, true, true);
            check.Expect(!result.SnapActive && rig.Host.Document.Position.Local.X != snapped, "Ctrl with snap on suspends snapping");
            check.Expect(result.Readout.find("+") == 0 || result.Readout.find("-") == 0, "readout sign");
            rig.Frame(to, false);

            // A one metre pointer move along the arrow lands on the next lattice point, and the readout reports the snapped distance.
            rig.Host.Calls.clear();
            rig.Host.Document = start;
            rig.Input.Snap.TranslateStep = 1.0;
            rig.Frame(grab, false);
            rig.Frame(grab, true);
            const DVec3 pivot = rig.Relative(start.Position);
            const DVec3 axis = rig.AxisDirection(TransformTool::Translate, GizmoHandle::AxisX);
            const double t0 = OracleParameter(rig, pivot, axis, grab);
            const ScreenPoint onTarget = OraclePixel(rig.Scene, pivot + axis * (t0 + 1.0));
            result = rig.Frame(onTarget, true);
            const double rawX = start.Position.Local.X + axis.X * (OracleParameter(rig, pivot, axis, onTarget) - t0);
            check.ExpectNear(rig.Host.Document.Position.Local.X, std::floor(rawX + 0.5), 1e-9, "the one metre move snaps to the lattice point nearest the raw position");
            char text[32];
            std::snprintf(text, sizeof(text), "%+.2f m", (rig.Host.Document.Position.Local.X - start.Position.Local.X) * axis.X);
            check.Expect(result.Readout == text, std::string("readout reports the snapped distance: ") + result.Readout + " vs " + text);
            rig.Frame(onTarget, false);
        }

        // Local space moves along the rotated axis, and Center moves in the screen plane.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Translate;
            rig.Input.Space = TransformSpace::Local;
            rig.Target.Transform.RotationDegrees = { 0.0f, 40.0f, 0.0f };
            rig.Host.Document = rig.Target.Transform;
            const GizmoTransform start = rig.Host.Document;
            const DVec3 pivot = rig.Relative(start.Position);
            const DVec3 axis = rig.AxisDirection(TransformTool::Translate, GizmoHandle::AxisX);
            const Rotation3 rotation = EngineRotationRows(start.RotationDegrees);
            check.ExpectNear(std::abs(Dot(axis, Row(rotation, 0))), 1.0, 1e-6, "the local X arrow is the entity's own X axis");
            const ScreenPoint grab = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 0.55);
            const ScreenPoint to = Lerp(grab, rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 1.0), 1.5);
            rig.Frame(grab, false);
            rig.Frame(grab, true);
            rig.Frame(to, true);
            const double travelled = OracleParameter(rig, pivot, axis, to) - OracleParameter(rig, pivot, axis, grab);
            DVec3 moved;
            Engine::Math::TryGetSectorLocalRelativePosition(rig.Host.Document.Position, start.Position, rig.Policy, moved);
            check.ExpectNear(Length(moved - axis * travelled), 0.0, 5e-4, "the move is along the rotated local axis by the solved distance");
            check.Expect(BitsEqualExceptPosition(rig.Host.Document, start), "rotation and scale untouched");
            rig.Frame(to, false);
        }

        {
            Rig rig;
            rig.Input.Tool = TransformTool::Translate;
            const GizmoTransform start = rig.Host.Document;
            const DVec3 pivot = rig.Relative(start.Position);
            const double depth = OracleDepth(rig.Scene, pivot);
            const ScreenPoint grab = OraclePixel(rig.Scene, pivot);
            const ScreenPoint to { grab.X + 70.0, grab.Y - 45.0 };
            rig.Frame(grab, false);
            check.Expect(rig.Last.Hover == GizmoHandle::Center, "the origin dot is the centre handle");
            rig.Frame(grab, true);
            rig.Frame(to, true);
            DVec3 moved;
            Engine::Math::TryGetSectorLocalRelativePosition(rig.Host.Document.Position, start.Position, rig.Policy, moved);
            const DVec3 expected = OracleWorldFromPixel(rig.Scene, to, depth) - OracleWorldFromPixel(rig.Scene, grab, depth);
            check.ExpectNear(Length(moved - expected), 0.0, 5e-4, "the centre handle moves in the plane parallel to the screen through the pivot");
            check.ExpectNear(Dot(moved, rig.Scene.ViewAxis(2)), 0.0, 5e-4, "no change in depth");
            rig.Frame(to, false);
        }

        // Escape cancels exactly, latches until release, and a new press starts a new gesture.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Translate;
            const GizmoTransform start = rig.Host.Document;
            const ScreenPoint grab = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 0.55);
            const ScreenPoint to = Lerp(grab, rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 1.0), 1.5);
            rig.Frame(grab, false);
            rig.Frame(grab, true);
            rig.Frame(to, true);
            check.Expect(!SameTransformBits(rig.Host.Document, start), "moved before the cancel");
            GizmoFrameResult result = rig.Frame(to, true, false, true);
            check.Expect(result.Phase == GizmoPhase::Cancelled && !result.WantsPointer && result.Active == GizmoHandle::None, "Esc cancels while the button is still down");
            check.Expect(SameTransformBits(rig.Host.Document, start), "the start transform is restored bit-exactly");
            check.Expect(rig.Host.Calls[rig.Host.Calls.size() - 2].What == Kind::Apply && rig.Host.Calls.back().What == Kind::Cancel, "restore then Cancel");
            check.Expect(rig.Host.Count(Kind::End) == 0, "a cancelled gesture never ends");
            const size_t calls = rig.Host.Calls.size();
            result = rig.Frame(grab, true);
            check.Expect(result.Phase == GizmoPhase::Cancelled && rig.Host.Calls.size() == calls, "further movement with the button held does nothing");
            result = rig.Frame(grab, true);
            check.Expect(rig.Host.Calls.size() == calls && rig.Gizmo.DrawList().empty(), "still latched, nothing drawn or written");
            result = rig.Frame(grab, false);
            check.Expect(result.Phase == GizmoPhase::Hover, "release leaves the latch (the pointer is over the arrow again)");
            rig.Frame(grab, true);
            check.Expect(rig.Last.Phase == GizmoPhase::Active && rig.Last.GestureId == 2, "a new press starts gesture 2");
            rig.Frame(to, true);
            rig.Frame(to, false);
            check.Expect(rig.Host.Count(Kind::Begin) == 2 && rig.Host.Count(Kind::End) == 1 && rig.Host.Count(Kind::Cancel) == 1, "two begins, one end, one cancel");

            // Escape with no drag does nothing.
            rig.Host.Calls.clear();
            rig.Frame(grab, false, false, true);
            check.Expect(rig.Host.Calls.empty(), "Esc outside a drag is ignored by the gizmo");
        }

        // Every other way a drag can be lost cancels it and restores through the host.
        {
            struct Loss
            {
                const char* Name;
                void (*Apply)(Rig&);
            };
            const Loss losses[] = {
                { "focus lost", [](Rig& rig) { rig.Input.WindowFocused = false; } },
                { "target removed", [](Rig& rig) { rig.HasTarget = false; } },
                { "other entity selected", [](Rig& rig) { rig.Target.EntityId = 99; } },
                { "view removed", [](Rig& rig) { rig.HasView = false; } },
                { "camera moved", [](Rig& rig) { rig.Scene.View.View.Values[12] += 0.01f; } },
                { "viewport resized", [](Rig& rig) { rig.Scene.View.Viewport.Width += 1.0; } }
            };
            for (const Loss& loss : losses)
            {
                Rig rig;
                rig.Input.Tool = TransformTool::Translate;
                const GizmoTransform start = rig.Host.Document;
                const ScreenPoint grab = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 0.55);
                const ScreenPoint to = Lerp(grab, rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 1.0), 1.5);
                rig.Frame(grab, false);
                rig.Frame(grab, true);
                rig.Frame(to, true);
                const bool moved = !SameTransformBits(rig.Host.Document, start);
                // Apply the loss without the rig refreshing the target from the document.
                loss.Apply(rig);
                const GizmoTarget target = rig.Target;
                GizmoFrameInput input = rig.Input;
                input.View = rig.HasView ? &rig.Scene.View : nullptr;
                input.Target = rig.HasTarget ? &target : nullptr;
                input.Pointer = { to, true, true, false, false };
                const GizmoFrameResult result = rig.Gizmo.Update(input, rig.Host);
                const std::string name = loss.Name;
                check.Expect(moved, name + ": the entity had moved");
                check.Expect(result.Phase == GizmoPhase::Cancelled && rig.Host.Count(Kind::Cancel) == 1 && rig.Host.Count(Kind::End) == 0, name + ": cancelled");
                check.Expect(SameTransformBits(rig.Host.Document, start), name + ": the host was asked to restore the start transform");
            }
        }

        // Rejected writes keep the last accepted transform and the drag continues from the cursor, not from stale deltas.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Translate;
            const GizmoTransform start = rig.Host.Document;
            const ScreenPoint grab = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 0.55);
            const ScreenPoint far = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 1.0);
            rig.Frame(grab, false);
            rig.Frame(grab, true);
            rig.Frame(Lerp(grab, far, 0.5), true);
            const GizmoTransform accepted = rig.Host.Document;
            rig.Host.Reject = true;
            rig.Frame(Lerp(grab, far, 1.0), true);
            rig.Frame(Lerp(grab, far, 1.5), true);
            check.Expect(SameTransformBits(rig.Host.Document, accepted) && rig.Gizmo.RejectedUpdateCount() == 2, "rejected updates leave the document at the last accepted transform");
            check.Expect(rig.Last.Phase == GizmoPhase::Active, "and the drag continues");
            rig.Host.Reject = false;
            rig.Frame(Lerp(grab, far, 1.0), true);
            const double single = rig.Host.Document.Position.Local.X;
            rig.Frame(grab, true);
            rig.Frame(Lerp(grab, far, 1.0), true);
            check.ExpectNear(rig.Host.Document.Position.Local.X, single, 1e-9, "after rejections the transform depends only on the current cursor");
            rig.Host.Reject = true;
            rig.Frame(Lerp(grab, far, 2.0), true);
            rig.Host.Reject = false;
            rig.Frame(Lerp(grab, far, 2.0), true, false, true);
            check.Expect(SameTransformBits(rig.Host.Document, start) && rig.Host.Count(Kind::Cancel) == 1, "cancel after rejections still restores the start");
            check.Expect(rig.Host.Count(Kind::Apply) >= 1, "the restore went through the host");
        }

        // Rotation: Rodrigues oracle for the applied orientation, the accumulated angle through half a turn, snapping.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Rotate;
            const GizmoTransform start = rig.Host.Document;
            const DVec3 pivot = rig.Relative(start.Position);
            const DVec3 axis { 0.0, 0.0, 1.0 };
            const GizmoGeometry geometry = rig.Geometry(TransformTool::Rotate);
            const DVec3 toEye = rig.Scene.View.Eye - pivot;
            DVec3 arm0;
            Engine::Math::TryNormalize(toEye - axis * Dot(toEye, axis), arm0);
            const double length = geometry.Length;
            const auto at = [&](double theta) { return OraclePixel(rig.Scene, pivot + Rodrigues(arm0, axis, theta) * length); };
            rig.Frame(at(0.0), false);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisZ, "the Z ring is hovered at its camera-facing point");
            rig.Frame(at(0.0), true);
            check.Expect(rig.Last.Phase == GizmoPhase::Active && rig.Host.Calls[0].Key.Property == GizmoProperty::Rotation, "rotation gesture with the Rotation property");

            const Rotation3 before = EngineRotationRows(start.RotationDegrees);
            for (double degrees : { 25.0, 100.0, 170.0, 215.0, 250.0, 300.0 })
            {
                const double theta = degrees * kPi / 180.0;
                const GizmoFrameResult result = rig.Frame(at(theta), true);
                const Rotation3 after = EngineRotationRows(rig.Host.Document.RotationDegrees);
                for (u32 row = 0; row < 3; ++row)
                {
                    const DVec3 expected = Rodrigues(Row(before, row), axis, theta);
                    check.ExpectNear(Length(Row(after, row) - expected), 0.0, 5e-5, "local axis " + std::to_string(row) + " at " + std::to_string(degrees) + " degrees follows the Rodrigues model");
                }

                char expectedText[32];
                std::snprintf(expectedText, sizeof(expectedText), "%+.1f", degrees);
                check.Expect(result.Readout == std::string(expectedText) + kDegree, "readout accumulates through +-180: got " + result.Readout + " for " + std::to_string(degrees));
            }

            check.Expect(rig.Host.Document.Position.Local.X == start.Position.Local.X && SameBits(rig.Host.Document.Scale, start.Scale), "rotation leaves position and scale alone");
            for (double degrees : { 250.0, 215.0, 170.0, 100.0, 25.0 })
                rig.Frame(at(degrees * kPi / 180.0), true);
            rig.Frame(at(0.0), true);
            check.ExpectNear(rig.Host.Document.RotationDegrees.X, start.RotationDegrees.X, 1e-6, "unwinding back to the grab angle restores the start pitch");
            check.ExpectNear(rig.Host.Document.RotationDegrees.Y, start.RotationDegrees.Y, 1e-6, "unwinding back to the grab angle restores the start yaw");
            check.ExpectNear(rig.Host.Document.RotationDegrees.Z, start.RotationDegrees.Z, 1e-6, "unwinding back to the grab angle restores the start roll");
            rig.Frame(at(0.0), false);

            // Snapping to 15 degrees: 22.4 -> 15, Ctrl on inverts.
            rig.Host.Calls.clear();
            rig.Host.Document = start;
            rig.Input.Snap.Enabled = true;
            rig.Frame(at(0.0), false);
            rig.Frame(at(0.0), true);
            GizmoFrameResult result = rig.Frame(at(22.4 * kPi / 180.0), true);
            check.Expect(result.SnapActive && result.Readout == std::string("+15.0") + kDegree, "22.4 degrees snaps to +15.0: " + result.Readout);
            result = rig.Frame(at(-22.6 * kPi / 180.0), true);
            check.Expect(result.Readout == std::string("-30.0") + kDegree, "-22.6 degrees snaps to -30.0: " + result.Readout);
            result = rig.Frame(at(22.4 * kPi / 180.0), true, true);
            check.Expect(!result.SnapActive && result.Readout == std::string("+22.4") + kDegree, "Ctrl suspends rotation snapping: " + result.Readout);
            rig.Frame(at(0.0), false);
        }

        // A ring seen edge-on falls back to a screen-space drag.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Rotate;
            rig.Scene = TestScene();
            BuildScene({ 0.0, 0.0, -10.0 }, { 0.0f, 0.0f, 0.0f }, 60.0f, 4.0f / 3.0f, 0.1f, 500.0f, { 0, 0, 800, 600 }, rig.Scene);
            rig.Target.Transform.Position = { { 0, 0, 0 }, { 0.0, 0.0, 0.0 } };
            rig.Host.Document = rig.Target.Transform;
            const GizmoGeometry geometry = rig.Geometry(TransformTool::Rotate);
            const HandleShape* ring = FindShape(geometry, GizmoHandle::AxisY);
            check.Expect(ring && ring->Enabled, "the Y ring exists in an edge-on view");
            // The Y ring lies in the XZ plane seen exactly edge-on: a horizontal line through the origin.
            const ScreenPoint onLine { geometry.OriginScreen.X + 60.0, geometry.OriginScreen.Y };
            rig.Frame(onLine, false);
            if (rig.Last.Hover == GizmoHandle::AxisY)
            {
                rig.Frame(onLine, true);
                check.Expect(rig.Last.Phase == GizmoPhase::Active, "an edge-on ring can still be grabbed");
                rig.Frame({ onLine.X + 24.0, onLine.Y + 3.0 }, true);
                check.Expect(rig.Host.Count(Kind::Apply) >= 1 && !SameBits(rig.Host.Document.RotationDegrees, rig.Target.Transform.RotationDegrees),
                    "the screen-space fallback rotates the entity");
                rig.Frame({ onLine.X + 24.0, onLine.Y + 3.0 }, false);
                check.Expect(rig.Host.Count(Kind::End) == 1, "and ends normally");
            }
            else
            {
                check.Expect(rig.Last.Hover != GizmoHandle::None, "the edge-on view hovers some ring at 60 px along the line");
            }
        }

        // Scale: the axis ratio from the solver, the uniform centre handle, and the minimum.
        {
            Rig rig;
            rig.Input.Tool = TransformTool::Scale;
            const GizmoTransform start = rig.Host.Document;
            const DVec3 pivot = rig.Relative(start.Position);
            const GizmoGeometry geometry = rig.Geometry(TransformTool::Scale);
            const DVec3 axis = geometry.Axes[1];
            const ScreenPoint grab = rig.OnHandle(TransformTool::Scale, GizmoHandle::AxisY, 0.8);
            const ScreenPoint to = Lerp(rig.OnHandle(TransformTool::Scale, GizmoHandle::AxisY, 0.0), grab, 1.5);
            rig.Frame(grab, false);
            check.Expect(rig.Last.Hover == GizmoHandle::AxisY, "the Y scale box is hovered");
            rig.Frame(grab, true);
            check.Expect(rig.Host.Calls[0].Key.Property == GizmoProperty::Scale, "Scale property");
            const GizmoFrameResult result = rig.Frame(to, true);
            const double tg = OracleParameter(rig, pivot, axis, grab);
            const double tc = OracleParameter(rig, pivot, axis, to);
            const double reference = std::max(std::abs(tg), 0.25 * geometry.Length);
            const double factor = std::max(kMinimumScaleFactor, 1.0 + (tc - tg) / (tg < 0.0 ? -reference : reference));
            check.ExpectNear(rig.Host.Document.Scale.Y, start.Scale.Y * factor, 2e-3 * factor, "Y scale follows the ratio of pointer distances");
            check.Expect(rig.Host.Document.Scale.X == start.Scale.X && rig.Host.Document.Scale.Z == start.Scale.Z, "other scale components untouched");
            check.Expect(result.Readout.size() > 1 && result.Readout[0] == 'x', "scale readout starts with x: " + result.Readout);
            rig.Frame(to, false);

            // Uniform: 48 px right is x1.50.
            rig.Host.Document = start;
            rig.Host.Calls.clear();
            const ScreenPoint origin = OraclePixel(rig.Scene, pivot);
            rig.Frame(origin, false);
            check.Expect(rig.Last.Hover == GizmoHandle::Center, "the uniform scale handle");
            rig.Frame(origin, true);
            const GizmoFrameResult uniform = rig.Frame({ origin.X + 48.0, origin.Y }, true);
            check.ExpectNear(rig.Host.Document.Scale.X, 1.5, 1e-5, "uniform x");
            check.ExpectNear(rig.Host.Document.Scale.Y, 3.0, 1e-5, "uniform y");
            check.ExpectNear(rig.Host.Document.Scale.Z, 4.5, 1e-5, "uniform z");
            check.Expect(uniform.Readout == "x1.50", "uniform readout: " + uniform.Readout);
            rig.Frame({ origin.X - 400.0, origin.Y }, true);
            check.Expect(rig.Host.Document.Scale.X == kMinimumScale || rig.Host.Document.Scale.X < 0.2f, "dragging far left shrinks toward the minimum and never goes negative");
            check.Expect(rig.Host.Document.Scale.X >= kMinimumScale && rig.Host.Document.Scale.Y >= kMinimumScale && rig.Host.Document.Scale.Z >= kMinimumScale, "scale stays within the Inspector range");
            rig.Frame({ origin.X - 400.0, origin.Y }, false);

            // Snap: 1.26 -> 1.30.
            rig.Host.Document = start;
            rig.Input.Snap.Enabled = true;
            rig.Frame(origin, false);
            rig.Frame(origin, true);
            const GizmoFrameResult snapped = rig.Frame({ origin.X + 25.0, origin.Y }, true);
            check.Expect(snapped.SnapActive && snapped.Readout == "x1.30", "1.26 snaps to x1.30: " + snapped.Readout);
            rig.Frame({ origin.X + 25.0, origin.Y }, false);
        }

        // Sector-local authority: far from the origin, across a sector edge, the delta lands exactly.
        {
            Rig rig(SectorLocalPosition { { 1000000000, -1000000000, 500000000 }, { 2040.0, 5.0, -2040.0 } });
            rig.Input.Tool = TransformTool::Translate;
            check.Expect(rig.Target.Transform.Position.Sector.X >= 1000000000 && rig.Target.Transform.Position.Sector.Y <= -999999999,
                "the entity is in the far sector range");
            const GizmoTransform start = rig.Host.Document;
            const DVec3 pivot = rig.Relative(start.Position);
            check.Expect(Length(pivot) < 30.0 && Length(pivot - (rig.Scene.Eye + rig.Scene.ViewAxis(2) * rig.Distance)) < 1e-9, "the relative position is the exact small difference");
            const DVec3 axis = rig.AxisDirection(TransformTool::Translate, GizmoHandle::AxisX);
            const ScreenPoint grab = rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 0.55);
            const ScreenPoint to = Lerp(grab, rig.OnHandle(TransformTool::Translate, GizmoHandle::AxisX, 1.0), 4.0);
            rig.Frame(grab, false);
            rig.Frame(grab, true);
            rig.Frame(to, true);
            const double travelled = OracleParameter(rig, pivot, axis, to) - OracleParameter(rig, pivot, axis, grab);
            DVec3 moved;
            check.Expect(Engine::Math::TryGetSectorLocalRelativePosition(rig.Host.Document.Position, start.Position, rig.Policy, moved)
                    && Length(moved - axis * travelled) < 5e-4 * (1.0 + std::abs(travelled)),
                "a far-sector drag moves by the solved distance with no precision loss");
            check.Expect(Engine::Math::IsCanonical(rig.Host.Document.Position, rig.Policy), "the written position is canonical");
            rig.Frame(to, false);
        }

        return check.Ok;
    }

    namespace
    {
        GizmoPalette DistinctPalette()
        {
            GizmoPalette palette;
            palette.AxisX = { 1, 2, 3, 255 };
            palette.AxisY = { 4, 5, 6, 255 };
            palette.AxisZ = { 7, 8, 9, 255 };
            palette.Center = { 10, 11, 12, 255 };
            palette.Outline = { 13, 14, 15, 200 };
            palette.Hover = { 16, 17, 18, 255 };
            palette.Active = { 19, 20, 21, 255 };
            palette.Label = { 22, 23, 24, 255 };
            return palette;
        }

        // Handle strokes in order of appearance, skipping the outline pass.
        std::vector<const GizmoDrawPrimitive*> Strokes(const std::vector<GizmoDrawPrimitive>& list, const GizmoPalette& palette)
        {
            std::vector<const GizmoDrawPrimitive*> result;
            for (const GizmoDrawPrimitive& primitive : list)
            {
                if (primitive.Kind == GizmoPrimitiveKind::Line && !(primitive.Color == palette.Outline))
                    result.push_back(&primitive);
            }

            return result;
        }
    }

    bool TestGizmoDrawListContracts()
    {
        Checker check { "drawlist" };
        const GizmoStyle style = DefaultStyle();
        const GizmoPalette palette = DistinctPalette();
        TestScene& hand = HandScene();
        const DVec3 origin { 0, 0, 10 };
        GizmoGeometry translate;
        BuildGizmoGeometry(hand.View, TransformTool::Translate, origin, kWorldBasis, true, style, translate);

        std::vector<GizmoDrawPrimitive> list;
        AppendGizmoDrawList(translate, GizmoHandle::None, GizmoHandle::None, palette, list);
        check.Expect(!list.empty(), "a visible gizmo produces primitives");

        // Palette colours reach the axes, planes and the centre; hidden handles draw nothing and no letter.
        const auto strokes = Strokes(list, palette);
        check.Expect(strokes.size() == 2 && strokes[0]->Color == palette.AxisX && strokes[1]->Color == palette.AxisY,
            "only the visible X and Y arrows are stroked, in the injected axis colours");
        bool hasZLetter = false;
        bool hasXLetter = false;
        bool hasYLetter = false;
        for (const GizmoDrawPrimitive& primitive : list)
        {
            if (primitive.Kind != GizmoPrimitiveKind::Text)
                continue;
            hasXLetter |= primitive.Text == "X" && primitive.Color == palette.Label;
            hasYLetter |= primitive.Text == "Y";
            hasZLetter |= primitive.Text == "Z";
        }

        check.Expect(hasXLetter && hasYLetter && !hasZLetter, "axis letters mark X and Y, not the hidden Z");
        bool planeFill = false;
        bool centreRing = false;
        for (const GizmoDrawPrimitive& primitive : list)
        {
            planeFill |= primitive.Kind == GizmoPrimitiveKind::Polygon && primitive.Filled && primitive.Points.size() == 4
                && primitive.Color.R == palette.AxisZ.R && primitive.Color.A < 255;
            centreRing |= primitive.Kind == GizmoPrimitiveKind::Circle && primitive.Color == palette.Center && !primitive.Filled;
        }

        check.Expect(planeFill, "the XY square is a translucent fill in the Z axis colour");
        check.Expect(centreRing, "the idle centre is an unfilled ring in the centre colour");

        // Every outline stroke is immediately followed by its handle stroke, two pixels thinner.
        for (size_t index = 0; index + 1 < list.size(); ++index)
        {
            if (list[index].Kind == GizmoPrimitiveKind::Line && list[index].Color == palette.Outline)
            {
                check.Expect(list[index + 1].Kind == GizmoPrimitiveKind::Line && list[index + 1].Points.size() == list[index].Points.size()
                        && list[index + 1].Thickness + 2.0f == list[index].Thickness && !(list[index + 1].Color == palette.Outline),
                    "an outline precedes the stroke it backs");
            }
        }

        // Hover and active use the Selection tokens, thicker, and active wins over hover.
        const auto thicknessOf = [&](GizmoHandle hover, GizmoHandle active, const GizmoColor& colour)
        {
            std::vector<GizmoDrawPrimitive> out;
            AppendGizmoDrawList(translate, hover, active, palette, out);
            for (const GizmoDrawPrimitive* stroke : Strokes(out, palette))
            {
                if (stroke->Color == colour)
                    return stroke->Thickness;
            }

            return 0.0f;
        };
        check.Expect(thicknessOf(GizmoHandle::AxisX, GizmoHandle::None, palette.Hover) == 3.0f, "hovered arrow: hover colour, 3 px");
        check.Expect(thicknessOf(GizmoHandle::None, GizmoHandle::AxisX, palette.Active) == 4.0f, "active arrow: active colour, 4 px");
        check.Expect(thicknessOf(GizmoHandle::AxisY, GizmoHandle::AxisX, palette.Hover) == 0.0f, "while a handle is active no other handle shows hover");
        check.Expect(thicknessOf(GizmoHandle::None, GizmoHandle::None, palette.Active) == 0.0f && thicknessOf(GizmoHandle::None, GizmoHandle::None, palette.Hover) == 0.0f,
            "idle uses neither state colour");
        std::vector<GizmoDrawPrimitive> centreHover;
        AppendGizmoDrawList(translate, GizmoHandle::Center, GizmoHandle::None, palette, centreHover);
        bool filledCircle = false;
        for (const GizmoDrawPrimitive& primitive : centreHover)
            filledCircle |= primitive.Kind == GizmoPrimitiveKind::Circle && primitive.Filled && primitive.Color == palette.Hover;
        check.Expect(filledCircle, "a hovered centre fills with the hover colour");

        // Appending preserves what is already in the list; invalid and empty geometry add nothing.
        const size_t before = list.size();
        AppendGizmoDrawList(translate, GizmoHandle::None, GizmoHandle::None, palette, list);
        check.Expect(list.size() == 2 * before, "AppendGizmoDrawList appends");
        std::vector<GizmoDrawPrimitive> none;
        AppendGizmoDrawList(GizmoGeometry {}, GizmoHandle::None, GizmoHandle::None, palette, none);
        GizmoGeometry select;
        BuildGizmoGeometry(hand.View, TransformTool::Select, origin, kWorldBasis, true, style, select);
        AppendGizmoDrawList(select, GizmoHandle::None, GizmoHandle::None, palette, none);
        check.Expect(none.empty(), "invalid geometry and the Select tool draw nothing");

        // Rings: a faint full circle behind, the camera-facing arcs on top, one letter per ring.
        GizmoGeometry rotate;
        BuildGizmoGeometry(hand.View, TransformTool::Rotate, origin, kWorldBasis, true, style, rotate);
        std::vector<GizmoDrawPrimitive> rings;
        AppendGizmoDrawList(rotate, GizmoHandle::None, GizmoHandle::None, palette, rings);
        size_t letters = 0;
        size_t faint = 0;
        for (const GizmoDrawPrimitive& primitive : rings)
        {
            letters += primitive.Kind == GizmoPrimitiveKind::Text ? 1 : 0;
            faint += primitive.Kind == GizmoPrimitiveKind::Polyline && primitive.Closed && primitive.Color.A == 60 ? 1 : 0;
        }

        check.Expect(letters == 3 && faint == 3, "three ring letters and three faint back circles");
        std::vector<GizmoDrawPrimitive> activeRing;
        AppendGizmoDrawList(rotate, GizmoHandle::None, GizmoHandle::AxisZ, palette, activeRing);
        size_t activeStrokes = 0;
        for (const GizmoDrawPrimitive* stroke : Strokes(activeRing, palette))
            activeStrokes += stroke->Color == palette.Active && stroke->Thickness == 4.0f ? 1 : 0;
        check.Expect(activeStrokes > 20, "the active ring's facing arcs are drawn in the active colour");

        // Defaults satisfy the product rule: axis colours exist only on handles; hover and active are the Selection tokens.
        const GizmoPalette defaults;
        check.Expect(defaults.Hover == GizmoColor { 61, 97, 128, 255 } && defaults.Active == GizmoColor { 69, 133, 179, 255 },
            "default hover and active are DESIGN.md's Selection hover and Docking preview colours");
        return check.Ok;
    }

    namespace
    {
        // A cursor position that hovers a handle of the tool in the rig's current view.
        ScreenPoint CursorOnHandle(const Rig& rig, TransformTool tool)
        {
            switch (tool)
            {
            case TransformTool::Translate:
                return rig.OnHandle(tool, GizmoHandle::AxisX, 0.55);
            case TransformTool::Scale:
                return rig.OnHandle(tool, GizmoHandle::AxisY, 0.8);
            case TransformTool::Rotate:
            {
                const DVec3 pivot = rig.Relative(rig.Host.Document.Position);
                const DVec3 axis { 0.0, 0.0, 1.0 };
                const DVec3 toEye = rig.Scene.View.Eye - pivot;
                DVec3 arm;
                Engine::Math::TryNormalize(toEye - axis * Dot(toEye, axis), arm);
                return OraclePixel(rig.Scene, pivot + arm * rig.Geometry(tool).Length);
            }
            case TransformTool::Select:
                break;
            }

            return { 700.0, 60.0 };
        }

        const char* PhaseName(GizmoPhase phase)
        {
            switch (phase)
            {
            case GizmoPhase::Idle: return "Idle";
            case GizmoPhase::Hover: return "Hover";
            case GizmoPhase::Active: return "Active";
            case GizmoPhase::Cancelled: return "Cancelled";
            }
            return "?";
        }

        // Independent reference of the phase rules; see TransformGizmo.h.
        struct PhaseModel
        {
            bool Open = false;
            bool Latched = false;
            bool PreviousDown = false;
            u64 NextId = 1;
            u64 OpenId = 0;
            u64 OpenEntity = 0;
            GizmoView OpenView;
        };
    }

    bool TestGizmoStateMachineModelProperty()
    {
        Checker check { "model" };
        size_t gestures = 0;
        size_t cancels = 0;
        size_t ends = 0;
        const bool ok = RunProperty("TestGizmoStateMachineModelProperty",
            [&](ChoiceStream& stream, std::string& message)
            {
                using Kind = RecordingHost::Kind;
                Rig rig;
                PhaseModel model;
                bool down = false;
                TransformTool tool = TransformTool::Translate;
                bool allowScale = true;
                GizmoTransform openStart;
                GizmoTransform lastAccepted;
                u64 expectedEntity = 42;
                size_t rejectedSeen = 0;

                for (int frame = 0; frame < 80; ++frame)
                {
                    if (stream.NextSize(0, 99) < 6)
                        tool = static_cast<TransformTool>(stream.NextSize(0, 3));
                    allowScale = stream.NextSize(0, 99) >= 8;
                    const bool focused = stream.NextSize(0, 99) >= 4;
                    const bool over = stream.NextSize(0, 99) >= 8;
                    const bool targetPresent = stream.NextSize(0, 99) >= 3;
                    const bool otherEntity = stream.NextSize(0, 99) < 3;
                    const bool viewPresent = stream.NextSize(0, 99) >= 2;
                    if (stream.NextSize(0, 99) < 3)
                        rig.Scene.View.View.Values[12] += 0.002f;
                    const bool escape = stream.NextSize(0, 99) < 6;
                    const bool ctrl = stream.NextSize(0, 99) < 10;
                    down = down ? stream.NextSize(0, 99) >= 22 : stream.NextSize(0, 99) < 38;

                    ScreenPoint cursor = CursorOnHandle(rig, tool);
                    const size_t mode = stream.NextSize(0, 5);
                    if (mode == 0)
                        cursor = { 700.0, 60.0 };
                    else if (mode >= 4)
                        cursor = { cursor.X + RangeDouble(stream, -12.0, 12.0), cursor.Y + RangeDouble(stream, -12.0, 12.0) };

                    GizmoTarget target = rig.Target;
                    target.Transform = rig.Host.Document;
                    target.RelativePosition = rig.Relative(rig.Host.Document.Position);
                    target.EntityId = otherEntity ? 99 : 42;
                    target.AllowScale = allowScale;
                    GizmoFrameInput input = rig.Input;
                    input.Tool = tool;
                    input.View = viewPresent ? &rig.Scene.View : nullptr;
                    input.Target = targetPresent ? &target : nullptr;
                    input.Policy = rig.Policy;
                    input.WindowFocused = focused;
                    input.Pointer = { cursor, over, down, ctrl, escape };

                    // The reference decides this frame's outcome before the gizmo runs.
                    const bool pressed = down && !model.PreviousDown;
                    model.PreviousDown = down;
                    bool expectCancel = false;
                    bool expectEnd = false;
                    GizmoPhase expectedPhase = GizmoPhase::Idle;
                    bool expectBegin = false;
                    bool fallThrough = true;
                    if (model.Open)
                    {
                        const bool sameView = viewPresent && SameView(rig.Scene.View, model.OpenView);
                        expectCancel = !focused || escape || !targetPresent || target.EntityId != model.OpenEntity || !sameView;
                        if (expectCancel)
                        {
                            model.Open = false;
                            model.Latched = down;
                            if (down)
                            {
                                expectedPhase = GizmoPhase::Cancelled;
                                fallThrough = false;
                            }
                        }
                        else if (!down)
                        {
                            expectEnd = true;
                            model.Open = false;
                        }
                        else
                        {
                            expectedPhase = GizmoPhase::Active;
                            fallThrough = false;
                        }
                    }
                    else if (model.Latched)
                    {
                        if (down)
                        {
                            expectedPhase = GizmoPhase::Cancelled;
                            fallThrough = false;
                        }
                        else
                        {
                            model.Latched = false;
                        }
                    }

                    GizmoHandle expectedHover = GizmoHandle::None;
                    if (fallThrough)
                    {
                        const bool visible = viewPresent && targetPresent && tool != TransformTool::Select && !(tool == TransformTool::Scale && !allowScale);
                        if (visible && over && focused)
                        {
                            DVec3 basis[3];
                            BuildToolBasis(tool, input.Space, target.Transform.RotationDegrees, basis);
                            GizmoGeometry geometry;
                            if (BuildGizmoGeometry(rig.Scene.View, tool, target.RelativePosition, basis, true, input.Style, geometry))
                                expectedHover = HitTestHandles(geometry, cursor, input.Style);
                        }

                        if (pressed && expectedHover != GizmoHandle::None)
                        {
                            expectBegin = true;
                            expectedPhase = GizmoPhase::Active;
                        }
                        else if (expectedHover != GizmoHandle::None)
                        {
                            expectedPhase = GizmoPhase::Hover;
                        }
                    }

                    rig.Host.Reject = !expectCancel && stream.NextSize(0, 99) < 12;
                    const size_t firstCall = rig.Host.Calls.size();
                    const GizmoTransform documentBefore = rig.Host.Document;
                    const GizmoFrameResult result = rig.Gizmo.Update(input, rig.Host);
                    const std::vector<RecordingHost::Call> calls(rig.Host.Calls.begin() + static_cast<std::ptrdiff_t>(firstCall), rig.Host.Calls.end());

                    const auto fail = [&](const std::string& text)
                    {
                        message = "frame " + std::to_string(frame) + ": " + text + " (phase " + PhaseName(result.Phase)
                            + ", expected " + PhaseName(expectedPhase) + ", calls " + std::to_string(calls.size()) + ")";
                        return false;
                    };

                    if (result.Phase != expectedPhase)
                        return fail("phase mismatch");

                    // Call grammar for this frame: Begin alone, or at most one write
                    // followed by End or Cancel, or just a write.
                    bool sawBegin = false;
                    bool sawEnd = false;
                    bool sawCancel = false;
                    size_t applies = 0;
                    for (size_t index = 0; index < calls.size(); ++index)
                    {
                        const RecordingHost::Call& call = calls[index];
                        switch (call.What)
                        {
                        case Kind::Begin:
                            if (sawBegin || applies != 0 || sawEnd || sawCancel || index != 0)
                                return fail("Begin out of order");
                            sawBegin = true;
                            break;
                        case Kind::Apply:
                            if (sawEnd || sawCancel)
                                return fail("Apply after the gesture closed");
                            ++applies;
                            break;
                        case Kind::End:
                            if (sawEnd || sawCancel || sawBegin)
                                return fail("End out of order");
                            sawEnd = true;
                            break;
                        case Kind::Cancel:
                            if (sawEnd || sawCancel || sawBegin)
                                return fail("Cancel out of order");
                            sawCancel = true;
                            break;
                        }
                    }

                    if (sawBegin != expectBegin)
                        return fail(expectBegin ? "the reference expects a Begin" : "unexpected Begin");
                    if (sawEnd != expectEnd)
                        return fail(expectEnd ? "the reference expects an End" : "unexpected End");
                    if (sawCancel != expectCancel)
                        return fail(expectCancel ? "the reference expects a Cancel" : "unexpected Cancel");
                    if (sawBegin && applies != 0)
                        return fail("a press writes nothing");
                    if (applies > 1)
                        return fail("more than one write in one frame");
                    if (!sawBegin && !sawEnd && !sawCancel && !model.Open && applies != 0)
                        return fail("a write with no open gesture");
                    if (applies != 0 && calls.size() > 1 && sawCancel && calls[calls.size() - 2].What != Kind::Apply)
                        return fail("the restore must directly precede Cancel");

                    if (sawBegin)
                    {
                        const RecordingHost::Call& begin = calls.front();
                        if (begin.Key.GestureId != model.NextId || begin.Key.EntityId != target.EntityId)
                            return fail("gesture ids must count up from 1 and name the target entity");
                        const GizmoProperty property = tool == TransformTool::Rotate ? GizmoProperty::Rotation
                            : tool == TransformTool::Scale ? GizmoProperty::Scale : GizmoProperty::Position;
                        if (begin.Key.Property != property)
                            return fail("the gesture property must follow the tool");
                        model.Open = true;
                        model.OpenId = model.NextId++;
                        model.OpenEntity = target.EntityId;
                        model.OpenView = rig.Scene.View;
                        openStart = documentBefore;
                        lastAccepted = documentBefore;
                        expectedEntity = target.EntityId;
                        ++gestures;
                    }
                    else if (!calls.empty())
                    {
                        for (const RecordingHost::Call& call : calls)
                        {
                            if (call.Key.GestureId != model.OpenId || call.Key.EntityId != expectedEntity)
                                return fail("a call used another gesture key");
                        }
                    }

                    for (const RecordingHost::Call& call : calls)
                    {
                        if (call.What == Kind::Apply && !sawCancel)
                        {
                            if (call.Accepted)
                                lastAccepted = call.Transform;
                            else
                                ++rejectedSeen;
                        }
                    }

                    if (sawEnd)
                    {
                        ++ends;
                        if (!SameTransformBits(rig.Host.Document, lastAccepted))
                            return fail("End must leave the last accepted transform");
                    }

                    if (sawCancel)
                    {
                        ++cancels;
                        if (!SameTransformBits(rig.Host.Document, openStart))
                            return fail("Cancel must restore the start transform bit-exactly");
                    }

                    // Result fields follow the phase.
                    const bool active = result.Phase == GizmoPhase::Active;
                    const bool hovering = result.Phase == GizmoPhase::Hover;
                    if (result.WantsPointer != (active || hovering))
                        return fail("WantsPointer must be true exactly while hovering or dragging");
                    if ((result.Active != GizmoHandle::None) != active || (result.Hover != GizmoHandle::None) != hovering)
                        return fail("Hover/Active handle fields must follow the phase");
                    if (active && result.GestureId != model.OpenId)
                        return fail("an active frame reports the open gesture id");
                    if (!active && result.GestureId != 0)
                        return fail("only active frames report a gesture id");
                    if (hovering && result.Hover != expectedHover)
                        return fail("hovered handle differs from the reference");
                    if (result.SnapActive != (rig.Input.Snap.Enabled != ctrl) && (active || hovering || result.Phase == GizmoPhase::Idle) && fallThrough)
                        return fail("SnapActive must be the toggle xor Ctrl");
                    if (tool == TransformTool::Select && !model.Open && !rig.Gizmo.DrawList().empty())
                        return fail("the Select tool draws nothing");
                    if (rig.Gizmo.RejectedUpdateCount() != rejectedSeen)
                        return fail("RejectedUpdateCount must equal the rejected writes");

                }

                return true;
            });
        check.Expect(ok, "generated pointer-trace property");
        check.Expect(gestures > 300 && cancels > 50 && ends > 50,
            "the generator exercised gestures, ends and cancels: " + std::to_string(gestures) + " / " + std::to_string(ends) + " / " + std::to_string(cancels));
        return check.Ok;
    }
}
