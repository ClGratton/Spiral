#include "Engine/Math/WorldGrid.h"

#include <cmath>
#include <limits>

namespace Engine::Math
{
    namespace
    {
        bool IsFinite(const DVec3& value)
        {
            return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
        }

        bool TryAdd(i64 left, i64 right, i64& outValue)
        {
            if ((right > 0 && left > std::numeric_limits<i64>::max() - right)
                || (right < 0 && left < std::numeric_limits<i64>::min() - right))
            {
                return false;
            }

            outValue = left + right;
            return true;
        }

        bool TryDecomposeAxis(double world, double extent, i64& outSector, double& outLocal)
        {
            const double halfExtent = extent * 0.5;
            double local = std::remainder(world, extent);
            double sectorValue = std::round((world - local) / extent);
            if (local >= halfExtent)
            {
                local -= extent;
                sectorValue += 1.0;
            }
            else if (local < -halfExtent)
            {
                local += extent;
                sectorValue -= 1.0;
            }

            if (!std::isfinite(sectorValue)
                || sectorValue < static_cast<double>(std::numeric_limits<i64>::min())
                || sectorValue >= -static_cast<double>(std::numeric_limits<i64>::min()))
            {
                return false;
            }

            if (!std::isfinite(local) || local < -halfExtent || local >= halfExtent)
                return false;

            outSector = static_cast<i64>(sectorValue);
            outLocal = local;
            return true;
        }

        bool TryNormalizeAxis(i64 sector, double local, double extent, i64& outSector, double& outLocal)
        {
            if (!std::isfinite(local))
                return false;

            const double halfExtent = extent * 0.5;
            double normalizedLocal = std::remainder(local, extent);
            double carryValue = std::round((local - normalizedLocal) / extent);
            if (normalizedLocal >= halfExtent)
            {
                normalizedLocal -= extent;
                carryValue += 1.0;
            }
            else if (normalizedLocal < -halfExtent)
            {
                normalizedLocal += extent;
                carryValue -= 1.0;
            }

            if (!std::isfinite(carryValue)
                || carryValue < static_cast<double>(std::numeric_limits<i64>::min())
                || carryValue >= -static_cast<double>(std::numeric_limits<i64>::min()))
            {
                return false;
            }

            const i64 carry = static_cast<i64>(carryValue);
            if (!TryAdd(sector, carry, sector))
                return false;

            if (!std::isfinite(normalizedLocal) || normalizedLocal < -halfExtent || normalizedLocal >= halfExtent)
                return false;

            outSector = sector;
            outLocal = normalizedLocal;
            return true;
        }

        bool TryGetRelativeAxis(
            i64 sector,
            double local,
            i64 originSector,
            double originLocal,
            double extent,
            double& outRelative)
        {
            if (!std::isfinite(local) || !std::isfinite(originLocal))
                return false;

            i64 sectorDelta = 0;
            if ((originSector > 0 && sector < std::numeric_limits<i64>::min() + originSector)
                || (originSector < 0 && sector > std::numeric_limits<i64>::max() + originSector))
            {
                return false;
            }
            sectorDelta = sector - originSector;

            const double result = static_cast<double>(sectorDelta) * extent + (local - originLocal);
            if (!std::isfinite(result)
                || result < -static_cast<double>(std::numeric_limits<float>::max())
                || result > static_cast<double>(std::numeric_limits<float>::max()))
                return false;

            outRelative = result;
            return true;
        }

        bool TryCountRange(const SectorIndex& min, const SectorIndex& max, u64 limit, u64& outCount)
        {
            const long double spans[3] = {
                static_cast<long double>(max.X) - static_cast<long double>(min.X) + 1.0L,
                static_cast<long double>(max.Y) - static_cast<long double>(min.Y) + 1.0L,
                static_cast<long double>(max.Z) - static_cast<long double>(min.Z) + 1.0L
            };

            u64 count = 1;
            for (long double span : spans)
            {
                if (span <= 0.0L || span > static_cast<long double>(limit / count))
                    return false;
                count *= static_cast<u64>(span);
            }

            outCount = count;
            return true;
        }
    }

    bool IsWorldGridPolicyValid(const WorldGridPolicy& policy)
    {
        return policy.Version == kWorldGridPolicyVersion
            && std::isfinite(policy.SectorExtent)
            && policy.SectorExtent > 0.0
            && std::isfinite(policy.OriginHysteresis)
            && policy.OriginHysteresis >= 0.0
            && policy.OriginHysteresis < policy.SectorExtent * 0.5
            && (policy.OriginMode == WorldOriginMode::ExactCamera
                || policy.OriginMode == WorldOriginMode::SectorSnapped);
    }

    bool IsCanonical(const SectorLocalPosition& position, const WorldGridPolicy& policy)
    {
        return IsWorldGridPolicyValid(policy)
            && IsFinite(position.Local)
            && position.Local.X >= -policy.SectorExtent * 0.5 && position.Local.X < policy.SectorExtent * 0.5
            && position.Local.Y >= -policy.SectorExtent * 0.5 && position.Local.Y < policy.SectorExtent * 0.5
            && position.Local.Z >= -policy.SectorExtent * 0.5 && position.Local.Z < policy.SectorExtent * 0.5;
    }

