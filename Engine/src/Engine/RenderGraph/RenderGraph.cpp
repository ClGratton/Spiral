#include "Engine/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <utility>

namespace Engine
{
    SubmittedRenderGraphFrameOwner::PollResult SubmittedRenderGraphFrameOwner::Poll(RHI::Device& device)
    {
        PollResult result;
        for (auto frame = m_Frames.begin(); frame != m_Frames.end();)
        {
            bool complete = true;
            for (const RHI::CompletionToken& token : frame->Identity.Completions)
            {
                const RHI::CompletionStatus status = device.QueryCompletion(token);
                if (status == RHI::CompletionStatus::Invalid || status == RHI::CompletionStatus::Failed)
                {
                    const std::vector<RenderGraph::RawTimestampScope> failedScopes = frame->Graph->CollectTimestampScopes(frame->Identity.FrameIndex);
                    result.TimestampScopes.insert(result.TimestampScopes.end(), failedScopes.begin(), failedScopes.end());
                    result.Success = false;
                    result.Error = "Submitted RenderGraph frame " + std::to_string(frame->Identity.FrameIndex)
                        + " has an invalid or failed exact completion token.";
                    result.PendingCount = m_Frames.size();
                    return result;
                }
                if (status != RHI::CompletionStatus::Complete)
                    complete = false;
            }
            if (!complete)
            {
                ++frame;
                continue;
            }

            const std::vector<RenderGraph::RawTimestampScope> scopes = frame->Graph->CollectTimestampScopes(frame->Identity.FrameIndex);
            for (const RenderGraph::RawTimestampScope& scope : scopes)
            {
                // Scope results are read before graph/pool destruction and only
                // after their exact submission is terminal. A pending raw read
                // is retained rather than converted to a made-up duration.
                if (scope.Start.Status == RHI::QueryResultStatus::Pending || scope.End.Status == RHI::QueryResultStatus::Pending)
                {
                    complete = false;
                    break;
                }
            }
            if (!complete) { ++frame; continue; }

            frame->Identity.TimestampScopes = scopes;
            result.TimestampScopes.insert(result.TimestampScopes.end(), scopes.begin(), scopes.end());
            result.Retired.emplace_back(std::move(frame->Identity));
            frame = m_Frames.erase(frame);
        }
        result.PendingCount = m_Frames.size();
        return result;
    }

    bool SubmittedRenderGraphFrameOwner::Retain(u64 frameIndex, Scope<RenderGraph> graph,
        const RenderGraph::CompileResult& compiled, const RenderGraph::ExecuteResult& executed,
        std::vector<Ref<void>> retainedPayloads, std::string* error)
    {
        const auto fail = [&](std::string_view message)
        {
            if (error)
                *error = message;
            return false;
        };
        if (!graph || !compiled.Success || executed.Completions.empty()
            || executed.Completions.size() != executed.AcceptedPassCount
            || executed.Completions.size() > compiled.Passes.size())
            return fail("A submitted RenderGraph frame has inconsistent graph, compile, or accepted-prefix state.");
        if (!HasCapacity())
            return fail("The submitted RenderGraph frame retirement owner is at bounded capacity.");
        if (std::any_of(m_Frames.begin(), m_Frames.end(), [frameIndex](const PendingFrame& pending)
            { return pending.Identity.FrameIndex == frameIndex; }))
            return fail("The submitted RenderGraph frame retirement owner already contains this frame ID.");
        if (std::any_of(retainedPayloads.begin(), retainedPayloads.end(), [](const Ref<void>& payload) { return !payload; }))
            return fail("A submitted RenderGraph frame contains an empty retained payload.");

        const u64 deviceId = executed.Completions.front().DeviceId;
        for (size_t index = 0; index < executed.Completions.size(); ++index)
        {
            const RHI::CompletionToken& token = executed.Completions[index];
            if (!token.IsValid() || token.DeviceId != deviceId || compiled.Passes[index].DebugName.empty())
                return fail("A submitted RenderGraph frame has an invalid token device or pass label.");
            if (std::any_of(executed.Completions.begin(), executed.Completions.begin() + static_cast<std::ptrdiff_t>(index),
                [&token](const RHI::CompletionToken& prior)
                { return prior.DeviceId == token.DeviceId && prior.SubmissionId == token.SubmissionId; }))
                return fail("A submitted RenderGraph frame repeats an exact completion token.");
        }
        const RHI::CompletionToken& last = executed.Completions.back();
        if (executed.Completion.DeviceId != last.DeviceId || executed.Completion.SubmissionId != last.SubmissionId)
            return fail("A submitted RenderGraph frame final token does not match its accepted prefix.");

        PendingFrame pending;
        pending.Identity.FrameIndex = frameIndex;
        pending.Identity.Completions = executed.Completions;
        pending.Identity.PassLabels.reserve(executed.Completions.size());
        for (size_t index = 0; index < executed.Completions.size(); ++index)
            pending.Identity.PassLabels.push_back(compiled.Passes[index].DebugName);
        if (!graph->m_TimestampScopes.empty() && graph->m_TimestampScopes.size() != executed.Completions.size())
            return fail("A submitted RenderGraph frame has incomplete timestamp-scope identity.");
        for (size_t index = 0; index < graph->m_TimestampScopes.size(); ++index)
        {
            const RenderGraph::RecordedTimestampScope& recorded = graph->m_TimestampScopes[index];
            const RHI::CompletionToken& expected = executed.Completions[index];
            if (!recorded.Pool || !recorded.Generation || recorded.PassLabel != compiled.Passes[index].DebugName
                || recorded.EffectiveQueue != RHI::QueueType::Graphics || recorded.PeriodNanoseconds <= 0.0
                || recorded.Token.DeviceId != expected.DeviceId || recorded.Token.SubmissionId != expected.SubmissionId)
                return fail("A submitted RenderGraph frame has invalid timestamp-scope identity.");
        }
        pending.Graph = std::move(graph);
        pending.RetainedPayloads = std::move(retainedPayloads);
        m_Frames.emplace_back(std::move(pending));
        return true;
    }

    std::vector<RenderGraph::RawTimestampScope> RenderGraph::CollectTimestampScopes(u64 frameIndex) const
    {
        std::vector<RawTimestampScope> scopes;
        scopes.reserve(m_TimestampScopes.size());
        for (const RecordedTimestampScope& recorded : m_TimestampScopes)
        {
            RawTimestampScope scope;
            scope.FrameIndex = frameIndex;
            scope.PassLabel = recorded.PassLabel;
            scope.Token = recorded.Token;
            scope.EffectiveQueue = recorded.EffectiveQueue;
            scope.PeriodNanoseconds = recorded.PeriodNanoseconds;
            scope.Start = recorded.Pool ? recorded.Pool->ReadResult(0, recorded.Generation) : RHI::QueryResult {};
            scope.End = recorded.Pool ? recorded.Pool->ReadResult(1, recorded.Generation) : RHI::QueryResult {};
            scopes.emplace_back(std::move(scope));
        }
        return scopes;
    }

    RHI::CapabilityGroupState RenderGraph::BuildTransientResourceCapabilityGroup(
        const RHI::DeviceCapabilities& capabilities)
    {
        RHI::CapabilityGroupState group;
        group.Group = RHI::CapabilityGroupId::Phase3TransientResourcesV1;
        group.ProfileName = "Phase 3 Transient Resources V1";
        group.PreferredPath = RHI::CapabilityPath::PlacedAliasedTransient;

        const RHI::CapabilityState& placement = capabilities.GetFeature(RHI::DeviceFeature::PlacedResources);
        const RHI::CapabilityState& aliasBarriers = capabilities.GetFeature(RHI::DeviceFeature::AliasingBarriers);
        if (placement.IsUsable() && aliasBarriers.IsUsable())
        {
            group.SelectedPath = RHI::CapabilityPath::PlacedAliasedTransient;
            group.Implemented = true;
            return group;
        }

        group.SelectedPath = RHI::CapabilityPath::NonAliasedGpuRetiredPool;
        group.Implemented = true;
        group.Fallbacks.emplace_back(
            "placed aliasing is unavailable; selected separately allocated or non-aliased pooled resources with GPU-retired reuse");
        group.UnsupportedReasons.emplace_back(placement.Detail.empty()
            ? "placed-resource capability is not usable on the active RHI device"
            : placement.Detail);
        group.UnsupportedReasons.emplace_back(aliasBarriers.Detail.empty()
            ? "alias-barrier capability is not usable on the active RHI device"
            : aliasBarriers.Detail);
        return group;
    }

