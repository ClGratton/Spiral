#pragma once

#include "Engine/RHI/RHICommon.h"

#include <string_view>

namespace Engine::RHI
{
    class CommandList
    {
    public:
        virtual ~CommandList() = default;

        virtual QueueType GetQueueType() const = 0;
        virtual void Begin() = 0;
        virtual void End() = 0;
        virtual void BeginDebugMarker(std::string_view name) = 0;
        virtual void EndDebugMarker() = 0;
    };
}
