#include "Engine/RHI/Query.h"

#include <algorithm>

namespace Engine::RHI
{
    class TimestampQueryPoolState
    {
    public:
        struct Generation
        {
            u64 Id = 0;
            CompletionToken Token;
            std::vector<bool> Resolved;
            std::vector<QueryResult> Results;
        };

        TimestampQueryPoolState(u64 ownerDeviceId, QueryPoolDescription description)
            : OwnerDeviceId(ownerDeviceId), Description(std::move(description)) {}

        bool IsRangeValid(u32 firstQuery, u32 queryCount) const
        {
            return queryCount != 0 && firstQuery < Description.Count && queryCount <= Description.Count - firstQuery;
        }

        bool CanPrepare(const TimestampQueryRecording& recording) const
        {
            if (recording.m_Pool.get() != this || recording.m_Failed || recording.m_BaseGeneration != GenerationId
                || recording.m_Reset.size() != Description.Count || recording.m_Written.size() != Description.Count
                || recording.m_Resolved.size() != Description.Count)
                return false;
            bool hasReset = false;
            for (u32 index = 0; index < Description.Count; ++index)
            {
                if (!recording.m_Reset[index]) continue;
                hasReset = true;
                if (!recording.m_Written[index] || !recording.m_Resolved[index]) return false;
            }
            return hasReset;
        }

        bool CanPublish(const TimestampQueryRecording& recording, const CompletionToken& token) const
        {
            return CanPrepare(recording) && token.IsValid() && token.DeviceId == OwnerDeviceId
                && std::none_of(History.begin(), History.end(), [&](const Generation& generation)
                    { return generation.Token.DeviceId == token.DeviceId && generation.Token.SubmissionId == token.SubmissionId; });
        }

        bool Publish(TimestampQueryRecording& recording, const CompletionToken& token)
        {
            if (!CanPublish(recording, token)) return false;
            Generation generation;
            generation.Id = ++GenerationId;
            generation.Token = token;
            generation.Resolved = recording.m_Resolved;
            generation.Results.resize(Description.Count);
            for (u32 index = 0; index < Description.Count; ++index)
            {
                generation.Results[index].Generation = generation.Id;
                if (generation.Resolved[index]) generation.Results[index].Status = QueryResultStatus::Pending;
            }
            History.push_back(std::move(generation));
            while (History.size() > 4) History.pop_front();
            recording.m_Pool.reset();
            return true;
        }

        bool Complete(const CompletionToken& token, CompletionStatus status, const std::vector<u64>& values)
        {
            Generation* generation = FindGeneration(token);
            if (!generation || status == CompletionStatus::Invalid || status == CompletionStatus::Incomplete) return false;
            if (status == CompletionStatus::Complete && values.size() != Description.Count) return false;
            for (u32 index = 0; index < Description.Count; ++index)
                if (generation->Resolved[index] && generation->Results[index].Status != QueryResultStatus::Pending) return false;
            for (u32 index = 0; index < Description.Count; ++index)
            {
                if (!generation->Resolved[index]) continue;
                QueryResult& result = generation->Results[index];
                result.Status = status == CompletionStatus::Complete ? QueryResultStatus::Ready : QueryResultStatus::Disjoint;
                result.Value = status == CompletionStatus::Complete ? values[index] : 0;
            }
            return true;
        }

        bool IsTerminal(const CompletionToken& token) const
        {
            const Generation* generation = FindGeneration(token);
            return generation && std::none_of(generation->Results.begin(), generation->Results.end(),
                [](const QueryResult& result) { return result.Status == QueryResultStatus::Pending; });
        }

        const Generation* FindGeneration(u64 generation) const
        {
            const auto found = std::find_if(History.begin(), History.end(), [generation](const Generation& candidate) { return candidate.Id == generation; });
            return found == History.end() ? nullptr : &*found;
        }

        Generation* FindGeneration(const CompletionToken& token)
        {
            const auto found = std::find_if(History.begin(), History.end(), [&](const Generation& candidate)
                { return candidate.Token.DeviceId == token.DeviceId && candidate.Token.SubmissionId == token.SubmissionId; });
            return found == History.end() ? nullptr : &*found;
        }

