#include "GizmoProjection.h"

#include "Engine/Math/DVec3Ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Gizmo
{
    namespace
    {
        using Engine::Math::DVec3;
        using Engine::Math::Mat4;

        constexpr double kMinimumDepth = 1e-9;

        bool AllFinite(const Mat4& matrix)
        {
            for (float value : matrix.Values)
            {
                if (!std::isfinite(value))
                    return false;
            }

            return true;
        }

        void MultiplyRowMajor(const double* lhs, const double* rhs, double* out)
        {
            for (int row = 0; row < 4; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    double sum = 0.0;
                    for (int index = 0; index < 4; ++index)
                        sum += lhs[row * 4 + index] * rhs[index * 4 + column];
                    out[row * 4 + column] = sum;
                }
            }
        }

        // Gauss-Jordan elimination with partial pivoting.
        bool Invert(const double* input, double* output)
        {
            double work[4][8] {};
            for (int row = 0; row < 4; ++row)
            {
                for (int column = 0; column < 4; ++column)
                    work[row][column] = input[row * 4 + column];
                work[row][4 + row] = 1.0;
            }

            for (int column = 0; column < 4; ++column)
            {
                int pivot = column;
                for (int row = column + 1; row < 4; ++row)
                {
                    if (std::abs(work[row][column]) > std::abs(work[pivot][column]))
                        pivot = row;
                }

                if (!(std::abs(work[pivot][column]) > 1e-14))
                    return false;

                if (pivot != column)
                {
                    for (int index = 0; index < 8; ++index)
                        std::swap(work[pivot][index], work[column][index]);
                }

                const double inverse = 1.0 / work[column][column];
                for (int index = 0; index < 8; ++index)
                    work[column][index] *= inverse;

                for (int row = 0; row < 4; ++row)
                {
                    if (row == column)
                        continue;

                    const double factor = work[row][column];
                    for (int index = 0; index < 8; ++index)
                        work[row][index] -= factor * work[column][index];
                }
            }

            for (int row = 0; row < 4; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    output[row * 4 + column] = work[row][4 + column];
                    if (!std::isfinite(output[row * 4 + column]))
                        return false;
                }
            }

            return true;
        }

        struct Clip
        {
            double X;
            double Y;
            double Z;
            double W;
        };

        Clip Transform(const double* matrix, double x, double y, double z, double w)
        {
            return {
                x * matrix[0] + y * matrix[4] + z * matrix[8] + w * matrix[12],
                x * matrix[1] + y * matrix[5] + z * matrix[9] + w * matrix[13],
                x * matrix[2] + y * matrix[6] + z * matrix[10] + w * matrix[14],
                x * matrix[3] + y * matrix[7] + z * matrix[11] + w * matrix[15]
            };
        }
    }

    bool TryBuildGizmoView(const Mat4& view, const Mat4& projection, const ViewportRect& viewport, GizmoView& outView)
    {
        if (!AllFinite(view) || !AllFinite(projection)
            || !std::isfinite(viewport.X) || !std::isfinite(viewport.Y)
            || !(viewport.Width > 0.0) || !(viewport.Height > 0.0)
            || !std::isfinite(viewport.Width) || !std::isfinite(viewport.Height))
        {
            return false;
        }

        const float* p = projection.Values;
        if (std::abs(p[11] - 1.0f) > 1e-5f || p[15] != 0.0f || !(p[0] > 0.0f) || !(p[5] > 0.0f) || !(p[10] > 0.0f))
            return false;

        const double nearDepth = -static_cast<double>(p[14]) / static_cast<double>(p[10]);
        if (!(nearDepth > 0.0) || !std::isfinite(nearDepth))
            return false;

        GizmoView result;
        result.View = view;
        result.Projection = projection;
        result.Viewport = viewport;
        double viewD[16];
        double projectionD[16];
        for (int index = 0; index < 16; ++index)
        {
            viewD[index] = view.Values[index];
            projectionD[index] = projection.Values[index];
        }

        MultiplyRowMajor(viewD, projectionD, result.ViewProjection);
        double inverseView[16];
        if (!Invert(result.ViewProjection, result.InverseViewProjection) || !Invert(viewD, inverseView))
            return false;

        result.Eye = { inverseView[12], inverseView[13], inverseView[14] };
        if (!Engine::Math::TryNormalize({ inverseView[8], inverseView[9], inverseView[10] }, result.Forward)
            || !Engine::Math::AllFinite(result.Eye))
        {
            return false;
        }

        result.NearDepth = nearDepth;
        result.YScale = p[5];
        outView = result;
        return true;
    }

    bool SameView(const GizmoView& lhs, const GizmoView& rhs)
    {
        return std::memcmp(lhs.View.Values, rhs.View.Values, sizeof(lhs.View.Values)) == 0
            && std::memcmp(lhs.Projection.Values, rhs.Projection.Values, sizeof(lhs.Projection.Values)) == 0
            && lhs.Viewport == rhs.Viewport;
    }

    bool ProjectToScreen(const GizmoView& view, const DVec3& point, ProjectedPoint& outProjected)
    {
        if (!Engine::Math::AllFinite(point))
            return false;

        const Clip clip = Transform(view.ViewProjection, point.X, point.Y, point.Z, 1.0);
        if (!(clip.W > kMinimumDepth) || !std::isfinite(clip.W))
            return false;

        const double ndcX = clip.X / clip.W;
        const double ndcY = clip.Y / clip.W;
        outProjected.Screen = {
            view.Viewport.X + (ndcX * 0.5 + 0.5) * view.Viewport.Width,
            view.Viewport.Y + (0.5 - ndcY * 0.5) * view.Viewport.Height
        };
        outProjected.Depth = clip.W;
        outProjected.NdcZ = clip.Z / clip.W;
        return std::isfinite(outProjected.Screen.X) && std::isfinite(outProjected.Screen.Y);
    }

    bool TryScreenToRay(const GizmoView& view, const ScreenPoint& pixel, Engine::Math::Ray& outRay)
    {
        if (!std::isfinite(pixel.X) || !std::isfinite(pixel.Y))
            return false;

        const double ndcX = ((pixel.X - view.Viewport.X) / view.Viewport.Width - 0.5) * 2.0;
        const double ndcY = (0.5 - (pixel.Y - view.Viewport.Y) / view.Viewport.Height) * 2.0;
        const Clip nearClip = Transform(view.InverseViewProjection, ndcX, ndcY, 0.0, 1.0);
        if (!(std::abs(nearClip.W) > 1e-12))
            return false;

        const DVec3 nearPoint { nearClip.X / nearClip.W, nearClip.Y / nearClip.W, nearClip.Z / nearClip.W };
        DVec3 direction;
        if (!Engine::Math::TryNormalize(nearPoint - view.Eye, direction))
            return false;

        outRay = { view.Eye, direction };
        return true;
    }

    bool TryComputeGizmoLength(const GizmoView& view, const DVec3& origin, double targetPixels, double& outLength)
    {
        ProjectedPoint projected;
        if (!ProjectToScreen(view, origin, projected) || !(targetPixels > 0.0))
            return false;

        const double depth = std::max(projected.Depth, view.NearDepth);
        outLength = targetPixels * depth * 2.0 / (view.YScale * view.Viewport.Height);
        return std::isfinite(outLength) && outLength > 0.0;
    }
}
