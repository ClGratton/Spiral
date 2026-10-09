#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"
#include "Engine/Math/WorldGrid.h"

#include <cstddef>
#include <vector>

// Pure viewport picking and framing math for the Editor. It uses Engine::Math
// types only: no ImGui, GLFW, Scene, or renderer include, so it compiles into
// EngineTests and every convention below is checked against the renderer's own
// matrices there.
//
// Conventions, taken from Engine::BuildCameraView and TransformComponent:
// - Row vectors, left-handed, +Z forward, +Y up, +X right.
// - The camera transform stores rotation as (pitch X, yaw Y, roll Z) degrees
//   and the renderer rotates world points by View = T(-camera) * R(-yaw,-pitch,-roll)
//   with R = RotationYawPitchRoll. A view-space direction therefore maps back to
//   world space through the transpose of that rotation (its columns are the
//   camera right, up, and forward axes).
// - An object matrix is Scale * RotationYawPitchRoll(Y, X, Z) * Translation, so
//   a local point maps to p_local * L + offset with L = Scale * R.
// - Pixel coordinates have their origin at the top-left of the viewport image,
//   x to the right and y down, exactly like the ImGui image rectangle.
// - Positions are carried camera-relative in double precision. Sector-local
//   positions are differenced with Engine::Math::TryGetSectorLocalRelativePosition
//   so a far-from-origin scene never composes an absolute double.
namespace SpiralEditor::Picking
{
    using Engine::Math::DVec3;
    using Engine::Math::Vec3;

    // Brute-force triangle refinement is attempted only for meshes at or below
    // this many triangles. A larger mesh is picked by its oriented bounding box
    // alone and the result says so (PickResult::Refined == false). The number is
    // an Editor policy, not a format limit; the measured latency that justifies
    // it is reported by the pick latency test.
    inline constexpr size_t kDefaultTriangleBudget = 500000;
    // Fraction of the tight screen dimension left empty around a framed subject.
    inline constexpr double kFramingMargin = 0.15;
    // Bounding-sphere radius used for an entity that has no mesh bounds.
    inline constexpr double kDefaultFramingRadius = 2.0;

    struct ViewportRect
    {
        double X = 0.0;
        double Y = 0.0;
        double Width = 0.0;
        double Height = 0.0;
    };

    struct ViewCamera
    {
        // Stored on the camera transform: X is pitch, Y is yaw, Z is roll.
        Vec3 RotationDegrees;
        double VerticalFovDegrees = 60.0;
        double AspectRatio = 16.0 / 9.0;
    };

    struct CameraBasis
    {
        DVec3 Right { 1.0, 0.0, 0.0 };
        DVec3 Up { 0.0, 1.0, 0.0 };
        DVec3 Forward { 0.0, 0.0, 1.0 };
    };

    // Axis-aligned box in object space (the mesh bounds).
    struct LocalBox
    {
        float Min[3] {};
        float Max[3] {};
    };

    // 3x3 part of an object matrix, kept in double with its inverse. Built from
    // the same float expression the renderer uses so both agree to the bit.
    struct LinearMap
    {
        double Matrix[9] {};
        double Inverse[9] {};
        bool Valid = false;
    };

    // Vertex positions are tightly packed xyz floats; Indices are triangle lists
    // (IndexCount is a multiple of three). Out-of-range indices are skipped.
    struct TriangleMeshView
    {
        const float* Positions = nullptr;
        size_t VertexCount = 0;
        const Engine::u32* Indices = nullptr;
        size_t IndexCount = 0;

        size_t TriangleCount() const { return IndexCount / 3; }
    };

    struct BoxHit
    {
        double Enter = 0.0;
        double Exit = 0.0;
    };

    struct PickCandidate
    {
        Engine::u32 EntityId = 0;
        // Stable tie-break: lower Order wins when two hits are at the same distance.
        Engine::u32 Order = 0;
        Engine::Math::SectorLocalPosition Position;
        Vec3 RotationDegrees;
        Vec3 Scale { 1.0f, 1.0f, 1.0f };
        LocalBox Box;
        // Optional exact geometry; nullptr means the oriented box is the pick shape.
        const TriangleMeshView* Triangles = nullptr;
    };

    struct PickOptions
    {
        size_t TriangleBudget = kDefaultTriangleBudget;
    };

    struct PickResult
    {
        bool Hit = false;
        Engine::u32 EntityId = 0;
        double Distance = 0.0;
        // True when Distance is a triangle hit, false when it is the box entry.
        bool Refined = false;
        // Tie-break order of the winning candidate.
        Engine::u32 Order = 0;
        size_t Candidates = 0;
        size_t BoxHits = 0;
        size_t TrianglesTested = 0;
    };

