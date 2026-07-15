#include "Engine/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace Engine
{
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

    bool RenderGraph::BindTexture(ResourceHandle resource, RHI::Texture& texture)
    {
        if (!IsValid(resource) || m_Resources[resource.Index].Kind != ResourceKind::Texture || !Matches(m_Resources[resource.Index].Texture, texture.GetDescription())) return false;
        if (m_BoundTextures.size() != m_Resources.size()) { m_BoundTextures.resize(m_Resources.size()); m_BoundBuffers.resize(m_Resources.size()); }
        m_BoundTextures[resource.Index] = &texture;
        return true;
    }

    bool RenderGraph::BindBuffer(ResourceHandle resource, RHI::Buffer& buffer)
    {
        if (!IsValid(resource) || m_Resources[resource.Index].Kind != ResourceKind::Buffer || !Matches(m_Resources[resource.Index].Buffer, buffer.GetDescription())) return false;
        if (m_BoundTextures.size() != m_Resources.size()) { m_BoundTextures.resize(m_Resources.size()); m_BoundBuffers.resize(m_Resources.size()); }
        m_BoundBuffers[resource.Index] = &buffer;
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
    }

    RenderGraph::ExecuteResult RenderGraph::Execute(RHI::Device& device, const CompileResult& compiled)
    {
        ExecuteResult result;
        if (!compiled.Success) { result.Error = "Cannot execute an unsuccessful render-graph compilation."; return result; }
        if (m_RecordingDevice && m_RecordingDevice != &device) { result.Error = "Render graph recording contexts belong to another RHI device."; return result; }
        if (compiled.Passes.size() != m_Passes.size() || m_Callbacks.size() != m_Passes.size()) { result.Error = "Compiled passes do not match this render graph."; return result; }
        if (m_BoundTextures.size() != m_Resources.size() || m_BoundBuffers.size() != m_Resources.size()) { result.Error = "Render graph resources are not all explicitly bound."; return result; }
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
        for (u32 index = 0; index < m_Resources.size(); ++index)
        {
            const ResourceDescription& resource = m_Resources[index];
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
        for (const CompiledPass& compiledPass : compiled.Passes)
        {
            const RHI::QueueResolution& resolution = resolutions[compiledPass.Pass.Index];
            u32 contextIndex = InvalidIndex;
            RecordingContext* context = acquireContext(compiledPass.Queue, resolution.Effective, compiledPass.Pass.Index, contextIndex);
            if (!context) { result.Error = "All bounded graph recording contexts are in flight or a completion query failed."; return result; }
            if (result.RecordingContextIndex == InvalidIndex) result.RecordingContextIndex = contextIndex;
            const auto discardContext = [this, contextIndex]() { m_RecordingContexts.erase(m_RecordingContexts.begin() + contextIndex); };
            if (!context->CommandList->Begin()) { result.Error = "Could not begin a GPU-retired graph recording context."; discardContext(); return result; }
            std::vector<RHI::CompletionToken> dependencies;
            for (const Dependency& dependency : compiled.Dependencies)
                if (dependency.Consumer.Index == compiledPass.Pass.Index)
                {
                    const RHI::CompletionToken producerToken = passTokens[dependency.Producer.Index];
                    if (!producerToken.IsValid()) { result.Error = "A graph dependency has no accepted producer token."; discardContext(); return result; }
                    if (std::find_if(dependencies.begin(), dependencies.end(), [&](const RHI::CompletionToken& token) { return token.DeviceId == producerToken.DeviceId && token.SubmissionId == producerToken.SubmissionId; }) == dependencies.end()) dependencies.push_back(producerToken);
                }
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
                    ? context->CommandList->TransitionTexture(*m_BoundTextures[barrier.Resource.Index], barrier.After)
                    : context->CommandList->TransitionBuffer(*m_BoundBuffers[barrier.Resource.Index], barrier.After);
                if (!transitioned) { result.Error = "Could not record a compiled graph transition."; discardContext(); return result; }
            }
            const PassDescription& pass = m_Passes[compiledPass.Pass.Index];
            std::vector<bool> declared(m_Resources.size(), false);
            for (const ResourceUse& use : pass.Uses) declared[use.Resource.Index] = true;
            ExecutionContext execution(*context->CommandList, m_BoundTextures, m_BoundBuffers, declared);
            try
            {
                if (!m_Callbacks[compiledPass.Pass.Index](execution)) { result.Error = "A graph pass callback failed."; discardContext(); return result; }
            }
            catch (...)
            {
                result.Error = "A graph pass callback threw an exception."; discardContext(); return result;
            }
            for (const QueueTransition* transition : releases[compiledPass.Pass.Index])
            {
                const bool released = m_Resources[transition->Resource.Index].Kind == ResourceKind::Texture
                    ? context->CommandList->ReleaseTextureOwnership({ m_BoundTextures[transition->Resource.Index], transition->SourceQueue, transition->DestinationQueue, transition->Before, transition->After })
                    : context->CommandList->ReleaseBufferOwnership({ m_BoundBuffers[transition->Resource.Index], transition->SourceQueue, transition->DestinationQueue, transition->Before, transition->After });
                if (!released) { result.Error = "Could not record a compiled graph ownership release."; discardContext(); return result; }
            }
            if (!context->CommandList->End()) { result.Error = "Could not close graph recording."; discardContext(); return result; }
            const RHI::CompletionToken token = device.Submit(*context->CommandList, dependencies);
            if (!token.IsValid()) { result.Error = "Could not submit graph recording for pass '" + compiledPass.DebugName + "'."; discardContext(); return result; }
            context->Completion = token;
            passTokens[compiledPass.Pass.Index] = token;
            result.Completions.push_back(token);
            result.Completion = token;
            ++result.AcceptedPassCount;
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
            && expected.InitialState == actual.InitialState && expected.MipLevels == actual.MipLevels
            && expected.ArrayLayers == actual.ArrayLayers && expected.SampleCount == actual.SampleCount;
    }

    bool RenderGraph::Matches(const RHI::BufferDescription& expected, const RHI::BufferDescription& actual)
    {
        return expected.SizeBytes == actual.SizeBytes && expected.StrideBytes == actual.StrideBytes
            && expected.Usage == actual.Usage && expected.CpuAccess == actual.CpuAccess && expected.InitialState == actual.InitialState;
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
