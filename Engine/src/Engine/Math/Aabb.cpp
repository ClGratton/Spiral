#include "Engine/Math/Aabb.h"

#include "Engine/Math/DVec3Ops.h"

namespace Engine::Math
{
    bool IsValid(const Aabb& box)
    {
        return AllFinite(box.Min) && AllFinite(box.Max)
            && box.Min.X <= box.Max.X && box.Min.Y <= box.Max.Y && box.Min.Z <= box.Max.Z;
    }

    bool Contains(const Aabb& box, const DVec3& point)
    {
        return point.X >= box.Min.X && point.X <= box.Max.X
            && point.Y >= box.Min.Y && point.Y <= box.Max.Y
            && point.Z >= box.Min.Z && point.Z <= box.Max.Z;
    }

    DVec3 Center(const Aabb& box)
    {
        return (box.Min + box.Max) * 0.5;
    }

    DVec3 HalfExtents(const Aabb& box)
    {
        return (box.Max - box.Min) * 0.5;
    }
}
