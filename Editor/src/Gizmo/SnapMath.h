#pragma once

#include "GizmoTypes.h"

namespace Gizmo
{
    enum class TieRule : u8
    {
        // Exact half-steps round up. Used for the absolute position lattice.
        TowardPositive,
        // Exact half-steps round away from zero, so +x and -x snap
        // symmetrically. Used for relative distances, angles and factors.
        AwayFromZero
    };

    // Integer k (as a double) with value within step/2 of k * step. The tie
    // rule is decided from the exact residual of the double operands, so it
    // holds even when value / step rounds. Fails for non-finite input,
    // step <= 0, or |value / step| >= 2^52.
    bool TryRoundToStepIndex(double value, double step, TieRule rule, double& outIndex);

    // k * step for the nearest k (AwayFromZero); never returns -0.
    bool TrySnapRelative(double value, double step, double& outSnapped);
    bool TrySnapAngleDegrees(double degrees, double stepDegrees, double& outSnapped);
    // 1 + round((factor - 1) / step) * step. The result is not clamped.
    bool TrySnapScaleFactor(double factor, double step, double& outSnapped);

    // True when the sector extent is an exact multiple of the step, so the
    // per-sector lattice coincides with the absolute world lattice and snapping
    // may carry into the next sector. When false the lattice restarts at each
    // sector centre and the UI must say so.
    bool IsSectorLatticeAligned(double step, const Engine::Math::WorldGridPolicy& policy);

    // Snaps each constrained axis of the canonical position to the lattice
    // {k * step} anchored at the sector centre. Rules:
    // - ties round toward +infinity;
    // - a snapped value that reaches +E/2 carries into the neighbouring sector
    //   (local -E/2 + remainder) only when the lattice is aligned, otherwise
    //   the nearest lattice point inside [-E/2, E/2) is used, so snapping is
    //   idempotent in both cases;
    // - unconstrained axes (sector and local) are returned bit-identical;
    // - the result is canonical.
    // Fails (out untouched) for a non-canonical input, an invalid step, or a
    // sector index that would overflow on carry.
    bool TrySnapTranslation(
        const Engine::Math::SectorLocalPosition& position,
        AxisMask axes,
        double step,
        const Engine::Math::WorldGridPolicy& policy,
        Engine::Math::SectorLocalPosition& outSnapped);
}
