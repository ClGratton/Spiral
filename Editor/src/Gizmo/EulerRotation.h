#pragma once

#include "GizmoTypes.h"

namespace Gizmo
{
    // Upper 3x3 of an Engine::Math object matrix: row-major, row-vector
    // (v * R). Row i is the world direction of local axis i.
    struct Rotation3
    {
        double M[3][3] {};

        static Rotation3 Identity();
    };

    // R * S applies R first, then S, the same order as Math::Multiply.
    Rotation3 Compose(const Rotation3& first, const Rotation3& second);

    // Equals the upper 3x3 of Math::RotationYawPitchRoll(Y, X, Z) for
    // degrees = { pitch X, yaw Y, roll Z }, evaluated in double.
    Rotation3 RotationFromEulerDegrees(const Engine::Math::Vec3& degrees);

    // Rotation of row vectors by radians about a unit axis (right-hand
    // Rodrigues convention, identical to Math::RotationX/Y/Z for the
    // coordinate axes).
    Rotation3 RotationAboutAxis(const Engine::Math::DVec3& unitAxis, double radians);

    Engine::Math::DVec3 Row(const Rotation3& rotation, u32 index);

    // Inverse of RotationFromEulerDegrees. Of the two Euler triples that
    // describe the rotation, returns the one nearest previousDegrees after
    // unwrapping each angle by multiples of 360 so the Inspector never jumps.
    // Within ~6e-5 degrees of pitch +-90 (gimbal) roll is kept from
    // previousDegrees and yaw absorbs the remainder.
    Engine::Math::Vec3 EulerDegreesFromRotation(const Rotation3& rotation, const Engine::Math::Vec3& previousDegrees);
}
