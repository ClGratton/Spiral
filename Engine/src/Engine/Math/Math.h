#pragma once

#include "Engine/Core/Base.h"

namespace Engine::Math
{
    struct Vec3
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    struct DVec3
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
    };

    struct Mat4
    {
        float Values[16] {};

        static Mat4 Identity();
    };

    float DegreesToRadians(float degrees);
    Vec3 CameraRelative(const DVec3& worldPosition, const DVec3& translationOrigin);
    Mat4 Multiply(const Mat4& lhs, const Mat4& rhs);
    Mat4 Translation(const Vec3& translation);
    Mat4 Scale(const Vec3& scale);
    Mat4 RotationX(float radians);
    Mat4 RotationY(float radians);
    Mat4 RotationZ(float radians);
    Mat4 RotationYawPitchRoll(float yawRadians, float pitchRadians, float rollRadians);
    Mat4 PerspectiveLH(float verticalFovRadians, float aspectRatio, float nearPlane, float farPlane);
}
