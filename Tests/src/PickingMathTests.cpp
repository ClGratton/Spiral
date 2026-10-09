#include "PickingMathTests.h"

#include "PickingMath.h"
#include "TestSupport/GeneratedTest.h"

#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"
#include "Engine/Scene/Camera.h"
#include "Engine/Scene/Components.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace SpiralEditor::Picking;
    using Engine::u32;
    using Spiral::Tests::ChoiceStream;

    // Failure hypotheses, oracles, and non-claims for the whole file:
    // - The pick ray must be the exact inverse of what the renderer draws. The
    //   renderer oracle is Engine::BuildCameraView (float matrices, row vectors):
    //   projecting a point through its ViewProjection and then casting a ray
    //   through the resulting pixel must recover the point's direction. A
    //   yaw/pitch/roll order mistake (the inverse of a product is the reversed
    //   product) only shows when yaw and pitch are both nonzero, so the
    //   generator draws both.
    // - Oriented-box tests have two independent oracles: hand-computed cases
    //   with a rotated and non-uniformly scaled box, and a world-space
    //   brute-force test (transform the eight corners, test twelve triangles
    //   with a plane/side-of-edge test) that shares no slab or inverse code.
    // - Triangle refinement is checked against the same plane/side-of-edge test.
    // - Properties are deterministic and replayable through SPIRAL_PICKING_SEED /
    //   SPIRAL_PICKING_REPLAY; a failure prints the seed, the minimized trace
    //   and writes a counterexample JSON under the system temp directory.
    // - Tier: fast and in-process, except the large-mesh latency test.
    // - Not claimed: ImGui hover/focus, real GPU output, click timing, or the
    //   latency of a Release build (the reported number is the Debug test binary).

    struct Checker
    {
        const char* Suite;
        bool Ok = true;

        void Expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                std::cerr << "Picking test failed [" << Suite << "]: " << message << '\n';
                Ok = false;
            }
        }

        void Near(double actual, double expected, double tolerance, const std::string& message)
        {
            if (!(std::abs(actual - expected) <= tolerance))
            {
                std::cerr << "Picking test failed [" << Suite << "]: " << message << " (actual "
                          << actual << ", expected " << expected << ", tolerance " << tolerance << ")\n";
                Ok = false;
            }
        }

        void Near(const DVec3& actual, const DVec3& expected, double tolerance, const std::string& message)
        {
            if (!(std::abs(actual.X - expected.X) <= tolerance && std::abs(actual.Y - expected.Y) <= tolerance
                    && std::abs(actual.Z - expected.Z) <= tolerance))
            {
                std::cerr << "Picking test failed [" << Suite << "]: " << message << " (actual "
                          << actual.X << ',' << actual.Y << ',' << actual.Z << ", expected "
                          << expected.X << ',' << expected.Y << ',' << expected.Z << ")\n";
                Ok = false;
            }
        }
    };

    bool RunProperty(std::string_view name, const Spiral::Tests::Property& property, size_t iterations)
    {
        Spiral::Tests::CampaignOptions options;
        options.Iterations = iterations;
        if (const char* seed = std::getenv("SPIRAL_PICKING_SEED"))
            options.Seed = std::strtoull(seed, nullptr, 10);
        Spiral::Tests::ChoiceTrace replay;
        if (const char* trace = std::getenv("SPIRAL_PICKING_REPLAY");
            trace && Spiral::Tests::ParseTrace(trace, replay))
            options.Replay = replay;

        Spiral::Tests::Counterexample failure;
        if (Spiral::Tests::RunCampaign(options, property, failure))
            return true;

        const std::string minimized = Spiral::Tests::SerializeTrace(failure.MinimizedTrace);
        const std::string rerun = "SPIRAL_PICKING_SEED=" + std::to_string(failure.Seed)
            + " SPIRAL_PICKING_REPLAY=\"" + minimized + "\" EngineTests --test <registered name of "
            + std::string(name) + ">";
        const std::filesystem::path artifact = std::filesystem::temp_directory_path()
            / ("spiral-picking-counterexample-" + std::string(name) + ".json");
        std::string artifactError;
        const bool written = Spiral::Tests::WriteCounterexample(artifact, name, failure, rerun, artifactError);
        std::cerr << "Picking property failed [" << name << "]: " << failure.Message << "\n  seed=" << failure.Seed
                  << " iteration=" << failure.Iteration << "\n  original trace (" << failure.OriginalTrace.size()
                  << " choices) minimized to " << failure.MinimizedTrace.size() << "\n  rerun: " << rerun << '\n';
        if (written)
            std::cerr << "  counterexample: " << artifact.string() << '\n';
        else
            std::cerr << "  counterexample not written: " << artifactError << '\n';
        return false;
    }

    double Unit(ChoiceStream& stream)
    {
        return static_cast<double>(stream.Next() >> 11) / 9007199254740992.0;
    }

    double Range(ChoiceStream& stream, double low, double high)
    {
        return low + (high - low) * Unit(stream);
    }

    float FloatRange(ChoiceStream& stream, double low, double high)
    {
        return static_cast<float>(Range(stream, low, high));
    }

    DVec3 RandomUnitVector(ChoiceStream& stream)
    {
        // Rejection sampling with a bounded number of attempts: a replayed (minimized) trace
        // can run out of choices, and an exhausted stream yields a constant that would
        // otherwise be rejected forever.
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const DVec3 value { Range(stream, -1.0, 1.0), Range(stream, -1.0, 1.0), Range(stream, -1.0, 1.0) };
            const double length = Length(value);
            if (length > 0.1 && length <= 1.0)
                return Scaled(value, 1.0 / length);
        }
        return { 0.0, 1.0, 0.0 };
    }

    DVec3 Cross(const DVec3& left, const DVec3& right)
    {
        return { left.Y * right.Z - left.Z * right.Y, left.Z * right.X - left.X * right.Z,
            left.X * right.Y - left.Y * right.X };
    }

    // Oracle triangle test: plane intersection plus three same-side edge tests.
    // Shares nothing with the Moller-Trumbore code under test. Double-sided.
    bool PlaneTriangleHit(const DVec3& a, const DVec3& b, const DVec3& c, const DVec3& origin,
        const DVec3& direction, double& outT)
    {
        const DVec3 normal = Cross(Subtract(b, a), Subtract(c, a));
        const double denominator = Dot(normal, direction);
        if (std::abs(denominator) < 1e-14)
            return false;
        const double t = Dot(normal, Subtract(a, origin)) / denominator;
        if (t < 0.0)
            return false;
        const DVec3 p = Add(origin, Scaled(direction, t));
        if (Dot(Cross(Subtract(b, a), Subtract(p, a)), normal) < 0.0
            || Dot(Cross(Subtract(c, b), Subtract(p, b)), normal) < 0.0
            || Dot(Cross(Subtract(a, c), Subtract(p, c)), normal) < 0.0)
            return false;
        outT = t;
        return true;
    }

    // Twelve-triangle brute force over a box already placed in world space.
    bool BruteForceBox(const DVec3 (&corners)[8], const DVec3& origin, const DVec3& direction, double& outNearest)
    {
        // Corner index bits: x = 1, y = 2, z = 4.
        static constexpr int faces[6][4] = {
            { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, // z = min, z = max
            { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, // y = min, y = max
            { 0, 4, 6, 2 }, { 1, 3, 7, 5 }  // x = min, x = max
        };
        bool hit = false;
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto& face : faces)
        {
            for (int half = 0; half < 2; ++half)
            {
                const DVec3& a = corners[face[0]];
                const DVec3& b = corners[face[half == 0 ? 1 : 2]];
                const DVec3& c = corners[face[half == 0 ? 2 : 3]];
                double t = 0.0;
                if (PlaneTriangleHit(a, b, c, origin, direction, t) && t < nearest)
                {
                    nearest = t;
                    hit = true;
                }
            }
        }
        if (hit)
            outNearest = nearest;
        return hit;
    }

    // Row vector times a float Mat4 clip transform (the renderer's own convention).
    void ClipTransform(const Engine::Math::Mat4& matrix, const DVec3& point, double (&outClip)[4])
    {
        const float* m = matrix.Values;
        const double v[4] = { point.X, point.Y, point.Z, 1.0 };
        for (int column = 0; column < 4; ++column)
        {
            outClip[column] = 0.0;
            for (int row = 0; row < 4; ++row)
                outClip[column] += v[row] * m[row * 4 + column];
        }
    }

    LocalBox MakeBox(float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
    {
        LocalBox box;
        box.Min[0] = minX; box.Min[1] = minY; box.Min[2] = minZ;
        box.Max[0] = maxX; box.Max[1] = maxY; box.Max[2] = maxZ;
        return box;
    }

    LinearMap MustMakeMap(Checker& check, const Vec3& rotation, const Vec3& scale)
    {
        LinearMap map;
        check.Expect(MakeLinearMap(rotation, scale, map), "linear map is invertible");
        return map;
    }

    // Axis-aligned quad strip (x in [0, columns], y in [0, rows]) at z = 0 as a triangle list.
    struct GridMesh
    {
        std::vector<float> Positions;
        std::vector<u32> Indices;

        TriangleMeshView View() const
        {
            return { Positions.data(), Positions.size() / 3, Indices.data(), Indices.size() };
        }
    };

    GridMesh MakeGrid(u32 columns, u32 rows, float cell, float height)
    {
        GridMesh mesh;
        mesh.Positions.reserve(static_cast<size_t>(columns + 1) * (rows + 1) * 3);
        for (u32 y = 0; y <= rows; ++y)
        {
            for (u32 x = 0; x <= columns; ++x)
            {
                mesh.Positions.push_back(static_cast<float>(x) * cell);
                mesh.Positions.push_back(static_cast<float>(y) * cell);
                mesh.Positions.push_back(height);
            }
        }
        mesh.Indices.reserve(static_cast<size_t>(columns) * rows * 6);
        for (u32 y = 0; y < rows; ++y)
        {
            for (u32 x = 0; x < columns; ++x)
            {
                const u32 i0 = y * (columns + 1) + x;
                const u32 i1 = i0 + 1;
                const u32 i2 = i0 + columns + 1;
                const u32 i3 = i2 + 1;
                mesh.Indices.insert(mesh.Indices.end(), { i0, i1, i3, i0, i3, i2 });
            }
        }
        return mesh;
    }

    PickCandidate MakeCandidate(u32 id, u32 order, double localX, double localY, double localZ,
        const LocalBox& box)
    {
        PickCandidate candidate;
        candidate.EntityId = id;
        candidate.Order = order;
        candidate.Position.Local = { localX, localY, localZ };
        candidate.Box = box;
        return candidate;
    }
}

namespace SpiralTests
{
    bool TestPickingRayConventionsMatchRendererMatrices()
    {
        Checker check { "renderer conventions" };

        // Pin the object-matrix convention against the engine's own transform.
        {
            Engine::TransformComponent transform;
            transform.RotationDegrees = { 25.0f, -70.0f, 15.0f };
            transform.Scale = { 1.5f, 0.75f, 2.25f };
            const Engine::Math::Mat4 expected = transform.GetCameraRelativeTransform({}, Engine::Math::WorldGridPolicy {});
            LinearMap map;
            check.Expect(MakeLinearMap(transform.RotationDegrees, transform.Scale, map), "object map builds");
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                    check.Near(map.Matrix[row * 3 + column], expected.Values[row * 4 + column], 1e-12,
                        "object linear map equals TransformComponent::GetCameraRelativeTransform");
            }
            // Map then inverse is the identity.
            const DVec3 probe { 0.3, -1.2, 2.5 };
            check.Near(TransformByInverse(map, TransformPoint(map, probe, {})), probe, 1e-12, "map inverse round trip");
        }

        const bool generated = RunProperty("renderer-conventions", [&](ChoiceStream& stream, std::string& message)
        {
            Vec3 rotation { FloatRange(stream, -89.0, 89.0), FloatRange(stream, -360.0, 360.0),
                (stream.Next() & 1) ? FloatRange(stream, -35.0, 35.0) : 0.0f };
            ViewCamera camera;
            camera.RotationDegrees = rotation;
            camera.VerticalFovDegrees = static_cast<float>(Range(stream, 20.0, 110.0));
            camera.AspectRatio = static_cast<float>(Range(stream, 0.5, 2.5));
            ViewportRect rect { Range(stream, -300.0, 300.0), Range(stream, -300.0, 300.0),
                Range(stream, 200.0, 2000.0), Range(stream, 200.0, 2000.0) };

            const Engine::CameraProjection projection { static_cast<float>(camera.VerticalFovDegrees), 0.1f, 1000.0f };
            const Engine::CameraView view = Engine::BuildCameraView(
                { 0.0, 0.0, 0.0 }, rotation, projection, static_cast<float>(camera.AspectRatio), { 0.0, 0.0, 0.0 });
            if (!view.Valid)
            {
                message = "BuildCameraView rejected a finite camera";
                return false;
            }

            for (int sample = 0; sample < 12; ++sample)
            {
                const DVec3 point = Scaled(RandomUnitVector(stream), Range(stream, 0.5, 80.0));
                double clip[4];
                ClipTransform(view.ViewProjection, point, clip);
                if (clip[3] < 0.05)
                    continue;
                const double ndcX = clip[0] / clip[3];
                const double ndcY = clip[1] / clip[3];
                if (std::abs(ndcX) > 1.0 || std::abs(ndcY) > 1.0)
                    continue;
                const double expectedPixelX = rect.X + (ndcX * 0.5 + 0.5) * rect.Width;
                const double expectedPixelY = rect.Y + (0.5 - ndcY * 0.5) * rect.Height;

                double pixelX = 0.0, pixelY = 0.0, depth = 0.0;
                if (!ProjectToViewport(rect, camera, point, pixelX, pixelY, depth))
                {
                    message = "ProjectToViewport rejected a point the renderer projects inside the frustum";
                    return false;
                }
                if (std::abs(pixelX - expectedPixelX) > 0.05 || std::abs(pixelY - expectedPixelY) > 0.05
                    || std::abs(depth - clip[3]) > 1e-3)
                {
                    message = "ProjectToViewport disagrees with the renderer: pixel " + std::to_string(pixelX) + ","
                        + std::to_string(pixelY) + " vs " + std::to_string(expectedPixelX) + ","
                        + std::to_string(expectedPixelY);
                    return false;
                }

                DVec3 direction;
                if (!ViewportPixelToRayDirection(rect, expectedPixelX, expectedPixelY, camera, direction))
                {
                    message = "ray construction failed";
                    return false;
                }
                DVec3 pointDirection;
                Normalize(point, pointDirection);
                if (std::abs(Dot(direction, pointDirection) - 1.0) > 1e-9)
                {
                    message = "ray through the renderer's pixel misses the projected point; dot=" + std::to_string(Dot(direction, pointDirection));
                    return false;
                }
            }
            return true;
        }, 300);
        check.Expect(generated, "ray and projection agree with BuildCameraView under generated cameras");

        // A deliberately wrong order (R^-1 as RotationYawPitchRoll(yaw,pitch,roll) transposed)
        // differs from the renderer once yaw and pitch are both nonzero; the real basis must not.
        {
            CameraBasis basis;
            check.Expect(ComputeCameraBasis({ 30.0f, 40.0f, 0.0f }, basis), "basis builds");
            const double yaw = 40.0 * 3.14159265358979323846 / 180.0;
            const double pitch = 30.0 * 3.14159265358979323846 / 180.0;
            // Hand-expanded Ry(-yaw) * Rx(-pitch) columns (see the header comment).
            check.Near(basis.Forward, { std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) },
                1e-6, "forward axis for yaw 40 pitch 30");
            check.Near(basis.Right, { std::cos(yaw), 0.0, -std::sin(yaw) }, 1e-6, "right axis for yaw 40");
            check.Near(basis.Up, { std::sin(yaw) * std::sin(pitch), std::cos(pitch), std::cos(yaw) * std::sin(pitch) },
                1e-6, "up axis for yaw 40 pitch 30");
        }
        return check.Ok;
    }

    bool TestPickingViewportMappingHandComputedAndRoundTrip()
    {
        Checker check { "viewport mapping" };
        ViewCamera camera;
        camera.VerticalFovDegrees = 90.0;
        camera.AspectRatio = 2.0;
        const ViewportRect rect { 0.0, 0.0, 200.0, 100.0 };
        DVec3 direction;

        // Identity rotation, tan(45 degrees) = 1, aspect 2.
        check.Expect(ViewportPixelToRayDirection(rect, 100.0, 50.0, camera, direction), "center ray");
        check.Near(direction, { 0.0, 0.0, 1.0 }, 1e-9, "center looks along +Z");
        check.Expect(ViewportPixelToRayDirection(rect, 200.0, 50.0, camera, direction), "right edge ray");
        check.Near(direction, { 2.0 / std::sqrt(5.0), 0.0, 1.0 / std::sqrt(5.0) }, 1e-9, "right edge: (2,0,1)/sqrt5");
        check.Expect(ViewportPixelToRayDirection(rect, 100.0, 0.0, camera, direction), "top ray");
        check.Near(direction, { 0.0, 1.0 / std::sqrt(2.0), 1.0 / std::sqrt(2.0) }, 1e-9, "top edge: y up (pixel y down)");
        check.Expect(ViewportPixelToRayDirection(rect, 0.0, 100.0, camera, direction), "bottom-left ray");
        check.Near(direction, { -2.0 / std::sqrt(6.0), -1.0 / std::sqrt(6.0), 1.0 / std::sqrt(6.0) }, 1e-9,
            "bottom-left: (-2,-1,1)/sqrt6");

        // Yaw 90: forward +X, right -Z.
        camera.RotationDegrees = { 0.0f, 90.0f, 0.0f };
        check.Expect(ViewportPixelToRayDirection(rect, 200.0, 50.0, camera, direction), "yawed right edge ray");
        check.Near(direction, { 1.0 / std::sqrt(5.0), 0.0, -2.0 / std::sqrt(5.0) }, 1e-6, "yaw 90 right edge: (1,0,-2)/sqrt5");

        // Pitch 30 looks down: forward (0,-1/2,sqrt3/2), up (0,sqrt3/2,1/2).
        camera.RotationDegrees = { 30.0f, 0.0f, 0.0f };
        check.Expect(ViewportPixelToRayDirection(rect, 100.0, 0.0, camera, direction), "pitched top ray");
        DVec3 expectedTop;
        Normalize({ 0.0, -0.5 + std::sqrt(3.0) / 2.0, std::sqrt(3.0) / 2.0 + 0.5 }, expectedTop);
        check.Near(direction, expectedTop, 1e-6, "pitch 30 top-center ray");

        // A rectangle that does not start at the origin and an offset pixel.
        camera.RotationDegrees = {};
        const ViewportRect offset { 310.0, 40.0, 200.0, 100.0 };
        check.Expect(ViewportPixelToRayDirection(offset, 410.0, 90.0, camera, direction), "offset rect center ray");
        check.Near(direction, { 0.0, 0.0, 1.0 }, 1e-9, "offset rectangle center is still straight ahead");

        // Normalized coordinates.
        double pixelX = 0.0, pixelY = 0.0;
        check.Expect(NormalizedToViewportPixel(offset, 0.5, 0.5, pixelX, pixelY), "normalized center");
        check.Near(pixelX, 410.0, 1e-12, "normalized x");
        check.Near(pixelY, 90.0, 1e-12, "normalized y");
        check.Expect(NormalizedToViewportPixel(offset, 0.0, 1.0, pixelX, pixelY)
                && pixelX == 310.0 && pixelY == 140.0, "normalized corner is inclusive");
        check.Expect(!NormalizedToViewportPixel(offset, -0.001, 0.5, pixelX, pixelY)
                && !NormalizedToViewportPixel(offset, 0.5, 1.001, pixelX, pixelY)
                && !NormalizedToViewportPixel(offset, std::nan(""), 0.5, pixelX, pixelY),
            "normalized coordinates outside [0, 1] or non-finite are rejected");

        // Rejections.
        check.Expect(!ViewportPixelToRayDirection({ 0.0, 0.0, 0.0, 100.0 }, 0.0, 0.0, camera, direction), "empty rect");
        ViewCamera badFov = camera;
        badFov.VerticalFovDegrees = 180.0;
        check.Expect(!ViewportPixelToRayDirection(rect, 1.0, 1.0, badFov, direction), "fov 180 rejected");
        badFov.VerticalFovDegrees = 60.0;
        badFov.RotationDegrees = { std::nanf(""), 0.0f, 0.0f };
        check.Expect(!ViewportPixelToRayDirection(rect, 1.0, 1.0, badFov, direction), "NaN rotation rejected");
        double depth = 0.0;
        check.Expect(!ProjectToViewport(rect, camera, { 0.0, 0.0, -1.0 }, pixelX, pixelY, depth),
            "a point behind the camera does not project");

        // Round trip: pixel -> ray -> point at depth -> pixel, under generated cameras.
        const bool generated = RunProperty("viewport-round-trip", [&](ChoiceStream& stream, std::string& message)
        {
            ViewCamera cam;
            cam.RotationDegrees = { FloatRange(stream, -89.0, 89.0), FloatRange(stream, -720.0, 720.0),
                FloatRange(stream, -45.0, 45.0) };
            cam.VerticalFovDegrees = Range(stream, 15.0, 120.0);
            cam.AspectRatio = Range(stream, 0.4, 3.0);
            const ViewportRect r { Range(stream, -500.0, 500.0), Range(stream, -500.0, 500.0),
                Range(stream, 100.0, 3000.0), Range(stream, 100.0, 3000.0) };
            const double px = r.X + Unit(stream) * r.Width;
            const double py = r.Y + Unit(stream) * r.Height;
            DVec3 dir;
            if (!ViewportPixelToRayDirection(r, px, py, cam, dir))
            {
                message = "ray failed";
                return false;
            }
            const DVec3 point = Scaled(dir, Range(stream, 0.1, 500.0));
            double outX = 0.0, outY = 0.0, outDepth = 0.0;
            if (!ProjectToViewport(r, cam, point, outX, outY, outDepth))
            {
                message = "round-trip projection failed";
                return false;
            }
            if (std::abs(outX - px) > 1e-6 * std::max(1.0, r.Width) || std::abs(outY - py) > 1e-6 * std::max(1.0, r.Height))
            {
                message = "round trip drifted: " + std::to_string(outX - px) + "," + std::to_string(outY - py);
                return false;
            }
            return true;
        }, 400);
        check.Expect(generated, "pixel to ray to pixel round trip");
        return check.Ok;
    }

    bool TestPickingOrientedBoxHandComputedCases()
    {
        Checker check { "oriented box" };
        BoxHit hit;
        const LinearMap identity = MustMakeMap(check, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f });
        const LocalBox unit = MakeBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);

        check.Expect(IntersectRayOrientedBox({ 0, 0, -5 }, { 0, 0, 1 }, {}, identity, unit, hit), "ray at the unit box");
        check.Near(hit.Enter, 4.5, 1e-12, "enters the near face at 4.5");
        check.Near(hit.Exit, 5.5, 1e-12, "leaves the far face at 5.5");
        check.Expect(IntersectRayOrientedBox({ 0, 0, 0 }, { 0, 0, 1 }, {}, identity, unit, hit), "origin inside the box");
        check.Near(hit.Enter, 0.0, 0.0, "enter clamps to zero from inside");
        check.Near(hit.Exit, 0.5, 1e-12, "exit at the half extent");
        check.Expect(!IntersectRayOrientedBox({ 0, 0, 5 }, { 0, 0, 1 }, {}, identity, unit, hit), "box behind the origin is a miss");
        check.Expect(!IntersectRayOrientedBox({ 0.75, 0, -5 }, { 0, 0, 1 }, {}, identity, unit, hit), "parallel ray outside the slab");
        check.Expect(IntersectRayOrientedBox({ 0.5, 0, -5 }, { 0, 0, 1 }, {}, identity, unit, hit), "a ray exactly on the face plane touches the box");
        check.Expect(!IntersectRayOrientedBox({ 0, 0, -5 }, { 0, 1, 0 }, {}, identity, unit, hit), "perpendicular ray misses");
        // Offset places the box relative to the ray origin frame.
        check.Expect(IntersectRayOrientedBox({ 0, 0, 0 }, { 0, 0, 1 }, { 0, 0, 10 }, identity, unit, hit), "offset box ahead");
        check.Near(hit.Enter, 9.5, 1e-12, "offset box entry");
        // A flat (zero-thickness) box is still pickable.
        const LocalBox flat = MakeBox(-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f);
        check.Expect(IntersectRayOrientedBox({ 0.25, 0.25, -3 }, { 0, 0, 1 }, {}, identity, flat, hit), "flat box hit");
        check.Near(hit.Enter, 3.0, 1e-12, "flat box distance");
        // Inverted or non-finite boxes and rays are rejected.
        check.Expect(!IntersectRayOrientedBox({ 0, 0, -5 }, { 0, 0, 1 }, {}, identity, MakeBox(1, 0, 0, -1, 1, 1), hit), "inverted box rejected");
        check.Expect(!IntersectRayOrientedBox({ 0, 0, -5 }, { std::nan(""), 0, 1 }, {}, identity, unit, hit), "NaN ray rejected");

        // Non-uniform scale plus rotation. Yaw 90 sends local +X to world -Z (hand-expanded
        // from RotationY: x' = z*sin, z' = -x*sin); half extents (1.5, 0.5, 0.5) therefore
        // become z-extent 1.5 and x-extent 0.5 in the world.
        const LinearMap rotated = MustMakeMap(check, { 0.0f, 90.0f, 0.0f }, { 3.0f, 1.0f, 1.0f });
        check.Expect(IntersectRayOrientedBox({ 0, 0, -10 }, { 0, 0, 1 }, {}, rotated, unit, hit), "rotated, scaled box hit");
        check.Near(hit.Enter, 8.5, 1e-5, "rotated, scaled entry at z = -1.5");
        check.Near(hit.Exit, 11.5, 1e-5, "rotated, scaled exit at z = +1.5");
        check.Expect(!IntersectRayOrientedBox({ 1.0, 0, -10 }, { 0, 0, 1 }, {}, rotated, unit, hit),
            "a ray at x = 1 misses because the rotated box is only 0.5 wide in x");
        check.Expect(IntersectRayOrientedBox({ 1.0, 0, -10 }, { 0, 0, 1 }, {}, identity, MakeBox(-1.5f, -0.5f, -0.5f, 1.5f, 0.5f, 0.5f), hit),
            "the same ray hits the unrotated 3-wide box (control)");
        // Uniform scale 2 on the unit box moves the entry from 4.5 to 4.
        const LinearMap doubled = MustMakeMap(check, { 0.0f, 0.0f, 0.0f }, { 2.0f, 2.0f, 2.0f });
        check.Expect(IntersectRayOrientedBox({ 0, 0, -5 }, { 0, 0, 1 }, {}, doubled, unit, hit), "scaled box hit");
        check.Near(hit.Enter, 4.0, 1e-12, "scaled entry");

        // Singular scale is rejected by the map.
        LinearMap singular;
        check.Expect(!MakeLinearMap({ 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 1.0f }, singular), "zero scale is not invertible");
        check.Expect(!MakeLinearMap({ std::nanf(""), 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, singular), "NaN rotation rejected");
        return check.Ok;
    }

    bool TestPickingOrientedBoxMatchesBruteForceAndContainsInteriorRays()
    {
        Checker check { "box property" };
        size_t hitCases = 0;
        size_t missCases = 0;
        size_t insideCases = 0;
        size_t interiorRays = 0;

        const bool generated = RunProperty("oriented-box", [&](ChoiceStream& stream, std::string& message)
        {
            const Vec3 rotation { FloatRange(stream, -180.0, 180.0), FloatRange(stream, -180.0, 180.0),
                FloatRange(stream, -180.0, 180.0) };
            const Vec3 scale { FloatRange(stream, 0.2, 3.0), FloatRange(stream, 0.2, 3.0), FloatRange(stream, 0.2, 3.0) };
            LinearMap map;
            if (!MakeLinearMap(rotation, scale, map))
            {
                message = "map failed for a valid transform";
                return false;
            }
            LocalBox box;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float low = FloatRange(stream, -2.0, 1.0);
                box.Min[axis] = low;
                box.Max[axis] = low + FloatRange(stream, 0.05, 3.0);
            }
            const DVec3 offset { Range(stream, -20.0, 20.0), Range(stream, -20.0, 20.0), Range(stream, -20.0, 20.0) };
            DVec3 corners[8];
            OrientedBoxCorners(offset, map, box, corners);
            DVec3 center {};
            for (const DVec3& corner : corners)
                center = Add(center, Scaled(corner, 1.0 / 8.0));

            // Rays aimed near the box from outside, plus a few that start inside it.
            for (int sample = 0; sample < 6; ++sample)
            {
                const DVec3 target = Add(center, Scaled(RandomUnitVector(stream), Range(stream, 0.0, 3.0)));
                DVec3 origin = Add(center, Scaled(RandomUnitVector(stream), Range(stream, 0.5, 40.0)));
                if (sample == 5)
                    origin = Add(center, Scaled(RandomUnitVector(stream), Range(stream, 0.0, 0.3)));
                DVec3 direction;
                if (!Normalize(Subtract(target, origin), direction))
                    continue;

                BoxHit hit;
                const bool slab = IntersectRayOrientedBox(origin, direction, offset, map, box, hit);

                // Origin inside the box in local space?
                const DVec3 localOrigin = TransformByInverse(map, Subtract(origin, offset));
                const bool inside = localOrigin.X >= box.Min[0] && localOrigin.X <= box.Max[0]
                    && localOrigin.Y >= box.Min[1] && localOrigin.Y <= box.Max[1]
                    && localOrigin.Z >= box.Min[2] && localOrigin.Z <= box.Max[2];

                double nearest = 0.0;
                const bool brute = BruteForceBox(corners, origin, direction, nearest);
                if (inside)
                {
                    ++insideCases;
                    if (!slab || hit.Enter != 0.0 || std::abs(hit.Exit - nearest) > 1e-6 * std::max(1.0, nearest))
                    {
                        message = "origin inside the box must give Enter 0 and Exit at the brute-force surface hit";
                        return false;
                    }
                    continue;
                }
                if (slab != brute)
                {
                    message = std::string("slab/brute-force disagreement: slab=") + (slab ? "hit" : "miss")
                        + " brute=" + (brute ? "hit" : "miss");
                    return false;
                }
                if (slab)
                {
                    ++hitCases;
                    if (std::abs(hit.Enter - nearest) > 1e-6 * std::max(1.0, nearest))
                    {
                        message = "entry distance differs from the brute-force nearest triangle: "
                            + std::to_string(hit.Enter) + " vs " + std::to_string(nearest);
                        return false;
                    }
                }
                else
                {
                    ++missCases;
                }
            }

            // Property: a ray through a random point strictly inside the box always hits it.
            const double fx = Range(stream, 0.02, 0.98);
            const double fy = Range(stream, 0.02, 0.98);
            const double fz = Range(stream, 0.02, 0.98);
            const DVec3 interiorLocal { box.Min[0] + fx * (double(box.Max[0]) - box.Min[0]),
                box.Min[1] + fy * (double(box.Max[1]) - box.Min[1]),
                box.Min[2] + fz * (double(box.Max[2]) - box.Min[2]) };
            const DVec3 interiorWorld = TransformPoint(map, interiorLocal, offset);
            const DVec3 origin = Add(interiorWorld, Scaled(RandomUnitVector(stream), Range(stream, 1.0, 60.0)));
            DVec3 direction;
            Normalize(Subtract(interiorWorld, origin), direction);
            const double distanceToPoint = Length(Subtract(interiorWorld, origin));
            BoxHit hit;
            ++interiorRays;
            if (!IntersectRayOrientedBox(origin, direction, offset, map, box, hit)
                || hit.Enter > distanceToPoint + 1e-7 || hit.Exit < distanceToPoint - 1e-7)
            {
                message = "a ray through an interior point missed the oriented box or does not bracket the point";
                return false;
            }
            return true;
        }, 500);
        check.Expect(generated, "slab test matches the 12-triangle brute force and interior rays always hit");
        // Coverage evidence: the campaign must exercise hits, misses, and inside-origin rays.
        check.Expect(hitCases > 200 && missCases > 200 && insideCases > 50 && interiorRays >= 500,
            "generated campaign covers hits (" + std::to_string(hitCases) + "), misses (" + std::to_string(missCases)
                + "), inside origins (" + std::to_string(insideCases) + "), interior rays (" + std::to_string(interiorRays) + ")");
        return check.Ok;
    }

    bool TestPickingTriangleRefinementMatchesPlaneOracleAndBudgetPolicy()
    {
        Checker check { "triangles" };

        // Hand-computed: one triangle (0,0,0),(2,0,0),(0,2,0) at z = 0; ray from (0.5,0.5,-3) along +Z.
        {
            const std::vector<float> positions { 0, 0, 0, 2, 0, 0, 0, 2, 0 };
            const std::vector<u32> indices { 0, 1, 2 };
            const TriangleMeshView view { positions.data(), 3, indices.data(), 3 };
            double t = 0.0;
            size_t tested = 0;
            check.Expect(IntersectRayTriangles(view, { 0.5, 0.5, -3.0 }, { 0, 0, 1 }, t, tested), "ray inside the triangle");
            check.Near(t, 3.0, 1e-12, "distance to the plane");
            check.Expect(tested == 1, "one triangle tested");
            check.Expect(!IntersectRayTriangles(view, { 1.5, 1.5, -3.0 }, { 0, 0, 1 }, t, tested), "ray beyond the hypotenuse misses");
            check.Expect(!IntersectRayTriangles(view, { 0.5, 0.5, 3.0 }, { 0, 0, 1 }, t, tested), "triangle behind the origin misses");
            check.Expect(IntersectRayTriangles(view, { 0.5, 0.5, 3.0 }, { 0, 0, -1 }, t, tested) && std::abs(t - 3.0) < 1e-12,
                "double-sided: the back face is hit too");
            // Out-of-range indices are skipped, not read.
            const std::vector<u32> bad { 0, 1, 7 };
            const TriangleMeshView badView { positions.data(), 3, bad.data(), 3 };
            size_t badTested = 0;
            check.Expect(!IntersectRayTriangles(badView, { 0.5, 0.5, -3.0 }, { 0, 0, 1 }, t, badTested) && badTested == 0,
                "an out-of-range index is skipped without being tested");
        }

        // Generated soups against the plane/side-of-edge oracle.
        const bool generated = RunProperty("triangle-soup", [&](ChoiceStream& stream, std::string& message)
        {
            GridMesh soup;
            const size_t triangles = stream.NextSize(1, 60);
            for (size_t index = 0; index < triangles * 3; ++index)
            {
                soup.Positions.push_back(FloatRange(stream, -3.0, 3.0));
                soup.Positions.push_back(FloatRange(stream, -3.0, 3.0));
                soup.Positions.push_back(FloatRange(stream, -3.0, 3.0));
                soup.Indices.push_back(static_cast<u32>(index));
            }
            const TriangleMeshView view = soup.View();
            for (int sample = 0; sample < 8; ++sample)
            {
                const DVec3 origin = Scaled(RandomUnitVector(stream), Range(stream, 4.0, 12.0));
                const DVec3 target { Range(stream, -2.0, 2.0), Range(stream, -2.0, 2.0), Range(stream, -2.0, 2.0) };
                DVec3 direction;
                if (!Normalize(Subtract(target, origin), direction))
                    continue;
                double expected = std::numeric_limits<double>::infinity();
                bool expectedHit = false;
                for (size_t triangle = 0; triangle < triangles; ++triangle)
                {
                    const float* p = soup.Positions.data() + triangle * 9;
                    const DVec3 a { p[0], p[1], p[2] }, b { p[3], p[4], p[5] }, c { p[6], p[7], p[8] };
                    double t = 0.0;
                    if (PlaneTriangleHit(a, b, c, origin, direction, t) && t < expected)
                    {
                        expected = t;
                        expectedHit = true;
                    }
                }
                double actual = 0.0;
                size_t tested = 0;
                const bool hit = IntersectRayTriangles(view, origin, direction, actual, tested);
                if (hit != expectedHit || tested != triangles)
                {
                    message = std::string("triangle hit/miss disagreement: actual=") + (hit ? "hit" : "miss")
                        + " oracle=" + (expectedHit ? "hit" : "miss");
                    return false;
                }
                if (hit && std::abs(actual - expected) > 1e-9 * std::max(1.0, expected))
                {
                    message = "nearest distance disagreement " + std::to_string(actual) + " vs " + std::to_string(expected);
                    return false;
                }
            }
            return true;
        }, 300);
        check.Expect(generated, "Moller-Trumbore refinement matches the plane oracle on random soups");

        // Budget policy at the exact boundary: refine when count <= budget, fall back above it.
        {
            const GridMesh grid = MakeGrid(4, 4, 1.0f, 0.0f); // 32 triangles, a plane at z = 0 over [0,4]^2
            const TriangleMeshView view = grid.View();
            // A hole: remove the quad at (1,1) so the box is hit but the geometry is not.
            GridMesh holey = grid;
            const size_t quadIndex = 1 * 4 + 1;
            holey.Indices.erase(holey.Indices.begin() + static_cast<std::ptrdiff_t>(quadIndex * 6),
                holey.Indices.begin() + static_cast<std::ptrdiff_t>(quadIndex * 6 + 6));
            const TriangleMeshView holeyView = holey.View();
            check.Expect(view.TriangleCount() == 32 && holeyView.TriangleCount() == 30, "fixture triangle counts");

            const LocalBox box = MakeBox(0, 0, 0, 4, 4, 0);
            PickCandidate candidate = MakeCandidate(7, 0, 0, 0, 5, box);
            candidate.Triangles = &holeyView;
            const Engine::Math::WorldGridPolicy policy;
            const Engine::Math::SectorLocalPosition origin {};
            // Aim through the hole at (1.5, 1.5): the box is hit, the surface is not.
            DVec3 direction;
            Normalize({ 1.5, 1.5, 5.0 }, direction);
            PickOptions options;
            options.TriangleBudget = 30; // equal to the count: refine
            PickResult result = PickNearest(origin, direction, policy, { candidate }, options);
            check.Expect(!result.Hit && result.BoxHits == 1 && result.TrianglesTested == 30,
                "at budget == triangle count the mesh is refined and the hole is a miss");
            options.TriangleBudget = 29; // one below: bounds fallback
            result = PickNearest(origin, direction, policy, { candidate }, options);
            check.Expect(result.Hit && !result.Refined && result.TrianglesTested == 0 && result.EntityId == 7,
                "budget - 1 falls back to the oriented box and reports it");
            check.Near(result.Distance, std::sqrt(1.5 * 1.5 + 1.5 * 1.5 + 25.0), 1e-9, "fallback distance is the box entry");
            // A ray through solid geometry refines to the surface.
            Normalize({ 3.5, 3.5, 5.0 }, direction);
            options.TriangleBudget = 30;
            result = PickNearest(origin, direction, policy, { candidate }, options);
            check.Expect(result.Hit && result.Refined && result.EntityId == 7, "solid area is a refined hit");
            check.Near(result.Distance, std::sqrt(3.5 * 3.5 + 3.5 * 3.5 + 25.0), 1e-9, "refined distance");
        }
        return check.Ok;
    }

    bool TestPickingNearestHitTieBreakConcaveMissAndSectorPrecision()
    {
        Checker check { "nearest hit" };
        const Engine::Math::WorldGridPolicy policy;
        const Engine::Math::SectorLocalPosition origin {};
        const LocalBox unit = MakeBox(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
        const DVec3 forward { 0.0, 0.0, 1.0 };

        // Nearest wins regardless of Order or list position.
        {
            std::vector<PickCandidate> candidates {
                MakeCandidate(10, 0, 0, 0, 20, unit), // farther, lowest order
                MakeCandidate(11, 5, 0, 0, 10, unit), // nearer, higher order
                MakeCandidate(12, 2, 0, 0, 30, unit)
            };
            PickResult result = PickNearest(origin, forward, policy, candidates, {});
            check.Expect(result.Hit && result.EntityId == 11 && result.BoxHits == 3 && result.Candidates == 3,
                "the nearest box wins over a lower-order farther box");
            check.Near(result.Distance, 9.5, 1e-12, "nearest distance");
        }

        // Exact tie: identical boxes at the same place; the lower Order wins in either list order.
        {
            std::vector<PickCandidate> ascending {
                MakeCandidate(21, 2, 0, 0, 10, unit), MakeCandidate(22, 5, 0, 0, 10, unit) };
            std::vector<PickCandidate> descending { ascending[1], ascending[0] };
            const PickResult first = PickNearest(origin, forward, policy, ascending, {});
            const PickResult second = PickNearest(origin, forward, policy, descending, {});
            check.Expect(first.Hit && second.Hit && first.EntityId == 21 && second.EntityId == 21 && first.Order == 2,
                "equal distances resolve to the lower Order independent of list order");
        }

        // A concave L-shaped mesh: the ray passes through the bounding box's empty corner and
        // must pick the farther entity behind it; without refinement the L's box wins.
        {
            // Quads: [0,2]x[0,1] and [0,1]x[1,2] (an L) in the z = 0 plane, as a triangle list.
            std::vector<float> positions;
            std::vector<u32> indices;
            const auto addQuad = [&](float x0, float y0, float x1, float y1)
            {
                const u32 base = static_cast<u32>(positions.size() / 3);
                positions.insert(positions.end(), { x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0 });
                indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
            };
            addQuad(0, 0, 2, 1);
            addQuad(0, 1, 1, 2);
            const TriangleMeshView lShape { positions.data(), positions.size() / 3, indices.data(), indices.size() };

            PickCandidate concave = MakeCandidate(31, 0, 0, 0, 0, MakeBox(0, 0, 0, 2, 2, 0));
            concave.Triangles = &lShape;
            PickCandidate behind = MakeCandidate(32, 1, 1.5, 1.5, 6, MakeBox(-0.4f, -0.4f, -0.4f, 0.4f, 0.4f, 0.4f));

            Engine::Math::SectorLocalPosition cameraAtX = origin;
            cameraAtX.Local = { 1.5, 1.5, -4.0 };
            const PickResult beside = PickNearest(cameraAtX, forward, policy, { concave, behind }, {});
            check.Expect(beside.Hit && beside.EntityId == 32,
                "a click beside the concave mesh (inside its bounds) picks the entity behind it");
            PickOptions boundsOnly;
            boundsOnly.TriangleBudget = 0;
            const PickResult fallback = PickNearest(cameraAtX, forward, policy, { concave, behind }, boundsOnly);
            check.Expect(fallback.Hit && fallback.EntityId == 31 && !fallback.Refined,
                "with refinement disabled the oriented box of the concave mesh wins (documented fallback)");
            cameraAtX.Local = { 0.5, 0.5, -4.0 };
            const PickResult onMesh = PickNearest(cameraAtX, forward, policy, { concave, behind }, {});
            check.Expect(onMesh.Hit && onMesh.EntityId == 31 && onMesh.Refined, "a click on the L itself picks it");
            check.Near(onMesh.Distance, 4.0, 1e-9, "refined distance to the L");
        }

        // Sector-local precision: far from the origin, a composed absolute double cannot
        // resolve a centimeter-scale offset but the sector-local difference is exact.
        {
            Engine::Math::SectorLocalPosition far;
            far.Sector = { 1000000000, 0, 0 };
            far.Local = { 0.1234567, 0.0, 0.0 };
            PickCandidate target;
            target.EntityId = 41;
            target.Position = far;
            target.Position.Local.X += 10.3; // 10.3 m ahead of the ray origin along +X (not a multiple of the absolute-double ulp)
            target.Box = unit;
            DVec3 composedOrigin, composedTarget;
            check.Expect(Engine::Math::TryComposeApproximateWorldPosition(far, policy, composedOrigin)
                    && Engine::Math::TryComposeApproximateWorldPosition(target.Position, policy, composedTarget),
                "absolute positions compose");
            const double composedDifference = composedTarget.X - composedOrigin.X;
            check.Expect(std::abs(composedDifference - 10.3) > 1e-5,
                "control: composing absolute doubles at sector 1e9 loses more than 10 micrometers");
            const PickResult result = PickNearest(far, { 1.0, 0.0, 0.0 }, policy, { target }, {});
            check.Expect(result.Hit && result.EntityId == 41, "the far-sector entity is picked");
            check.Near(result.Distance, 9.8, 1e-9, "sector-local differencing keeps the distance exact");
        }

        // Rejections: zero and non-finite directions never hit.
        check.Expect(!PickNearest(origin, { 0.0, 0.0, 0.0 }, policy, { MakeCandidate(1, 0, 0, 0, 5, unit) }, {}).Hit, "zero direction");
        check.Expect(!PickNearest(origin, { std::nan(""), 0.0, 1.0 }, policy, { MakeCandidate(1, 0, 0, 0, 5, unit) }, {}).Hit, "NaN direction");
        check.Expect(!PickNearest(origin, forward, policy, {}, {}).Hit, "no candidates");
        PickCandidate singularCandidate = MakeCandidate(1, 0, 0, 0, 5, unit);
        singularCandidate.Scale = { 0.0f, 1.0f, 1.0f };
        check.Expect(!PickNearest(origin, forward, policy, { singularCandidate }, {}).Hit, "a singular transform is skipped");
        return check.Ok;
    }

    bool TestPickingFramingFitsSphereAndKeepsViewDirection()
    {
        Checker check { "framing" };

        // Hand-computed: rotation-invariant enclosing sphere of a 1x2x3 box (half diagonal sqrt(14)/2).
        for (const Vec3 rotation : { Vec3 { 0.0f, 0.0f, 0.0f }, Vec3 { 33.0f, 71.0f, 12.0f }, Vec3 { 0.0f, 90.0f, 0.0f } })
        {
            const LinearMap map = MustMakeMap(check, rotation, { 1.0f, 1.0f, 1.0f });
            DVec3 corners[8];
            OrientedBoxCorners({ 5.0, -2.0, 7.0 }, map, MakeBox(-0.5f, -1.0f, -1.5f, 0.5f, 1.0f, 1.5f), corners);
            BoundingSphere sphere;
            check.Expect(EnclosingSphere(std::vector<DVec3>(corners, corners + 8), sphere), "sphere builds");
            check.Near(sphere.Radius, std::sqrt(3.5), 1e-5,
                "radius is the half diagonal of the 1x2x3 box for every rotation");
            check.Near(sphere.Center, { 5.0, -2.0, 7.0 }, 1e-5, "center is the box center");
        }
        {
            BoundingSphere unused;
            check.Expect(!EnclosingSphere({}, unused), "no points, no sphere");
        }

        // Hand-computed distance: fov 90 (tangent 1), aspect 2 -> tight axis is vertical (45 degrees).
        // r = 1, margin 0.15 -> d = 1.15 / sin(45 degrees).
        {
            CameraBasis basis;
            check.Expect(ComputeCameraBasis({ 0.0f, 0.0f, 0.0f }, basis), "basis");
            FramingSolution solution;
            check.Expect(SolveFraming({ { 0, 0, 0 }, 1.0 }, basis, 90.0, 2.0, 0.1, kFramingMargin, solution), "solves");
            check.Near(solution.Distance, 1.15 / std::sin(3.14159265358979323846 / 4.0), 1e-9, "vertical-limited distance");
            check.Near(solution.CameraOffset, { 0.0, 0.0, -solution.Distance }, 1e-9, "camera backs off along -forward");
            // Portrait: aspect 0.5 -> horizontal half angle atan(0.5) is the tight one.
            check.Expect(SolveFraming({ { 0, 0, 0 }, 1.0 }, basis, 90.0, 0.5, 0.1, kFramingMargin, solution), "solves portrait");
            check.Near(solution.Distance, 1.15 / std::sin(std::atan(0.5)), 1e-9, "horizontal-limited distance");
        }

        // Generated: the framed outer sphere is tangent to the tight frustum plane and the inner
        // sphere fits with the margin; the camera keeps its view direction and looks at the center.
        const bool generated = RunProperty("framing", [&](ChoiceStream& stream, std::string& message)
        {
            ViewCamera cam;
            cam.RotationDegrees = { FloatRange(stream, -85.0, 85.0), FloatRange(stream, -360.0, 360.0), 0.0f };
            cam.VerticalFovDegrees = Range(stream, 20.0, 110.0);
            cam.AspectRatio = Range(stream, 0.5, 2.5);
            CameraBasis basis;
            ComputeCameraBasis(cam.RotationDegrees, basis);
            const BoundingSphere subject { { Range(stream, -50.0, 50.0), Range(stream, -50.0, 50.0), Range(stream, -50.0, 50.0) },
                Range(stream, 0.1, 40.0) };
            FramingSolution solution;
            if (!SolveFraming(subject, basis, cam.VerticalFovDegrees, cam.AspectRatio, 0.1, kFramingMargin, solution))
            {
                message = "framing failed";
                return false;
            }

            // Move the camera; every point on the subject sphere must project inside the viewport.
            const ViewportRect rect { 0.0, 0.0, 1600.0, 900.0 };
            double centerX = 0.0, centerY = 0.0, depth = 0.0;
            const DVec3 centerRelative = Subtract(subject.Center, solution.CameraOffset);
            if (!ProjectToViewport(rect, cam, centerRelative, centerX, centerY, depth)
                || std::abs(centerX - 800.0) > 1e-3 || std::abs(centerY - 450.0) > 1e-3
                || std::abs(depth - solution.Distance) > 1e-6 * solution.Distance)
            {
                message = "the framed subject is not centered at the solved distance";
                return false;
            }
            const double tangent = std::tan(cam.VerticalFovDegrees * 3.14159265358979323846 / 360.0);
            const double tightTangent = std::min(tangent, tangent * cam.AspectRatio);
            // Silhouette tangent point of a sphere of radius R seen from distance d lies at angle alpha,
            // tan(alpha) = (R/d)/sqrt(1-(R/d)^2); the outer (margin-enlarged) sphere must be tangent to
            // the tight frustum plane: tan(alpha) == tightTangent.
            const double ratioOuter = subject.Radius * (1.0 + kFramingMargin) / solution.Distance;
            if (solution.Distance > subject.Radius + 0.2 + 1e-9) // not clamped by the near plane
            {
                const double alphaTangent = ratioOuter / std::sqrt(1.0 - ratioOuter * ratioOuter);
                if (std::abs(alphaTangent - tightTangent) > 1e-9)
                {
                    message = "outer sphere is not tangent to the tight frustum plane: " + std::to_string(alphaTangent)
                        + " vs " + std::to_string(tightTangent);
                    return false;
                }
            }
            // Sample the subject sphere (surface and interior): every point projects inside the
            // viewport, and the tight-axis extent never exceeds the analytic silhouette bound
            // tan(alpha_inner) / tightTangent, which is below 1 because of the margin.
            const double ratioInner = subject.Radius / solution.Distance;
            const double expectedMaximum = (ratioInner / std::sqrt(1.0 - ratioInner * ratioInner)) / tightTangent;
            const bool verticalIsTight = cam.AspectRatio >= 1.0;
            for (int sample = 0; sample < 24; ++sample)
            {
                const double radiusFraction = (sample % 4 == 0) ? 1.0 : std::cbrt(Unit(stream));
                const DVec3 point = Add(centerRelative, Scaled(RandomUnitVector(stream), subject.Radius * radiusFraction));
                double px = 0.0, py = 0.0, d = 0.0;
                if (!ProjectToViewport(rect, cam, point, px, py, d) || px < 0.0 || px > 1600.0 || py < 0.0 || py > 900.0)
                {
                    message = "a point of the framed sphere projects outside the viewport";
                    return false;
                }
                const double ndcX = (px / 1600.0 - 0.5) * 2.0;
                const double ndcY = (0.5 - py / 900.0) * 2.0;
                // The viewport rectangle here is 16:9 while the camera aspect varies, so measure
                // the tight axis in the camera's own normalized coordinates.
                const double cameraNdcX = ndcX;
                const double cameraNdcY = ndcY;
                const double tightExtent = verticalIsTight ? std::abs(cameraNdcY) : std::abs(cameraNdcX);
                if (tightExtent > expectedMaximum + 1e-9 || expectedMaximum >= 1.0)
                {
                    message = "tight-axis extent " + std::to_string(tightExtent) + " exceeds the silhouette bound "
                        + std::to_string(expectedMaximum);
                    return false;
                }
            }
            return true;
        }, 300);
        check.Expect(generated, "framing is tangent to the tight frustum and keeps the sphere inside");

        // The near-plane clamp: a tiny subject at a wide field of view never ends up inside the near plane.
        {
            CameraBasis basis;
            ComputeCameraBasis({}, basis);
            FramingSolution solution;
            check.Expect(SolveFraming({ { 0, 0, 0 }, 0.5 }, basis, 170.0, 1.0, 0.5, kFramingMargin, solution), "wide fov solves");
            check.Expect(solution.Distance >= 0.5 + 2.0 * 0.5 - 1e-12, "distance keeps the sphere beyond the near plane");
        }
        // Invalid inputs fail closed.
        {
            CameraBasis basis;
            ComputeCameraBasis({}, basis);
            FramingSolution solution;
            check.Expect(!SolveFraming({ { 0, 0, 0 }, 1.0 }, basis, 0.0, 1.0, 0.1, 0.15, solution), "zero fov");
            check.Expect(!SolveFraming({ { 0, 0, 0 }, 1.0 }, basis, 60.0, 0.0, 0.1, 0.15, solution), "zero aspect");
            check.Expect(!SolveFraming({ { std::nan(""), 0, 0 }, 1.0 }, basis, 60.0, 1.0, 0.1, 0.15, solution), "NaN center");
            check.Expect(!SolveFraming({ { 0, 0, 0 }, -1.0 }, basis, 60.0, 1.0, 0.1, 0.15, solution), "negative radius");
        }
        return check.Ok;
    }

    bool TestPickingLatencyForLargeMeshes()
    {
        Checker check { "latency" };
        // 500 x 500 quads = 500,000 triangles; a plane facing the ray, so every triangle must be
        // visited (there is no acceleration structure) before the nearest hit is known.
        const GridMesh big = MakeGrid(500, 500, 0.01f, 0.0f);
        const TriangleMeshView view = big.View();
        check.Expect(view.TriangleCount() == 500000, "fixture has 500,000 triangles");

        double best = std::numeric_limits<double>::infinity();
        double hitDistance = 0.0;
        for (int run = 0; run < 5; ++run)
        {
            size_t tested = 0;
            const auto start = std::chrono::steady_clock::now();
            const bool hit = IntersectRayTriangles(view, { 2.5, 2.5, -3.0 }, { 0.0, 0.0, 1.0 }, hitDistance, tested);
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            best = std::min(best, elapsed);
            check.Expect(hit && tested == 500000, "every triangle is visited");
        }
        check.Near(hitDistance, 3.0, 1e-9, "distance to the plane");

        // Policy: a mesh exactly at the default budget (500,000 triangles) is refined; one above it is
        // picked by its oriented box alone, instantly, and the result says so.
        const Engine::Math::WorldGridPolicy policy;
        Engine::Math::SectorLocalPosition origin;
        origin.Local = { 2.5, 2.5, -3.0 };
        PickCandidate candidate = MakeCandidate(1, 0, 0, 0, 0, MakeBox(0, 0, 0, 5, 5, 0));
        candidate.Triangles = &view;
        const PickResult exact = PickNearest(origin, { 0.0, 0.0, 1.0 }, policy, { candidate }, {});
        check.Expect(exact.Hit && exact.Refined && exact.TrianglesTested == 500000 && kDefaultTriangleBudget == 500000,
            "a mesh exactly at the default budget is refined");
        const GridMesh over = MakeGrid(501, 500, 0.01f, 0.0f);
        const TriangleMeshView overView = over.View();
        check.Expect(overView.TriangleCount() == 501000 && overView.TriangleCount() > kDefaultTriangleBudget, "over-budget fixture");
        PickCandidate overCandidate = candidate;
        overCandidate.Triangles = &overView;
        const auto policyStart = std::chrono::steady_clock::now();
        const PickResult capped = PickNearest(origin, { 0.0, 0.0, 1.0 }, policy, { overCandidate }, {});
        const double policyMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - policyStart).count();
        check.Expect(capped.Hit && !capped.Refined && capped.TrianglesTested == 0,
            "a mesh above the default budget falls back to its oriented box and reports it");
        check.Near(capped.Distance, 3.0, 1e-9, "fallback distance is the box entry");

        // Half the budget, for scale.
        const GridMesh budgeted = MakeGrid(500, 250, 0.01f, 0.0f);
        const TriangleMeshView budgetedView = budgeted.View();
        double budgetedBest = std::numeric_limits<double>::infinity();
        for (int run = 0; run < 5; ++run)
        {
            size_t tested = 0;
            double distance = 0.0;
            const auto start = std::chrono::steady_clock::now();
            IntersectRayTriangles(budgetedView, { 2.5, 1.0, -3.0 }, { 0.0, 0.0, 1.0 }, distance, tested);
            budgetedBest = std::min(budgetedBest,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }

        std::cerr << "PickingLatencyV1 triangles500k_ms=" << best << " triangles250k_ms=" << budgetedBest
                  << " aboveBudgetFallback_ms=" << policyMilliseconds << " budget=" << kDefaultTriangleBudget
                  << " build=Debug-test-binary\n";
        // A generous bound that only catches an accidental quadratic or per-triangle allocation.
        check.Expect(best < 5000.0 && budgetedBest < 5000.0, "brute-force refinement stays within seconds even in a Debug binary");
        return check.Ok;
    }
}
