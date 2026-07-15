#include "Engine/RHI/BufferOwnership.h"

#include <algorithm>

namespace Engine::RHI
{
    namespace
    {
        bool SameToken(const CompletionToken& left, const CompletionToken& right)
        {
            return left.DeviceId == right.DeviceId && left.SubmissionId == right.SubmissionId;
        }

        bool SamePair(const auto& pending,
            QueueType source, QueueType destination, ResourceState before, ResourceState after)
        {
            return pending.Source == source && pending.Destination == destination
                && pending.Before == before && pending.After == after;
        }
    }

    bool BufferOwnershipTracker::Register(Buffer& buffer, QueueType owner, ResourceState state)
    {
        return m_Buffers.emplace(&buffer, BufferRecord { owner, state, std::nullopt }).second;
    }

    bool BufferOwnershipTracker::Unregister(Buffer& buffer)
    {
        const auto found = m_Buffers.find(&buffer);
        if (found == m_Buffers.end() || found->second.Pending)
            return false;
        m_Buffers.erase(found);
        return true;
    }

    bool BufferOwnershipTracker::IsLive(const Buffer* buffer) const
    {
        return buffer && m_Buffers.contains(buffer);
    }

    bool BufferOwnershipTracker::CanUse(const Buffer* buffer) const
    {
        const auto found = m_Buffers.find(buffer);
        return found != m_Buffers.end() && !found->second.Pending;
    }

    bool BufferOwnershipTracker::CanDestroy(const Buffer* buffer) const
    {
        return CanUse(buffer);
    }

    bool BufferOwnershipTracker::PublishOrdinaryState(Buffer& buffer, ResourceState state)
    {
        auto found = m_Buffers.find(&buffer);
        if (found == m_Buffers.end() || found->second.Pending
            || !IsBufferStateCompatible(buffer.GetDescription().Usage, buffer.GetDescription().CpuAccess, state))
            return false;
        found->second.State = state;
        return true;
    }

    bool BufferOwnershipTracker::RecordRelease(const BufferOwnershipRelease& release, QueueType listQueue,
        QueueType effectiveSource, QueueType effectiveDestination, RecordedBufferOwnershipOperation& operation) const
    {
        const auto found = m_Buffers.find(release.Resource);
        if (found == m_Buffers.end() || found->second.Pending || effectiveSource == effectiveDestination
            || listQueue != effectiveSource || found->second.Owner != effectiveSource || found->second.State != release.Before
            || !IsBufferStateCompatible(release.Resource->GetDescription().Usage, release.Resource->GetDescription().CpuAccess, release.Before)
            || !IsBufferStateCompatible(release.Resource->GetDescription().Usage, release.Resource->GetDescription().CpuAccess, release.After))
            return false;
        operation = { BufferOwnershipOperationType::Release, release.Resource, effectiveSource, effectiveDestination,
            release.Before, release.After, {} };
        return true;
    }

    bool BufferOwnershipTracker::RecordAcquire(const BufferOwnershipAcquire& acquire, QueueType listQueue,
        QueueType effectiveSource, QueueType effectiveDestination, RecordedBufferOwnershipOperation& operation) const
    {
        const auto found = m_Buffers.find(acquire.Resource);
        if (found == m_Buffers.end() || !found->second.Pending || !acquire.ReleaseToken.IsValid()
            || effectiveSource == effectiveDestination || listQueue != effectiveDestination
            || !SamePair(*found->second.Pending, effectiveSource, effectiveDestination, acquire.Before, acquire.After)
            || !SameToken(found->second.Pending->ReleaseToken, acquire.ReleaseToken))
            return false;
        operation = { BufferOwnershipOperationType::Acquire, acquire.Resource, effectiveSource, effectiveDestination,
            acquire.Before, acquire.After, acquire.ReleaseToken };
        return true;
    }

