#pragma once

#include "Engine/Math/Math.h"

#include <cmath>

namespace Engine::Math
{
    inline DVec3 operator+(const DVec3& lhs, const DVec3& rhs)
    {
        return { lhs.X + rhs.X, lhs.Y + rhs.Y, lhs.Z + rhs.Z };
    }

    inline DVec3 operator-(const DVec3& lhs, const DVec3& rhs)
    {
        return { lhs.X - rhs.X, lhs.Y - rhs.Y, lhs.Z - rhs.Z };
    }

    inline DVec3 operator-(const DVec3& value)
    {
        return { -value.X, -value.Y, -value.Z };
    }

    inline DVec3 operator*(const DVec3& value, double scalar)
    {
        return { value.X * scalar, value.Y * scalar, value.Z * scalar };
    }

    inline DVec3 operator*(double scalar, const DVec3& value)
    {
        return value * scalar;
    }

    inline double Dot(const DVec3& lhs, const DVec3& rhs)
    {
        return lhs.X * rhs.X + lhs.Y * rhs.Y + lhs.Z * rhs.Z;
    }

    inline DVec3 Cross(const DVec3& lhs, const DVec3& rhs)
    {
        return {
            lhs.Y * rhs.Z - lhs.Z * rhs.Y,
            lhs.Z * rhs.X - lhs.X * rhs.Z,
            lhs.X * rhs.Y - lhs.Y * rhs.X
        };
    }

    inline double LengthSquared(const DVec3& value)
    {
        return Dot(value, value);
    }

    inline double Length(const DVec3& value)
    {
        return std::sqrt(LengthSquared(value));
    }

    inline bool AllFinite(const DVec3& value)
    {
        return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
    }

    // Fails for non-finite input and for lengths whose square underflows or overflows.
    inline bool TryNormalize(const DVec3& value, DVec3& outNormalized)
    {
        const double lengthSquared = LengthSquared(value);
        if (!AllFinite(value) || !std::isfinite(lengthSquared) || lengthSquared < 1e-300)
            return false;

        outNormalized = value * (1.0 / std::sqrt(lengthSquared));
        return true;
    }
}
