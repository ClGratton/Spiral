#include "Engine/Math/Math.h"

#include <cmath>

namespace Engine::Math
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
    }

    Mat4 Mat4::Identity()
    {
        Mat4 result {};
        result.Values[0] = 1.0f;
        result.Values[5] = 1.0f;
        result.Values[10] = 1.0f;
        result.Values[15] = 1.0f;
        return result;
    }

    float DegreesToRadians(float degrees)
    {
        return degrees * (kPi / 180.0f);
    }

    Mat4 Multiply(const Mat4& lhs, const Mat4& rhs)
    {
        Mat4 result {};
        for (u32 row = 0; row < 4; ++row)
        {
            for (u32 column = 0; column < 4; ++column)
            {
                for (u32 index = 0; index < 4; ++index)
                    result.Values[row * 4 + column] += lhs.Values[row * 4 + index] * rhs.Values[index * 4 + column];
            }
        }

        return result;
    }

    Mat4 Translation(const Vec3& translation)
    {
        Mat4 result = Mat4::Identity();
        result.Values[12] = translation.X;
        result.Values[13] = translation.Y;
        result.Values[14] = translation.Z;
        return result;
    }

    Mat4 Scale(const Vec3& scale)
    {
        Mat4 result = Mat4::Identity();
        result.Values[0] = scale.X;
        result.Values[5] = scale.Y;
        result.Values[10] = scale.Z;
        return result;
    }

    Mat4 RotationX(float radians)
    {
        Mat4 result = Mat4::Identity();
        const float s = std::sin(radians);
        const float c = std::cos(radians);
        result.Values[5] = c;
        result.Values[6] = s;
        result.Values[9] = -s;
        result.Values[10] = c;
        return result;
    }

    Mat4 RotationY(float radians)
    {
        Mat4 result = Mat4::Identity();
        const float s = std::sin(radians);
        const float c = std::cos(radians);
        result.Values[0] = c;
        result.Values[2] = -s;
        result.Values[8] = s;
        result.Values[10] = c;
        return result;
    }

    Mat4 RotationZ(float radians)
    {
        Mat4 result = Mat4::Identity();
        const float s = std::sin(radians);
        const float c = std::cos(radians);
        result.Values[0] = c;
        result.Values[1] = s;
        result.Values[4] = -s;
        result.Values[5] = c;
        return result;
    }

    Mat4 RotationYawPitchRoll(float yawRadians, float pitchRadians, float rollRadians)
    {
        return Multiply(Multiply(RotationY(yawRadians), RotationX(pitchRadians)), RotationZ(rollRadians));
    }

    Mat4 PerspectiveLH(float verticalFovRadians, float aspectRatio, float nearPlane, float farPlane)
    {
        if (aspectRatio <= 0.0f)
            aspectRatio = 1.0f;
        if (nearPlane <= 0.0f)
            nearPlane = 0.01f;
        if (farPlane <= nearPlane)
            farPlane = nearPlane + 1.0f;

        Mat4 result {};
        const float yScale = 1.0f / std::tan(verticalFovRadians * 0.5f);
        const float xScale = yScale / aspectRatio;
        const float zRange = farPlane / (farPlane - nearPlane);

        result.Values[0] = xScale;
        result.Values[5] = yScale;
        result.Values[10] = zRange;
        result.Values[11] = 1.0f;
        result.Values[14] = -nearPlane * zRange;
        return result;
    }
}