    struct BoundingSphere
    {
        DVec3 Center;
        double Radius = 0.0;
    };

    struct FramingSolution
    {
        BoundingSphere Subject;
        // Distance from the camera position to Subject.Center after framing.
        double Distance = 0.0;
        // Where the camera moves, relative to where it is now.
        DVec3 CameraOffset;
    };

    bool IsFinite(const DVec3& value);
    DVec3 Add(const DVec3& left, const DVec3& right);
    DVec3 Subtract(const DVec3& left, const DVec3& right);
    DVec3 Scaled(const DVec3& value, double factor);
    double Dot(const DVec3& left, const DVec3& right);
    double Length(const DVec3& value);
    bool Normalize(const DVec3& value, DVec3& outNormalized);

    // Camera axes in world space. Fails for non-finite angles.
    bool ComputeCameraBasis(const Vec3& rotationDegrees, CameraBasis& outBasis);

    // Unit world-space direction of the ray through viewport pixel (pixelX, pixelY).
    // The ray starts at the camera position. Fails for an empty rectangle, a
    // non-finite input, or a field of view outside (0, 180) degrees.
    bool ViewportPixelToRayDirection(const ViewportRect& rect, double pixelX, double pixelY,
        const ViewCamera& camera, DVec3& outDirection);
    // Normalized coordinates in [0, 1] (origin top-left) to pixels of `rect`.
    bool NormalizedToViewportPixel(const ViewportRect& rect, double normalizedX,
        double normalizedY, double& outPixelX, double& outPixelY);
    // Inverse of ViewportPixelToRayDirection for a point given relative to the
    // camera position. outDepth is the distance along the camera forward axis;
    // fails for a point at or behind the camera plane.
    bool ProjectToViewport(const ViewportRect& rect, const ViewCamera& camera,
        const DVec3& relativePoint, double& outPixelX, double& outPixelY, double& outDepth);

    bool MakeLinearMap(const Vec3& rotationDegrees, const Vec3& scale, LinearMap& outMap);
    // Row-vector transforms through the map.
    DVec3 TransformPoint(const LinearMap& map, const DVec3& localPoint, const DVec3& offset);
    DVec3 TransformByInverse(const LinearMap& map, const DVec3& vector);

    // Slab test of a ray against `box` placed by `map` and `objectOffset`, both
    // relative to the ray origin frame. Enter is clamped to zero when the origin
    // is inside the box. A hit entirely behind the origin is a miss.
    bool IntersectRayOrientedBox(const DVec3& rayOrigin, const DVec3& rayDirection,
        const DVec3& objectOffset, const LinearMap& map, const LocalBox& box, BoxHit& outHit);

    // Nearest double-sided triangle along the ray in object space. The ray is
    // expressed in the same parameterization as the world ray (the direction is
    // not renormalized), so the returned distance is directly comparable.
    bool IntersectRayTriangles(const TriangleMeshView& mesh, const DVec3& localOrigin,
        const DVec3& localDirection, double& outDistance, size_t& inOutTrianglesTested);

    // Nearest hit among `candidates` along the ray from `rayOrigin` in direction
    // `rayDirection`: smaller distance first, then lower Order. Boxes are tested
    // first; triangle refinement runs only for meshes within the budget and only
    // for boxes that could still beat the best hit found so far.
    PickResult PickNearest(const Engine::Math::SectorLocalPosition& rayOrigin,
        const DVec3& rayDirection, const Engine::Math::WorldGridPolicy& policy,
        const std::vector<PickCandidate>& candidates, const PickOptions& options);

    // Eight corners of the oriented box, relative to the same frame as `objectOffset`.
    void OrientedBoxCorners(const DVec3& objectOffset, const LinearMap& map,
        const LocalBox& box, DVec3 (&outCorners)[8]);
    // Center of the points' axis-aligned box and the largest distance from that
    // center to a point. Rotation-invariant for a box's own corners.
    bool EnclosingSphere(const std::vector<DVec3>& points, BoundingSphere& outSphere);
    // Places the camera so the sphere, enlarged by `margin`, fits the tighter of
    // the vertical and horizontal fields of view, looking along the current
    // forward axis, and never puts the sphere closer than the near plane.
    bool SolveFraming(const BoundingSphere& subject, const CameraBasis& basis,
        double verticalFovDegrees, double aspectRatio, double nearClip, double margin,
        FramingSolution& outSolution);
}
