#include "SnapMath.h"

#include <cmath>
#include <limits>

namespace Gizmo
{
    namespace
    {
        using Engine::Math::SectorLocalPosition;
        using Engine::Math::WorldGridPolicy;

        constexpr double kMaximumIndex = 4503599627370496.0;

        bool TryRoundTowardPositive(double value, double step, double& outIndex)
        {
            if (!std::isfinite(value) || !std::isfinite(step) || step <= 0.0)
                return false;

            const double ratio = value / step;
            if (!std::isfinite(ratio) || std::abs(ratio) >= kMaximumIndex)
                return false;

            // Decide the tie from the sign of an exact fused expression, never from
            // value / step: 2 * value - (2k + 1) * step is a single rounding of
            // the true quantity, so its sign (and exact zero) is the true one.
            double index = std::floor(ratio + 0.5);
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                if (std::fma(-(2.0 * index + 1.0), step, 2.0 * value) >= 0.0)
                    index += 1.0;
                else if (std::fma(-(2.0 * index - 1.0), step, 2.0 * value) < 0.0)
                    index -= 1.0;
                else
                    break;
            }

            outIndex = index;
            return true;
        }

        bool TrySnapAxis(
            i64 sector,
            double local,
            double step,
            const WorldGridPolicy& policy,
            bool aligned,
            i64& outSector,
            double& outLocal)
        {
            const double half = policy.SectorExtent * 0.5;
            double index = 0.0;
            if (!TryRoundTowardPositive(local, step, index))
                return false;

            if (aligned && std::fma(index, step, -half) >= 0.0)
            {
                SectorLocalPosition carried;
                const SectorLocalPosition axisOnly { { sector, 0, 0 }, { index * step, 0.0, 0.0 } };
                if (!Engine::Math::TryNormalizeSectorLocal(axisOnly, policy, carried))
                    return false;

                outSector = carried.Sector.X;
                outLocal = carried.Local.X;
                return true;
            }

            while (std::fma(index, step, -half) >= 0.0)
                index -= 1.0;
            while (std::fma(index, step, half) < 0.0)
                index += 1.0;

            double value = index * step;
            if (value >= half)
                value = std::nextafter(half, -std::numeric_limits<double>::infinity());
            outSector = sector;
            outLocal = value + 0.0;
            return true;
        }
    }

    bool TryRoundToStepIndex(double value, double step, TieRule rule, double& outIndex)
    {
        if (rule == TieRule::TowardPositive)
            return TryRoundTowardPositive(value, step, outIndex);

        double magnitudeIndex = 0.0;
        if (!TryRoundTowardPositive(std::abs(value), step, magnitudeIndex))
            return false;

        outIndex = value < 0.0 ? -magnitudeIndex : magnitudeIndex;
        return true;
    }

    bool TrySnapRelative(double value, double step, double& outSnapped)
    {
        double index = 0.0;
        if (!TryRoundToStepIndex(value, step, TieRule::AwayFromZero, index))
            return false;

        outSnapped = index * step + 0.0;
        return std::isfinite(outSnapped);
    }

    bool TrySnapAngleDegrees(double degrees, double stepDegrees, double& outSnapped)
    {
        return TrySnapRelative(degrees, stepDegrees, outSnapped);
    }

    bool TrySnapScaleFactor(double factor, double step, double& outSnapped)
    {
        double relative = 0.0;
        if (!std::isfinite(factor) || !TrySnapRelative(factor - 1.0, step, relative))
            return false;

        outSnapped = 1.0 + relative;
        return true;
    }

    bool IsSectorLatticeAligned(double step, const WorldGridPolicy& policy)
    {
        return Engine::Math::IsWorldGridPolicyValid(policy)
            && std::isfinite(step) && step > 0.0
            && std::fmod(policy.SectorExtent, step) == 0.0;
    }

    bool TrySnapTranslation(
        const SectorLocalPosition& position,
        AxisMask axes,
        double step,
        const WorldGridPolicy& policy,
        SectorLocalPosition& outSnapped)
    {
        if (!Engine::Math::IsCanonical(position, policy) || !std::isfinite(step) || step <= 0.0)
            return false;

        const bool aligned = IsSectorLatticeAligned(step, policy);
        SectorLocalPosition result = position;
        if (axes.X && !TrySnapAxis(position.Sector.X, position.Local.X, step, policy, aligned, result.Sector.X, result.Local.X))
            return false;
        if (axes.Y && !TrySnapAxis(position.Sector.Y, position.Local.Y, step, policy, aligned, result.Sector.Y, result.Local.Y))
            return false;
        if (axes.Z && !TrySnapAxis(position.Sector.Z, position.Local.Z, step, policy, aligned, result.Sector.Z, result.Local.Z))
            return false;

        outSnapped = result;
        return true;
    }
}
