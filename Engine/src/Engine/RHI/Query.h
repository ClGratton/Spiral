#pragma once

#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/CompletionToken.h"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
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
    class TimestampQueryTransaction;
    class TimestampQueryPoolState;

    class TimestampQueryRecording
    {
    public:
        bool Reset(u32 firstQuery, u32 queryCount);
        bool Write(u32 queryIndex);
        bool Resolve(u32 firstQuery, u32 queryCount);
        bool IsValid() const { return m_Pool != nullptr && !m_Failed; }

    private:
        friend class TimestampQueryPool;
        friend class TimestampQueryTransaction;
        friend class TimestampQueryPoolState;
        TimestampQueryRecording(std::shared_ptr<TimestampQueryPoolState> pool, u64 baseGeneration, std::vector<bool> reset,
            std::vector<bool> written, std::vector<bool> resolved);

        std::shared_ptr<TimestampQueryPoolState> m_Pool;
        u64 m_BaseGeneration = 0;
        std::vector<bool> m_Reset;
        std::vector<bool> m_Written;
        std::vector<bool> m_Resolved;
        bool m_Failed = false;
    };

    // P2A retains a backend-private allocation/result object as opaque shared
    // state. P2B and P2C may cast their private shared ownership to this type,
    // but no native handle crosses the Engine::RHI contract.
    using NativeQueryState = std::shared_ptr<void>;

    // Device-owned retirement storage for P2B/P2C. Capacity is reserved before
    // submission so an accepted submit can always retain its native state.
    class TimestampQueryRetirementQueue
    {
    public:
        static constexpr size_t kMaximumPendingRetirements = 4;

        explicit TimestampQueryRetirementQueue(u64 ownerDeviceId) : m_OwnerDeviceId(ownerDeviceId) {}

        bool IsRetained(const NativeQueryState& state) const;
        bool Complete(const NativeQueryState& state, const CompletionToken& token, CompletionStatus status,
            const std::vector<u64>& values = {});
        bool Retire(const CompletionToken& token, CompletionStatus status);
        size_t GetPendingRetirementCount() const { return m_Pending.size(); }

    private:
        friend class TimestampQueryTransaction;
        struct RetainedState
        {
            std::shared_ptr<TimestampQueryPoolState> Pool;
            NativeQueryState State;
            CompletionToken Token;
        };

        struct ReservedState
        {
            std::shared_ptr<TimestampQueryPoolState> Pool;
            NativeQueryState State;
        };

        bool Reserve(const std::shared_ptr<TimestampQueryPoolState>& pool, const NativeQueryState& state);
        void ReleaseReservation(const NativeQueryState& state);
        bool CanPrepare(const std::shared_ptr<TimestampQueryPoolState>& pool, const NativeQueryState& state) const;
        bool CanPublish(const std::shared_ptr<TimestampQueryPoolState>& pool, const NativeQueryState& state, const CompletionToken& token) const;
        void Publish(const std::shared_ptr<TimestampQueryPoolState>& pool, const NativeQueryState& state, const CompletionToken& token);

        u64 m_OwnerDeviceId = 0;
        std::vector<ReservedState> m_Reserved;
        std::deque<RetainedState> m_Pending;
    };

    // One closed command-list query transaction. It advances P1 logical state
    // before invoking each native callback, then publishes only after Device
    // has returned an accepted exact completion token. Failed recording and
    // abandoned transactions release their reservation without publication.
    class TimestampQueryTransaction
    {
    public:
        static std::optional<TimestampQueryTransaction> Begin(TimestampQueryPool& pool,
            TimestampQueryRetirementQueue& retirements, NativeQueryState nativeState);

        TimestampQueryTransaction(const TimestampQueryTransaction&) = delete;
        TimestampQueryTransaction& operator=(const TimestampQueryTransaction&) = delete;
        TimestampQueryTransaction(TimestampQueryTransaction&&) noexcept = default;
        TimestampQueryTransaction& operator=(TimestampQueryTransaction&&) = delete;
        ~TimestampQueryTransaction();

        bool Reset(u32 firstQuery, u32 queryCount, const std::function<bool()>& nativeOperation);
        bool Write(u32 queryIndex, const std::function<bool()>& nativeOperation);
        bool Resolve(u32 firstQuery, u32 queryCount, const std::function<bool()>& nativeOperation);
        bool PrepareForSubmit();
        bool Publish(const CompletionToken& token);
        bool IsValid() const;

    private:
        TimestampQueryTransaction(std::shared_ptr<TimestampQueryPoolState> pool, TimestampQueryRetirementQueue& retirements,
            TimestampQueryRecording recording, NativeQueryState nativeState);
        bool Record(const std::function<bool(TimestampQueryRecording&)>& logicalOperation,
            const std::function<bool()>& nativeOperation);
        void ReleaseReservation();

        std::shared_ptr<TimestampQueryPoolState> m_Pool;
        TimestampQueryRetirementQueue* m_Retirements = nullptr;
        std::optional<TimestampQueryRecording> m_Recording;
        NativeQueryState m_NativeState;
        bool m_Prepared = false;
        bool m_Published = false;
    };

    class TimestampQueryPool final : public QueryPool
    {
    public:
        static constexpr u32 kMaximumQueryCount = 4096;
        static constexpr size_t kMaximumRetainedGenerations = 4;

        static Scope<TimestampQueryPool> Create(u64 ownerDeviceId, const QueryPoolDescription& description);

        const QueryPoolDescription& GetDescription() const override;
        QueryResult ReadResult(u32 queryIndex) const override;
        QueryResult ReadResult(u32 queryIndex, u64 generation) const override;
        u64 GetGeneration() const;
        u64 GetOwnerDeviceId() const;

        TimestampQueryRecording BeginRecording();
        bool Publish(TimestampQueryRecording& recording, const CompletionToken& token);
        bool Complete(const CompletionToken& token, CompletionStatus status, const std::vector<u64>& values = {});
        size_t GetRetainedGenerationCount() const;

    private:
        friend class TimestampQueryRecording;
        TimestampQueryPool(u64 ownerDeviceId, QueryPoolDescription description);
        std::shared_ptr<TimestampQueryPoolState> m_State;
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