    bool BufferOwnershipTracker::ValidateSubmission(const RecordedBufferOwnershipOperation& operation,
        const std::vector<CompletionToken>& dependencies) const
    {
        const auto found = m_Buffers.find(operation.Resource);
        if (found == m_Buffers.end())
            return false;
        if (operation.Type == BufferOwnershipOperationType::Release)
            return !found->second.Pending && found->second.Owner == operation.Source && found->second.State == operation.Before;
        if (!found->second.Pending || !SamePair(*found->second.Pending, operation.Source, operation.Destination,
            operation.Before, operation.After) || !SameToken(found->second.Pending->ReleaseToken, operation.ReleaseToken))
            return false;
        return std::count_if(dependencies.begin(), dependencies.end(), [&](const CompletionToken& dependency)
        {
            return SameToken(dependency, operation.ReleaseToken);
        }) == 1;
    }

    bool BufferOwnershipTracker::PublishRelease(const RecordedBufferOwnershipOperation& operation, const CompletionToken& acceptedToken)
    {
        auto found = m_Buffers.find(operation.Resource);
        if (operation.Type != BufferOwnershipOperationType::Release || !acceptedToken.IsValid()
            || found == m_Buffers.end() || found->second.Pending || found->second.Owner != operation.Source
            || found->second.State != operation.Before)
            return false;
        found->second.Pending = PendingTransfer { operation.Source, operation.Destination,
            operation.Before, operation.After, acceptedToken };
        return true;
    }

    bool BufferOwnershipTracker::PublishAcquire(const RecordedBufferOwnershipOperation& operation)
    {
        auto found = m_Buffers.find(operation.Resource);
        if (operation.Type != BufferOwnershipOperationType::Acquire || found == m_Buffers.end() || !found->second.Pending
            || !SamePair(*found->second.Pending, operation.Source, operation.Destination, operation.Before, operation.After)
            || !SameToken(found->second.Pending->ReleaseToken, operation.ReleaseToken))
            return false;
        found->second.Owner = operation.Destination;
        found->second.State = operation.After;
        found->second.Pending.reset();
        return true;
    }

    bool BufferOwnershipTracker::Recover(Buffer& buffer, const CompletionToken& releaseToken, CompletionStatus completion)
    {
        auto found = m_Buffers.find(&buffer);
        if (found == m_Buffers.end() || !found->second.Pending || completion != CompletionStatus::Complete
            || !SameToken(found->second.Pending->ReleaseToken, releaseToken))
            return false;
        found->second.Owner = found->second.Pending->Source;
        found->second.State = found->second.Pending->Before;
        found->second.Pending.reset();
        return true;
    }

    bool BufferOwnershipTracker::QueryPending(const Buffer* buffer, PendingBufferOwnershipTransfer& pending) const
    {
        const auto found = m_Buffers.find(buffer);
        if (found == m_Buffers.end() || !found->second.Pending)
            return false;
        const PendingTransfer& transfer = *found->second.Pending;
        pending = { buffer, transfer.Source, transfer.Destination,
            transfer.Before, transfer.After, transfer.ReleaseToken };
        return true;
    }

    bool BufferOwnershipTracker::QueryOwner(const Buffer* buffer, QueueType& owner) const
    {
        const auto found = m_Buffers.find(buffer);
        if (found == m_Buffers.end() || found->second.Pending)
            return false;
        owner = found->second.Owner;
        return true;
    }

    bool BufferOwnershipTracker::QueryState(const Buffer* buffer, ResourceState& state) const
    {
        const auto found = m_Buffers.find(buffer);
        if (found == m_Buffers.end() || found->second.Pending || found->second.State == ResourceState::Unknown)
            return false;
        state = found->second.State;
        return true;
    }

    bool BufferOwnershipTracker::HasPending(const Buffer* buffer) const
    {
        const auto found = m_Buffers.find(buffer);
        return found != m_Buffers.end() && found->second.Pending.has_value();
    }
}
