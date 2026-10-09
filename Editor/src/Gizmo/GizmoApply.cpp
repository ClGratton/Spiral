#include "GizmoApply.h"

#include "EulerRotation.h"
#include "SnapMath.h"

#include "Engine/Math/DVec3Ops.h"

#include <algorithm>
#include <cmath>

namespace Gizmo
{
    namespace
    {
        using Engine::Math::DVec3;
        using Engine::Math::SectorLocalPosition;

        constexpr double kPi = 3.14159265358979323846;
    }

    void BuildToolBasis(TransformTool tool, TransformSpace space, const Engine::Math::Vec3& rotationDegrees, DVec3 outBasis[3])
    {
        if (tool == TransformTool::Select || (tool != TransformTool::Scale && space == TransformSpace::World))
        {
            outBasis[0] = { 1.0, 0.0, 0.0 };
            outBasis[1] = { 0.0, 1.0, 0.0 };
            outBasis[2] = { 0.0, 0.0, 1.0 };
            return;
        }

        const Rotation3 rotation = RotationFromEulerDegrees(rotationDegrees);
        for (u32 axis = 0; axis < 3; ++axis)
        {
            if (!Engine::Math::TryNormalize(Row(rotation, axis), outBasis[axis]))
                outBasis[axis] = Row(Rotation3::Identity(), axis);
        }
    }

    bool ApplyTranslation(
        const GizmoTransform& start,
        const DVec3 basis[3],
        GizmoHandle handle,
        TransformSpace space,
        const DVec3& rawDelta,
        bool snapActive,
        const SnapSettings& snap,
        const Engine::Math::WorldGridPolicy& policy,
        GizmoTransform& outTransform)
    {
        const AxisMask mask = ConstrainedAxes(handle);
        const bool constrained[3] = { mask.X, mask.Y, mask.Z };
        if (!Engine::Math::AllFinite(rawDelta) || (!mask.X && !mask.Y && !mask.Z))
            return false;

        DVec3 delta;
        for (u32 axis = 0; axis < 3; ++axis)
        {
            if (!constrained[axis])
                continue;

            double component = Engine::Math::Dot(rawDelta, basis[axis]);
            if (snapActive && space == TransformSpace::Local && !TrySnapRelative(component, snap.TranslateStep, component))
                return false;

            delta = delta + basis[axis] * component;
        }

        const SectorLocalPosition moved {
            start.Position.Sector,
            { start.Position.Local.X + delta.X, start.Position.Local.Y + delta.Y, start.Position.Local.Z + delta.Z }
        };
        SectorLocalPosition position;
        if (!Engine::Math::TryNormalizeSectorLocal(moved, policy, position))
            return false;

        if (snapActive && space == TransformSpace::World
            && !TrySnapTranslation(position, mask, snap.TranslateStep, policy, position))
        {
            return false;
        }

        outTransform = start;
        outTransform.Position = position;
        return true;
    }

    bool ApplyRotation(
        const GizmoTransform& start,
        const DVec3& unitAxis,
        double radians,
        bool snapActive,
        const SnapSettings& snap,
        GizmoTransform& outTransform,
        double& outAppliedDegrees)
    {
        if (!std::isfinite(radians) || !Engine::Math::AllFinite(unitAxis))
            return false;

        double degrees = radians * (180.0 / kPi);
        if (snapActive && !TrySnapAngleDegrees(degrees, snap.RotateStepDegrees, degrees))
            return false;

        outTransform = start;
        outAppliedDegrees = degrees;
        if (degrees == 0.0)
            return true;

        const Rotation3 rotated = Compose(
            RotationFromEulerDegrees(start.RotationDegrees),
            RotationAboutAxis(unitAxis, degrees * (kPi / 180.0)));
        outTransform.RotationDegrees = EulerDegreesFromRotation(rotated, start.RotationDegrees);
        return true;
    }

    bool ApplyScale(
        const GizmoTransform& start,
        GizmoHandle handle,
        double factor,
        bool snapActive,
        const SnapSettings& snap,
        GizmoTransform& outTransform,
        double& outAppliedFactor)
    {
        const AxisMask mask = ConstrainedAxes(handle);
        if (!std::isfinite(factor) || factor <= 0.0 || (!mask.X && !mask.Y && !mask.Z))
            return false;

        if (snapActive && !TrySnapScaleFactor(factor, snap.ScaleStep, factor))
            return false;

        outTransform = start;
        outAppliedFactor = factor;
        if (factor == 1.0)
            return true;

        const auto scaled = [factor](float value)
        {
            return std::clamp(static_cast<float>(static_cast<double>(value) * factor), kMinimumScale, kMaximumScale);
        };
        if (mask.X) outTransform.Scale.X = scaled(start.Scale.X);
        if (mask.Y) outTransform.Scale.Y = scaled(start.Scale.Y);
        if (mask.Z) outTransform.Scale.Z = scaled(start.Scale.Z);
        return true;
    }
}
