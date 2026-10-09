#pragma once

#include "GizmoTypes.h"
#include "SnapSettings.h"

namespace Gizmo
{
    // The basis a tool operates in: the world axes or the entity's local axes
    // (rows of its rotation). Scale is always local because a Transform has no
    // shear; Select has no basis and yields the world axes.
    void BuildToolBasis(
        TransformTool tool,
        TransformSpace space,
        const Engine::Math::Vec3& rotationDegrees,
        Engine::Math::DVec3 outBasis[3]);

    // The part of a drag below the pointer: applies a solved raw translation
    // (camera-relative metres along world axes) to the start transform.
    // The raw delta is projected onto the axes the handle constrains, then:
    // - World space: start + delta is normalized into the canonical sector
    //   range and, when snapping, snapped to the absolute sector-aware lattice
    //   on the constrained world axes only;
    // - Local space: each constrained component is snapped as a relative
    //   increment before the add, since a rotated axis never meets the lattice.
    // Rotation and scale are returned unchanged and unconstrained position axes
    // stay bit-identical. Fails (outTransform untouched) when the result
    // cannot be represented.
    bool ApplyTranslation(
        const GizmoTransform& start,
        const Engine::Math::DVec3 basis[3],
        GizmoHandle handle,
        TransformSpace space,
        const Engine::Math::DVec3& rawDelta,
        bool snapActive,
        const SnapSettings& snap,
        const Engine::Math::WorldGridPolicy& policy,
        GizmoTransform& outTransform);

    // Rotates the start orientation by radians about a unit world axis
    // (R1 = R0 * Rot(axis, angle)) and recovers the Euler triple nearest the
    // start. The angle is snapped in degrees relative to the start when
    // snapping. A zero applied angle returns the start rotation bit-identical.
    // outAppliedDegrees reports the angle after snapping.
    bool ApplyRotation(
        const GizmoTransform& start,
        const Engine::Math::DVec3& unitAxis,
        double radians,
        bool snapActive,
        const SnapSettings& snap,
        GizmoTransform& outTransform,
        double& outAppliedDegrees);

    // Multiplies the constrained scale components (one local axis, or all
    // three for Center) by the factor, snapped relatively when snapping, then
    // clamps each to [kMinimumScale, kMaximumScale]. A factor of exactly 1
    // returns the start scale bit-identical. outAppliedFactor reports the
    // factor after snapping and before clamping.
    bool ApplyScale(
        const GizmoTransform& start,
        GizmoHandle handle,
        double factor,
        bool snapActive,
        const SnapSettings& snap,
        GizmoTransform& outTransform,
        double& outAppliedFactor);
}