    RenderGraph::ExecutionContext::ExecutionContext(RHI::CommandList& commandList, const std::vector<RHI::Texture*>& textures,
        const std::vector<RHI::Buffer*>& buffers, const std::vector<bool>& declared)
        : m_CommandList(&commandList), m_Textures(&textures), m_Buffers(&buffers), m_Declared(&declared) {}

    RHI::CommandList& RenderGraph::ExecutionContext::GetCommandList() const { return *m_CommandList; }
    RHI::Texture* RenderGraph::ExecutionContext::GetTexture(ResourceHandle resource) const
    {
        return resource.IsValid() && resource.Index < m_Declared->size() && (*m_Declared)[resource.Index] ? (*m_Textures)[resource.Index] : nullptr;
    }
    RHI::Buffer* RenderGraph::ExecutionContext::GetBuffer(ResourceHandle resource) const
    {
        return resource.IsValid() && resource.Index < m_Declared->size() && (*m_Declared)[resource.Index] ? (*m_Buffers)[resource.Index] : nullptr;
    }

    RenderGraph::ResourceHandle RenderGraph::AddTexture(RHI::TextureDescription description, ResourceLifetimeKind lifetime)
    {
        ResourceDescription resource;
        resource.DebugName = description.DebugName;
        resource.Kind = ResourceKind::Texture;
        resource.Lifetime = lifetime;
        resource.InitialState = description.InitialState;
        resource.Texture = std::move(description);
        return AddResource(std::move(resource));
    }

    RenderGraph::ResourceHandle RenderGraph::AddBuffer(RHI::BufferDescription description, ResourceLifetimeKind lifetime)
    {
        ResourceDescription resource;
        resource.DebugName = description.DebugName;
        resource.Kind = ResourceKind::Buffer;
        resource.Lifetime = lifetime;
        resource.InitialState = description.InitialState;
        resource.Buffer = std::move(description);
        return AddResource(std::move(resource));
    }

    RenderGraph::PassHandle RenderGraph::AddPass(std::string name, RHI::QueueType queue, bool allowCulling)
    {
        const PassHandle handle { static_cast<u32>(m_Passes.size()) };

        PassDescription pass;
        pass.DebugName = std::move(name);
        pass.Queue = queue;
        pass.AllowCulling = allowCulling;

        m_DebugPasses.emplace_back(pass.DebugName);
        m_Passes.emplace_back(std::move(pass));
        m_Callbacks.emplace_back();
        return handle;
    }

    void RenderGraph::SetPassCallback(PassHandle pass, PassCallback callback)
    {
        if (IsValid(pass)) m_Callbacks[pass.Index] = std::move(callback);
    }

    void RenderGraph::SetPassWorkerRecordingEligible(PassHandle pass, bool eligible)
    {
        if (IsValid(pass)) m_Passes[pass.Index].WorkerRecordingEligible = eligible;
    }

    bool RenderGraph::BindTexture(ResourceHandle resource, RHI::Texture& texture)
    {
        if (!IsValid(resource) || m_Resources[resource.Index].Kind != ResourceKind::Texture || !Matches(m_Resources[resource.Index].Texture, texture.GetDescription())) return false;
        if (m_BoundTextures.size() != m_Resources.size())
        {
            m_BoundTextures.resize(m_Resources.size());
            m_BoundBuffers.resize(m_Resources.size());
            m_AutoTransientBindings.resize(m_Resources.size());
        }
        m_BoundTextures[resource.Index] = &texture;
        m_AutoTransientBindings[resource.Index] = false;
        return true;
    }

    bool RenderGraph::BindBuffer(ResourceHandle resource, RHI::Buffer& buffer)
    {
        if (!IsValid(resource) || m_Resources[resource.Index].Kind != ResourceKind::Buffer || !Matches(m_Resources[resource.Index].Buffer, buffer.GetDescription())) return false;
        if (m_BoundTextures.size() != m_Resources.size()) { m_BoundTextures.resize(m_Resources.size()); m_BoundBuffers.resize(m_Resources.size()); m_AutoTransientBindings.resize(m_Resources.size()); }
        m_BoundBuffers[resource.Index] = &buffer;
        m_AutoTransientBindings[resource.Index] = false;
        return true;
    }

    void RenderGraph::AddDebugPass(std::string name)
    {
        AddPass(std::move(name));
    }

    void RenderGraph::AddResourceUse(PassHandle pass, ResourceUse use)
    {
        if (!IsValid(pass))
            return;

        m_Passes[pass.Index].Uses.emplace_back(use);
    }

    void RenderGraph::AddDependency(PassHandle producer, PassHandle consumer)
    {
        m_ExplicitDependencies.push_back({ producer, consumer, {} });
    }

    void RenderGraph::AddRead(PassHandle pass, ResourceHandle resource, RHI::ResourceState state, RHI::ShaderStage stages)
    {
        AddResourceUse(pass, Read(resource, state, stages));
    }

    void RenderGraph::AddWrite(PassHandle pass, ResourceHandle resource, RHI::ResourceState state)
    {
        AddResourceUse(pass, Write(resource, state));
    }

    void RenderGraph::AddReadWrite(PassHandle pass, ResourceHandle resource, RHI::ResourceState state, RHI::ShaderStage stages)
    {
        AddResourceUse(pass, ReadWrite(resource, state, stages));
    }

    void RenderGraph::Reset()
    {
        m_Resources.clear();
        m_Passes.clear();
        m_ExplicitDependencies.clear();
        m_DebugPasses.clear();
        m_Callbacks.clear();
        m_BoundTextures.clear();
        m_BoundBuffers.clear();
        m_AutoTransientBindings.clear();
    }

    RenderGraph::ExecuteResult RenderGraph::Execute(RHI::Device& device, const CompileResult& compiled)
    {
        return Execute(device, compiled, ExecuteOptions {});
    }

