#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Math/Math.h"

namespace Engine::Math
{
    inline constexpr u32 kWorldGridPolicyVersion = 1;

    enum class WorldOriginMode
    {
        ExactCamera,
        SectorSnapped
    };

    struct WorldGridPolicy
    {
        u32 Version = kWorldGridPolicyVersion;
        double SectorExtent = 4096.0;
        double OriginHysteresis = 256.0;
        WorldOriginMode OriginMode = WorldOriginMode::ExactCamera;
    };

    struct SectorIndex
    {
        i64 X = 0;
        i64 Y = 0;
        i64 Z = 0;

        bool operator==(const SectorIndex&) const = default;
    };

    struct SectorLocalPosition
    {
        SectorIndex Sector;
        DVec3 Local;
    };

    enum class SectorRangeStatus
    {
        Invalid,
        Empty,
        Finite,
        Oversized
    };

    struct SectorRange
    {
        SectorIndex Min;
        SectorIndex Max;
        u64 SectorCount = 0;
        SectorRangeStatus Status = SectorRangeStatus::Invalid;
    };

    bool IsWorldGridPolicyValid(const WorldGridPolicy& policy);
    bool IsCanonical(const SectorLocalPosition& position, const WorldGridPolicy& policy);
    bool TryDecomposeWorldPosition(
        const DVec3& worldPosition,
        const WorldGridPolicy& policy,
        SectorLocalPosition& outPosition);
    bool TryNormalizeSectorLocal(
        const SectorLocalPosition& position,
        const WorldGridPolicy& policy,
        SectorLocalPosition& outPosition);
    bool TryComposeApproximateWorldPosition(
        const SectorLocalPosition& position,
        const WorldGridPolicy& policy,
        DVec3& outWorldPosition);
    SectorRange GetOverlappingSectorRange(
        const DVec3& minInclusive,
        const DVec3& maxExclusive,
        const WorldGridPolicy& policy,
        u64 maxEnumeratedSectors);
}