        const Generation* FindGeneration(const CompletionToken& token) const
        {
            const auto found = std::find_if(History.begin(), History.end(), [&](const Generation& candidate)
                { return candidate.Token.DeviceId == token.DeviceId && candidate.Token.SubmissionId == token.SubmissionId; });
            return found == History.end() ? nullptr : &*found;
        }

        u64 OwnerDeviceId = 0;
        QueryPoolDescription Description;
        u64 GenerationId = 0;
        std::deque<Generation> History;
    };

    TimestampQueryRecording::TimestampQueryRecording(std::shared_ptr<TimestampQueryPoolState> pool, u64 baseGeneration,
        std::vector<bool> reset, std::vector<bool> written, std::vector<bool> resolved)
        : m_Pool(std::move(pool)), m_BaseGeneration(baseGeneration), m_Reset(std::move(reset)), m_Written(std::move(written)), m_Resolved(std::move(resolved)) {}

    bool TimestampQueryRecording::Reset(u32 firstQuery, u32 queryCount)
    {
        if (!m_Pool || m_Failed || !m_Pool->IsRangeValid(firstQuery, queryCount)) { m_Failed = true; return false; }
        for (u32 index = firstQuery; index < firstQuery + queryCount; ++index) { m_Reset[index] = true; m_Written[index] = false; m_Resolved[index] = false; }
        return true;
    }

    bool TimestampQueryRecording::Write(u32 queryIndex)
    {
        if (!m_Pool || m_Failed || queryIndex >= m_Written.size() || !m_Reset[queryIndex] || m_Written[queryIndex]) { m_Failed = true; return false; }
        m_Written[queryIndex] = true;
        return true;
    }

    bool TimestampQueryRecording::Resolve(u32 firstQuery, u32 queryCount)
    {
        if (!m_Pool || m_Failed || !m_Pool->IsRangeValid(firstQuery, queryCount)) { m_Failed = true; return false; }
        for (u32 index = firstQuery; index < firstQuery + queryCount; ++index)
            if (!m_Written[index] || m_Resolved[index]) { m_Failed = true; return false; }
        for (u32 index = firstQuery; index < firstQuery + queryCount; ++index) m_Resolved[index] = true;
        return true;
    }

    bool TimestampQueryRetirementQueue::IsRetained(const NativeQueryState& state) const
    {
        if (!state) return false;
        const auto matches = [&](const NativeQueryState& candidate) { return candidate.get() == state.get(); };
        return std::any_of(m_Reserved.begin(), m_Reserved.end(), [&](const ReservedState& candidate) { return matches(candidate.State); })
            || std::any_of(m_Pending.begin(), m_Pending.end(), [&](const RetainedState& candidate) { return matches(candidate.State); });
    }

    bool TimestampQueryRetirementQueue::Reserve(const std::shared_ptr<TimestampQueryPoolState>& pool, const NativeQueryState& state)
    {
        if (!pool || !state || IsRetained(state) || m_Reserved.size() + m_Pending.size() >= kMaximumPendingRetirements) return false;
        m_Reserved.push_back({ pool, state });
        return true;
    }

    void TimestampQueryRetirementQueue::ReleaseReservation(const NativeQueryState& state)
    {
        const auto found = std::find_if(m_Reserved.begin(), m_Reserved.end(),
            [&](const ReservedState& candidate) { return candidate.State.get() == state.get(); });
        if (found != m_Reserved.end()) m_Reserved.erase(found);
    }

    bool TimestampQueryRetirementQueue::CanPrepare(const std::shared_ptr<TimestampQueryPoolState>& pool, const NativeQueryState& state) const
    {
        return pool && state && std::any_of(m_Reserved.begin(), m_Reserved.end(), [&](const ReservedState& candidate)
            { return candidate.Pool.get() == pool.get() && candidate.State.get() == state.get(); });
    }

    bool TimestampQueryRetirementQueue::CanPublish(const std::shared_ptr<TimestampQueryPoolState>& pool, const NativeQueryState& state,
        const CompletionToken& token) const
    {
        return CanPrepare(pool, state) && token.IsValid() && token.DeviceId == m_OwnerDeviceId;
    }

