#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Buffer.h"
#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/Texture.h"

#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Engine
{
    class RenderGraph
    {
    public:
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();

        struct ResourceHandle
        {
            u32 Index = InvalidIndex;

            bool IsValid() const { return Index != InvalidIndex; }
        };

        struct PassHandle
        {
            u32 Index = InvalidIndex;

            bool IsValid() const { return Index != InvalidIndex; }
        };

        enum class ResourceKind
        {
            Texture,
            Buffer
        };

        enum class ResourceLifetimeKind
        {
            Transient,
            Imported
        };

        enum class Access
        {
            Read,
            Write,
            ReadWrite
        };

        struct ResourceDescription
        {
            std::string DebugName;
            ResourceKind Kind = ResourceKind::Texture;
            ResourceLifetimeKind Lifetime = ResourceLifetimeKind::Transient;
            RHI::ResourceState InitialState = RHI::ResourceState::Common;
            RHI::TextureDescription Texture;
            RHI::BufferDescription Buffer;
        };

        struct ResourceUse
        {
            ResourceHandle Resource;
            Access Usage = Access::Read;
            RHI::ResourceState RequiredState = RHI::ResourceState::ShaderResource;
            RHI::ShaderStage Stages = RHI::ShaderStage::All;
        };

        struct PassDescription
        {
            std::string DebugName;
            RHI::QueueType Queue = RHI::QueueType::Graphics;
            bool AllowCulling = false;
            std::vector<ResourceUse> Uses;
        };

        struct ResourceLifetime
        {
            ResourceHandle Resource;
            u32 FirstPass = InvalidIndex;
            u32 LastPass = InvalidIndex;
            bool Used = false;
        };

        struct Barrier
        {
            PassHandle Pass;
            ResourceHandle Resource;
            RHI::ResourceState Before = RHI::ResourceState::Unknown;
            RHI::ResourceState After = RHI::ResourceState::Unknown;
        };

        struct CompiledPass
        {
            PassHandle Pass;
            std::string DebugName;
            RHI::QueueType Queue = RHI::QueueType::Graphics;
            u32 FirstBarrier = 0;
            u32 BarrierCount = 0;
        };

        struct CompileResult
        {
            bool Success = false;
            std::string Error;
            std::vector<ResourceLifetime> ResourceLifetimes;
            std::vector<Barrier> Barriers;
            std::vector<CompiledPass> Passes;
        };

        ResourceHandle AddTexture(RHI::TextureDescription description, ResourceLifetimeKind lifetime = ResourceLifetimeKind::Transient);
        ResourceHandle AddBuffer(RHI::BufferDescription description, ResourceLifetimeKind lifetime = ResourceLifetimeKind::Transient);
        PassHandle AddPass(std::string name, RHI::QueueType queue = RHI::QueueType::Graphics, bool allowCulling = false);
        void AddDebugPass(std::string name);
        void AddResourceUse(PassHandle pass, ResourceUse use);
        void AddRead(PassHandle pass, ResourceHandle resource, RHI::ResourceState state = RHI::ResourceState::ShaderResource, RHI::ShaderStage stages = RHI::ShaderStage::All);
        void AddWrite(PassHandle pass, ResourceHandle resource, RHI::ResourceState state);
        void AddReadWrite(PassHandle pass, ResourceHandle resource, RHI::ResourceState state, RHI::ShaderStage stages = RHI::ShaderStage::All);
        void Reset();

        [[nodiscard]] CompileResult Compile() const;

        [[nodiscard]] const std::vector<ResourceDescription>& GetResources() const { return m_Resources; }
        [[nodiscard]] const std::vector<PassDescription>& GetPasses() const { return m_Passes; }
        [[nodiscard]] const std::vector<std::string>& GetDebugPasses() const { return m_DebugPasses; }
        [[nodiscard]] bool IsValid(ResourceHandle handle) const;
        [[nodiscard]] bool IsValid(PassHandle handle) const;

        [[nodiscard]] static ResourceUse Read(ResourceHandle resource, RHI::ResourceState state = RHI::ResourceState::ShaderResource, RHI::ShaderStage stages = RHI::ShaderStage::All);
        [[nodiscard]] static ResourceUse Write(ResourceHandle resource, RHI::ResourceState state);
        [[nodiscard]] static ResourceUse ReadWrite(ResourceHandle resource, RHI::ResourceState state, RHI::ShaderStage stages = RHI::ShaderStage::All);
        [[nodiscard]] static const char* ToString(ResourceKind kind);
        [[nodiscard]] static const char* ToString(ResourceLifetimeKind lifetime);
        [[nodiscard]] static const char* ToString(Access access);
        [[nodiscard]] static const char* ToString(RHI::ResourceState state);

    private:
        ResourceHandle AddResource(ResourceDescription description);
        static CompileResult Fail(std::string_view message);

    private:
        std::vector<ResourceDescription> m_Resources;
        std::vector<PassDescription> m_Passes;
        std::vector<std::string> m_DebugPasses;
    };
}