    RenderGraph::ExecuteResult RenderGraph::Execute(RHI::Device& device, const CompileResult& compiled, const ExecuteOptions& options)
    {
        ExecuteResult result;
        if (!compiled.Success) { result.Error = "Cannot execute an unsuccessful render-graph compilation."; return result; }
        if (options.EnableTimestampScopes && !m_TimestampScopes.empty())
        { result.Error = "This RenderGraph already owns submitted timestamp scopes."; return result; }
        if (m_RecordingDevice && m_RecordingDevice != &device) { result.Error = "Render graph recording contexts belong to another RHI device."; return result; }
        if (compiled.Passes.size() != m_Passes.size() || m_Callbacks.size() != m_Passes.size()) { result.Error = "Compiled passes do not match this render graph."; return result; }
        if (compiled.ResourceLifetimes.size() != m_Resources.size()) { result.Error = "Compiled resource lifetimes do not match this render graph."; return result; }
        for (u32 index = 0; index < compiled.ResourceLifetimes.size(); ++index)
            if (!IsValid(compiled.ResourceLifetimes[index].Resource) || compiled.ResourceLifetimes[index].Resource.Index != index)
            { result.Error = "A compiled resource lifetime has an invalid handle."; return result; }
        const RHI::CapabilityGroupState allocationGroup = BuildTransientResourceCapabilityGroup(device.GetCapabilities());
        result.TransientAllocationMode = allocationGroup.SelectedPath;
        if (allocationGroup.SelectedPath != RHI::CapabilityPath::NonAliasedGpuRetiredPool)
        {
            result.Error = "The selected placed/aliased transient path has no RHI resource or alias-barrier implementation.";
            return result;
        }
        if (m_BoundTextures.size() != m_Resources.size()) { m_BoundTextures.resize(m_Resources.size()); m_BoundBuffers.resize(m_Resources.size()); m_AutoTransientBindings.resize(m_Resources.size()); }
        for (u32 index = 0; index < m_Resources.size(); ++index)
            if (m_Resources[index].Lifetime == ResourceLifetimeKind::Transient && m_AutoTransientBindings[index])
            {
                m_BoundTextures[index] = nullptr;
                m_BoundBuffers[index] = nullptr;
                m_AutoTransientBindings[index] = false;
            }
        std::vector<u32> allocationForResource(m_Resources.size(), InvalidIndex);
        std::vector<bool> allocationClaimed(m_TransientAllocations.size(), false);
        std::vector<bool> allocationUsed(m_TransientAllocations.size(), false);
        std::vector<u32> allocationLastResource(m_TransientAllocations.size(), InvalidIndex);
        const auto lifetimeFor = [&](u32 resourceIndex) -> const ResourceLifetime*
        {
            return resourceIndex < compiled.ResourceLifetimes.size() ? &compiled.ResourceLifetimes[resourceIndex] : nullptr;
        };
        const auto finalStateFor = [&](u32 resourceIndex)
        {
            RHI::ResourceState state = m_Resources[resourceIndex].InitialState;
            for (const Barrier& barrier : compiled.Barriers) if (barrier.Resource.Index == resourceIndex) state = barrier.After;
            return state;
        };
        const auto singleEffectiveQueueFor = [&](u32 resourceIndex, RHI::QueueType& queue)
        {
            bool found = false;
            for (const CompiledPass& pass : compiled.Passes)
            {
                const bool used = std::any_of(m_Passes[pass.Pass.Index].Uses.begin(), m_Passes[pass.Pass.Index].Uses.end(),
                    [&](const ResourceUse& use) { return use.Resource.Index == resourceIndex; });
                if (!used) continue;
                const RHI::QueueType effective = device.ResolveQueue(pass.Queue).Effective;
                if (found && queue != effective) return false;
                queue = effective;
                found = true;
            }
            return found;
        };
        for (u32 index = 0; index < m_Resources.size(); ++index)
        {
            const ResourceDescription& resource = m_Resources[index];
            const ResourceLifetime& lifetime = compiled.ResourceLifetimes[index];
            if (resource.Lifetime == ResourceLifetimeKind::Imported)
            {
                if ((resource.Kind == ResourceKind::Texture && (!m_BoundTextures[index] || m_BoundBuffers[index]))
                    || (resource.Kind == ResourceKind::Buffer && (!m_BoundBuffers[index] || m_BoundTextures[index])))
                { result.Error = "An imported graph resource is not explicitly bound."; return result; }
                continue;
            }
            if (!lifetime.Used) continue;

            // Explicit bindings remain caller-owned. Unbound transients are
            // supplied from the selected RenderGraph pool.
            if ((resource.Kind == ResourceKind::Texture && m_BoundTextures[index] && !m_BoundBuffers[index])
                || (resource.Kind == ResourceKind::Buffer && m_BoundBuffers[index] && !m_BoundTextures[index]))
                continue;
            if ((resource.Kind == ResourceKind::Texture && m_BoundBuffers[index])
                || (resource.Kind == ResourceKind::Buffer && m_BoundTextures[index]))
            { result.Error = "A transient graph resource has an incompatible explicit binding."; return result; }

            const auto compatible = [&](const TransientAllocation& allocation)
            {
                return allocation.Kind == resource.Kind && (resource.Kind == ResourceKind::Texture
                    ? allocation.TextureResource && Matches(resource.Texture, allocation.Texture)
                    : allocation.BufferResource && Matches(resource.Buffer, allocation.Buffer));
            };
            bool selected = false;
            for (u32 allocationIndex = 0; allocationIndex < m_TransientAllocations.size(); ++allocationIndex)
            {
                TransientAllocation& allocation = m_TransientAllocations[allocationIndex];
                if (!compatible(allocation)) continue;
                if (allocationClaimed[allocationIndex])
                {
                    // The fallback has no native aliasing path. A single RHI
                    // object can nevertheless represent sequential logical
                    // lifetimes when they stay ordered on one effective queue
                    // and preserve the same declared state at the hand-off.
                    const u32 previousResource = allocationLastResource[allocationIndex];
                    const ResourceLifetime* previousLifetime = lifetimeFor(previousResource);
                    const ResourceLifetime* nextLifetime = lifetimeFor(index);
                    RHI::QueueType previousQueue = RHI::QueueType::Graphics;
                    RHI::QueueType nextQueue = RHI::QueueType::Graphics;
                    const bool sequential = previousLifetime && nextLifetime && previousLifetime->Used && nextLifetime->Used
                        && previousLifetime->LastPass < nextLifetime->FirstPass
                        && singleEffectiveQueueFor(previousResource, previousQueue) && singleEffectiveQueueFor(index, nextQueue)
                        && previousQueue == nextQueue && m_Resources[previousResource].InitialState == resource.InitialState
                        && finalStateFor(previousResource) == resource.InitialState;
                    if (!sequential) continue;
                    allocationLastResource[allocationIndex] = index;
                    allocationForResource[index] = allocationIndex;
                    allocationUsed[allocationIndex] = true;
                    m_AutoTransientBindings[index] = true;
                    if (resource.Kind == ResourceKind::Texture) m_BoundTextures[index] = allocation.TextureResource.get();
                    else m_BoundBuffers[index] = allocation.BufferResource.get();
                    selected = true;
                    break;
                }
                bool retired = true;
                for (const RHI::CompletionToken& token : allocation.RetirementTokens)
                {
                    const RHI::CompletionStatus completion = device.QueryCompletion(token);
                    if (completion == RHI::CompletionStatus::Invalid || completion == RHI::CompletionStatus::Failed)
                    { result.Error = "A pooled transient resource has an invalid or failed exact retirement token."; return result; }
                    if (completion != RHI::CompletionStatus::Complete) { retired = false; break; }
                }
                RHI::ResourceState state = RHI::ResourceState::Unknown;
                const bool stateMatches = resource.Kind == ResourceKind::Texture
                    ? device.QueryResourceState(allocation.TextureResource.get(), state) && state == resource.InitialState
                    : device.QueryResourceState(allocation.BufferResource.get(), state) && state == resource.InitialState;
                if (!retired || !stateMatches) continue;
                // The prior tokens were all observed complete and the object
                // still has the required state, so they no longer need to
                // grow the retirement history for this new use.
                allocation.RetirementTokens.clear();
                allocationClaimed[allocationIndex] = true;
                allocationLastResource[allocationIndex] = index;
                allocationForResource[index] = allocationIndex;
                allocationUsed[allocationIndex] = true;
                m_AutoTransientBindings[index] = true;
                if (resource.Kind == ResourceKind::Texture) m_BoundTextures[index] = allocation.TextureResource.get();
                else m_BoundBuffers[index] = allocation.BufferResource.get();
                ++result.ReusedRetiredTransientCount;
                selected = true;
                break;
            }
            if (selected) continue;

            TransientAllocation allocation;
            allocation.Kind = resource.Kind;
            allocation.Texture = resource.Texture;
            allocation.Buffer = resource.Buffer;
            allocation.EstimatedLogicalBytes = resource.Kind == ResourceKind::Texture ? EstimateLogicalBytes(resource.Texture) : EstimateLogicalBytes(resource.Buffer);
            if (!allocation.EstimatedLogicalBytes) { result.Error = "A transient graph resource has an unsupported, zero, or overflowing logical memory estimate."; return result; }
            if (resource.Kind == ResourceKind::Texture) allocation.TextureResource = device.CreateTexture(resource.Texture);
            else allocation.BufferResource = device.CreateBuffer(resource.Buffer);
            if ((resource.Kind == ResourceKind::Texture && !allocation.TextureResource)
                || (resource.Kind == ResourceKind::Buffer && !allocation.BufferResource))
            { result.Error = "Could not allocate a non-aliased transient graph resource."; return result; }
            m_TransientAllocations.emplace_back(std::move(allocation));
            allocationClaimed.emplace_back(true);
            allocationUsed.emplace_back(true);
            allocationLastResource.emplace_back(index);
            allocationForResource[index] = static_cast<u32>(m_TransientAllocations.size() - 1);
            m_AutoTransientBindings[index] = true;
            if (resource.Kind == ResourceKind::Texture) m_BoundTextures[index] = m_TransientAllocations.back().TextureResource.get();
            else m_BoundBuffers[index] = m_TransientAllocations.back().BufferResource.get();
            result.EstimatedTransientAllocatedBytes += m_TransientAllocations.back().EstimatedLogicalBytes;
        }
        for (const TransientAllocation& allocation : m_TransientAllocations) result.EstimatedTransientPooledBytes += allocation.EstimatedLogicalBytes;
        for (u32 index = 0; index < m_Resources.size(); ++index)
            if (m_Resources[index].Lifetime == ResourceLifetimeKind::Transient && allocationForResource[index] != InvalidIndex)
                ++result.TransientResourceCount;
        for (const CompiledPass& pass : compiled.Passes)
        {
            if (!IsValid(pass.Pass) || pass.DebugName != m_Passes[pass.Pass.Index].DebugName || pass.Queue != m_Passes[pass.Pass.Index].Queue) { result.Error = "A compiled pass does not match this render graph."; return result; }
            if (pass.FirstBarrier > compiled.Barriers.size() || pass.BarrierCount > compiled.Barriers.size() - pass.FirstBarrier) { result.Error = "A compiled pass has an invalid barrier range."; return result; }
            if (!m_Callbacks[pass.Pass.Index]) { result.Error = "A compiled graph pass has no execution callback."; return result; }
            for (u32 barrierIndex = pass.FirstBarrier; barrierIndex < pass.FirstBarrier + pass.BarrierCount; ++barrierIndex)
                if (!IsValid(compiled.Barriers[barrierIndex].Resource) || compiled.Barriers[barrierIndex].Pass.Index != pass.Pass.Index) { result.Error = "A compiled graph barrier is invalid."; return result; }
        }
        std::vector<RHI::QueueResolution> resolutions(m_Passes.size());
        for (const CompiledPass& pass : compiled.Passes)
            resolutions[pass.Pass.Index] = device.ResolveQueue(pass.Queue);
        std::vector<Scope<RHI::QueryPool>> timestampPools(m_Passes.size());
        if (options.EnableTimestampScopes)
        {
            for (const CompiledPass& pass : compiled.Passes)
            {
                if (resolutions[pass.Pass.Index].Effective != RHI::QueueType::Graphics)
                { result.Error = "RenderGraph timestamp scopes require one effective Graphics clock domain."; return result; }
                RHI::QueryPoolDescription description;
                description.DebugName = "RenderGraph Timestamp: " + pass.DebugName;
                description.Type = RHI::QueryType::Timestamp;
                description.Count = 2;
                timestampPools[pass.Pass.Index] = device.CreateQueryPool(description);
                if (!timestampPools[pass.Pass.Index] || timestampPools[pass.Pass.Index]->GetTimestampPeriodNanoseconds() <= 0.0)
                { result.Error = "Could not allocate a usable timestamp query pool for RenderGraph pass '" + pass.DebugName + "'."; return result; }
            }
        }
        const auto beginTimestampScope = [&](u32 passIndex, RHI::CommandList& commands)
        {
            RHI::QueryPool* pool = timestampPools[passIndex].get();
            return !pool || (commands.ResetQueryPool(*pool, 0, 2) && commands.WriteTimestamp(*pool, 0));
        };
        const auto endTimestampScope = [&](u32 passIndex, RHI::CommandList& commands)
        {
            RHI::QueryPool* pool = timestampPools[passIndex].get();
            return !pool || (commands.WriteTimestamp(*pool, 1) && commands.ResolveQueryPool(*pool, 0, 2));
        };
        const auto retainTimestampScope = [&](const CompiledPass& pass, const RHI::CompletionToken& token)
        {
            Scope<RHI::QueryPool>& pool = timestampPools[pass.Pass.Index];
            if (!pool)
                return true;
            const RHI::QueryResult start = pool->ReadResult(0);
            if (start.Status != RHI::QueryResultStatus::Pending || !start.Generation)
                return false;
            RecordedTimestampScope scope;
            scope.PassLabel = pass.DebugName;
            scope.Token = token;
            scope.EffectiveQueue = resolutions[pass.Pass.Index].Effective;
            scope.Generation = start.Generation;
            scope.PeriodNanoseconds = pool->GetTimestampPeriodNanoseconds();
            scope.Pool = std::move(pool);
            m_TimestampScopes.emplace_back(std::move(scope));
            return true;
        };
        for (u32 index = 0; index < m_Resources.size(); ++index)
        {
            const ResourceDescription& resource = m_Resources[index];
            if (resource.Lifetime == ResourceLifetimeKind::Transient && !compiled.ResourceLifetimes[index].Used) continue;
            RHI::ResourceState observed = RHI::ResourceState::Unknown;
            const bool valid = resource.Kind == ResourceKind::Texture
                ? m_BoundTextures[index] && !m_BoundBuffers[index] && Matches(resource.Texture, m_BoundTextures[index]->GetDescription())
                    && device.OwnsResource(m_BoundTextures[index]) && device.QueryResourceState(m_BoundTextures[index], observed)
                : m_BoundBuffers[index] && !m_BoundTextures[index] && Matches(resource.Buffer, m_BoundBuffers[index]->GetDescription())
                    && device.OwnsResource(m_BoundBuffers[index]) && device.QueryResourceState(m_BoundBuffers[index], observed);
            RHI::QueueType owner = RHI::QueueType::Graphics;
            const bool ownerValid = resource.Kind == ResourceKind::Texture
                ? device.QueryTextureQueueOwner(m_BoundTextures[index], owner)
                : device.QueryBufferQueueOwner(m_BoundBuffers[index], owner);
            if (!valid || !ownerValid || owner != RHI::QueueType::Graphics || observed != resource.InitialState)
            { result.Error = "A bound graph resource has invalid ownership or does not match its declared initial state."; return result; }
        }
        m_RecordingDevice = &device;
        std::vector<std::vector<const QueueTransition*>> releases(m_Passes.size()), acquires(m_Passes.size());
        for (const QueueTransition& transition : compiled.QueueTransitions)
        {
            if (!IsValid(transition.Producer) || !IsValid(transition.Consumer) || !IsValid(transition.Resource))
            { result.Error = "A compiled graph queue transition is invalid."; return result; }
            if (resolutions[transition.Producer.Index].Effective != resolutions[transition.Consumer.Index].Effective)
            {
                releases[transition.Producer.Index].push_back(&transition);
                acquires[transition.Consumer.Index].push_back(&transition);
            }
        }
        std::vector<RHI::CompletionToken> passTokens(m_Passes.size());
        const auto acquireContext = [&](RHI::QueueType requested, RHI::QueueType effective, u32 passIndex, u32& contextIndex) -> RecordingContext*
        {
            contextIndex = InvalidIndex;
            u32 contextsOnQueue = 0;
            for (u32 index = 0; index < m_RecordingContexts.size(); ++index)
            {
                RecordingContext& candidate = m_RecordingContexts[index];
                if (candidate.EffectiveQueue != effective || candidate.PassIndex != passIndex) continue;
                ++contextsOnQueue;
                if (!candidate.Completion.IsValid()) { contextIndex = index; return &candidate; }
                const RHI::CompletionStatus completion = device.QueryCompletion(candidate.Completion);
                if (completion == RHI::CompletionStatus::Complete) { contextIndex = index; result.ReusedRetiredContext = true; return &candidate; }
                if (completion == RHI::CompletionStatus::Invalid || completion == RHI::CompletionStatus::Failed) return nullptr;
            }
            if (contextsOnQueue >= 3) return nullptr;
            RecordingContext candidate;
            candidate.EffectiveQueue = effective;
            candidate.PassIndex = passIndex;
            candidate.CommandList = device.CreateCommandList(requested, "RenderGraph Execution");
            if (!candidate.CommandList) return nullptr;
            m_RecordingContexts.emplace_back(std::move(candidate));
            contextIndex = static_cast<u32>(m_RecordingContexts.size() - 1);
            return &m_RecordingContexts.back();
        };
        // A same-effective dependency does not itself forbid pre-recording:
        // each RHI context starts from the immutable execution-start wrapper
        // state. Token-dependent ownership acquisition still must wait for the
        // producer submission below.
        std::vector<u8> recordedOnWorker(m_Passes.size(), 0);
        std::vector<std::string> recordingErrors(m_Passes.size());
        std::vector<u32> workerContextIndices(m_Passes.size(), InvalidIndex);
        std::vector<RHI::CommandList*> workerCommandLists(m_Passes.size(), nullptr);
        std::vector<u32> workerCandidates;
        for (const CompiledPass& pass : compiled.Passes)
        {
            const u32 passIndex = pass.Pass.Index;
            const bool dependencyIsSameEffective = std::all_of(compiled.Dependencies.begin(), compiled.Dependencies.end(),
                [&](const Dependency& dependency) { return dependency.Consumer.Index != passIndex || resolutions[dependency.Producer.Index].Effective == resolutions[passIndex].Effective; });
            if (m_Passes[passIndex].WorkerRecordingEligible && dependencyIsSameEffective && acquires[passIndex].empty() && releases[passIndex].empty()) workerCandidates.push_back(passIndex);
        }
        for (u32 passIndex : workerCandidates)
        {
            u32 contextIndex = InvalidIndex;
            if (!acquireContext(m_Passes[passIndex].Queue, resolutions[passIndex].Effective, passIndex, contextIndex))
            { result.Error = "All bounded graph recording contexts are in flight or a completion query failed."; return result; }
            workerContextIndices[passIndex] = contextIndex;
            workerCommandLists[passIndex] = m_RecordingContexts[contextIndex].CommandList.get();
        }
        if (!workerCandidates.empty())
        {
            std::atomic<u32> active { 0 }, peak { 0 };
            const auto recordWorkerCandidate = [&](u32 passIndex)
            {
                const u32 now = active.fetch_add(1) + 1;
                u32 observed = peak.load(); while (observed < now && !peak.compare_exchange_weak(observed, now)) {}
                RecordingContext& context = m_RecordingContexts[workerContextIndices[passIndex]];
                const CompiledPass& compiledPass = *std::find_if(compiled.Passes.begin(), compiled.Passes.end(), [passIndex](const CompiledPass& value) { return value.Pass.Index == passIndex; });
                bool ok = context.CommandList->Begin();
                if (ok) ok = beginTimestampScope(passIndex, *context.CommandList);
                for (u32 barrierIndex = compiledPass.FirstBarrier; ok && barrierIndex < compiledPass.FirstBarrier + compiledPass.BarrierCount; ++barrierIndex)
                {
                    const Barrier& barrier = compiled.Barriers[barrierIndex];
                    ok = m_Resources[barrier.Resource.Index].Kind == ResourceKind::Texture
                        ? context.CommandList->TransitionTexture(*m_BoundTextures[barrier.Resource.Index], barrier.Before, barrier.After)
                        : context.CommandList->TransitionBuffer(*m_BoundBuffers[barrier.Resource.Index], barrier.Before, barrier.After);
                }
                std::vector<bool> declared(m_Resources.size(), false); for (const ResourceUse& use : m_Passes[passIndex].Uses) declared[use.Resource.Index] = true;
                if (ok)
                {
                    ExecutionContext execution(*context.CommandList, m_BoundTextures, m_BoundBuffers, declared);
                    context.CommandList->BeginDebugMarker(m_Passes[passIndex].DebugName);
                    try { ok = m_Callbacks[passIndex](execution); }
                    catch (...) { recordingErrors[passIndex] = "A graph pass callback threw an exception."; ok = false; }
                    context.CommandList->EndDebugMarker();
                }
                if (ok) ok = endTimestampScope(passIndex, *context.CommandList);
                if (ok) ok = context.CommandList->End();
                if (!ok && recordingErrors[passIndex].empty()) recordingErrors[passIndex] = "A graph worker recording failed.";
                recordedOnWorker[passIndex] = ok ? 1 : 2;
                active.fetch_sub(1);
            };
            // Recording is deliberately independent of submission dependencies.
            // Every candidate was admitted only when it needs neither a
            // cross-effective acquire nor a release; its compiler-supplied
            // expected-before state is validated at its later compiled-order
            // submission. This lets an eligible same-effective consumer (the
            // Scene Output Handoff) pre-record beside its producer while the
            // submission loop still supplies the producer token and publishes
            // no state before native acceptance.
            FrameTaskGraph recordingTasks;
            for (u32 passIndex : workerCandidates)
            {
                FrameTaskDescription task; task.Name = "RenderGraph.Record:" + m_Passes[passIndex].DebugName; task.Lane = FrameTaskLane::Worker;
                task.Execute = [&, passIndex] { recordWorkerCandidate(passIndex); };
                recordingTasks.AddTask(std::move(task));
            }
            FrameTaskExecutionOptions recordingOptions; recordingOptions.Mode = options.RecordingMode;
            const FrameTaskGraphResult recordingResult = recordingTasks.Execute(JobSystem::Get(), recordingOptions);
            if (!recordingResult.Succeeded()) { result.Error = "Render graph worker-recording task failed."; return result; }
            result.WorkerRecordedPassCount = static_cast<u32>(workerCandidates.size());
            result.WorkerRecordingOverlapObserved = peak.load() > 1;
        }
        for (const CompiledPass& compiledPass : compiled.Passes)
        {
            const auto discardUnsubmittedWorkerContexts = [&]()
            {
                for (u32 index = static_cast<u32>(m_RecordingContexts.size()); index-- > 0;)
                {
                    const bool isUnsubmittedWorkerContext = std::any_of(workerCommandLists.begin(), workerCommandLists.end(),
                        [&](RHI::CommandList* commandList) { return commandList == m_RecordingContexts[index].CommandList.get(); })
                        && !m_RecordingContexts[index].Completion.IsValid();
                    if (isUnsubmittedWorkerContext) m_RecordingContexts.erase(m_RecordingContexts.begin() + index);
                }
            };
            if (recordedOnWorker[compiledPass.Pass.Index] == 2)
            {
                discardUnsubmittedWorkerContexts();
                result.Error = recordingErrors[compiledPass.Pass.Index];
                return result;
            }
            const RHI::QueueResolution& resolution = resolutions[compiledPass.Pass.Index];
            u32 contextIndex = InvalidIndex;
            RecordingContext* context = recordedOnWorker[compiledPass.Pass.Index] == 1
                ? &m_RecordingContexts[workerContextIndices[compiledPass.Pass.Index]]
                : acquireContext(compiledPass.Queue, resolution.Effective, compiledPass.Pass.Index, contextIndex);
            if (recordedOnWorker[compiledPass.Pass.Index] == 1)
                contextIndex = workerContextIndices[compiledPass.Pass.Index];
            if (!context) { result.Error = "All bounded graph recording contexts are in flight or a completion query failed."; return result; }
            if (result.RecordingContextIndex == InvalidIndex) result.RecordingContextIndex = contextIndex;
            const auto discardContext = [this, contextIndex]() { m_RecordingContexts.erase(m_RecordingContexts.begin() + contextIndex); };
            std::vector<RHI::CompletionToken> dependencies;
            for (const Dependency& dependency : compiled.Dependencies)
                if (dependency.Consumer.Index == compiledPass.Pass.Index)
                {
                    const RHI::CompletionToken producerToken = passTokens[dependency.Producer.Index];
                    if (!producerToken.IsValid()) { result.Error = "A graph dependency has no accepted producer token."; discardContext(); return result; }
                    if (std::find_if(dependencies.begin(), dependencies.end(), [&](const RHI::CompletionToken& token) { return token.DeviceId == producerToken.DeviceId && token.SubmissionId == producerToken.SubmissionId; }) == dependencies.end()) dependencies.push_back(producerToken);
                }
            if (recordedOnWorker[compiledPass.Pass.Index] == 1)
            {
                const RHI::CompletionToken token = device.Submit(*context->CommandList, dependencies);
                if (!token.IsValid()) { result.Error = "Could not submit worker-recorded graph pass '" + compiledPass.DebugName + "'."; discardContext(); return result; }
                context->Completion = token; passTokens[compiledPass.Pass.Index] = token;
                const bool timestampPublished = retainTimestampScope(compiledPass, token);
                for (const ResourceUse& use : m_Passes[compiledPass.Pass.Index].Uses)
                {
                    const u32 allocationIndex = allocationForResource[use.Resource.Index]; if (allocationIndex == InvalidIndex) continue;
                    std::vector<RHI::CompletionToken>& retirement = m_TransientAllocations[allocationIndex].RetirementTokens;
                    if (std::find_if(retirement.begin(), retirement.end(), [&](const RHI::CompletionToken& prior) { return prior.DeviceId == token.DeviceId && prior.SubmissionId == token.SubmissionId; }) == retirement.end()) retirement.push_back(token);
                }
                result.Completions.push_back(token); result.Completion = token; ++result.AcceptedPassCount;
                if (!timestampPublished) { result.Error = "Accepted RenderGraph timestamp scope did not publish its exact generation."; return result; }
                continue;
            }
            if (!context->CommandList->Begin() || !beginTimestampScope(compiledPass.Pass.Index, *context->CommandList))
            { result.Error = "Could not begin a GPU-retired graph recording context or timestamp scope."; discardContext(); return result; }
            for (const QueueTransition* transition : acquires[compiledPass.Pass.Index])
            {
                const RHI::CompletionToken releaseToken = passTokens[transition->Producer.Index];
                if (!releaseToken.IsValid()) { result.Error = "A graph ownership acquire has no accepted release token."; discardContext(); return result; }
                const bool acquired = m_Resources[transition->Resource.Index].Kind == ResourceKind::Texture
                    ? context->CommandList->AcquireTextureOwnership({ m_BoundTextures[transition->Resource.Index], transition->SourceQueue, transition->DestinationQueue, transition->Before, transition->After, releaseToken })
                    : context->CommandList->AcquireBufferOwnership({ m_BoundBuffers[transition->Resource.Index], transition->SourceQueue, transition->DestinationQueue, transition->Before, transition->After, releaseToken });
                if (!acquired) { result.Error = "Could not record a compiled graph ownership acquire."; discardContext(); return result; }
                if (std::find_if(dependencies.begin(), dependencies.end(), [&](const RHI::CompletionToken& token) { return token.DeviceId == releaseToken.DeviceId && token.SubmissionId == releaseToken.SubmissionId; }) == dependencies.end()) dependencies.push_back(releaseToken);
            }
            for (u32 barrierIndex = compiledPass.FirstBarrier; barrierIndex < compiledPass.FirstBarrier + compiledPass.BarrierCount; ++barrierIndex)
            {
                const Barrier& barrier = compiled.Barriers[barrierIndex];
                // A cross-effective ownership acquire records the destination
                // state transition itself. Re-emitting the compiler barrier
                // would be an ordinary use while the tracker is pending (and
                // is therefore both redundant and invalid on Vulkan).
                const bool ownershipAcquireTransitionsBarrier = std::any_of(acquires[compiledPass.Pass.Index].begin(),
                    acquires[compiledPass.Pass.Index].end(), [&](const QueueTransition* transition)
                    {
                        return transition->Resource.Index == barrier.Resource.Index
                            && transition->Before == barrier.Before && transition->After == barrier.After;
                    });
                if (ownershipAcquireTransitionsBarrier)
                    continue;
                const bool transitioned = m_Resources[barrier.Resource.Index].Kind == ResourceKind::Texture
                    ? context->CommandList->TransitionTexture(*m_BoundTextures[barrier.Resource.Index], barrier.Before, barrier.After)
                    : context->CommandList->TransitionBuffer(*m_BoundBuffers[barrier.Resource.Index], barrier.Before, barrier.After);
                if (!transitioned) { result.Error = "Could not record a compiled graph transition."; discardContext(); return result; }
            }
            const PassDescription& pass = m_Passes[compiledPass.Pass.Index];
            std::vector<bool> declared(m_Resources.size(), false);
            for (const ResourceUse& use : pass.Uses) declared[use.Resource.Index] = true;
            ExecutionContext execution(*context->CommandList, m_BoundTextures, m_BoundBuffers, declared);
            context->CommandList->BeginDebugMarker(pass.DebugName);
            bool callbackSucceeded = false;
            try
            {
                callbackSucceeded = m_Callbacks[compiledPass.Pass.Index](execution);
            }
            catch (...)
            {
                context->CommandList->EndDebugMarker();
                result.Error = "A graph pass callback threw an exception."; discardContext(); return result;
            }
            context->CommandList->EndDebugMarker();
            if (!callbackSucceeded) { result.Error = "A graph pass callback failed."; discardContext(); return result; }
            for (const QueueTransition* transition : releases[compiledPass.Pass.Index])
            {
                const bool released = m_Resources[transition->Resource.Index].Kind == ResourceKind::Texture
                    ? context->CommandList->ReleaseTextureOwnership({ m_BoundTextures[transition->Resource.Index], transition->SourceQueue, transition->DestinationQueue, transition->Before, transition->After })
                    : context->CommandList->ReleaseBufferOwnership({ m_BoundBuffers[transition->Resource.Index], transition->SourceQueue, transition->DestinationQueue, transition->Before, transition->After });
                if (!released) { result.Error = "Could not record a compiled graph ownership release."; discardContext(); return result; }
            }
            if (!endTimestampScope(compiledPass.Pass.Index, *context->CommandList) || !context->CommandList->End())
            { result.Error = "Could not close graph recording or its timestamp scope."; discardContext(); return result; }
            const RHI::CompletionToken token = device.Submit(*context->CommandList, dependencies);
            if (!token.IsValid()) { result.Error = "Could not submit graph recording for pass '" + compiledPass.DebugName + "'."; discardContext(); return result; }
            context->Completion = token;
            passTokens[compiledPass.Pass.Index] = token;
            const bool timestampPublished = retainTimestampScope(compiledPass, token);
            // Publish retirement at the accepted-submission boundary. A later
            // graph failure must retain this exact token on every transient
            // physical object touched by the accepted pass.
            for (const ResourceUse& use : m_Passes[compiledPass.Pass.Index].Uses)
            {
                const u32 allocationIndex = allocationForResource[use.Resource.Index];
                if (allocationIndex == InvalidIndex) continue;
                std::vector<RHI::CompletionToken>& retirement = m_TransientAllocations[allocationIndex].RetirementTokens;
                if (std::find_if(retirement.begin(), retirement.end(), [&](const RHI::CompletionToken& prior)
                    { return prior.DeviceId == token.DeviceId && prior.SubmissionId == token.SubmissionId; }) == retirement.end())
                    retirement.push_back(token);
            }
            result.Completions.push_back(token);
            result.Completion = token;
            ++result.AcceptedPassCount;
            if (!timestampPublished) { result.Error = "Accepted RenderGraph timestamp scope did not publish its exact generation."; return result; }
        }
        for (u32 index = 0; index < m_Resources.size(); ++index)
        {
            if (m_Resources[index].Lifetime != ResourceLifetimeKind::Imported) continue;
            RHI::ResourceState observed = RHI::ResourceState::Unknown;
            const bool queried = m_Resources[index].Kind == ResourceKind::Texture
                ? device.QueryResourceState(m_BoundTextures[index], observed) : device.QueryResourceState(m_BoundBuffers[index], observed);
            RHI::ResourceState expected = m_Resources[index].InitialState;
            for (const Barrier& barrier : compiled.Barriers) if (barrier.Resource.Index == index) expected = barrier.After;
            RHI::QueueType expectedOwner = RHI::QueueType::Graphics;
            for (const CompiledPass& pass : compiled.Passes)
                for (const ResourceUse& use : m_Passes[pass.Pass.Index].Uses)
                    if (use.Resource.Index == index) expectedOwner = resolutions[pass.Pass.Index].Effective;
            RHI::QueueType observedOwner = RHI::QueueType::Graphics;
            const bool queriedOwner = m_Resources[index].Kind == ResourceKind::Texture
                ? device.QueryTextureQueueOwner(m_BoundTextures[index], observedOwner) : device.QueryBufferQueueOwner(m_BoundBuffers[index], observedOwner);
            if (!queried || !queriedOwner || observed != expected || observedOwner != expectedOwner)
            {
                result.Error = "Submitted graph did not publish its imported final state or owner for resource '" + m_Resources[index].DebugName
                    + "' (state=" + ToString(observed) + ", expected=" + ToString(expected) + ").";
                return result;
            }
        }
        for (u32 allocationIndex = 0; allocationIndex < m_TransientAllocations.size(); ++allocationIndex)
            if (allocationUsed[allocationIndex] && m_TransientAllocations[allocationIndex].RetirementTokens.empty())
            { result.Error = "A transient graph allocation has no accepted lifetime token."; return result; }
        result.Success = true;
        return result;
    }

