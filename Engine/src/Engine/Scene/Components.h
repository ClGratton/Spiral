#pragma once

#include "Engine/Math/Math.h"
#include "Engine/Scene/Camera.h"

namespace Engine
{
    struct TransformComponent
    {
        Math::Vec3 Position = { 0.0f, 0.0f, 0.0f };
        Math::Vec3 RotationDegrees = { 0.0f, 0.0f, 0.0f };
        Math::Vec3 Scale = { 1.0f, 1.0f, 1.0f };

        Math::Mat4 GetTransformMatrix() const
        {
            return Math::Multiply(
                Math::Multiply(
                    Math::Scale(Scale),
                    Math::RotationYawPitchRoll(
                        Math::DegreesToRadians(RotationDegrees.Y),
                        Math::DegreesToRadians(RotationDegrees.X),
                        Math::DegreesToRadians(RotationDegrees.Z))),
                Math::Translation(Position));
        }
    };

    struct CameraComponent
    {
        CameraProjection Projection;
        bool Primary = true;
    };
}
