#pragma once

#include "Engine/Core/Base.h"

namespace Engine
{
    using EntityId = u32;
    constexpr EntityId kInvalidEntityId = 0;

    struct Entity
    {
        EntityId Id = kInvalidEntityId;

        bool IsValid() const { return Id != kInvalidEntityId; }
        explicit operator bool() const { return IsValid(); }

        bool operator==(const Entity& other) const { return Id == other.Id; }
        bool operator!=(const Entity& other) const { return !(*this == other); }
    };
}