    void TimestampQueryRetirementQueue::Publish(const std::shared_ptr<TimestampQueryPoolState>& pool, const NativeQueryState& state,
        const CompletionToken& token)
    {
        ReleaseReservation(state);
        m_Pending.push_back({ pool, state, token });
    }

    bool TimestampQueryRetirementQueue::Retire(const CompletionToken& token, CompletionStatus status)
    {
        if (!token.IsValid() || token.DeviceId != m_OwnerDeviceId || status == CompletionStatus::Invalid || status == CompletionStatus::Incomplete) return false;
        const auto first = std::find_if(m_Pending.begin(), m_Pending.end(), [&](const RetainedState& candidate)
            { return candidate.Token.DeviceId == token.DeviceId && candidate.Token.SubmissionId == token.SubmissionId; });
        if (first == m_Pending.end()) return false;
        if (std::any_of(m_Pending.begin(), m_Pending.end(), [&](const RetainedState& candidate)
            { return candidate.Token.DeviceId == token.DeviceId && candidate.Token.SubmissionId == token.SubmissionId
                && (!candidate.Pool || !candidate.Pool->IsTerminal(token)); })) return false;
        m_Pending.erase(std::remove_if(m_Pending.begin(), m_Pending.end(), [&](const RetainedState& candidate)
            { return candidate.Token.DeviceId == token.DeviceId && candidate.Token.SubmissionId == token.SubmissionId; }), m_Pending.end());
        return true;
    }

    bool TimestampQueryRetirementQueue::Complete(const NativeQueryState& state, const CompletionToken& token,
        CompletionStatus status, const std::vector<u64>& values)
    {
        if (!state || !token.IsValid() || token.DeviceId != m_OwnerDeviceId) return false;
        const auto found = std::find_if(m_Pending.begin(), m_Pending.end(), [&](const RetainedState& candidate)
            { return candidate.State.get() == state.get() && candidate.Token.DeviceId == token.DeviceId
                && candidate.Token.SubmissionId == token.SubmissionId; });
        if (found == m_Pending.end() || !found->Pool) return false;
        return found->Pool->Complete(token, status, values);
    }

    std::optional<TimestampQueryTransaction> TimestampQueryTransaction::Begin(TimestampQueryPool& pool,
        TimestampQueryRetirementQueue& retirements, NativeQueryState nativeState)
    {
        if (retirements.m_OwnerDeviceId == 0 || pool.GetOwnerDeviceId() != retirements.m_OwnerDeviceId) return std::nullopt;
        TimestampQueryRecording recording = pool.BeginRecording();
        if (!recording.IsValid() || !retirements.Reserve(recording.m_Pool, nativeState)) return std::nullopt;
        std::shared_ptr<TimestampQueryPoolState> sharedPool = recording.m_Pool;
        return TimestampQueryTransaction(std::move(sharedPool), retirements, std::move(recording), std::move(nativeState));
    }

    TimestampQueryTransaction::TimestampQueryTransaction(std::shared_ptr<TimestampQueryPoolState> pool, TimestampQueryRetirementQueue& retirements,
        TimestampQueryRecording recording, NativeQueryState nativeState)
        : m_Pool(std::move(pool)), m_Retirements(&retirements), m_Recording(std::move(recording)), m_NativeState(std::move(nativeState)) {}

    TimestampQueryTransaction::~TimestampQueryTransaction() { ReleaseReservation(); }

    bool TimestampQueryTransaction::IsValid() const
    {
        return !m_Published && !m_Prepared && m_Pool && m_Retirements && m_Recording && m_Recording->IsValid() && m_NativeState;
    }

    bool TimestampQueryTransaction::Record(const std::function<bool(TimestampQueryRecording&)>& logicalOperation, const std::function<bool()>& nativeOperation)
    {
        if (!IsValid() || !logicalOperation(*m_Recording)) return false;
        if (nativeOperation && nativeOperation()) return true;
        m_Recording->m_Failed = true;
        return false;
    }