    RenderGraph::CompileResult RenderGraph::Compile() const
    {
        CompileResult result;
        const auto failPass = [](const PassDescription& pass, std::string_view message)
        {
            std::ostringstream stream;
            stream << "Pass '" << pass.DebugName << "' " << message;
            return Fail(stream.str());
        };

        const auto allowsState = [](RHI::QueueType queue, RHI::ResourceState state)
        {
            if (queue == RHI::QueueType::Copy)
                return state == RHI::ResourceState::CopySource || state == RHI::ResourceState::CopyDest;
            if (queue == RHI::QueueType::Compute)
                return state != RHI::ResourceState::RenderTarget && state != RHI::ResourceState::DepthWrite && state != RHI::ResourceState::Present;
            return state != RHI::ResourceState::Unknown;
        };

        const u32 passCount = static_cast<u32>(m_Passes.size());
        std::vector<std::vector<u32>> outgoing(passCount);
        std::vector<u32> indegrees(passCount, 0);
        std::vector<std::optional<u32>> lastWriter(m_Resources.size());
        std::vector<std::vector<u32>> readersSinceWrite(m_Resources.size());

        const auto addDependency = [&result, &outgoing, &indegrees](PassHandle producer, PassHandle consumer, ResourceHandle resource)
        {
            if (producer.Index == consumer.Index)
                return;

            const auto& consumers = outgoing[producer.Index];
            if (std::find(consumers.begin(), consumers.end(), consumer.Index) != consumers.end())
                return;

            outgoing[producer.Index].push_back(consumer.Index);
            ++indegrees[consumer.Index];
            result.Dependencies.push_back({ producer, consumer, resource });
        };

        for (const Dependency& dependency : m_ExplicitDependencies)
        {
            if (!IsValid(dependency.Producer) || !IsValid(dependency.Consumer))
                return Fail("An explicit pass dependency references an invalid pass handle.");
            if (dependency.Producer.Index == dependency.Consumer.Index)
                return Fail("An explicit pass dependency cannot reference the same producer and consumer.");
            addDependency(dependency.Producer, dependency.Consumer, {});
        }

        for (u32 resourceIndex = 0; resourceIndex < m_Resources.size(); ++resourceIndex)
        {
            if (m_Resources[resourceIndex].DebugName.empty())
                return Fail("A resource has an empty debug name.");
            if (m_Resources[resourceIndex].InitialState == RHI::ResourceState::Unknown)
                return Fail("Resource '" + m_Resources[resourceIndex].DebugName + "' has an unknown initial state.");
        }

        for (u32 passIndex = 0; passIndex < passCount; ++passIndex)
        {
            const PassDescription& pass = m_Passes[passIndex];
            if (pass.DebugName.empty())
                return Fail("A pass has an empty debug name.");

            std::vector<bool> usedResources(m_Resources.size(), false);
            for (const ResourceUse& use : pass.Uses)
            {
                if (!IsValid(use.Resource))
                    return failPass(pass, "references an invalid resource handle.");
                if (usedResources[use.Resource.Index])
                    return failPass(pass, "declares the same resource more than once; use ReadWrite for a single combined use.");
                usedResources[use.Resource.Index] = true;
                if (use.RequiredState == RHI::ResourceState::Unknown || !allowsState(pass.Queue, use.RequiredState))
                    return failPass(pass, "declares a state that is invalid for its queue.");
                if (m_Resources[use.Resource.Index].Kind == ResourceKind::Buffer
                    && (use.RequiredState == RHI::ResourceState::RenderTarget || use.RequiredState == RHI::ResourceState::DepthWrite || use.RequiredState == RHI::ResourceState::Present))
                    return failPass(pass, "declares a texture-only state for a buffer.");
                if ((use.Usage == Access::Read || use.Usage == Access::ReadWrite) && use.Stages == RHI::ShaderStage::None)
                    return failPass(pass, "declares a read without shader-stage intent.");
                if (use.Usage == Access::Write && use.Stages != RHI::ShaderStage::None)
                    return failPass(pass, "declares a write with shader-stage intent.");
                if (use.Usage == Access::Read && (use.RequiredState == RHI::ResourceState::RenderTarget || use.RequiredState == RHI::ResourceState::DepthWrite || use.RequiredState == RHI::ResourceState::CopyDest || use.RequiredState == RHI::ResourceState::Present))
                    return failPass(pass, "declares a read with a write-only state.");
                if (use.Usage == Access::ReadWrite && use.RequiredState != RHI::ResourceState::UnorderedAccess)
                    return failPass(pass, "declares ReadWrite without unordered-access state.");
                if (use.Usage == Access::Write && (use.RequiredState == RHI::ResourceState::ShaderResource || use.RequiredState == RHI::ResourceState::CopySource))
                    return failPass(pass, "declares a write with a read-only state.");

                const bool reads = use.Usage != Access::Write;
                const bool writes = use.Usage != Access::Read;
                if (reads)
                {
                    if (lastWriter[use.Resource.Index].has_value())
                        addDependency({ *lastWriter[use.Resource.Index] }, { passIndex }, use.Resource);
                    else if (m_Resources[use.Resource.Index].Lifetime == ResourceLifetimeKind::Transient)
                        return failPass(pass, "reads transient resource '" + m_Resources[use.Resource.Index].DebugName + "' before any write.");
                    readersSinceWrite[use.Resource.Index].push_back(passIndex);
                }
                if (writes)
                {
                    if (lastWriter[use.Resource.Index].has_value())
                        addDependency({ *lastWriter[use.Resource.Index] }, { passIndex }, use.Resource);
                    for (u32 reader : readersSinceWrite[use.Resource.Index])
                        addDependency({ reader }, { passIndex }, use.Resource);
                    readersSinceWrite[use.Resource.Index].clear();
                    lastWriter[use.Resource.Index] = passIndex;
                }
            }
        }

        std::vector<u32> ready;
        for (u32 passIndex = 0; passIndex < passCount; ++passIndex)
            if (indegrees[passIndex] == 0)
                ready.push_back(passIndex);
        std::sort(ready.begin(), ready.end());

        std::vector<u32> orderedPasses;
        orderedPasses.reserve(passCount);
        while (!ready.empty())
        {
            const u32 passIndex = ready.front();
            ready.erase(ready.begin());
            orderedPasses.push_back(passIndex);
            for (u32 consumer : outgoing[passIndex])
            {
                if (--indegrees[consumer] == 0)
                {
                    const auto insertAt = std::lower_bound(ready.begin(), ready.end(), consumer);
                    ready.insert(insertAt, consumer);
                }
            }
        }
        if (orderedPasses.size() != passCount)
            return Fail("Render graph contains a dependency cycle.");

        result.Success = true;
        result.ResourceLifetimes.resize(m_Resources.size());
        std::vector<RHI::ResourceState> currentStates;
        std::vector<std::optional<u32>> lastUsePass(m_Resources.size());
        currentStates.reserve(m_Resources.size());
        for (u32 resourceIndex = 0; resourceIndex < m_Resources.size(); ++resourceIndex)
        {
            result.ResourceLifetimes[resourceIndex].Resource = { resourceIndex };
            currentStates.push_back(m_Resources[resourceIndex].InitialState);
        }

        for (u32 orderIndex = 0; orderIndex < orderedPasses.size(); ++orderIndex)
        {
            const u32 passIndex = orderedPasses[orderIndex];
            const PassDescription& pass = m_Passes[passIndex];
            const u32 firstBarrier = static_cast<u32>(result.Barriers.size());
            for (const ResourceUse& use : pass.Uses)
            {
                ResourceLifetime& lifetime = result.ResourceLifetimes[use.Resource.Index];
                if (!lifetime.Used)
                {
                    lifetime.FirstPass = orderIndex;
                    lifetime.Used = true;
                }
                lifetime.LastPass = orderIndex;

                if (lastUsePass[use.Resource.Index].has_value())
                {
                    const u32 previousPass = *lastUsePass[use.Resource.Index];
                    // Dependency edges are de-duplicated by pass pair. A second
                    // resource crossing the same edge still needs its own paired
                    // ownership lifecycle, so derive it from this resource's
                    // resolved last use rather than the de-duplicated edge list.
                    if (m_Passes[previousPass].Queue != pass.Queue)
                    {
                        result.QueueTransitions.push_back({
                            { previousPass }, { passIndex }, use.Resource,
                            m_Passes[previousPass].Queue, pass.Queue,
                            currentStates[use.Resource.Index], use.RequiredState
                        });
                    }
                }
                if (currentStates[use.Resource.Index] != use.RequiredState)
                {
                    result.Barriers.push_back({ { passIndex }, use.Resource, currentStates[use.Resource.Index], use.RequiredState });
                    currentStates[use.Resource.Index] = use.RequiredState;
                }
                lastUsePass[use.Resource.Index] = passIndex;
            }
            result.Passes.push_back({ { passIndex }, pass.DebugName, pass.Queue, firstBarrier, static_cast<u32>(result.Barriers.size() - firstBarrier) });
        }
        return result;
    }

