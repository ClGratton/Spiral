#include "Engine/RenderGraph/RenderGraph.h"

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
        m_DebugPasses.clear();
    }

    RenderGraph::CompileResult RenderGraph::Compile() const
    {
        CompileResult result;
        result.Success = true;
        result.ResourceLifetimes.resize(m_Resources.size());

        std::vector<RHI::ResourceState> currentStates;
        currentStates.reserve(m_Resources.size());

        for (u32 resourceIndex = 0; resourceIndex < m_Resources.size(); ++resourceIndex)
        {
            result.ResourceLifetimes[resourceIndex].Resource = { resourceIndex };
            currentStates.emplace_back(m_Resources[resourceIndex].InitialState);
        }

        for (u32 passIndex = 0; passIndex < m_Passes.size(); ++passIndex)
        {
            const PassDescription& pass = m_Passes[passIndex];
            if (pass.AllowCulling && pass.Uses.empty())
                continue;

            const u32 firstBarrier = static_cast<u32>(result.Barriers.size());

            for (const ResourceUse& use : pass.Uses)
            {
                if (!IsValid(use.Resource))
                {
                    std::ostringstream stream;
                    stream << "Pass '" << pass.DebugName << "' references an invalid resource handle.";
                    return Fail(stream.str());
                }

                ResourceLifetime& lifetime = result.ResourceLifetimes[use.Resource.Index];
                if (!lifetime.Used)
                {
                    lifetime.FirstPass = passIndex;
                    lifetime.Used = true;
                }
                lifetime.LastPass = passIndex;

                RHI::ResourceState& currentState = currentStates[use.Resource.Index];
                if (currentState != use.RequiredState)
                {
                    result.Barriers.push_back({
                        { passIndex },
                        use.Resource,
                        currentState,
                        use.RequiredState
                    });
                    currentState = use.RequiredState;
                }
            }

            result.Passes.push_back({
                { passIndex },
                pass.DebugName,
                pass.Queue,
                firstBarrier,
                static_cast<u32>(result.Barriers.size() - firstBarrier)
            });
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
