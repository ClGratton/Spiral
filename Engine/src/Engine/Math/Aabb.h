#pragma once

#include "Engine/Math/Math.h"

namespace Engine::Math
{
    // Axis-aligned box in one coordinate space; callers using camera-relative
    // space must pass rays and boxes expressed relative to the same origin.
    // Min == Max on an axis is a valid zero-thickness box.
    struct Aabb
    {
        DVec3 Min;
        DVec3 Max;
    };

    bool IsValid(const Aabb& box);
    bool Contains(const Aabb& box, const DVec3& point);
    DVec3 Center(const Aabb& box);
    DVec3 HalfExtents(const Aabb& box);
}
