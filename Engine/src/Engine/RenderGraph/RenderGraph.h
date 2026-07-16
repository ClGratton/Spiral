#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Buffer.h"
#include "Engine/RHI/Device.h"
#include "Engine/RHI/RHICommon.h"
#include "Engine/RHI/Texture.h"
#include "Engine/Jobs/FrameTaskGraph.h"

#include <functional>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(DeviceCapabilities)
    #undef DeviceCapabilities
#endif

namespace Engine
{
    class RenderGraph
    {
    public:
        static constexpr u32 InvalidIndex = std::numeric_limits<u32>::max();

        // This policy selection deliberately precedes physical transient allocation.
        // The allocation item consumes it so graph policy never infers native support.
        static RHI::CapabilityGroupState BuildTransientResourceCapabilityGroup(
            const RHI::DeviceCapabilities& capabilities);

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
            // Caller-affine unless the pass owner explicitly proves that its
            // callback and captures may record on a worker-owned RHI context.
            bool WorkerRecordingEligible = false;
            std::vector<ResourceUse> Uses;
        };

        class ExecutionContext
        {
        public:
            [[nodiscard]] RHI::CommandList& GetCommandList() const;
            [[nodiscard]] RHI::Texture* GetTexture(ResourceHandle resource) const;
            [[nodiscard]] RHI::Buffer* GetBuffer(ResourceHandle resource) const;

        private:
            friend class RenderGraph;
            ExecutionContext(RHI::CommandList& commandList, const std::vector<RHI::Texture*>& textures,
                const std::vector<RHI::Buffer*>& buffers, const std::vector<bool>& declared);
            RHI::CommandList* m_CommandList = nullptr;
            const std::vector<RHI::Texture*>* m_Textures = nullptr;
            const std::vector<RHI::Buffer*>* m_Buffers = nullptr;
            const std::vector<bool>* m_Declared = nullptr;
        };

        using PassCallback = std::function<bool(ExecutionContext&)>;

        struct ExecuteResult
        {
            bool Success = false;
            std::string Error;
            RHI::CompletionToken Completion;
            std::vector<RHI::CompletionToken> Completions;
            u32 RecordingContextIndex = InvalidIndex;
            bool ReusedRetiredContext = false;
            // Allocation accounting is intentionally RenderGraph policy. The
            // current adapters select the non-aliased pool; a placed/aliased
            // result is rejected until RHI can create it and emit its barriers.
            RHI::CapabilityPath TransientAllocationMode = RHI::CapabilityPath::NonAliasedGpuRetiredPool;
            u64 EstimatedTransientAllocatedBytes = 0;
            u64 EstimatedTransientPooledBytes = 0;
            u32 TransientResourceCount = 0;
            u32 ReusedRetiredTransientCount = 0;
            u32 AcceptedPassCount = 0;
            u32 WorkerRecordedPassCount = 0;
            bool WorkerRecordingOverlapObserved = false;
        };

        struct ExecuteOptions
        {
            FrameTaskExecutionMode RecordingMode = FrameTaskExecutionMode::Parallel;
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

        struct Dependency
        {
            PassHandle Producer;
            PassHandle Consumer;
            ResourceHandle Resource;
        };

        struct QueueTransition
        {
            PassHandle Producer;
            PassHandle Consumer;
            ResourceHandle Resource;
            RHI::QueueType SourceQueue = RHI::QueueType::Graphics;
            RHI::QueueType DestinationQueue = RHI::QueueType::Graphics;
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
            std::vector<Dependency> Dependencies;
            std::vector<Barrier> Barriers;
            std::vector<QueueTransition> QueueTransitions;
            std::vector<CompiledPass> Passes;
        };

