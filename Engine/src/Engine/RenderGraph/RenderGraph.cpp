#include "Engine/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace Engine
{
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
        return handle;
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
                    const bool hasResourceDependency = std::any_of(result.Dependencies.begin(), result.Dependencies.end(), [previousPass, passIndex, &use](const Dependency& dependency)
                    {
                        return dependency.Producer.Index == previousPass
                            && dependency.Consumer.Index == passIndex
                            && dependency.Resource.Index == use.Resource.Index;
                    });
                    if (hasResourceDependency && m_Passes[previousPass].Queue != pass.Queue)
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
