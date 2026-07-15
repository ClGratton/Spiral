#pragma once

#include "Engine/RHI/CompletionToken.h"
#include "Engine/RHI/Texture.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Engine::RHI
{
    struct TextureOwnershipRelease
    {
        Texture* Resource = nullptr;
        QueueType SourceQueue = QueueType::Graphics;
        QueueType DestinationQueue = QueueType::Graphics;
        ResourceState Before = ResourceState::Unknown;
        ResourceState After = ResourceState::Unknown;
    };
    struct TextureOwnershipAcquire : TextureOwnershipRelease { CompletionToken ReleaseToken; };
    enum class TextureOwnershipOperationType { Release, Acquire };
    struct RecordedTextureOwnershipOperation
    {
        TextureOwnershipOperationType Type = TextureOwnershipOperationType::Release;
        Texture* Resource = nullptr;
        QueueType Source = QueueType::Graphics, Destination = QueueType::Graphics;
        ResourceState Before = ResourceState::Unknown, After = ResourceState::Unknown;
        CompletionToken ReleaseToken;
        ResourceState CommittedBaseline = ResourceState::Unknown;
    };
    struct PendingTextureOwnershipTransfer
    {
        const Texture* Resource = nullptr;
        QueueType Source = QueueType::Graphics, Destination = QueueType::Graphics;
        ResourceState Before = ResourceState::Unknown, After = ResourceState::Unknown;
        CompletionToken ReleaseToken;
    };

    // Same accepted-submission authority as BufferOwnershipTracker. Textures
    // deliberately retain whole-resource/subresource-0 policy until a later
    // contract widens the portable subresource range.
    class TextureOwnershipTracker
    {
    public:
        bool Register(Texture& resource, QueueType owner, ResourceState state)
        {
            return state != ResourceState::Unknown && SupportsWholeResource(resource) && Compatible(resource, state)
                && m_Records.emplace(&resource, Record { owner, state, {} }).second;
        }
        bool Unregister(Texture& resource) { const auto i = m_Records.find(&resource); if (i == m_Records.end() || i->second.Pending) return false; m_Records.erase(i); return true; }
        bool IsLive(const Texture* resource) const { return resource && m_Records.contains(resource); }
        bool CanUse(const Texture* resource) const { const auto i = m_Records.find(resource); return i != m_Records.end() && !i->second.Pending; }
        bool CanDestroy(const Texture* resource) const { return CanUse(resource); }
        bool PublishOrdinaryState(Texture& resource, ResourceState state)
        { auto i = m_Records.find(&resource); if (i == m_Records.end() || i->second.Pending || !Compatible(resource, state)) return false; i->second.State = state; return true; }
        bool RecordRelease(const TextureOwnershipRelease& release, QueueType list, QueueType source, QueueType destination, ResourceState baseline, RecordedTextureOwnershipOperation& operation) const
        { const auto i=m_Records.find(release.Resource); if (i==m_Records.end() || !SupportsWholeResource(*release.Resource) || i->second.Pending || source==destination || list!=source || i->second.Owner!=source || i->second.State!=baseline || !Compatible(*release.Resource,release.Before) || !Compatible(*release.Resource,release.After)) return false; operation={TextureOwnershipOperationType::Release,release.Resource,source,destination,release.Before,release.After,{}}; operation.CommittedBaseline=baseline; return true; }
        bool RecordRelease(const TextureOwnershipRelease& release, QueueType list, QueueType source, QueueType destination, RecordedTextureOwnershipOperation& operation) const
        { return RecordRelease(release, list, source, destination, release.Before, operation); }
        bool RecordAcquire(const TextureOwnershipAcquire& acquire, QueueType list, QueueType source, QueueType destination, RecordedTextureOwnershipOperation& operation) const
        { const auto i=m_Records.find(acquire.Resource); if (i==m_Records.end() || !SupportsWholeResource(*acquire.Resource) || !i->second.Pending || !acquire.ReleaseToken.IsValid() || source==destination || list!=destination || !SamePair(*i->second.Pending,source,destination,acquire.Before,acquire.After) || !SameToken(i->second.Pending->Token,acquire.ReleaseToken)) return false; operation={TextureOwnershipOperationType::Acquire,acquire.Resource,source,destination,acquire.Before,acquire.After,acquire.ReleaseToken}; return true; }
        bool ValidateSubmission(const RecordedTextureOwnershipOperation& op, const std::vector<CompletionToken>& dependencies) const
        { const auto i=m_Records.find(op.Resource); if(i==m_Records.end()) return false; if(op.Type==TextureOwnershipOperationType::Release) return !i->second.Pending && i->second.Owner==op.Source && i->second.State==op.CommittedBaseline; return i->second.Pending && SamePair(*i->second.Pending,op.Source,op.Destination,op.Before,op.After) && SameToken(i->second.Pending->Token,op.ReleaseToken) && std::count_if(dependencies.begin(),dependencies.end(),[&](const auto& d){return SameToken(d,op.ReleaseToken);})==1; }
        bool PublishRelease(const RecordedTextureOwnershipOperation& op, const CompletionToken& token)
        { auto i=m_Records.find(op.Resource); if(op.Type!=TextureOwnershipOperationType::Release || !token.IsValid() || i==m_Records.end() || i->second.Pending || i->second.Owner!=op.Source || (i->second.State!=op.CommittedBaseline && i->second.State!=op.Before && i->second.State!=op.After)) return false; i->second.Pending=PendingTransfer{op.Source,op.Destination,op.Before,op.After,token}; return true; }
        bool PublishAcquire(const RecordedTextureOwnershipOperation& op)
        { auto i=m_Records.find(op.Resource); if(op.Type!=TextureOwnershipOperationType::Acquire || i==m_Records.end() || !i->second.Pending || !SamePair(*i->second.Pending,op.Source,op.Destination,op.Before,op.After) || !SameToken(i->second.Pending->Token,op.ReleaseToken)) return false; i->second.Owner=op.Destination; i->second.State=op.After; i->second.Pending.reset(); return true; }
        bool Recover(Texture& resource, const CompletionToken& token, CompletionStatus status)
        { auto i=m_Records.find(&resource); if(i==m_Records.end() || !i->second.Pending || status!=CompletionStatus::Complete || !SameToken(i->second.Pending->Token,token)) return false; i->second.Owner=i->second.Pending->Source; i->second.State=i->second.Pending->Before; i->second.Pending.reset(); return true; }
        bool QueryPending(const Texture* resource, PendingTextureOwnershipTransfer& pending) const
        { const auto i=m_Records.find(resource); if(i==m_Records.end() || !i->second.Pending) return false; const PendingTransfer& transfer=*i->second.Pending; pending={ resource, transfer.Source, transfer.Destination, transfer.Before, transfer.After, transfer.Token }; return true; }
        bool QueryOwner(const Texture* resource, QueueType& owner) const { const auto i=m_Records.find(resource); if(i==m_Records.end()||i->second.Pending) return false; owner=i->second.Owner; return true; }
        bool QueryState(const Texture* resource, ResourceState& state) const { const auto i=m_Records.find(resource); if(i==m_Records.end()||i->second.Pending||i->second.State==ResourceState::Unknown) return false; state=i->second.State; return true; }
        bool HasPending(const Texture* resource) const { const auto i=m_Records.find(resource); return i!=m_Records.end() && i->second.Pending.has_value(); }
    private:
        struct PendingTransfer { QueueType Source, Destination; ResourceState Before, After; CompletionToken Token; };
        struct Record { QueueType Owner; ResourceState State; std::optional<PendingTransfer> Pending; };
        static bool SameToken(const CompletionToken& a,const CompletionToken& b) { return a.DeviceId==b.DeviceId && a.SubmissionId==b.SubmissionId; }
        static bool SamePair(const PendingTransfer& p,QueueType s,QueueType d,ResourceState b,ResourceState a) { return p.Source==s&&p.Destination==d&&p.Before==b&&p.After==a; }
        static bool SupportsWholeResource(const Texture& texture)
        {
            const TextureDescription& description = texture.GetDescription();
            return description.Extent.Width != 0 && description.Extent.Height != 0
                && description.MipLevels == 1 && description.ArrayLayers == 1 && description.SampleCount == 1;
        }
        static bool Compatible(const Texture& texture, ResourceState state)
        { const auto usage=static_cast<u32>(texture.GetDescription().Usage); const auto has=[&](TextureUsage u){return (usage&static_cast<u32>(u))!=0;}; switch(state) { case ResourceState::Common: return true; case ResourceState::RenderTarget:return has(TextureUsage::RenderTarget); case ResourceState::DepthWrite:return has(TextureUsage::DepthStencil); case ResourceState::ShaderResource:return has(TextureUsage::ShaderResource); case ResourceState::UnorderedAccess:return has(TextureUsage::UnorderedAccess); case ResourceState::CopySource:return has(TextureUsage::CopySource); case ResourceState::CopyDest:return has(TextureUsage::CopyDest); case ResourceState::Present:return has(TextureUsage::Present); default:return false; } }
        std::unordered_map<const Texture*,Record> m_Records;
    };
}