    bool RenderGraph::IsValid(ResourceHandle handle) const
    {
        return handle.IsValid() && handle.Index < m_Resources.size();
    }

    bool RenderGraph::IsValid(PassHandle handle) const
    {
        return handle.IsValid() && handle.Index < m_Passes.size();
    }

    bool RenderGraph::Matches(const RHI::TextureDescription& expected, const RHI::TextureDescription& actual)
    {
        return expected.Extent.Width == actual.Extent.Width && expected.Extent.Height == actual.Extent.Height
            && expected.TextureFormat == actual.TextureFormat && expected.Usage == actual.Usage
            && expected.MipLevels == actual.MipLevels
            && expected.ArrayLayers == actual.ArrayLayers && expected.SampleCount == actual.SampleCount;
    }

    bool RenderGraph::Matches(const RHI::BufferDescription& expected, const RHI::BufferDescription& actual)
    {
        return expected.SizeBytes == actual.SizeBytes && expected.StrideBytes == actual.StrideBytes
            && expected.Usage == actual.Usage && expected.CpuAccess == actual.CpuAccess;
    }

    u64 RenderGraph::EstimateLogicalBytes(const RHI::TextureDescription& description)
    {
        u64 bytesPerTexel = 0;
        switch (description.TextureFormat)
        {
            case RHI::Format::R8Unorm: bytesPerTexel = 1; break;
            case RHI::Format::R8G8B8A8Unorm:
            case RHI::Format::R8G8B8A8UnormSrgb:
            case RHI::Format::R11G11B10Float:
            case RHI::Format::R32Uint:
            case RHI::Format::D24UnormS8Uint:
            case RHI::Format::D32Float: bytesPerTexel = 4; break;
            case RHI::Format::R32G32Float:
            case RHI::Format::R16G16B16A16Float:
                bytesPerTexel = 8; break;
            case RHI::Format::R32G32B32Float: bytesPerTexel = 12; break;
            case RHI::Format::R32G32B32A32Float: bytesPerTexel = 16; break;
            case RHI::Format::Unknown: return 0;
        }
        const auto multiply = [](u64 left, u64 right, u64& product)
        {
            if (!left || !right) { product = 0; return false; }
            if (left > std::numeric_limits<u64>::max() / right) return false;
            product = left * right;
            return true;
        };
        if (!description.Extent.Width || !description.Extent.Height || !description.MipLevels || !description.ArrayLayers || !description.SampleCount) return 0;
        u64 total = 0;
        u64 width = description.Extent.Width, height = description.Extent.Height;
        for (u32 mip = 0; mip < description.MipLevels; ++mip)
        {
            u64 level = 0, texels = 0;
            if (!multiply(width, height, texels) || !multiply(texels, bytesPerTexel, level)
                || total > std::numeric_limits<u64>::max() - level) return 0;
            total += level;
            width = std::max<u64>(1, width / 2);
            height = std::max<u64>(1, height / 2);
        }
        u64 layered = 0, sampled = 0;
        return multiply(total, description.ArrayLayers, layered) && multiply(layered, description.SampleCount, sampled) ? sampled : 0;
    }

