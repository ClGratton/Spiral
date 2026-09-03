#pragma once

#include "Engine/RHI/CompletionToken.h"

#include <cstddef>
#include <limits>
#include <unordered_set>

namespace Engine::RHI
{
    // A Vulkan completion record remains queryable for the device lifetime.
    // Native NVRHI command-buffer references may be collected exactly once,
    // when that record first crosses from Incomplete to a terminal status.
    inline bool ShouldCollectVulkanNativeGarbage(
        CompletionStatus previous, CompletionStatus observed)
    {
        return previous == CompletionStatus::Incomplete
            && (observed == CompletionStatus::Complete || observed == CompletionStatus::Failed);
    }

    struct VulkanCompletionHistorySnapshot
    {
        u64 Issued = 0;
        u64 Compacted = 0;
        size_t Live = 0;
        size_t Failed = 0;
        size_t Incomplete = 0;
    };

    // Successful terminal submissions are represented by one contiguous scalar
    // prefix. Failed submissions inside that prefix remain explicit so every
    // issued token preserves its exact device-lifetime terminal result.
    class VulkanCompletionHistory final
    {
    public:
        bool IsCompacted(u64 submissionId) const
        {
            return submissionId != 0 && submissionId <= m_CompactedPrefix;
        }

        CompletionStatus QueryCompacted(u64 submissionId) const
        {
            if (!IsCompacted(submissionId))
                return CompletionStatus::Invalid;
            return m_FailedSubmissionIds.contains(submissionId)
                ? CompletionStatus::Failed : CompletionStatus::Complete;
        }

        bool IsIssued(const CompletionToken& token, u64 deviceId, bool hasLiveEntry) const
        {
            return token.IsValid() && token.DeviceId == deviceId
                && (IsCompacted(token.SubmissionId) || hasLiveEntry);
        }

        template <typename Query, typename Erase>
        size_t CompactTerminalPrefix(Query&& query, Erase&& erase)
        {
            size_t compacted = 0;
            while (m_CompactedPrefix != std::numeric_limits<u64>::max())
            {
                const u64 submissionId = m_CompactedPrefix + 1;
                const CompletionStatus status = query(submissionId);
                if (status != CompletionStatus::Complete && status != CompletionStatus::Failed)
                    break;
                if (status == CompletionStatus::Failed)
                    m_FailedSubmissionIds.insert(submissionId);
                erase(submissionId);
                m_CompactedPrefix = submissionId;
                ++compacted;
            }
            return compacted;
        }

        VulkanCompletionHistorySnapshot Snapshot(
            u64 issued, size_t live, size_t incomplete) const
        {
            return { issued, m_CompactedPrefix, live, m_FailedSubmissionIds.size(), incomplete };
        }

    private:
        u64 m_CompactedPrefix = 0;
        std::unordered_set<u64> m_FailedSubmissionIds;
    };
}