        ResourceHandle AddTexture(RHI::TextureDescription description, ResourceLifetimeKind lifetime = ResourceLifetimeKind::Transient);
        ResourceHandle AddBuffer(RHI::BufferDescription description, ResourceLifetimeKind lifetime = ResourceLifetimeKind::Transient);
        PassHandle AddPass(std::string name, RHI::QueueType queue = RHI::QueueType::Graphics, bool allowCulling = false);
        void SetPassCallback(PassHandle pass, PassCallback callback);
        void SetPassWorkerRecordingEligible(PassHandle pass, bool eligible = true);
        bool BindTexture(ResourceHandle resource, RHI::Texture& texture);
        bool BindBuffer(ResourceHandle resource, RHI::Buffer& buffer);
        ExecuteResult Execute(RHI::Device& device, const CompileResult& compiled, const ExecuteOptions& options = {});
        void AddDebugPass(std::string name);
        void AddResourceUse(PassHandle pass, ResourceUse use);
        void AddDependency(PassHandle producer, PassHandle consumer);
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
        struct RecordingContext
        {
            Scope<RHI::CommandList> CommandList;
            RHI::CompletionToken Completion;
            RHI::QueueType EffectiveQueue = RHI::QueueType::Graphics;
            u32 PassIndex = InvalidIndex;
        };
        struct TransientAllocation
        {
            ResourceKind Kind = ResourceKind::Texture;
            RHI::TextureDescription Texture;
            RHI::BufferDescription Buffer;
            Scope<RHI::Texture> TextureResource;
            Scope<RHI::Buffer> BufferResource;
            std::vector<RHI::CompletionToken> RetirementTokens;
            u64 EstimatedLogicalBytes = 0;
        };
        ResourceHandle AddResource(ResourceDescription description);
        static CompileResult Fail(std::string_view message);
        static bool Matches(const RHI::TextureDescription& expected, const RHI::TextureDescription& actual);
        static bool Matches(const RHI::BufferDescription& expected, const RHI::BufferDescription& actual);
        static u64 EstimateLogicalBytes(const RHI::TextureDescription& description);
        static u64 EstimateLogicalBytes(const RHI::BufferDescription& description);

    private:
        std::vector<ResourceDescription> m_Resources;
        std::vector<PassDescription> m_Passes;
        std::vector<Dependency> m_ExplicitDependencies;
        std::vector<std::string> m_DebugPasses;
        std::vector<PassCallback> m_Callbacks;
        std::vector<RHI::Texture*> m_BoundTextures;
        std::vector<RHI::Buffer*> m_BoundBuffers;
        std::vector<bool> m_AutoTransientBindings;
        std::vector<RecordingContext> m_RecordingContexts;
        std::vector<TransientAllocation> m_TransientAllocations;
        RHI::Device* m_RecordingDevice = nullptr;
    };

    // Retains every object whose lifetime is coupled to an accepted graph
    // submission without turning GPU retirement into a per-frame CPU wait.
    class SubmittedRenderGraphFrameOwner final
    {
    public:
        static constexpr size_t Capacity = 4;

        struct RetiredFrame
        {
            u64 FrameIndex = 0;
            std::vector<std::string> PassLabels;
            std::vector<RHI::CompletionToken> Completions;
        };

        struct PollResult
        {
            bool Success = true;
            std::string Error;
            std::vector<RetiredFrame> Retired;
            size_t PendingCount = 0;
        };

        [[nodiscard]] bool HasCapacity() const { return m_Frames.size() < Capacity; }
        [[nodiscard]] size_t GetPendingCount() const { return m_Frames.size(); }
        [[nodiscard]] PollResult Poll(RHI::Device& device);

        // Retained payloads cover resources captured by pass recordings but
        // owned outside RenderGraph (for example, per-frame constant buffers).
        bool Retain(u64 frameIndex, Scope<RenderGraph> graph,
            const RenderGraph::CompileResult& compiled,
            const RenderGraph::ExecuteResult& executed,
            std::vector<Ref<void>> retainedPayloads = {},
            std::string* error = nullptr);

        // The caller must establish device-idle before releasing pending GPU
        // state during shutdown or device replacement.
        void ReleaseAfterDeviceIdle() { m_Frames.clear(); }

    private:
        struct PendingFrame
        {
            RetiredFrame Identity;
            Scope<RenderGraph> Graph;
            std::vector<Ref<void>> RetainedPayloads;
        };

        std::deque<PendingFrame> m_Frames;
    };
}