    u64 RenderGraph::EstimateLogicalBytes(const RHI::BufferDescription& description)
    {
        return description.CpuAccess == RHI::BufferCpuAccess::None ? description.SizeBytes : 0;
    }

    RenderGraph::ResourceUse RenderGraph::Read(ResourceHandle resource, RHI::ResourceState state, RHI::ShaderStage stages)
    {
        return {
            resource,
            Access::Read,
            state,
            stages
        };
    }

    RenderGraph::ResourceUse RenderGraph::Write(ResourceHandle resource, RHI::ResourceState state)
    {
        return {
            resource,
            Access::Write,
            state,
            RHI::ShaderStage::None
        };
    }

    RenderGraph::ResourceUse RenderGraph::ReadWrite(ResourceHandle resource, RHI::ResourceState state, RHI::ShaderStage stages)
    {
        return {
            resource,
            Access::ReadWrite,
            state,
            stages
        };
    }

    const char* RenderGraph::ToString(ResourceKind kind)
    {
        switch (kind)
        {
            case ResourceKind::Texture: return "Texture";
            case ResourceKind::Buffer: return "Buffer";
        }

        return "Unknown";
    }

    const char* RenderGraph::ToString(ResourceLifetimeKind lifetime)
    {
        switch (lifetime)
        {
            case ResourceLifetimeKind::Transient: return "Transient";
            case ResourceLifetimeKind::Imported: return "Imported";
        }

        return "Unknown";
    }

