#pragma once

#include "Engine/RHI/Buffer.h"
#include "Engine/RHI/CompletionToken.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace Engine::RHI
{
    struct QueueResolution;

    struct BufferOwnershipRelease
    {
        Buffer* Resource = nullptr;
        QueueType SourceQueue = QueueType::Graphics;
        QueueType DestinationQueue = QueueType::Graphics;
        ResourceState Before = ResourceState::Unknown;
        ResourceState After = ResourceState::Unknown;
    };

    struct BufferOwnershipAcquire : BufferOwnershipRelease
    {
        CompletionToken ReleaseToken;
    };

    enum class BufferOwnershipOperationType
    {
        Release,
        Acquire
    };

    struct RecordedBufferOwnershipOperation
    {
        BufferOwnershipOperationType Type = BufferOwnershipOperationType::Release;
        Buffer* Resource = nullptr;
        QueueType Source = QueueType::Graphics;
        QueueType Destination = QueueType::Graphics;
        ResourceState Before = ResourceState::Unknown;
        ResourceState After = ResourceState::Unknown;
        CompletionToken ReleaseToken;
    };

    // Device-owned authority shared by the production command lists and the
    // deterministic contract harness. Recording is side-effect free; only an
    // accepted submission publishes a pending transfer or its destination.
    class BufferOwnershipTracker
    {
    public:
        bool Register(Buffer& buffer, QueueType owner, ResourceState state);
        bool Unregister(Buffer& buffer);
        bool IsLive(const Buffer* buffer) const;
        bool CanUse(const Buffer* buffer) const;
        bool CanDestroy(const Buffer* buffer) const;
        bool PublishOrdinaryState(Buffer& buffer, ResourceState state);

        bool RecordRelease(const BufferOwnershipRelease& release, QueueType listQueue,
            QueueType effectiveSource, QueueType effectiveDestination, RecordedBufferOwnershipOperation& operation) const;
        bool RecordAcquire(const BufferOwnershipAcquire& acquire, QueueType listQueue,
            QueueType effectiveSource, QueueType effectiveDestination, RecordedBufferOwnershipOperation& operation) const;
        bool ValidateSubmission(const RecordedBufferOwnershipOperation& operation,
            const std::vector<CompletionToken>& dependencies) const;
        bool PublishRelease(const RecordedBufferOwnershipOperation& operation, const CompletionToken& acceptedToken);
        bool PublishAcquire(const RecordedBufferOwnershipOperation& operation);
        bool Recover(Buffer& buffer, const CompletionToken& releaseToken, CompletionStatus completion);

        bool QueryOwner(const Buffer* buffer, QueueType& owner) const;
        bool QueryState(const Buffer* buffer, ResourceState& state) const;
        bool HasPending(const Buffer* buffer) const;

    private:
        struct PendingTransfer
        {
            QueueType Source = QueueType::Graphics;
            QueueType Destination = QueueType::Graphics;
            ResourceState Before = ResourceState::Unknown;
            ResourceState After = ResourceState::Unknown;
            CompletionToken ReleaseToken;
        };

        struct BufferRecord
        {
            QueueType Owner = QueueType::Graphics;
            ResourceState State = ResourceState::Unknown;
            std::optional<PendingTransfer> Pending;
        };

        std::unordered_map<const Buffer*, BufferRecord> m_Buffers;
    };
}