    bool TryDecomposeWorldPosition(
        const DVec3& worldPosition,
        const WorldGridPolicy& policy,
        SectorLocalPosition& outPosition)
    {
        if (!IsWorldGridPolicyValid(policy) || !IsFinite(worldPosition))
            return false;

        SectorLocalPosition result;
        if (!TryDecomposeAxis(worldPosition.X, policy.SectorExtent, result.Sector.X, result.Local.X)
            || !TryDecomposeAxis(worldPosition.Y, policy.SectorExtent, result.Sector.Y, result.Local.Y)
            || !TryDecomposeAxis(worldPosition.Z, policy.SectorExtent, result.Sector.Z, result.Local.Z))
        {
            return false;
        }

        outPosition = result;
        return true;
    }

    bool TryNormalizeSectorLocal(
        const SectorLocalPosition& position,
        const WorldGridPolicy& policy,
        SectorLocalPosition& outPosition)
    {
        if (!IsWorldGridPolicyValid(policy))
            return false;

        SectorLocalPosition result;
        if (!TryNormalizeAxis(position.Sector.X, position.Local.X, policy.SectorExtent, result.Sector.X, result.Local.X)
            || !TryNormalizeAxis(position.Sector.Y, position.Local.Y, policy.SectorExtent, result.Sector.Y, result.Local.Y)
            || !TryNormalizeAxis(position.Sector.Z, position.Local.Z, policy.SectorExtent, result.Sector.Z, result.Local.Z))
        {
            return false;
        }

        outPosition = result;
        return true;
    }

    bool TryComposeApproximateWorldPosition(
        const SectorLocalPosition& position,
        const WorldGridPolicy& policy,
        DVec3& outWorldPosition)
    {
        if (!IsCanonical(position, policy))
            return false;

        const DVec3 result {
            static_cast<double>(position.Sector.X) * policy.SectorExtent + position.Local.X,
            static_cast<double>(position.Sector.Y) * policy.SectorExtent + position.Local.Y,
            static_cast<double>(position.Sector.Z) * policy.SectorExtent + position.Local.Z
        };
        if (!IsFinite(result))
            return false;

        outWorldPosition = result;
        return true;
    }

    bool TryGetSectorLocalRelativePosition(
        const SectorLocalPosition& position,
        const SectorLocalPosition& origin,
        const WorldGridPolicy& policy,
        DVec3& outRelativePosition)
    {
        if (!IsCanonical(position, policy) || !IsCanonical(origin, policy))
            return false;

        DVec3 result;
        if (!TryGetRelativeAxis(position.Sector.X, position.Local.X, origin.Sector.X, origin.Local.X, policy.SectorExtent, result.X)
            || !TryGetRelativeAxis(position.Sector.Y, position.Local.Y, origin.Sector.Y, origin.Local.Y, policy.SectorExtent, result.Y)
            || !TryGetRelativeAxis(position.Sector.Z, position.Local.Z, origin.Sector.Z, origin.Local.Z, policy.SectorExtent, result.Z))
        {
            return false;
        }

        outRelativePosition = result;
        return true;
    }

    SectorRange GetOverlappingSectorRange(
        const DVec3& minInclusive,
        const DVec3& maxExclusive,
        const WorldGridPolicy& policy,
        u64 maxEnumeratedSectors)
    {
        SectorRange range;
        if (!IsWorldGridPolicyValid(policy)
            || !IsFinite(minInclusive)
            || !IsFinite(maxExclusive)
            || maxEnumeratedSectors == 0)
        {
            return range;
        }

        if (maxExclusive.X <= minInclusive.X
            || maxExclusive.Y <= minInclusive.Y
            || maxExclusive.Z <= minInclusive.Z)
        {
            range.Status = SectorRangeStatus::Empty;
            return range;
        }

        const DVec3 adjustedMax {
            std::nextafter(maxExclusive.X, -std::numeric_limits<double>::infinity()),
            std::nextafter(maxExclusive.Y, -std::numeric_limits<double>::infinity()),
            std::nextafter(maxExclusive.Z, -std::numeric_limits<double>::infinity())
        };
        SectorLocalPosition minPosition;
        SectorLocalPosition maxPosition;
        if (!TryDecomposeWorldPosition(minInclusive, policy, minPosition)
            || !TryDecomposeWorldPosition(adjustedMax, policy, maxPosition))
        {
            return range;
        }

        range.Min = minPosition.Sector;
        range.Max = maxPosition.Sector;
        if (!TryCountRange(range.Min, range.Max, maxEnumeratedSectors, range.SectorCount))
        {
            range.SectorCount = 0;
            range.Status = SectorRangeStatus::Oversized;
            return range;
        }

        range.Status = SectorRangeStatus::Finite;
        return range;
    }
}