    const char* RenderGraph::ToString(Access access)
    {
        switch (access)
        {
            case Access::Read: return "Read";
            case Access::Write: return "Write";
            case Access::ReadWrite: return "ReadWrite";
        }

        return "Unknown";
    }

    const char* RenderGraph::ToString(RHI::ResourceState state)
    {
        switch (state)
        {
            case RHI::ResourceState::Unknown: return "Unknown";
            case RHI::ResourceState::Common: return "Common";
            case RHI::ResourceState::RenderTarget: return "RenderTarget";
            case RHI::ResourceState::DepthWrite: return "DepthWrite";
            case RHI::ResourceState::ShaderResource: return "ShaderResource";
            case RHI::ResourceState::UnorderedAccess: return "UnorderedAccess";
            case RHI::ResourceState::CopySource: return "CopySource";
            case RHI::ResourceState::CopyDest: return "CopyDest";
            case RHI::ResourceState::Present: return "Present";
        }

        return "Unknown";
    }

    RenderGraph::ResourceHandle RenderGraph::AddResource(ResourceDescription description)
    {
        if (description.DebugName.empty())
        {
            description.DebugName = description.Kind == ResourceKind::Texture
                ? description.Texture.DebugName
                : description.Buffer.DebugName;
        }

        const ResourceHandle handle { static_cast<u32>(m_Resources.size()) };
        m_Resources.emplace_back(std::move(description));
        return handle;
    }

    RenderGraph::CompileResult RenderGraph::Fail(std::string_view message)
    {
        CompileResult result;
        result.Success = false;
        result.Error = message;
        return result;
    }
}
