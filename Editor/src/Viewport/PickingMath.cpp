#include "PickingMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace SpiralEditor::Picking
{
    namespace
    {
        constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;

        bool IsFiniteFloat3(const Vec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
        }

        DVec3 Cross(const DVec3& left, const DVec3& right)
        {
            return {
                left.Y * right.Z - left.Z * right.Y,
                left.Z * right.X - left.X * right.Z,
                left.X * right.Y - left.Y * right.X
            };
        }

        // Row vector times a row-major 3x3 matrix.
        DVec3 RowTimesMatrix(const DVec3& row, const double (&matrix)[9])
        {
            return {
                row.X * matrix[0] + row.Y * matrix[3] + row.Z * matrix[6],
                row.X * matrix[1] + row.Y * matrix[4] + row.Z * matrix[7],
                row.X * matrix[2] + row.Y * matrix[5] + row.Z * matrix[8]
            };
        }

        double Component(const DVec3& value, int axis)
        {
            return axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z);
        }

        struct BoxEntry
        {
            const PickCandidate* Candidate = nullptr;
            DVec3 Offset;
            LinearMap Map;
            double Enter = 0.0;
        };
    }

    bool IsFinite(const DVec3& value)
    {
        return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
    }

    DVec3 Add(const DVec3& left, const DVec3& right)
    {
        return { left.X + right.X, left.Y + right.Y, left.Z + right.Z };
    }

    DVec3 Subtract(const DVec3& left, const DVec3& right)
    {
        return { left.X - right.X, left.Y - right.Y, left.Z - right.Z };
    }

    DVec3 Scaled(const DVec3& value, double factor)
    {
        return { value.X * factor, value.Y * factor, value.Z * factor };
    }

    double Dot(const DVec3& left, const DVec3& right)
    {
        return left.X * right.X + left.Y * right.Y + left.Z * right.Z;
    }

    double Length(const DVec3& value)
    {
        return std::sqrt(Dot(value, value));
    }

    bool Normalize(const DVec3& value, DVec3& outNormalized)
    {
        const double length = Length(value);
        if (!(length > 0.0) || !std::isfinite(length))
            return false;
        outNormalized = Scaled(value, 1.0 / length);
        return true;
    }

    bool ComputeCameraBasis(const Vec3& rotationDegrees, CameraBasis& outBasis)
    {
        if (!IsFiniteFloat3(rotationDegrees))
            return false;

        // The renderer's view rotation, evaluated with the same float expression.
        const Engine::Math::Mat4 view = Engine::Math::RotationYawPitchRoll(
            -Engine::Math::DegreesToRadians(rotationDegrees.Y),
            -Engine::Math::DegreesToRadians(rotationDegrees.X),
            -Engine::Math::DegreesToRadians(rotationDegrees.Z));
        const float* v = view.Values;
        outBasis.Right = { v[0], v[4], v[8] };
        outBasis.Up = { v[1], v[5], v[9] };
        outBasis.Forward = { v[2], v[6], v[10] };
        return true;
    }

    bool ViewportPixelToRayDirection(const ViewportRect& rect, double pixelX, double pixelY,
        const ViewCamera& camera, DVec3& outDirection)
    {
        if (!(rect.Width > 0.0) || !(rect.Height > 0.0) || !std::isfinite(rect.X)
            || !std::isfinite(rect.Y) || !std::isfinite(rect.Width) || !std::isfinite(rect.Height)
            || !std::isfinite(pixelX) || !std::isfinite(pixelY)
            || !(camera.VerticalFovDegrees > 0.0) || !(camera.VerticalFovDegrees < 180.0)
            || !(camera.AspectRatio > 0.0) || !std::isfinite(camera.AspectRatio))
            return false;

        CameraBasis basis;
        if (!ComputeCameraBasis(camera.RotationDegrees, basis))
            return false;

        const double ndcX = (pixelX - rect.X) / rect.Width * 2.0 - 1.0;
        const double ndcY = 1.0 - (pixelY - rect.Y) / rect.Height * 2.0;
        const double tangent = std::tan(camera.VerticalFovDegrees * kRadiansPerDegree * 0.5);
        const DVec3 direction = Add(basis.Forward,
            Add(Scaled(basis.Right, ndcX * tangent * camera.AspectRatio),
                Scaled(basis.Up, ndcY * tangent)));
        return Normalize(direction, outDirection);
    }

    bool NormalizedToViewportPixel(const ViewportRect& rect, double normalizedX,
        double normalizedY, double& outPixelX, double& outPixelY)
    {
        if (!(rect.Width > 0.0) || !(rect.Height > 0.0) || !std::isfinite(normalizedX)
            || !std::isfinite(normalizedY) || normalizedX < 0.0 || normalizedX > 1.0
            || normalizedY < 0.0 || normalizedY > 1.0)
            return false;
        outPixelX = rect.X + normalizedX * rect.Width;
        outPixelY = rect.Y + normalizedY * rect.Height;
        return true;
    }

    bool ProjectToViewport(const ViewportRect& rect, const ViewCamera& camera,
        const DVec3& relativePoint, double& outPixelX, double& outPixelY, double& outDepth)
    {
        if (!(rect.Width > 0.0) || !(rect.Height > 0.0) || !IsFinite(relativePoint)
            || !(camera.VerticalFovDegrees > 0.0) || !(camera.VerticalFovDegrees < 180.0)
            || !(camera.AspectRatio > 0.0))
            return false;
        CameraBasis basis;
        if (!ComputeCameraBasis(camera.RotationDegrees, basis))
            return false;
        const double depth = Dot(relativePoint, basis.Forward);
        if (!(depth > 0.0))
            return false;

        const double tangent = std::tan(camera.VerticalFovDegrees * kRadiansPerDegree * 0.5);
        const double ndcX = Dot(relativePoint, basis.Right) / (depth * tangent * camera.AspectRatio);
        const double ndcY = Dot(relativePoint, basis.Up) / (depth * tangent);
        outPixelX = rect.X + (ndcX * 0.5 + 0.5) * rect.Width;
        outPixelY = rect.Y + (0.5 - ndcY * 0.5) * rect.Height;
        outDepth = depth;
        return std::isfinite(outPixelX) && std::isfinite(outPixelY);
    }

    bool MakeLinearMap(const Vec3& rotationDegrees, const Vec3& scale, LinearMap& outMap)
    {
        if (!IsFiniteFloat3(rotationDegrees) || !IsFiniteFloat3(scale))
            return false;

        const Engine::Math::Mat4 object = Engine::Math::Multiply(
            Engine::Math::Scale(scale),
            Engine::Math::RotationYawPitchRoll(
                Engine::Math::DegreesToRadians(rotationDegrees.Y),
                Engine::Math::DegreesToRadians(rotationDegrees.X),
                Engine::Math::DegreesToRadians(rotationDegrees.Z)));
        LinearMap map;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
                map.Matrix[row * 3 + column] = object.Values[row * 4 + column];
        }

        const double* m = map.Matrix;
        const double cofactor00 = m[4] * m[8] - m[5] * m[7];
        const double cofactor01 = m[5] * m[6] - m[3] * m[8];
        const double cofactor02 = m[3] * m[7] - m[4] * m[6];
        const double determinant = m[0] * cofactor00 + m[1] * cofactor01 + m[2] * cofactor02;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-30)
            return false;

        const double inverseDeterminant = 1.0 / determinant;
        map.Inverse[0] = cofactor00 * inverseDeterminant;
        map.Inverse[1] = (m[2] * m[7] - m[1] * m[8]) * inverseDeterminant;
        map.Inverse[2] = (m[1] * m[5] - m[2] * m[4]) * inverseDeterminant;
        map.Inverse[3] = cofactor01 * inverseDeterminant;
        map.Inverse[4] = (m[0] * m[8] - m[2] * m[6]) * inverseDeterminant;
        map.Inverse[5] = (m[2] * m[3] - m[0] * m[5]) * inverseDeterminant;
        map.Inverse[6] = cofactor02 * inverseDeterminant;
        map.Inverse[7] = (m[1] * m[6] - m[0] * m[7]) * inverseDeterminant;
        map.Inverse[8] = (m[0] * m[4] - m[1] * m[3]) * inverseDeterminant;
        for (const double value : map.Inverse)
        {
            if (!std::isfinite(value))
                return false;
        }
        map.Valid = true;
        outMap = map;
        return true;
    }

    DVec3 TransformPoint(const LinearMap& map, const DVec3& localPoint, const DVec3& offset)
    {
        return Add(RowTimesMatrix(localPoint, map.Matrix), offset);
    }

    DVec3 TransformByInverse(const LinearMap& map, const DVec3& vector)
    {
        return RowTimesMatrix(vector, map.Inverse);
    }

    bool IntersectRayOrientedBox(const DVec3& rayOrigin, const DVec3& rayDirection,
        const DVec3& objectOffset, const LinearMap& map, const LocalBox& box, BoxHit& outHit)
    {
        if (!map.Valid || !IsFinite(rayOrigin) || !IsFinite(rayDirection) || !IsFinite(objectOffset))
            return false;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(box.Min[axis]) || !std::isfinite(box.Max[axis])
                || box.Min[axis] > box.Max[axis])
                return false;
        }

        const DVec3 origin = TransformByInverse(map, Subtract(rayOrigin, objectOffset));
        const DVec3 direction = TransformByInverse(map, rayDirection);
        double tEnter = -std::numeric_limits<double>::infinity();
        double tExit = std::numeric_limits<double>::infinity();
        for (int axis = 0; axis < 3; ++axis)
        {
            const double o = Component(origin, axis);
            const double d = Component(direction, axis);
            const double low = box.Min[axis];
            const double high = box.Max[axis];
            if (d == 0.0)
            {
                if (o < low || o > high)
                    return false;
                continue;
            }
            double slabEnter = (low - o) / d;
            double slabExit = (high - o) / d;
            if (slabEnter > slabExit)
                std::swap(slabEnter, slabExit);
            tEnter = std::max(tEnter, slabEnter);
            tExit = std::min(tExit, slabExit);
            if (tEnter > tExit)
                return false;
        }
        if (!(tExit >= 0.0))
            return false;
        outHit.Enter = std::max(tEnter, 0.0);
        outHit.Exit = tExit;
        return std::isfinite(outHit.Enter);
    }

    bool IntersectRayTriangles(const TriangleMeshView& mesh, const DVec3& localOrigin,
        const DVec3& localDirection, double& outDistance, size_t& inOutTrianglesTested)
    {
        if (!mesh.Positions || !mesh.Indices || mesh.IndexCount < 3)
            return false;

        bool found = false;
        double best = std::numeric_limits<double>::infinity();
        const size_t triangleCount = mesh.IndexCount / 3;
        for (size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            const Engine::u32 i0 = mesh.Indices[triangle * 3];
            const Engine::u32 i1 = mesh.Indices[triangle * 3 + 1];
            const Engine::u32 i2 = mesh.Indices[triangle * 3 + 2];
            if (i0 >= mesh.VertexCount || i1 >= mesh.VertexCount || i2 >= mesh.VertexCount)
                continue;
            ++inOutTrianglesTested;

            const float* a = mesh.Positions + static_cast<size_t>(i0) * 3;
            const float* b = mesh.Positions + static_cast<size_t>(i1) * 3;
            const float* c = mesh.Positions + static_cast<size_t>(i2) * 3;
            const DVec3 edge1 { double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2] };
            const DVec3 edge2 { double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2] };
            const DVec3 p = Cross(localDirection, edge2);
            const double determinant = Dot(edge1, p);
            if (determinant == 0.0 || !std::isfinite(determinant))
                continue;

            const double inverseDeterminant = 1.0 / determinant;
            const DVec3 toOrigin { localOrigin.X - a[0], localOrigin.Y - a[1], localOrigin.Z - a[2] };
            const double u = Dot(toOrigin, p) * inverseDeterminant;
            if (u < 0.0 || u > 1.0)
                continue;
            const DVec3 q = Cross(toOrigin, edge1);
            const double v = Dot(localDirection, q) * inverseDeterminant;
            if (v < 0.0 || u + v > 1.0)
                continue;
            const double t = Dot(edge2, q) * inverseDeterminant;
            if (t >= 0.0 && t < best)
            {
                best = t;
                found = true;
            }
        }
        if (found)
            outDistance = best;
        return found;
    }

    PickResult PickNearest(const Engine::Math::SectorLocalPosition& rayOrigin,
        const DVec3& rayDirection, const Engine::Math::WorldGridPolicy& policy,
        const std::vector<PickCandidate>& candidates, const PickOptions& options)
    {
        PickResult result;
        result.Candidates = candidates.size();
        DVec3 direction;
        if (!IsFinite(rayDirection) || !Normalize(rayDirection, direction))
            return result;

        std::vector<BoxEntry> entries;
        entries.reserve(candidates.size());
        for (const PickCandidate& candidate : candidates)
        {
            BoxEntry entry;
            entry.Candidate = &candidate;
            if (!MakeLinearMap(candidate.RotationDegrees, candidate.Scale, entry.Map)
                || !Engine::Math::TryGetSectorLocalRelativePosition(
                    candidate.Position, rayOrigin, policy, entry.Offset))
                continue;
            BoxHit hit;
            if (!IntersectRayOrientedBox({}, direction, entry.Offset, entry.Map, candidate.Box, hit))
                continue;
            entry.Enter = hit.Enter;
            entries.push_back(entry);
        }
        result.BoxHits = entries.size();
        std::sort(entries.begin(), entries.end(), [](const BoxEntry& left, const BoxEntry& right)
        {
            if (left.Enter != right.Enter)
                return left.Enter < right.Enter;
            return left.Candidate->Order < right.Candidate->Order;
        });

        for (const BoxEntry& entry : entries)
        {
            if (result.Hit && entry.Enter > result.Distance)
                break;

            double distance = entry.Enter;
            bool refined = false;
            const TriangleMeshView* mesh = entry.Candidate->Triangles;
            if (mesh && mesh->TriangleCount() > 0 && mesh->TriangleCount() <= options.TriangleBudget)
            {
                const DVec3 localOrigin = TransformByInverse(entry.Map, Scaled(entry.Offset, -1.0));
                const DVec3 localDirection = TransformByInverse(entry.Map, direction);
                if (!IntersectRayTriangles(*mesh, localOrigin, localDirection, distance,
                        result.TrianglesTested))
                    continue;
                refined = true;
            }

            const bool better = !result.Hit || distance < result.Distance
                || (distance == result.Distance && entry.Candidate->Order < result.Order);
            if (better)
            {
                result.Hit = true;
                result.EntityId = entry.Candidate->EntityId;
                result.Distance = distance;
                result.Refined = refined;
                result.Order = entry.Candidate->Order;
            }
        }
        return result;
    }

    void OrientedBoxCorners(const DVec3& objectOffset, const LinearMap& map,
        const LocalBox& box, DVec3 (&outCorners)[8])
    {
        for (int corner = 0; corner < 8; ++corner)
        {
            const DVec3 local {
                (corner & 1) ? box.Max[0] : box.Min[0],
                (corner & 2) ? box.Max[1] : box.Min[1],
                (corner & 4) ? box.Max[2] : box.Min[2]
            };
            outCorners[corner] = TransformPoint(map, local, objectOffset);
        }
    }

    bool EnclosingSphere(const std::vector<DVec3>& points, BoundingSphere& outSphere)
    {
        if (points.empty())
            return false;
        DVec3 minimum = points.front();
        DVec3 maximum = points.front();
        for (const DVec3& point : points)
        {
            if (!IsFinite(point))
                return false;
            minimum = { std::min(minimum.X, point.X), std::min(minimum.Y, point.Y),
                std::min(minimum.Z, point.Z) };
            maximum = { std::max(maximum.X, point.X), std::max(maximum.Y, point.Y),
                std::max(maximum.Z, point.Z) };
        }
        const DVec3 center { (minimum.X + maximum.X) * 0.5, (minimum.Y + maximum.Y) * 0.5,
            (minimum.Z + maximum.Z) * 0.5 };
        double radius = 0.0;
        for (const DVec3& point : points)
            radius = std::max(radius, Length(Subtract(point, center)));
        outSphere.Center = center;
        outSphere.Radius = radius;
        return true;
    }

    bool SolveFraming(const BoundingSphere& subject, const CameraBasis& basis,
        double verticalFovDegrees, double aspectRatio, double nearClip, double margin,
        FramingSolution& outSolution)
    {
        if (!IsFinite(subject.Center) || !(subject.Radius >= 0.0) || !std::isfinite(subject.Radius)
            || !(verticalFovDegrees > 0.0) || !(verticalFovDegrees < 180.0)
            || !(aspectRatio > 0.0) || !std::isfinite(aspectRatio) || !(margin >= 0.0)
            || !std::isfinite(margin) || !(nearClip >= 0.0) || !std::isfinite(nearClip))
            return false;

        const double radius = std::max(subject.Radius, 1e-6);
        const double verticalHalf = verticalFovDegrees * kRadiansPerDegree * 0.5;
        const double horizontalHalf = std::atan(std::tan(verticalHalf) * aspectRatio);
        const double tightHalf = std::min(verticalHalf, horizontalHalf);
        double distance = radius * (1.0 + margin) / std::sin(tightHalf);
        // Keep the whole sphere in front of the near plane even at extreme fields of view.
        distance = std::max(distance, radius + 2.0 * nearClip);
        if (!std::isfinite(distance))
            return false;

        outSolution.Subject = { subject.Center, radius };
        outSolution.Distance = distance;
        outSolution.CameraOffset = Subtract(subject.Center, Scaled(basis.Forward, distance));
        return true;
    }
}
