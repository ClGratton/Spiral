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

    enum class QueryResultStatus
    {
        Unavailable,
        Pending,
        Ready,
        Disjoint
    };

    struct QueryPoolDescription
    {
        std::string DebugName;
        QueryType Type = QueryType::Timestamp;
        u32 Count = 0;
    };

    struct QueryResult
    {
        QueryResultStatus Status = QueryResultStatus::Unavailable;
        u64 Value = 0;
    };

    class QueryPool
    {
    public:
        virtual ~QueryPool() = default;

        virtual const QueryPoolDescription& GetDescription() const = 0;
        virtual QueryResult ReadResult(u32 queryIndex) const = 0;
    };

    inline const char* ToString(QueryResultStatus status)
    {
        switch (status)
        {
            case QueryResultStatus::Unavailable: return "Unavailable";
            case QueryResultStatus::Pending: return "Pending";
            case QueryResultStatus::Ready: return "Ready";
            case QueryResultStatus::Disjoint: return "Disjoint";
        }

        return "Unknown";
    }
}
