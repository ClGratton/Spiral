#pragma once

#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/CompletionToken.h"

#include <deque>
#include <string>
#include <vector>

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
        u64 Generation = 0;
    };

    class QueryPool
    {
    public:
        virtual ~QueryPool() = default;

        virtual const QueryPoolDescription& GetDescription() const = 0;
        virtual QueryResult ReadResult(u32 queryIndex) const = 0;
        virtual QueryResult ReadResult(u32 queryIndex, u64 generation) const
        {
            const QueryResult result = ReadResult(queryIndex);
            return result.Generation == generation ? result : QueryResult {};
        }
    };

    // P1 owns only the backend-neutral state machine. P2 adapters translate
    // accepted operations and publish native completion data through it. One
    // logical pool is caller-serialized and admits one pending generation;
    // parallel recording uses distinct bounded logical pools.
    class TimestampQueryPool;

    class TimestampQueryRecording
    {
    public:
        bool Reset(u32 firstQuery, u32 queryCount);
        bool Write(u32 queryIndex);
        bool Resolve(u32 firstQuery, u32 queryCount);
        bool IsValid() const { return m_Pool != nullptr && !m_Failed; }

    private:
        friend class TimestampQueryPool;
        TimestampQueryRecording(TimestampQueryPool* pool, u64 baseGeneration, std::vector<bool> reset,
            std::vector<bool> written, std::vector<bool> resolved);

        TimestampQueryPool* m_Pool = nullptr;
        u64 m_BaseGeneration = 0;
        std::vector<bool> m_Reset;
        std::vector<bool> m_Written;
        std::vector<bool> m_Resolved;
        bool m_Failed = false;
    };

    class TimestampQueryPool final : public QueryPool
    {
    public:
        static constexpr u32 kMaximumQueryCount = 4096;
        static constexpr size_t kMaximumRetainedGenerations = 4;

        static Scope<TimestampQueryPool> Create(u64 ownerDeviceId, const QueryPoolDescription& description);

        const QueryPoolDescription& GetDescription() const override { return m_Description; }
        QueryResult ReadResult(u32 queryIndex) const override;
        QueryResult ReadResult(u32 queryIndex, u64 generation) const override;
        u64 GetGeneration() const { return m_Generation; }
        u64 GetOwnerDeviceId() const { return m_OwnerDeviceId; }

        TimestampQueryRecording BeginRecording();
        bool Publish(TimestampQueryRecording& recording, const CompletionToken& token);
        bool Complete(const CompletionToken& token, CompletionStatus status, const std::vector<u64>& values = {});
        size_t GetRetainedGenerationCount() const { return m_History.size(); }

    private:
        friend class TimestampQueryRecording;
        struct Generation
        {
            u64 Id = 0;
            CompletionToken Token;
            std::vector<bool> Resolved;
            std::vector<QueryResult> Results;
        };

        TimestampQueryPool(u64 ownerDeviceId, QueryPoolDescription description);
        bool IsRangeValid(u32 firstQuery, u32 queryCount) const;
        const Generation* FindGeneration(u64 generation) const;
        Generation* FindGeneration(const CompletionToken& token);

        u64 m_OwnerDeviceId = 0;
        QueryPoolDescription m_Description;
        u64 m_Generation = 0;
        std::deque<Generation> m_History;
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
