#include "EulerRotation.h"

#include <algorithm>
#include <cmath>

namespace Gizmo
{
    namespace
    {
        using Engine::Math::DVec3;
        using Engine::Math::Vec3;

        constexpr double kPi = 3.14159265358979323846;
        constexpr double kGimbalThreshold = 1e-6;

        double Unwrap(double angle, double reference)
        {
            return angle + 2.0 * kPi * std::round((reference - angle) / (2.0 * kPi));
        }

        struct Euler
        {
            double Pitch;
            double Yaw;
            double Roll;
        };

        double Cost(const Euler& candidate, const Euler& previous)
        {
            const double pitch = candidate.Pitch - previous.Pitch;
            const double yaw = candidate.Yaw - previous.Yaw;
            const double roll = candidate.Roll - previous.Roll;
            return pitch * pitch + yaw * yaw + roll * roll;
        }

        Euler UnwrapNear(const Euler& candidate, const Euler& previous)
        {
            return {
                Unwrap(candidate.Pitch, previous.Pitch),
                Unwrap(candidate.Yaw, previous.Yaw),
                Unwrap(candidate.Roll, previous.Roll)
            };
        }
    }

    Rotation3 Rotation3::Identity()
    {
        Rotation3 result;
        result.M[0][0] = 1.0;
        result.M[1][1] = 1.0;
        result.M[2][2] = 1.0;
        return result;
    }

    Rotation3 Compose(const Rotation3& first, const Rotation3& second)
    {
        Rotation3 result;
        for (u32 row = 0; row < 3; ++row)
        {
            for (u32 column = 0; column < 3; ++column)
            {
                for (u32 index = 0; index < 3; ++index)
                    result.M[row][column] += first.M[row][index] * second.M[index][column];
            }
        }

        return result;
    }

    Rotation3 RotationFromEulerDegrees(const Vec3& degrees)
    {
        const double toRadians = kPi / 180.0;
        const double sp = std::sin(static_cast<double>(degrees.X) * toRadians);
        const double cp = std::cos(static_cast<double>(degrees.X) * toRadians);
        const double sy = std::sin(static_cast<double>(degrees.Y) * toRadians);
        const double cy = std::cos(static_cast<double>(degrees.Y) * toRadians);
        const double sr = std::sin(static_cast<double>(degrees.Z) * toRadians);
        const double cr = std::cos(static_cast<double>(degrees.Z) * toRadians);

        Rotation3 result;
        result.M[0][0] = cy * cr - sy * sp * sr;
        result.M[0][1] = cy * sr + sy * sp * cr;
        result.M[0][2] = -sy * cp;
        result.M[1][0] = -cp * sr;
        result.M[1][1] = cp * cr;
        result.M[1][2] = sp;
        result.M[2][0] = sy * cr + cy * sp * sr;
        result.M[2][1] = sy * sr - cy * sp * cr;
        result.M[2][2] = cy * cp;
        return result;
    }

    Rotation3 RotationAboutAxis(const DVec3& unitAxis, double radians)
    {
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        const double t = 1.0 - c;
        const double x = unitAxis.X;
        const double y = unitAxis.Y;
        const double z = unitAxis.Z;

        Rotation3 result;
        result.M[0][0] = c + t * x * x;
        result.M[0][1] = t * x * y + s * z;
        result.M[0][2] = t * x * z - s * y;
        result.M[1][0] = t * y * x - s * z;
        result.M[1][1] = c + t * y * y;
        result.M[1][2] = t * y * z + s * x;
        result.M[2][0] = t * z * x + s * y;
        result.M[2][1] = t * z * y - s * x;
        result.M[2][2] = c + t * z * z;
        return result;
    }

    DVec3 Row(const Rotation3& rotation, u32 index)
    {
        return { rotation.M[index][0], rotation.M[index][1], rotation.M[index][2] };
    }

    Vec3 EulerDegreesFromRotation(const Rotation3& rotation, const Vec3& previousDegrees)
    {
        const double toRadians = kPi / 180.0;
        const auto finiteOrZero = [](float value) { return std::isfinite(value) ? static_cast<double>(value) : 0.0; };
        const Euler previous {
            finiteOrZero(previousDegrees.X) * toRadians,
            finiteOrZero(previousDegrees.Y) * toRadians,
            finiteOrZero(previousDegrees.Z) * toRadians
        };

        const double cosPitch = std::hypot(rotation.M[0][2], rotation.M[2][2]);
        Euler primary {};
        primary.Pitch = std::atan2(rotation.M[1][2], cosPitch);
        if (cosPitch > kGimbalThreshold)
        {
            primary.Yaw = std::atan2(-rotation.M[0][2], rotation.M[2][2]);
            primary.Roll = std::atan2(-rotation.M[1][0], rotation.M[1][1]);
        }
        else
        {
            primary.Roll = previous.Roll;
            primary.Yaw = rotation.M[1][2] > 0.0
                ? std::atan2(rotation.M[0][1], rotation.M[0][0]) - primary.Roll
                : std::atan2(-rotation.M[0][1], rotation.M[0][0]) + primary.Roll;
        }

        const Euler alternate { kPi - primary.Pitch, primary.Yaw + kPi, primary.Roll + kPi };
        const Euler first = UnwrapNear(primary, previous);
        const Euler second = UnwrapNear(alternate, previous);
        const Euler& best = Cost(second, previous) < Cost(first, previous) ? second : first;
        const double toDegrees = 180.0 / kPi;
        return {
            static_cast<float>(best.Pitch * toDegrees),
            static_cast<float>(best.Yaw * toDegrees),
            static_cast<float>(best.Roll * toDegrees)
        };
    }
}