    bool TimestampQueryTransaction::Reset(u32 firstQuery, u32 queryCount, const std::function<bool()>& nativeOperation)
    { return Record([firstQuery, queryCount](TimestampQueryRecording& recording) { return recording.Reset(firstQuery, queryCount); }, nativeOperation); }
    bool TimestampQueryTransaction::Write(u32 queryIndex, const std::function<bool()>& nativeOperation)
    { return Record([queryIndex](TimestampQueryRecording& recording) { return recording.Write(queryIndex); }, nativeOperation); }
    bool TimestampQueryTransaction::Resolve(u32 firstQuery, u32 queryCount, const std::function<bool()>& nativeOperation)
    { return Record([firstQuery, queryCount](TimestampQueryRecording& recording) { return recording.Resolve(firstQuery, queryCount); }, nativeOperation); }

    bool TimestampQueryTransaction::PrepareForSubmit()
    {
        if (!IsValid() || !m_Pool->CanPrepare(*m_Recording) || !m_Retirements->CanPrepare(m_Pool, m_NativeState)) return false;
        m_Prepared = true;
        return true;
    }

    bool TimestampQueryTransaction::Publish(const CompletionToken& token)
    {
        if (!m_Prepared || !m_Recording || !m_Retirements->CanPublish(m_Pool, m_NativeState, token)
            || !m_Pool->CanPublish(*m_Recording, token) || !m_Pool->Publish(*m_Recording, token)) return false;
        m_Retirements->Publish(m_Pool, m_NativeState, token);
        m_Recording.reset();
        m_NativeState.reset();
        m_Pool.reset();
        m_Published = true;
        return true;
    }

    void TimestampQueryTransaction::ReleaseReservation()
    {
        if (!m_Published && m_Retirements && m_NativeState) m_Retirements->ReleaseReservation(m_NativeState);
    }

    Scope<TimestampQueryPool> TimestampQueryPool::Create(u64 ownerDeviceId, const QueryPoolDescription& description)
    {
        if (ownerDeviceId == 0 || description.Type != QueryType::Timestamp || description.Count == 0 || description.Count > kMaximumQueryCount) return nullptr;
        return Scope<TimestampQueryPool>(new TimestampQueryPool(ownerDeviceId, description));
    }

    TimestampQueryPool::TimestampQueryPool(u64 ownerDeviceId, QueryPoolDescription description)
        : m_State(std::make_shared<TimestampQueryPoolState>(ownerDeviceId, std::move(description))) {}

    const QueryPoolDescription& TimestampQueryPool::GetDescription() const { return m_State->Description; }
    u64 TimestampQueryPool::GetGeneration() const { return m_State->GenerationId; }
    u64 TimestampQueryPool::GetOwnerDeviceId() const { return m_State->OwnerDeviceId; }
    size_t TimestampQueryPool::GetRetainedGenerationCount() const { return m_State->History.size(); }

    TimestampQueryRecording TimestampQueryPool::BeginRecording()
    {
        if (!m_State->History.empty() && std::any_of(m_State->History.back().Results.begin(), m_State->History.back().Results.end(),
            [](const QueryResult& result) { return result.Status == QueryResultStatus::Pending; })) return TimestampQueryRecording(nullptr, m_State->GenerationId, {}, {}, {});
        std::vector<bool> reset(m_State->Description.Count, false), written(m_State->Description.Count, false), resolved(m_State->Description.Count, false);
        return TimestampQueryRecording(m_State, m_State->GenerationId, std::move(reset), std::move(written), std::move(resolved));
    }

    bool TimestampQueryPool::Publish(TimestampQueryRecording& recording, const CompletionToken& token) { return m_State->Publish(recording, token); }

    bool TimestampQueryPool::Complete(const CompletionToken& token, CompletionStatus status, const std::vector<u64>& values)
    {
        return m_State->Complete(token, status, values);
    }

    QueryResult TimestampQueryPool::ReadResult(u32 queryIndex) const { return ReadResult(queryIndex, m_State->GenerationId); }
    QueryResult TimestampQueryPool::ReadResult(u32 queryIndex, u64 generation) const
    {
        const TimestampQueryPoolState::Generation* record = m_State->FindGeneration(generation);
        return record && queryIndex < record->Results.size() ? record->Results[queryIndex] : QueryResult {};
    }
}
