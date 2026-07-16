#include "Engine/RHI/Query.h"

#include <algorithm>

namespace Engine::RHI
{
    TimestampQueryRecording::TimestampQueryRecording(TimestampQueryPool* pool, u64 baseGeneration, std::vector<bool> reset,
        std::vector<bool> written, std::vector<bool> resolved)
        : m_Pool(pool), m_BaseGeneration(baseGeneration), m_Reset(std::move(reset)), m_Written(std::move(written)), m_Resolved(std::move(resolved)) {}

    bool TimestampQueryRecording::Reset(u32 firstQuery, u32 queryCount)
    {
        if (!m_Pool || m_Failed || !m_Pool->IsRangeValid(firstQuery, queryCount))
        {
            m_Failed = true;
            return false;
        }
        for (u32 index = firstQuery; index < firstQuery + queryCount; ++index)
        {
            m_Reset[index] = true;
            m_Written[index] = false;
            m_Resolved[index] = false;
        }
        return true;
    }

    bool TimestampQueryRecording::Write(u32 queryIndex)
    {
        if (!m_Pool || m_Failed || queryIndex >= m_Written.size() || !m_Reset[queryIndex] || m_Written[queryIndex])
        {
            m_Failed = true;
            return false;
        }
        m_Written[queryIndex] = true;
        return true;
    }

    bool TimestampQueryRecording::Resolve(u32 firstQuery, u32 queryCount)
    {
        if (!m_Pool || m_Failed || !m_Pool->IsRangeValid(firstQuery, queryCount))
        {
            m_Failed = true;
            return false;
        }
        for (u32 index = firstQuery; index < firstQuery + queryCount; ++index)
            if (!m_Written[index] || m_Resolved[index])
            {
                m_Failed = true;
                return false;
            }
        for (u32 index = firstQuery; index < firstQuery + queryCount; ++index)
            m_Resolved[index] = true;
        return true;
    }

    Scope<TimestampQueryPool> TimestampQueryPool::Create(u64 ownerDeviceId, const QueryPoolDescription& description)
    {
        if (ownerDeviceId == 0 || description.Type != QueryType::Timestamp || description.Count == 0
            || description.Count > kMaximumQueryCount)
            return nullptr;
        return Scope<TimestampQueryPool>(new TimestampQueryPool(ownerDeviceId, description));
    }

    TimestampQueryPool::TimestampQueryPool(u64 ownerDeviceId, QueryPoolDescription description)
        : m_OwnerDeviceId(ownerDeviceId), m_Description(std::move(description)) {}

    bool TimestampQueryPool::IsRangeValid(u32 firstQuery, u32 queryCount) const
    {
        return queryCount != 0 && firstQuery < m_Description.Count && queryCount <= m_Description.Count - firstQuery;
    }

    TimestampQueryRecording TimestampQueryPool::BeginRecording()
    {
        if (!m_History.empty() && std::any_of(m_History.back().Results.begin(), m_History.back().Results.end(),
            [](const QueryResult& result) { return result.Status == QueryResultStatus::Pending; }))
            return TimestampQueryRecording(nullptr, m_Generation, {}, {}, {});
        std::vector<bool> reset(m_Description.Count, false);
        std::vector<bool> written(m_Description.Count, false);
        std::vector<bool> resolved(m_Description.Count, false);
        return TimestampQueryRecording(this, m_Generation, std::move(reset), std::move(written), std::move(resolved));
    }

    bool TimestampQueryPool::Publish(TimestampQueryRecording& recording, const CompletionToken& token)
    {
        if (recording.m_Pool != this || recording.m_Failed || recording.m_BaseGeneration != m_Generation || token.DeviceId != m_OwnerDeviceId
            || !token.IsValid() || recording.m_Reset.size() != m_Description.Count) return false;
        if (std::any_of(m_History.begin(), m_History.end(), [&](const Generation& generation)
            { return generation.Token.DeviceId == token.DeviceId && generation.Token.SubmissionId == token.SubmissionId; })) return false;
        bool hasReset = false;
        for (u32 index = 0; index < m_Description.Count; ++index)
        {
            if (!recording.m_Reset[index]) continue;
            hasReset = true;
            if (!recording.m_Written[index] || !recording.m_Resolved[index]) return false;
        }
        if (!hasReset) return false;

        Generation generation;
        generation.Id = ++m_Generation;
        generation.Token = token;
        generation.Resolved = recording.m_Resolved;
        generation.Results.resize(m_Description.Count);
        for (u32 index = 0; index < m_Description.Count; ++index)
        {
            generation.Results[index].Generation = generation.Id;
            if (generation.Resolved[index]) generation.Results[index].Status = QueryResultStatus::Pending;
        }
        m_History.push_back(std::move(generation));
        while (m_History.size() > kMaximumRetainedGenerations) m_History.pop_front();
        recording.m_Pool = nullptr;
        return true;
    }

    bool TimestampQueryPool::Complete(const CompletionToken& token, CompletionStatus status, const std::vector<u64>& values)
    {
        Generation* generation = FindGeneration(token);
        if (!generation || status == CompletionStatus::Invalid || status == CompletionStatus::Incomplete) return false;
        if (status == CompletionStatus::Complete && values.size() != m_Description.Count) return false;
        for (u32 index = 0; index < m_Description.Count; ++index)
            if (generation->Resolved[index] && generation->Results[index].Status != QueryResultStatus::Pending) return false;

        for (u32 index = 0; index < m_Description.Count; ++index)
        {
            if (!generation->Resolved[index]) continue;
            QueryResult& result = generation->Results[index];
            result.Status = status == CompletionStatus::Complete ? QueryResultStatus::Ready : QueryResultStatus::Disjoint;
            result.Value = status == CompletionStatus::Complete ? values[index] : 0;
        }
        return true;
    }

    QueryResult TimestampQueryPool::ReadResult(u32 queryIndex) const
    {
        return ReadResult(queryIndex, m_Generation);
    }

    QueryResult TimestampQueryPool::ReadResult(u32 queryIndex, u64 generation) const
    {
        const Generation* record = FindGeneration(generation);
        return record && queryIndex < record->Results.size() ? record->Results[queryIndex] : QueryResult {};
    }

    const TimestampQueryPool::Generation* TimestampQueryPool::FindGeneration(u64 generation) const
    {
        const auto found = std::find_if(m_History.begin(), m_History.end(), [generation](const Generation& candidate) { return candidate.Id == generation; });
        return found == m_History.end() ? nullptr : &*found;
    }

    TimestampQueryPool::Generation* TimestampQueryPool::FindGeneration(const CompletionToken& token)
    {
        const auto found = std::find_if(m_History.begin(), m_History.end(), [&](const Generation& candidate)
        {
            return candidate.Token.DeviceId == token.DeviceId && candidate.Token.SubmissionId == token.SubmissionId;
        });
        return found == m_History.end() ? nullptr : &*found;
    }
}
