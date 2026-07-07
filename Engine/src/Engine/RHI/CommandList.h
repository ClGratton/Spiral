#pragma once

#include "Engine/RHI/Query.h"
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
        virtual void ResetQueryPool(QueryPool& queryPool, u32 firstQuery, u32 queryCount) = 0;
        virtual void WriteTimestamp(QueryPool& queryPool, u32 queryIndex) = 0;
        virtual void ResolveQueryPool(QueryPool& queryPool, u32 firstQuery, u32 queryCount) = 0;
    };
}
