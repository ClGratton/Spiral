#pragma once

#include "Engine/RHI/RHICommon.h"

#include <string>

namespace Engine::RHI
{
    enum class QueryType
    {
        Timestamp,
        Occlusion,
        PipelineStatistics
    };

    struct QueryPoolDescription
    {
        std::string DebugName;
        QueryType Type = QueryType::Timestamp;
        u32 Count = 0;
    };

    class QueryPool
    {
    public:
        virtual ~QueryPool() = default;

        virtual const QueryPoolDescription& GetDescription() const = 0;
    };
}
