#include "Engine/RHI/NVRHI/NVRHIVulkanDevice.h"

#include "Engine/RHI/NVRHI/VulkanDispatch.h"

#include "Engine/Core/Log.h"
#include "Engine/RHI/SubmissionDependency.h"
#include "Engine/RHI/TextureBindingTable.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <nvrhi/nvrhi.h>
    #include <nvrhi/vulkan.h>
    #include <atomic>
    #include <algorithm>
    #include <chrono>
    #include <cstring>
    #include <limits>
    #include <functional>
    #include <optional>
    #include <unordered_map>
    #include <thread>
    #include <vector>
#endif

namespace Engine::RHI
{
#if defined(GE_HAS_NVRHI_VULKAN)
    namespace
    {
        std::atomic<u64> s_NextCompletionDeviceId { 1 };
        std::atomic<u64> s_NextResourceOwnerId { 1 };
        bool HasBufferUsage(BufferUsage value, BufferUsage flag)
        {
            return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
        }

        bool HasTextureUsage(TextureUsage value, TextureUsage flag)
        {
            return (static_cast<u32>(value) & static_cast<u32>(flag)) != 0;
        }

        nvrhi::Format ConvertFormat(Format format)
        {
            switch (format)
            {
                case Format::R8Unorm: return nvrhi::Format::R8_UNORM;
                case Format::R8G8B8A8Unorm: return nvrhi::Format::RGBA8_UNORM;
                case Format::R8G8B8A8UnormSrgb: return nvrhi::Format::SRGBA8_UNORM;
                case Format::D32Float: return nvrhi::Format::D32;
                default: return nvrhi::Format::UNKNOWN;
            }
        }

        nvrhi::ResourceStates ConvertState(ResourceState state)
        {
            switch (state)
            {
                case ResourceState::Common: return nvrhi::ResourceStates::Common;
                case ResourceState::RenderTarget: return nvrhi::ResourceStates::RenderTarget;
                case ResourceState::DepthWrite: return nvrhi::ResourceStates::DepthWrite;
                case ResourceState::ShaderResource: return nvrhi::ResourceStates::ShaderResource;
                case ResourceState::UnorderedAccess: return nvrhi::ResourceStates::UnorderedAccess;
                case ResourceState::CopySource: return nvrhi::ResourceStates::CopySource;
                case ResourceState::CopyDest: return nvrhi::ResourceStates::CopyDest;
                case ResourceState::Present: return nvrhi::ResourceStates::Present;
                case ResourceState::Unknown: default: return nvrhi::ResourceStates::Unknown;
            }
        }

        nvrhi::CommandQueue ConvertQueue(QueueType queue)
        {
            switch (queue)
            {
                case QueueType::Compute: return nvrhi::CommandQueue::Compute;
                case QueueType::Copy: return nvrhi::CommandQueue::Copy;
                case QueueType::Graphics: default: return nvrhi::CommandQueue::Graphics;
            }
        }

        nvrhi::ShaderType ConvertShaderStage(ShaderStage stage)
        {
            switch (stage)
            {
                case ShaderStage::Vertex: return nvrhi::ShaderType::Vertex;
                case ShaderStage::Pixel: return nvrhi::ShaderType::Pixel;
                default: return nvrhi::ShaderType::None;
            }
        }

        struct NativeState
        {
            VkPipelineStageFlags2 Stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkAccessFlags2 Access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            VkImageLayout Layout = VK_IMAGE_LAYOUT_GENERAL;
        };

        NativeState ConvertNativeState(ResourceState state)
        {
            switch (state)
            {
                case ResourceState::Common: return { VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED };
                case ResourceState::RenderTarget: return { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
                case ResourceState::DepthWrite: return { VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
                case ResourceState::ShaderResource: return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                case ResourceState::UnorderedAccess: return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL };
                case ResourceState::CopySource: return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
                case ResourceState::CopyDest: return { VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
                case ResourceState::Present: return { VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
                case ResourceState::Unknown: default: return {};
            }
        }

        class VulkanBuffer final : public Buffer
        {
        public:
            VulkanBuffer(BufferDescription description, nvrhi::BufferHandle buffer, u64 ownerId)
                : m_Description(std::move(description)), m_Buffer(std::move(buffer)), m_OwnerId(ownerId) {}
            ~VulkanBuffer() override
            {
                if (m_OwnershipTracker && !m_OwnershipTracker->Unregister(*this))
                {
                    Log::Error("Vulkan RHI buffer destruction rejected while queue ownership is pending: ", m_Description.DebugName);
                    std::terminate();
                }
            }
            const BufferDescription& GetDescription() const override { return m_Description; }
            void* Map() override
            {
                if (!m_Buffer || !m_Device || m_Mapped || m_Description.CpuAccess == BufferCpuAccess::None)
                    return nullptr;
                const nvrhi::CpuAccessMode access = m_Description.CpuAccess == BufferCpuAccess::Read
                    ? nvrhi::CpuAccessMode::Read : nvrhi::CpuAccessMode::Write;
                void* mapped = m_Device->mapBuffer(m_Buffer, access);
                m_Mapped = mapped != nullptr;
                return mapped;
            }
            void Unmap() override
            {
                if (m_Buffer && m_Device && m_Mapped)
                {
                    m_Device->unmapBuffer(m_Buffer);
                    m_Mapped = false;
                }
            }
            void SetDevice(nvrhi::IDevice* device) { m_Device = device; }
            void SetOwnershipTracker(BufferOwnershipTracker* tracker) { m_OwnershipTracker = tracker; }
            nvrhi::IBuffer* Native() const { return m_Buffer; }
            ResourceState GetCurrentState() const { return m_CurrentState; }
            void SetCurrentState(ResourceState state) { m_CurrentState = state; }
            u64 GetOwnerId() const { return m_OwnerId; }
        private:
            BufferDescription m_Description;
            nvrhi::BufferHandle m_Buffer;
            const u64 m_OwnerId;
            nvrhi::IDevice* m_Device = nullptr;
            bool m_Mapped = false;
            ResourceState m_CurrentState = ResourceState::Common;
            BufferOwnershipTracker* m_OwnershipTracker = nullptr;
        };

        class VulkanTexture final : public Texture
        {
        public:
            VulkanTexture(TextureDescription description, nvrhi::TextureHandle texture, u64 ownerId)
                : m_Description(std::move(description)), m_Texture(std::move(texture)), m_OwnerId(ownerId) {}
            ~VulkanTexture() override
            {
                if (m_OwnershipTracker && !m_OwnershipTracker->Unregister(*this))
                {
                    Log::Error("Vulkan RHI texture destruction rejected while queue ownership is pending: ", m_Description.DebugName);
                    std::terminate();
                }
            }
            const TextureDescription& GetDescription() const override { return m_Description; }
            nvrhi::ITexture* Native() const { return m_Texture; }
            ResourceState GetCurrentState() const { return m_CurrentState; }
            void SetCurrentState(ResourceState state) { m_CurrentState = state; }
            u64 GetOwnerId() const { return m_OwnerId; }
            void SetOwnershipTracker(TextureOwnershipTracker* tracker) { m_OwnershipTracker = tracker; }
        private:
            TextureDescription m_Description;
            nvrhi::TextureHandle m_Texture;
            const u64 m_OwnerId;
            ResourceState m_CurrentState = ResourceState::Common;
            TextureOwnershipTracker* m_OwnershipTracker = nullptr;
        };

        class VulkanShader final : public Shader
        {
        public:
            VulkanShader(ShaderDescription description, nvrhi::ShaderHandle shader)
                : m_Description(std::move(description)), m_Shader(std::move(shader)) {}
            const ShaderDescription& GetDescription() const override { return m_Description; }
            nvrhi::IShader* Native() const { return m_Shader; }
            const nvrhi::ShaderHandle& NativeHandle() const { return m_Shader; }
        private:
            ShaderDescription m_Description;
            nvrhi::ShaderHandle m_Shader;
        };

        class VulkanPipeline final : public Pipeline
        {
        public:
            VulkanPipeline(PipelineDescription description, nvrhi::InputLayoutHandle inputLayout, nvrhi::BindingLayoutHandle bindings, nvrhi::BindingLayoutHandle tableBindings, nvrhi::ShaderHandle vertex, nvrhi::ShaderHandle pixel)
                : m_Description(std::move(description)), m_InputLayout(std::move(inputLayout)), m_Bindings(std::move(bindings)), m_TableBindings(std::move(tableBindings)), m_Vertex(std::move(vertex)), m_Pixel(std::move(pixel)) {}
            const PipelineDescription& GetDescription() const override { return m_Description; }
            nvrhi::IInputLayout* InputLayout() const { return m_InputLayout; }
            nvrhi::IBindingLayout* Bindings() const { return m_Bindings; }
            nvrhi::IBindingLayout* TableBindings() const { return m_TableBindings; }
            nvrhi::IGraphicsPipeline* GetOrCreateNative(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& framebufferInfo)
            {
                if (m_NativePipeline && m_FramebufferInfo == framebufferInfo)
                    return m_NativePipeline;
                if (!device)
                    return nullptr;
                nvrhi::GraphicsPipelineDesc pipeline;
                pipeline.setPrimType(nvrhi::PrimitiveType::TriangleList).setInputLayout(m_InputLayout)
                    .setVertexShader(m_Vertex).setPixelShader(m_Pixel).addBindingLayout(m_Bindings);
                if (m_TableBindings) pipeline.addBindingLayout(m_TableBindings);
                pipeline.renderState.rasterState.setCullNone().setFrontCounterClockwise(false);
                pipeline.renderState.depthStencilState.depthTestEnable = m_Description.DepthTestEnable;
                pipeline.renderState.depthStencilState.depthWriteEnable = m_Description.DepthWriteEnable;
                m_NativePipeline = device->createGraphicsPipeline(pipeline, framebufferInfo);
                if (m_NativePipeline)
                    m_FramebufferInfo = framebufferInfo;
                else
                    Log::Error("Vulkan native graphics pipeline creation failed: ", m_Description.DebugName);
                return m_NativePipeline;
            }
        private:
            PipelineDescription m_Description;
            nvrhi::InputLayoutHandle m_InputLayout;
            nvrhi::BindingLayoutHandle m_Bindings;
            nvrhi::BindingLayoutHandle m_TableBindings;
            nvrhi::ShaderHandle m_Vertex;
            nvrhi::ShaderHandle m_Pixel;
            nvrhi::GraphicsPipelineHandle m_NativePipeline;
            nvrhi::FramebufferInfo m_FramebufferInfo;
        };

        struct VulkanTimestampQueryState
        {
            VkDevice Device = VK_NULL_HANDLE;
            VkQueryPool Pool = VK_NULL_HANDLE;
            u32 Count = 0;
            u32 ValidBits = 0;
            double PeriodNanoseconds = 0.0;

            ~VulkanTimestampQueryState()
            {
                if (Device != VK_NULL_HANDLE && Pool != VK_NULL_HANDLE)
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyQueryPool(Device, Pool, nullptr);
            }
        };

        class VulkanTimestampQueryPool final : public QueryPool
        {
        public:
            VulkanTimestampQueryPool(Scope<TimestampQueryPool> logicalPool, std::shared_ptr<VulkanTimestampQueryState> state)
                : m_LogicalPool(std::move(logicalPool)), m_State(std::move(state)) {}

            const QueryPoolDescription& GetDescription() const override { return m_LogicalPool->GetDescription(); }
            QueryResult ReadResult(u32 queryIndex) const override { return m_LogicalPool->ReadResult(queryIndex); }
            QueryResult ReadResult(u32 queryIndex, u64 generation) const override { return m_LogicalPool->ReadResult(queryIndex, generation); }
            double GetTimestampPeriodNanoseconds() const override { return m_State->PeriodNanoseconds; }
            u64 GetOwnerDeviceId() const { return m_LogicalPool->GetOwnerDeviceId(); }
            TimestampQueryPool& GetLogicalPool() const { return *m_LogicalPool; }
            NativeQueryState GetNativeState() const { return std::static_pointer_cast<void>(m_State); }

        private:
            Scope<TimestampQueryPool> m_LogicalPool;
            std::shared_ptr<VulkanTimestampQueryState> m_State;
        };

        class VulkanCommandList final : public CommandList
        {
        public:
            VulkanCommandList(QueueType queueType, std::string name, nvrhi::CommandListHandle list, nvrhi::IDevice* device,
                std::function<CompletionStatus(const CompletionToken&)> queryCompletion,
                TimestampQueryRetirementQueue* timestampRetirements, BufferOwnershipTracker* ownershipTracker,
                TextureOwnershipTracker* textureOwnershipTracker,
                std::function<QueueType(QueueType)> resolveQueue,
                std::function<u32(QueueType)> queueFamily,
                std::function<bool(const Buffer*, QueueType)> canUseBuffer,
                std::function<bool(const Texture*, QueueType)> canUseTexture,
                Device* ownerDevice)
                : m_QueueType(queueType), m_Name(std::move(name)), m_List(std::move(list)), m_Device(device),
                m_QueryCompletion(std::move(queryCompletion)), m_TimestampRetirements(timestampRetirements),
                m_OwnershipTracker(ownershipTracker), m_TextureOwnershipTracker(textureOwnershipTracker), m_ResolveQueue(std::move(resolveQueue)), m_QueueFamily(std::move(queueFamily)), m_CanUseBuffer(std::move(canUseBuffer)), m_CanUseTexture(std::move(canUseTexture)), m_OwnerDevice(ownerDevice) {}
            QueueType GetQueueType() const override { return m_QueueType; }
            bool Begin() override
            {
                if (m_State == State::Recording || !m_List
                    || (m_State == State::Submitted && (!m_LastSubmission.IsValid() || !m_QueryCompletion
                        || m_QueryCompletion(m_LastSubmission) != CompletionStatus::Complete)))
                    return false;
                m_List->open();
                m_DebugMarkerNames.clear();
                m_Color = nullptr;
                m_Depth = nullptr;
                m_TextureStates.clear();
                m_BufferStates.clear();
                m_UsedBuffers.clear();
                m_OwnershipOperations.clear();
                m_UsedTextures.clear();
                m_TimestampTransactions.clear();
                m_PublishedTimestampStates.clear();
                m_TextureOwnershipOperations.clear();
                m_Pipeline = nullptr;
                m_BindingSet = nullptr;
                m_TableBindingSet = nullptr;
                m_ConstantBuffer = nullptr;
                m_TextureTable = nullptr;
                m_TableSamplers.clear();
                m_BoundTableTextures.clear();
                m_State = State::Recording;
                return true;
            }
            bool End() override
            {
                if (m_State != State::Recording || !m_DebugMarkerNames.empty())
                    return false;
                for (PendingTimestampTransaction& transaction : m_TimestampTransactions)
                    if (!transaction.Transaction.PrepareForSubmit())
                        return false;
                m_PublishedTimestampStates.reserve(m_TimestampTransactions.size());
                m_List->close();
                m_State = State::Closed;
                return true;
            }
            void BeginDebugMarker(std::string_view name) override
            {
                if (m_State != State::Recording)
                    return;
                m_DebugMarkerNames.emplace_back(name);
                m_List->beginMarker(m_DebugMarkerNames.back().c_str());
            }
            void EndDebugMarker() override
            {
                if (m_State != State::Recording || m_DebugMarkerNames.empty())
                    return;
                m_List->endMarker();
                m_DebugMarkerNames.pop_back();
            }
            bool BindViewportOutputs(Texture& color, Texture* depth) override
            {
                m_Color = dynamic_cast<VulkanTexture*>(&color); m_Depth = depth ? dynamic_cast<VulkanTexture*>(depth) : nullptr;
                if (!m_Color || !m_Depth || !HasTextureUsage(color.GetDescription().Usage, TextureUsage::RenderTarget)
                    || !HasTextureUsage(depth->GetDescription().Usage, TextureUsage::DepthStencil)
                    || !CanUseTexture(&color) || !CanUseTexture(depth)) return false;
                nvrhi::FramebufferDesc framebuffer;
                framebuffer.addColorAttachment(m_Color->Native()).setDepthAttachment(m_Depth->Native());
                m_Framebuffer = m_Device->createFramebuffer(framebuffer);
                return m_Framebuffer != nullptr;
            }
            bool ClearViewportOutputs(const ViewportClear& clear) override
            {
                if (m_State != State::Recording || !m_Color || !m_TextureOwnershipTracker
                    || !CanUseTexture(m_Color) || (m_Depth && !CanUseTexture(m_Depth))) return false;
                if (clear.ClearColor) m_List->clearTextureFloat(m_Color->Native(), nvrhi::AllSubresources, nvrhi::Color(clear.Color[0], clear.Color[1], clear.Color[2], clear.Color[3]));
                if (clear.ClearDepth && m_Depth) m_List->clearDepthStencilTexture(m_Depth->Native(), nvrhi::AllSubresources, true, clear.Depth, false, clear.Stencil);
                return true;
            }
            bool TransitionTexture(Texture& texture, ResourceState state) override
            {
                auto* native = dynamic_cast<VulkanTexture*>(&texture);
                if (m_State != State::Recording || !native || state == ResourceState::Unknown
                    || (!m_AllowPendingTexture && !CanUseTexture(&texture))) return false;
                if (GetTextureState(*native) == state) return true;
                m_List->setTextureState(native->Native(), nvrhi::AllSubresources, ConvertState(state));
                m_List->commitBarriers();
                StageTextureState(*native, state);
                return true;
            }
            bool TransitionTexture(Texture& texture, ResourceState expectedBefore, ResourceState state) override
            {
                auto* native = dynamic_cast<VulkanTexture*>(&texture);
                if (m_State != State::Recording || !native || expectedBefore == ResourceState::Unknown || state == ResourceState::Unknown
                    || (!m_AllowPendingTexture && !CanUseTexture(&texture))) return false;
                m_List->beginTrackingTextureState(native->Native(), nvrhi::AllSubresources, ConvertState(expectedBefore));
                if (expectedBefore != state) { m_List->setTextureState(native->Native(), nvrhi::AllSubresources, ConvertState(state)); m_List->commitBarriers(); }
                StageTextureState(*native, state, expectedBefore);
                return true;
            }
            bool ReleaseTextureOwnership(const TextureOwnershipRelease& release) override
            {
                if (m_State != State::Recording || !m_TextureOwnershipTracker
                    || !release.Resource) return false;
                if (std::any_of(m_TextureOwnershipOperations.begin(), m_TextureOwnershipOperations.end(), [&](const auto& item) { return item.Resource == release.Resource; })) return false;
                RecordedTextureOwnershipOperation operation;
                ResourceState baseline = ResourceState::Unknown;
                if (!m_TextureOwnershipTracker->QueryState(release.Resource, baseline)) return false;
                if (!m_TextureOwnershipTracker->RecordRelease(release, m_ResolveQueue(m_QueueType), m_ResolveQueue(release.SourceQueue),
                    m_ResolveQueue(release.DestinationQueue), baseline, operation)) return false;
                if (!RecordNativeTextureOwnership(operation)) return false;
                m_TextureOwnershipOperations.push_back(operation);
                return true;
            }
            bool AcquireTextureOwnership(const TextureOwnershipAcquire& acquire) override
            {
                if (m_State != State::Recording || !m_TextureOwnershipTracker
                    || !acquire.Resource
                    || std::find(m_UsedTextures.begin(), m_UsedTextures.end(), acquire.Resource) != m_UsedTextures.end()) return false;
                if (std::any_of(m_TextureOwnershipOperations.begin(), m_TextureOwnershipOperations.end(), [&](const auto& item) { return item.Resource == acquire.Resource; })) return false;
                RecordedTextureOwnershipOperation operation;
                if (!m_TextureOwnershipTracker->RecordAcquire(acquire, m_ResolveQueue(m_QueueType), m_ResolveQueue(acquire.SourceQueue),
                    m_ResolveQueue(acquire.DestinationQueue), operation)) return false;
                if (!RecordNativeTextureOwnership(operation)) return false;
                m_TextureOwnershipOperations.push_back(operation);
                return true;
            }
            bool TransitionBuffer(Buffer& buffer, ResourceState state) override
            {
                auto* native = dynamic_cast<VulkanBuffer*>(&buffer);
                if (m_State != State::Recording || !native
                    || !CanUseBuffer(&buffer)
                    || !IsBufferStateCompatible(buffer.GetDescription().Usage, buffer.GetDescription().CpuAccess, state))
                    return false;
                m_UsedBuffers.push_back(&buffer);
                if (GetBufferState(*native) == state)
                    return true;
                m_List->setBufferState(native->Native(), ConvertState(state));
                m_List->commitBarriers();
                StageBufferState(*native, state);
                return true;
            }
            bool TransitionBuffer(Buffer& buffer, ResourceState expectedBefore, ResourceState state) override
            {
                auto* native = dynamic_cast<VulkanBuffer*>(&buffer);
                if (m_State != State::Recording || !native || expectedBefore == ResourceState::Unknown
                    || !CanUseBuffer(&buffer) || !IsBufferStateCompatible(buffer.GetDescription().Usage, buffer.GetDescription().CpuAccess, state)) return false;
                m_List->beginTrackingBufferState(native->Native(), ConvertState(expectedBefore));
                if (expectedBefore != state) { m_List->setBufferState(native->Native(), ConvertState(state)); m_List->commitBarriers(); }
                StageBufferState(*native, state, expectedBefore);
                return true;
            }
            bool ReleaseBufferOwnership(const BufferOwnershipRelease& release) override
            {
                if (m_State != State::Recording || !m_OwnershipTracker
                    || !release.Resource)
                    return false;
                if (std::any_of(m_OwnershipOperations.begin(), m_OwnershipOperations.end(), [&](const auto& item) { return item.Resource == release.Resource; })) return false;
                RecordedBufferOwnershipOperation operation;
                ResourceState baseline = ResourceState::Unknown;
                if (!m_OwnershipTracker->QueryState(release.Resource, baseline)) return false;
                if (!m_OwnershipTracker->RecordRelease(release, m_ResolveQueue(m_QueueType), m_ResolveQueue(release.SourceQueue),
                    m_ResolveQueue(release.DestinationQueue), baseline, operation))
                    return false;
                if (!RecordNativeBufferOwnership(operation)) return false;
                m_OwnershipOperations.push_back(operation);
                return true;
            }
            bool AcquireBufferOwnership(const BufferOwnershipAcquire& acquire) override
            {
                if (m_State != State::Recording || !m_OwnershipTracker
                    || !acquire.Resource
                    || std::find(m_UsedBuffers.begin(), m_UsedBuffers.end(), acquire.Resource) != m_UsedBuffers.end())
                    return false;
                if (std::any_of(m_OwnershipOperations.begin(), m_OwnershipOperations.end(), [&](const auto& item) { return item.Resource == acquire.Resource; })) return false;
                RecordedBufferOwnershipOperation operation;
                if (!m_OwnershipTracker->RecordAcquire(acquire, m_ResolveQueue(m_QueueType), m_ResolveQueue(acquire.SourceQueue),
                    m_ResolveQueue(acquire.DestinationQueue), operation))
                    return false;
                if (!RecordNativeBufferOwnership(operation)) return false;
                m_OwnershipOperations.push_back(operation);
                return true;
            }
            void SetGraphicsPipeline(Pipeline& pipeline) override
            {
                m_Pipeline = dynamic_cast<VulkanPipeline*>(&pipeline);
                m_BindingSet = nullptr;
                m_TableBindingSet = nullptr;
                m_ConstantBuffer = nullptr;
                m_TextureTable = nullptr;
                m_TableSamplers.clear();
                m_BoundTableTextures.clear();
            }
            void SetGraphicsConstantBuffer(u32 rootParameterIndex, Buffer& buffer) override
            {
                auto* native = dynamic_cast<VulkanBuffer*>(&buffer);
                if (!m_Pipeline || !native || rootParameterIndex != 0 || !CanUseBuffer(&buffer)) { m_BindingSet = nullptr; return; }
                m_ConstantBuffer = native;
                nvrhi::BindingSetDesc bindings;
                bindings.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(rootParameterIndex, native->Native()));
                m_BindingSet = m_Device->createBindingSet(bindings, m_Pipeline->Bindings());
            }
            bool BindGraphicsSampledTextureTable(TextureBindingTable& table) override
            {
                if (m_State != State::Recording || !m_Pipeline || !m_OwnerDevice
                    || !m_Pipeline->GetDescription().SampledTextureTable
                    || !IsValidSampledTextureTablePipeline(m_Pipeline->GetDescription())
                    || m_Pipeline->GetDescription().SampledTextureTable->Capacity != table.GetCapacity()
                    || !table.IsOwnedBy(*m_OwnerDevice) || !m_Pipeline->TableBindings())
                    return false;
                nvrhi::BindingSetDesc tableBindings;
                std::vector<nvrhi::SamplerHandle> samplers;
                std::vector<Ref<Texture>> retained;
                samplers.reserve(table.GetCapacity());
                retained.reserve(table.GetCapacity());
                for (u32 index = 0; index < table.GetCapacity(); ++index)
                {
                    const TextureBindingView view = table.ResolvePublishedSlot(index);
                    auto* texture = dynamic_cast<VulkanTexture*>(view.TextureResource.get());
                    if (!texture || !CanUseTexture(texture) || texture->GetCurrentState() != ResourceState::ShaderResource)
                        return false;
                    nvrhi::SamplerDesc samplerDescription;
                    const bool point = view.Sampler == TextureSampler::PointClamp || view.Sampler == TextureSampler::PointWrap;
                    const bool wrap = view.Sampler == TextureSampler::LinearWrap || view.Sampler == TextureSampler::PointWrap;
                    samplerDescription.setAllFilters(!point).setAllAddressModes(wrap ? nvrhi::SamplerAddressMode::Wrap : nvrhi::SamplerAddressMode::Clamp);
                    nvrhi::SamplerHandle sampler = m_Device->createSampler(samplerDescription);
                    if (!sampler) return false;
                    tableBindings.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(0, texture->Native()).setArrayElement(index));
                    tableBindings.bindings.push_back(nvrhi::BindingSetItem::Sampler(0, sampler).setArrayElement(index));
                    samplers.push_back(sampler);
                    retained.push_back(view.TextureResource);
                }
                nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(tableBindings, m_Pipeline->TableBindings());
                if (!bindingSet) return false;
                m_TableBindingSet = std::move(bindingSet);
                m_TableSamplers = std::move(samplers);
                m_BoundTableTextures = std::move(retained);
                m_TextureTable = &table;
                return true;
            }
            void SetViewport(const Viewport& viewport) override { m_Viewport = viewport; }
            void SetScissorRect(const ScissorRect& rect) override { m_Scissor = rect; }
            void SetVertexBuffer(u32 slot, Buffer& buffer) override { if (slot == 0) m_Vertex = CanUseBuffer(&buffer) ? dynamic_cast<VulkanBuffer*>(&buffer) : nullptr; }
            void SetIndexBuffer(Buffer& buffer, IndexFormat format) override { m_Index = CanUseBuffer(&buffer) ? dynamic_cast<VulkanBuffer*>(&buffer) : nullptr; m_IndexFormat = format; }
            bool CopyBuffer(Buffer& destination, u64 destinationOffset, Buffer& source, u64 sourceOffset, u64 size) override
            {
                if (!CanUseBuffer(&destination) || !CanUseBuffer(&source))
                    return false;
                auto* nativeDestination = dynamic_cast<VulkanBuffer*>(&destination);
                auto* nativeSource = dynamic_cast<VulkanBuffer*>(&source);
                if (m_State != State::Recording || !nativeDestination || !nativeSource || !size
                    || destinationOffset > destination.GetDescription().SizeBytes || size > destination.GetDescription().SizeBytes - destinationOffset
                    || sourceOffset > source.GetDescription().SizeBytes || size > source.GetDescription().SizeBytes - sourceOffset) return false;
                m_List->beginTrackingBufferState(nativeDestination->Native(), ConvertState(GetBufferState(*nativeDestination)));
                m_List->beginTrackingBufferState(nativeSource->Native(), ConvertState(GetBufferState(*nativeSource)));
                m_List->copyBuffer(nativeDestination->Native(), destinationOffset, nativeSource->Native(), sourceOffset, size);
                StageBufferState(*nativeDestination, ResourceState::CopyDest); StageBufferState(*nativeSource, ResourceState::CopySource);
                return true;
            }
            void DrawIndexed(u32 indexCount, u32 instanceCount, u32 startIndex, int baseVertex, u32 startInstance) override
            {
                if (m_State != State::Recording || !m_Pipeline || !m_Framebuffer || !m_BindingSet || !m_Vertex || !m_Index || baseVertex != 0)
                {
                    Log::Error("Vulkan indexed draw rejected: recording=", m_State == State::Recording ? "yes" : "no", ", pipeline=", m_Pipeline ? "yes" : "no", ", framebuffer=", m_Framebuffer ? "yes" : "no", ", bindings=", m_BindingSet ? "yes" : "no", ", vertex=", m_Vertex ? "yes" : "no", ", index=", m_Index ? "yes" : "no", ", baseVertex=", baseVertex);
                    return;
                }
                nvrhi::IGraphicsPipeline* nativePipeline = m_Pipeline->GetOrCreateNative(m_Device, m_Framebuffer->getFramebufferInfo());
                if (!nativePipeline) return;
                nvrhi::ViewportState viewport;
                viewport.addViewport(nvrhi::Viewport(m_Viewport.X, m_Viewport.X + m_Viewport.Width, m_Viewport.Y, m_Viewport.Y + m_Viewport.Height, m_Viewport.MinDepth, m_Viewport.MaxDepth)).addScissorRect(nvrhi::Rect(m_Scissor.Left, m_Scissor.Right, m_Scissor.Top, m_Scissor.Bottom));
                nvrhi::GraphicsState state;
                state.setPipeline(nativePipeline).setFramebuffer(m_Framebuffer).setViewport(viewport).addBindingSet(m_BindingSet)
                    .addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(m_Vertex->Native()).setSlot(0).setOffset(0))
                    .setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(m_Index->Native()).setFormat(m_IndexFormat == IndexFormat::Uint16 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT).setOffset(0));
                if (m_Pipeline->GetDescription().SampledTextureTable && !m_TableBindingSet) return;
                if (m_TableBindingSet) state.addBindingSet(m_TableBindingSet);
                m_List->setGraphicsState(state);
                m_List->drawIndexed(nvrhi::DrawArguments().setVertexCount(indexCount).setInstanceCount(instanceCount).setStartIndexLocation(startIndex).setStartVertexLocation(0).setStartInstanceLocation(startInstance));
            }
            bool ResetQueryPool(QueryPool& queryPool, u32 firstQuery, u32 queryCount) override
            {
                PendingTimestampTransaction* transaction = FindOrBeginTimestampTransaction(queryPool);
                const auto state = transaction ? std::static_pointer_cast<VulkanTimestampQueryState>(transaction->State) : nullptr;
                const VkCommandBuffer commandBuffer = NativeCommandBuffer();
                return transaction && state && commandBuffer != VK_NULL_HANDLE
                    && transaction->Transaction.Reset(firstQuery, queryCount, [state, commandBuffer, firstQuery, queryCount]
                        {
                            VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdResetQueryPool(commandBuffer, state->Pool, firstQuery, queryCount);
                            return true;
                        });
            }
            bool WriteTimestamp(QueryPool& queryPool, u32 queryIndex) override
            {
                PendingTimestampTransaction* transaction = FindOrBeginTimestampTransaction(queryPool);
                const auto state = transaction ? std::static_pointer_cast<VulkanTimestampQueryState>(transaction->State) : nullptr;
                const VkCommandBuffer commandBuffer = NativeCommandBuffer();
                return transaction && state && commandBuffer != VK_NULL_HANDLE
                    && transaction->Transaction.Write(queryIndex, [state, commandBuffer, queryIndex]
                        {
                            VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteTimestamp(
                                commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, state->Pool, queryIndex);
                            return true;
                        });
            }
            bool ResolveQueryPool(QueryPool& queryPool, u32 firstQuery, u32 queryCount) override
            {
                PendingTimestampTransaction* transaction = FindOrBeginTimestampTransaction(queryPool);
                return transaction && transaction->Transaction.Resolve(firstQuery, queryCount, [] { return true; });
            }
            bool Ready() const { return m_State == State::Closed; }
            bool ValidateExpectedStates() const
            {
                for (const TextureState& state : m_TextureStates)
                {
                    ResourceState live = ResourceState::Unknown;
                    if (state.Expected != ResourceState::Unknown && (!m_TextureOwnershipTracker
                        || !m_TextureOwnershipTracker->QueryState(state.Resource, live) || live != state.Expected)) return false;
                }
                for (const BufferState& state : m_BufferStates)
                {
                    ResourceState live = ResourceState::Unknown;
                    if (state.Expected != ResourceState::Unknown && (!m_OwnershipTracker
                        || !m_OwnershipTracker->QueryState(state.Resource, live) || live != state.Expected)) return false;
                }
                return true;
            }
            bool MarkSubmitted(const CompletionToken& token)
            {
                if (!Ready() || !token.IsValid())
                    return false;
                for (PendingTimestampTransaction& transaction : m_TimestampTransactions)
                {
                    if (!transaction.Transaction.Publish(token))
                        return false;
                    m_PublishedTimestampStates.push_back(transaction.State);
                }
                m_LastSubmission = token;
                CommitTrackedStates();
                m_State = State::Submitted;
                return true;
            }
            nvrhi::ICommandList* Native() const { return m_List; }
            std::vector<NativeQueryState> TakePublishedTimestampStates() { return std::move(m_PublishedTimestampStates); }
            const std::vector<RecordedBufferOwnershipOperation>& GetOwnershipOperations() const { return m_OwnershipOperations; }
            const std::vector<RecordedTextureOwnershipOperation>& GetTextureOwnershipOperations() const { return m_TextureOwnershipOperations; }
            bool RecordNativeRecoveryBufferBarrier(const RecordedBufferOwnershipOperation& operation)
            {
                return m_State == State::Recording && RecordNativeBufferOwnership(operation, false);
            }
            bool RecordNativeRecoveryTextureBarrier(const RecordedTextureOwnershipOperation& operation)
            {
                return m_State == State::Recording && RecordNativeTextureOwnership(operation, false);
            }
        private:
            struct PendingTimestampTransaction
            {
                QueryPool* PublicPool = nullptr;
                NativeQueryState State;
                TimestampQueryTransaction Transaction;

                PendingTimestampTransaction(QueryPool& publicPool, NativeQueryState state, TimestampQueryTransaction transaction)
                    : PublicPool(&publicPool), State(std::move(state)), Transaction(std::move(transaction)) {}
            };

            PendingTimestampTransaction* FindOrBeginTimestampTransaction(QueryPool& queryPool)
            {
                if (m_State != State::Recording || !m_TimestampRetirements || !m_ResolveQueue
                    || m_ResolveQueue(m_QueueType) != QueueType::Graphics)
                    return nullptr;
                for (PendingTimestampTransaction& transaction : m_TimestampTransactions)
                    if (transaction.PublicPool == &queryPool)
                        return &transaction;
                auto* pool = dynamic_cast<VulkanTimestampQueryPool*>(&queryPool);
                if (!pool || pool->GetOwnerDeviceId() != m_TimestampRetirements->GetOwnerDeviceId())
                    return nullptr;
                NativeQueryState state = pool->GetNativeState();
                auto transaction = TimestampQueryTransaction::Begin(pool->GetLogicalPool(), *m_TimestampRetirements, state);
                if (!transaction)
                    return nullptr;
                m_TimestampTransactions.emplace_back(queryPool, std::move(state), std::move(*transaction));
                return &m_TimestampTransactions.back();
            }

            bool IsCrossFamily(QueueType source, QueueType destination) const { return m_QueueFamily && m_QueueFamily(source) != m_QueueFamily(destination); }
            VkCommandBuffer NativeCommandBuffer() const
            {
                const nvrhi::Object object = m_List->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
                return reinterpret_cast<VkCommandBuffer>(object.integer);
            }
            bool RecordNativeBufferOwnership(const RecordedBufferOwnershipOperation& operation, bool stageState = true)
            {
                auto* buffer = dynamic_cast<VulkanBuffer*>(operation.Resource);
                if (!buffer) return false;
                if (!IsCrossFamily(operation.Source, operation.Destination))
                {
                    if (operation.Type == BufferOwnershipOperationType::Acquire && operation.Before != operation.After)
                    { m_List->beginTrackingBufferState(buffer->Native(), ConvertState(operation.Before)); m_List->setBufferState(buffer->Native(), ConvertState(operation.After)); m_List->commitBarriers(); if (stageState) StageBufferState(*buffer, operation.After); }
                    return true;
                }
                const VkCommandBuffer commandBuffer = NativeCommandBuffer(); if (!commandBuffer) return false;
                const NativeState before = ConvertNativeState(operation.Before), after = ConvertNativeState(operation.After);
                VkBufferMemoryBarrier2 barrier { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
                barrier.srcStageMask = operation.Type == BufferOwnershipOperationType::Release ? before.Stage : VK_PIPELINE_STAGE_2_NONE;
                barrier.srcAccessMask = operation.Type == BufferOwnershipOperationType::Release ? before.Access : 0;
                barrier.dstStageMask = operation.Type == BufferOwnershipOperationType::Acquire ? after.Stage : VK_PIPELINE_STAGE_2_NONE;
                barrier.dstAccessMask = operation.Type == BufferOwnershipOperationType::Acquire ? after.Access : 0;
                barrier.srcQueueFamilyIndex = m_QueueFamily(operation.Source); barrier.dstQueueFamilyIndex = m_QueueFamily(operation.Destination);
                barrier.buffer = reinterpret_cast<VkBuffer>(buffer->Native()->getNativeObject(nvrhi::ObjectTypes::VK_Buffer).integer); barrier.offset = 0; barrier.size = VK_WHOLE_SIZE;
                VkDependencyInfo dependency { VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dependency.bufferMemoryBarrierCount = 1; dependency.pBufferMemoryBarriers = &barrier; VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2(commandBuffer, &dependency);
                // The native release changes the queue-family-visible state, but
                // the portable wrapper remains hidden/pending until acquire
                // publication. Reseed NVRHI without prematurely publishing it.
                m_List->beginTrackingBufferState(buffer->Native(), ConvertState(operation.After));
                if (stageState && operation.Type == BufferOwnershipOperationType::Acquire) StageBufferState(*buffer, operation.After);
                return true;
            }
            bool RecordNativeTextureOwnership(const RecordedTextureOwnershipOperation& operation, bool stageState = true)
            {
                auto* texture = dynamic_cast<VulkanTexture*>(operation.Resource); if (!texture) return false;
                if (!IsCrossFamily(operation.Source, operation.Destination))
                {
                    if (operation.Type == TextureOwnershipOperationType::Acquire && operation.Before != operation.After)
                    { m_List->beginTrackingTextureState(texture->Native(), nvrhi::AllSubresources, ConvertState(operation.Before)); m_List->setTextureState(texture->Native(), nvrhi::AllSubresources, ConvertState(operation.After)); m_List->commitBarriers(); if (stageState) StageTextureState(*texture, operation.After); }
                    return true;
                }
                const VkCommandBuffer commandBuffer = NativeCommandBuffer(); if (!commandBuffer) return false;
                const NativeState before = ConvertNativeState(operation.Before), after = ConvertNativeState(operation.After);
                VkImageMemoryBarrier2 barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                barrier.srcStageMask = operation.Type == TextureOwnershipOperationType::Release ? before.Stage : VK_PIPELINE_STAGE_2_NONE; barrier.srcAccessMask = operation.Type == TextureOwnershipOperationType::Release ? before.Access : 0;
                barrier.dstStageMask = operation.Type == TextureOwnershipOperationType::Acquire ? after.Stage : VK_PIPELINE_STAGE_2_NONE; barrier.dstAccessMask = operation.Type == TextureOwnershipOperationType::Acquire ? after.Access : 0;
                barrier.oldLayout = before.Layout; barrier.newLayout = after.Layout; barrier.srcQueueFamilyIndex = m_QueueFamily(operation.Source); barrier.dstQueueFamilyIndex = m_QueueFamily(operation.Destination);
                barrier.image = reinterpret_cast<VkImage>(texture->Native()->getNativeObject(nvrhi::ObjectTypes::VK_Image).integer); barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                if (texture->GetDescription().TextureFormat == Format::D32Float) barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                VkDependencyInfo dependency { VK_STRUCTURE_TYPE_DEPENDENCY_INFO }; dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &barrier; VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2(commandBuffer, &dependency);
                m_List->beginTrackingTextureState(texture->Native(), nvrhi::AllSubresources, ConvertState(operation.After));
                if (stageState && operation.Type == TextureOwnershipOperationType::Acquire) StageTextureState(*texture, operation.After);
                return true;
            }
            bool CanUseBuffer(const Buffer* resource) const { return m_CanUseBuffer && m_CanUseBuffer(resource, m_QueueType); }
            bool CanUseTexture(const Texture* resource) const { return m_CanUseTexture && m_CanUseTexture(resource, m_QueueType); }
            struct TextureState { VulkanTexture* Resource = nullptr; ResourceState State = ResourceState::Common; ResourceState Expected = ResourceState::Unknown; };
            struct BufferState { VulkanBuffer* Resource = nullptr; ResourceState State = ResourceState::Common; ResourceState Expected = ResourceState::Unknown; };

            ResourceState GetTextureState(const VulkanTexture& resource) const
            {
                for (const TextureState& state : m_TextureStates)
                    if (state.Resource == &resource) return state.State;
                return resource.GetCurrentState();
            }

            ResourceState GetBufferState(const VulkanBuffer& resource) const
            {
                for (const BufferState& state : m_BufferStates)
                    if (state.Resource == &resource) return state.State;
                return resource.GetCurrentState();
            }

            void StageTextureState(VulkanTexture& resource, ResourceState state, ResourceState expected = ResourceState::Unknown)
            {
                for (TextureState& pending : m_TextureStates)
                    if (pending.Resource == &resource) { pending.State = state; if (pending.Expected == ResourceState::Unknown) pending.Expected = expected; return; }
                m_TextureStates.push_back({ &resource, state, expected });
                m_UsedTextures.push_back(&resource);
            }

            void StageBufferState(VulkanBuffer& resource, ResourceState state, ResourceState expected = ResourceState::Unknown)
            {
                for (BufferState& pending : m_BufferStates)
                    if (pending.Resource == &resource) { pending.State = state; if (pending.Expected == ResourceState::Unknown) pending.Expected = expected; return; }
                m_BufferStates.push_back({ &resource, state, expected });
            }

            void CommitTrackedStates()
            {
                for (const TextureState& state : m_TextureStates)
                {
                    state.Resource->SetCurrentState(state.State);
                    if (m_TextureOwnershipTracker) m_TextureOwnershipTracker->PublishOrdinaryState(*state.Resource, state.State);
                }
                for (const BufferState& state : m_BufferStates)
                {
                    state.Resource->SetCurrentState(state.State);
                    if (m_OwnershipTracker) m_OwnershipTracker->PublishOrdinaryState(*state.Resource, state.State);
                }
            }

            enum class State
            {
                Ready,
                Recording,
                Closed,
                Submitted
            };

            QueueType m_QueueType;
            std::string m_Name;
            nvrhi::CommandListHandle m_List;
            nvrhi::IDevice* m_Device = nullptr;
            nvrhi::FramebufferHandle m_Framebuffer;
            nvrhi::BindingSetHandle m_BindingSet;
            nvrhi::BindingSetHandle m_TableBindingSet;
            VulkanBuffer* m_ConstantBuffer = nullptr;
            TextureBindingTable* m_TextureTable = nullptr;
            Device* m_OwnerDevice = nullptr;
            std::vector<nvrhi::SamplerHandle> m_TableSamplers;
            std::vector<Ref<Texture>> m_BoundTableTextures;
            VulkanTexture* m_Color = nullptr;
            VulkanTexture* m_Depth = nullptr;
            VulkanPipeline* m_Pipeline = nullptr;
            VulkanBuffer* m_Vertex = nullptr;
            VulkanBuffer* m_Index = nullptr;
            IndexFormat m_IndexFormat = IndexFormat::Uint16;
            Viewport m_Viewport;
            ScissorRect m_Scissor;
            std::vector<std::string> m_DebugMarkerNames;
            std::function<CompletionStatus(const CompletionToken&)> m_QueryCompletion;
            TimestampQueryRetirementQueue* m_TimestampRetirements = nullptr;
            BufferOwnershipTracker* m_OwnershipTracker = nullptr;
            TextureOwnershipTracker* m_TextureOwnershipTracker = nullptr;
            std::function<QueueType(QueueType)> m_ResolveQueue;
            std::function<u32(QueueType)> m_QueueFamily;
            std::function<bool(const Buffer*, QueueType)> m_CanUseBuffer;
            std::function<bool(const Texture*, QueueType)> m_CanUseTexture;
            CompletionToken m_LastSubmission;
            std::vector<TextureState> m_TextureStates;
            std::vector<BufferState> m_BufferStates;
            std::vector<Buffer*> m_UsedBuffers;
            std::vector<Texture*> m_UsedTextures;
            std::vector<PendingTimestampTransaction> m_TimestampTransactions;
            std::vector<NativeQueryState> m_PublishedTimestampStates;
            std::vector<RecordedBufferOwnershipOperation> m_OwnershipOperations;
            std::vector<RecordedTextureOwnershipOperation> m_TextureOwnershipOperations;
            bool m_AllowPendingTexture = false;
            State m_State = State::Ready;
        };

        class VulkanDevice final : public Device
        {
        public:
            VulkanDevice(DeviceDescription description, DeviceCapabilities capabilities, nvrhi::IDevice* device,
                nvrhi::vulkan::IDevice* completionDevice, NVRHIVulkanQueueTopology queueTopology,
                VkDevice vulkanDevice, u32 graphicsTimestampValidBits, double timestampPeriodNanoseconds)
                : m_Description(std::move(description)), m_Capabilities(std::move(capabilities)), m_Device(device)
                , m_CompletionDevice(completionDevice)
                , m_QueueTopology(queueTopology)
                , m_CompletionDeviceId(s_NextCompletionDeviceId.fetch_add(1))
                , m_ResourceOwnerId(s_NextResourceOwnerId.fetch_add(1))
                , m_TimestampRetirements(m_CompletionDeviceId)
                , m_VulkanDevice(vulkanDevice)
                , m_GraphicsTimestampValidBits(graphicsTimestampValidBits)
                , m_TimestampPeriodNanoseconds(timestampPeriodNanoseconds) {}
            const DeviceDescription& GetDescription() const override { return m_Description; }
            const DeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
            QueueResolution ResolveQueue(QueueType requested) const override
            {
                return ResolveVulkanQueue(m_QueueTopology, requested);
            }
            bool CanQueuesShareResources(QueueType source, QueueType destination) const override { return CanShareResource(source, destination); }
            Scope<Buffer> CreateBuffer(const BufferDescription& description) override
            {
                if (!m_Device || !description.SizeBytes) return nullptr;
                nvrhi::ResourceStates initialState = ConvertState(description.InitialState);
                if (initialState == nvrhi::ResourceStates::Common || initialState == nvrhi::ResourceStates::Unknown)
                    initialState = HasBufferUsage(description.Usage, BufferUsage::CopyDest)
                        ? nvrhi::ResourceStates::CopyDest : nvrhi::ResourceStates::Common;
                nvrhi::BufferDesc d; d.setByteSize(description.SizeBytes).setStructStride(description.StrideBytes).setDebugName(description.DebugName).enableAutomaticStateTracking(initialState);
                d.setIsVertexBuffer(HasBufferUsage(description.Usage, BufferUsage::Vertex)).setIsIndexBuffer(HasBufferUsage(description.Usage, BufferUsage::Index)).setIsConstantBuffer(HasBufferUsage(description.Usage, BufferUsage::Constant)).setCanHaveUAVs(HasBufferUsage(description.Usage, BufferUsage::Storage));
                d.setCpuAccess(description.CpuAccess == BufferCpuAccess::Read ? nvrhi::CpuAccessMode::Read : description.CpuAccess == BufferCpuAccess::Write ? nvrhi::CpuAccessMode::Write : nvrhi::CpuAccessMode::None);
                nvrhi::BufferHandle native = m_Device->createBuffer(d); if (!native) return nullptr;
                const ResourceState trackedInitialState = (description.InitialState == ResourceState::Common || description.InitialState == ResourceState::Unknown)
                    && HasBufferUsage(description.Usage, BufferUsage::CopyDest)
                    ? ResourceState::CopyDest : description.InitialState;
                auto result = CreateScope<VulkanBuffer>(description, native, m_ResourceOwnerId);
                result->SetDevice(m_Device);
                result->SetCurrentState(trackedInitialState);
                if (!m_BufferOwnership.Register(*result, QueueType::Graphics, trackedInitialState))
                    return nullptr;
                result->SetOwnershipTracker(&m_BufferOwnership);
                return result;
            }
            Scope<Texture> CreateTexture(const TextureDescription& description) override
            {
                if (!m_Device || !description.Extent.Width || !description.Extent.Height || description.MipLevels != 1 || description.ArrayLayers != 1 || description.SampleCount != 1) return nullptr;
                nvrhi::Format format = ConvertFormat(description.TextureFormat); if (format == nvrhi::Format::UNKNOWN) return nullptr;
                nvrhi::ResourceStates initialState = ConvertState(description.InitialState);
                // Vulkan has no useful image layout for generic Common. Pick the
                // first declared core use, while preserving explicit non-Common
                // states supplied by the caller.
                if (initialState == nvrhi::ResourceStates::Common || initialState == nvrhi::ResourceStates::Unknown)
                    initialState = HasTextureUsage(description.Usage, TextureUsage::DepthStencil)
                        ? nvrhi::ResourceStates::DepthWrite
                        : HasTextureUsage(description.Usage, TextureUsage::RenderTarget)
                            ? nvrhi::ResourceStates::RenderTarget
                            : HasTextureUsage(description.Usage, TextureUsage::CopyDest)
                                ? nvrhi::ResourceStates::CopyDest
                                : nvrhi::ResourceStates::CopySource;
                nvrhi::TextureDesc d; d.setWidth(description.Extent.Width).setHeight(description.Extent.Height).setMipLevels(1).setArraySize(1).setSampleCount(1).setFormat(format).setDebugName(description.DebugName).enableAutomaticStateTracking(initialState);
                d.setIsRenderTarget(HasTextureUsage(description.Usage, TextureUsage::RenderTarget) || HasTextureUsage(description.Usage, TextureUsage::DepthStencil)).setIsUAV(HasTextureUsage(description.Usage, TextureUsage::UnorderedAccess)); d.isShaderResource = HasTextureUsage(description.Usage, TextureUsage::ShaderResource);
                nvrhi::TextureHandle native = m_Device->createTexture(d);
                if (!native) return nullptr;
                auto result = CreateScope<VulkanTexture>(description, native, m_ResourceOwnerId);
                const ResourceState trackedInitialState = description.InitialState == ResourceState::Unknown ? ResourceState::Unknown
                    : description.InitialState == ResourceState::Common
                    ? HasTextureUsage(description.Usage, TextureUsage::DepthStencil) ? ResourceState::DepthWrite
                        : HasTextureUsage(description.Usage, TextureUsage::RenderTarget) ? ResourceState::RenderTarget
                        : HasTextureUsage(description.Usage, TextureUsage::CopyDest) ? ResourceState::CopyDest : ResourceState::CopySource
                    : description.InitialState;
                result->SetCurrentState(trackedInitialState);
                if (!m_TextureOwnership.Register(*result, QueueType::Graphics, trackedInitialState))
                    return nullptr;
                result->SetOwnershipTracker(&m_TextureOwnership);
                return result;
            }
            bool OwnsResource(const Buffer* resource) const override
            {
                const auto* buffer = dynamic_cast<const VulkanBuffer*>(resource);
                return buffer && buffer->GetOwnerId() == m_ResourceOwnerId && m_BufferOwnership.IsLive(resource);
            }
            bool OwnsResource(const Texture* resource) const override
            {
                const auto* texture = dynamic_cast<const VulkanTexture*>(resource);
                return texture && texture->GetOwnerId() == m_ResourceOwnerId && m_TextureOwnership.IsLive(resource);
            }
            bool OwnsQueryPool(const QueryPool* queryPool) const override
            {
                const auto* pool = dynamic_cast<const VulkanTimestampQueryPool*>(queryPool);
                return pool && pool->GetOwnerDeviceId() == m_CompletionDeviceId;
            }
            bool QueryResourceState(const Buffer* resource, ResourceState& state) const override
            {
                const auto* buffer = dynamic_cast<const VulkanBuffer*>(resource);
                return buffer && OwnsResource(resource) && m_BufferOwnership.QueryState(resource, state);
            }
            bool QueryBufferQueueOwner(const Buffer* resource, QueueType& owner) const override
            {
                return OwnsResource(resource) && m_BufferOwnership.QueryOwner(resource, owner);
            }
            bool HasPendingBufferOwnershipTransfer(const Buffer* resource) const override
            {
                return OwnsResource(resource) && m_BufferOwnership.HasPending(resource);
            }
            bool CanDestroyBuffer(const Buffer* resource) const override
            {
                return OwnsResource(resource) && m_BufferOwnership.CanDestroy(resource);
            }
            bool RecoverAbandonedBufferOwnershipTransfer(Buffer& resource, const CompletionToken& releaseToken) override
            {
                return OwnsResource(&resource) && RecoverAbandonedBufferOwnership(resource, releaseToken);
            }
            bool QueryResourceState(const Texture* resource, ResourceState& state) const override
            {
                const auto* texture = dynamic_cast<const VulkanTexture*>(resource);
                return texture && OwnsResource(resource) && m_TextureOwnership.QueryState(resource, state);
            }
            bool QueryTextureQueueOwner(const Texture* resource, QueueType& owner) const override { return OwnsResource(resource) && m_TextureOwnership.QueryOwner(resource, owner); }
            bool HasPendingTextureOwnershipTransfer(const Texture* resource) const override { return OwnsResource(resource) && m_TextureOwnership.HasPending(resource); }
            bool CanDestroyTexture(const Texture* resource) const override { return OwnsResource(resource) && m_TextureOwnership.CanDestroy(resource); }
            bool RecoverAbandonedTextureOwnershipTransfer(Texture& resource, const CompletionToken& token) override
            {
                return OwnsResource(&resource) && RecoverAbandonedTextureOwnership(resource, token);
            }
            Scope<Shader> CreateShader(const ShaderDescription& description) override
            {
                if (!m_Device || description.BinaryFormat != ShaderBinaryFormat::Spirv || description.Binary.empty() || ConvertShaderStage(description.Stage) == nvrhi::ShaderType::None)
                    return nullptr;
                nvrhi::ShaderDesc shaderDescription;
                shaderDescription.setShaderType(ConvertShaderStage(description.Stage)).setDebugName(description.DebugName).setEntryName(description.EntryPoint);
                nvrhi::ShaderHandle shader = m_Device->createShader(shaderDescription, description.Binary.data(), description.Binary.size());
                return shader ? CreateScope<VulkanShader>(description, shader) : nullptr;
            }
            Scope<Pipeline> CreatePipeline(const PipelineDescription& description) override
            {
                if (description.SampledTextureTable && !IsValidSampledTextureTablePipeline(description))
                { Log::Error("Vulkan sampled-table pipeline rejected by portable declaration/reflection validation"); return nullptr; }
                auto* vertex = dynamic_cast<VulkanShader*>(description.VertexShader);
                auto* pixel = dynamic_cast<VulkanShader*>(description.PixelShader);
                if (!m_Device || description.Type != PipelineType::Graphics || !vertex || !pixel || description.VertexInputs.size() != 3 || description.ConstantBufferBindings.size() != 1
                    || description.ConstantBufferBindings[0].ShaderRegister != 0 || description.ConstantBufferBindings[0].RegisterSpace != 0)
                { Log::Error("Vulkan sampled-table pipeline rejected by core graphics declaration validation"); return nullptr; }
                std::vector<nvrhi::VertexAttributeDesc> attributes;
                for (const VertexInputAttribute& input : description.VertexInputs)
                {
                    if (input.InputSlot != 0 || input.InputRate != VertexInputRate::PerVertex) return nullptr;
                    const nvrhi::Format format = input.AttributeFormat == Format::R32G32B32Float
                        ? nvrhi::Format::RGB32_FLOAT
                        : input.AttributeFormat == Format::R32G32Float ? nvrhi::Format::RG32_FLOAT : nvrhi::Format::UNKNOWN;
                    if (format == nvrhi::Format::UNKNOWN) return nullptr;
                    attributes.emplace_back(nvrhi::VertexAttributeDesc().setName(input.SemanticName + std::to_string(input.SemanticIndex)).setFormat(format).setBufferIndex(input.InputSlot).setOffset(input.OffsetBytes).setElementStride(32));
                }
                nvrhi::InputLayoutHandle inputLayout = m_Device->createInputLayout(attributes.data(), static_cast<u32>(attributes.size()), vertex->Native());
                nvrhi::VulkanBindingOffsets bindingOffsets;
                bindingOffsets.setConstantBufferOffset(0);
                nvrhi::BindingLayoutDesc bindingLayout;
                bindingLayout.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
                    .setRegisterSpaceAndDescriptorSet(0)
                    .setBindingOffsets(bindingOffsets)
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
                nvrhi::BindingLayoutHandle bindings = m_Device->createBindingLayout(bindingLayout);
                nvrhi::BindingLayoutHandle tableBindings;
                if (description.SampledTextureTable)
                {
                    nvrhi::VulkanBindingOffsets tableOffsets;
                    tableOffsets.setShaderResourceOffset(description.SampledTextureTable->VulkanTextureBindingOffset)
                        .setSamplerOffset(description.SampledTextureTable->VulkanSamplerBindingOffset);
                    nvrhi::BindingLayoutDesc tableLayout;
                    tableLayout.setVisibility(nvrhi::ShaderType::Pixel)
                        .setRegisterSpaceAndDescriptorSet(description.SampledTextureTable->RegisterSpace)
                        .setBindingOffsets(tableOffsets)
                        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(description.SampledTextureTable->TextureRegister).setSize(description.SampledTextureTable->Capacity))
                        .addItem(nvrhi::BindingLayoutItem::Sampler(description.SampledTextureTable->SamplerRegister).setSize(description.SampledTextureTable->Capacity));
                    tableBindings = m_Device->createBindingLayout(tableLayout);
                }
                if (!inputLayout || !bindings || (description.SampledTextureTable && !tableBindings))
                {
                    Log::Error("Vulkan sampled-table native layout creation failed: input=", inputLayout ? "yes" : "no",
                        ", constants=", bindings ? "yes" : "no", ", table=", tableBindings ? "yes" : "no");
                    return nullptr;
                }
                return CreateScope<VulkanPipeline>(description, inputLayout, bindings, tableBindings, vertex->NativeHandle(), pixel->NativeHandle());
            }
            Scope<QueryPool> CreateQueryPool(const QueryPoolDescription& description) override
            {
                if (m_VulkanDevice == VK_NULL_HANDLE || description.Type != QueryType::Timestamp || description.Count == 0
                    || description.Count > TimestampQueryPool::kMaximumQueryCount || m_GraphicsTimestampValidBits == 0
                    || m_TimestampPeriodNanoseconds <= 0.0)
                    return nullptr;
                Scope<TimestampQueryPool> logicalPool = TimestampQueryPool::Create(m_CompletionDeviceId, description);
                if (!logicalPool)
                    return nullptr;
                auto state = std::make_shared<VulkanTimestampQueryState>();
                state->Device = m_VulkanDevice;
                state->Count = description.Count;
                state->ValidBits = m_GraphicsTimestampValidBits;
                state->PeriodNanoseconds = m_TimestampPeriodNanoseconds;
                VkQueryPoolCreateInfo createInfo { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                createInfo.queryCount = description.Count;
                const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateQueryPool(
                    m_VulkanDevice, &createInfo, nullptr, &state->Pool);
                if (result != VK_SUCCESS)
                {
                    Log::Error("Could not create Vulkan timestamp query pool '", description.DebugName,
                        "': VkResult=", static_cast<int>(result));
                    return nullptr;
                }
                return CreateScope<VulkanTimestampQueryPool>(std::move(logicalPool), std::move(state));
            }
            Scope<CommandList> CreateCommandList(QueueType type, std::string_view name) override
            {
                if (!m_Device) return nullptr;
                const QueueResolution resolution = ResolveQueue(type);
                nvrhi::CommandListParameters parameters;
                // Vulkan RHI submissions are explicitly accepted by Device::Submit;
                // they must be independently recordable rather than validation's
                // single immediate-context mode.
                parameters.setQueueType(ConvertQueue(resolution.Effective)).setEnableImmediateExecution(false);
                nvrhi::CommandListHandle list = m_Device->createCommandList(parameters); return list ? CreateScope<VulkanCommandList>(resolution.Effective, std::string(name), list, m_Device,
                    [this](const CompletionToken& token) { return QueryCompletion(token); }, &m_TimestampRetirements,
                    &m_BufferOwnership, &m_TextureOwnership,
                    [this](QueueType requested) { return ResolveQueue(requested).Effective; },
                    [this](QueueType queue) { const QueueResolution resolution = ResolveQueue(queue); return resolution.Effective == QueueType::Compute ? m_QueueTopology.ComputeFamily : resolution.Effective == QueueType::Copy ? m_QueueTopology.CopyFamily : m_QueueTopology.GraphicsFamily; },
                    [this](const Buffer* resource, QueueType queue) { return CanUseBufferOnQueue(resource, queue); },
                    [this](const Texture* resource, QueueType queue) { return CanUseTextureOnQueue(resource, queue); }, this) : nullptr;
            }
            bool UploadBuffer(Buffer& destination, const void* data, u64 size, u64 offset) override
            {
                auto* buffer = dynamic_cast<VulkanBuffer*>(&destination); if (!buffer || !OwnsResource(&destination)
                    || !CanUseBufferOnQueue(&destination, QueueType::Graphics) || !data || !size || offset > destination.GetDescription().SizeBytes
                    || size > destination.GetDescription().SizeBytes - offset) return false;
                Scope<CommandList> list = CreateCommandList(QueueType::Graphics, "Vulkan RHI Buffer Upload"); if (!list || !list->Begin()) return false;
                static_cast<VulkanCommandList*>(list.get())->Native()->writeBuffer(buffer->Native(), data, size, offset); return list->End() && SubmitAndWait(*list);
            }
            bool UploadTexture(Texture& destination, const TextureUpload& upload) override
            {
                auto* texture = dynamic_cast<VulkanTexture*>(&destination);
                if (!texture || !OwnsResource(&destination) || !CanUseTextureOnQueue(&destination, QueueType::Graphics)
                    || !IsReadOnlyTextureUploadCompatible(destination.GetDescription(), upload)
                    || texture->GetCurrentState() != ResourceState::CopyDest)
                    return false;
                Scope<CommandList> list = CreateCommandList(QueueType::Graphics, "Vulkan RHI Texture Upload");
                if (!list || !list->Begin())
                    return false;
                auto* nativeList = static_cast<VulkanCommandList*>(list.get());
                nativeList->Native()->beginTrackingTextureState(texture->Native(), nvrhi::AllSubresources, ConvertState(ResourceState::CopyDest));
                nativeList->Native()->writeTexture(texture->Native(), 0, 0, upload.Bytes->data(), upload.RowPitchBytes,
                    static_cast<size_t>(upload.RowPitchBytes) * destination.GetDescription().Extent.Height);
                return nativeList->TransitionTexture(destination, ResourceState::ShaderResource)
                    && list->End() && SubmitAndWait(*list);
            }
            bool ReadbackTexture(Texture& source, TextureReadback& out) override
            {
                auto* texture = dynamic_cast<VulkanTexture*>(&source); const auto& d = source.GetDescription(); QueueType owner = QueueType::Graphics;
                if (!texture || !OwnsResource(&source) || !m_TextureOwnership.QueryOwner(&source, owner) || !CanUseTextureOnQueue(&source, owner) || d.TextureFormat != Format::R8G8B8A8Unorm || !HasTextureUsage(d.Usage, TextureUsage::CopySource)) return false;
                nvrhi::StagingTextureHandle staging = m_Device->createStagingTexture(texture->Native()->getDesc(), nvrhi::CpuAccessMode::Read); if (!staging) return false;
                Scope<CommandList> list = CreateCommandList(owner, "Vulkan RHI Texture Readback"); if (!list || !list->Begin()) return false;
                auto* nativeList = static_cast<VulkanCommandList*>(list.get()); nativeList->Native()->beginTrackingTextureState(texture->Native(), nvrhi::AllSubresources, ConvertState(texture->GetCurrentState()));
                nativeList->Native()->copyTexture(staging, nvrhi::TextureSlice(), texture->Native(), nvrhi::TextureSlice()); if (!list->End() || !SubmitAndWait(*list)) return false;
                size_t rowPitch = 0;
                void* mapped = m_Device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
                if (!mapped)
                    return false;
                struct ScopedStagingTextureUnmap final
                {
                    nvrhi::IDevice* Device;
                    nvrhi::IStagingTexture* Texture;
                    ~ScopedStagingTextureUnmap() { Device->unmapStagingTexture(Texture); }
                } unmap { m_Device, staging };
                if (!rowPitch || rowPitch > std::numeric_limits<u32>::max() || rowPitch > std::numeric_limits<size_t>::max() / d.Extent.Height)
                    return false;
                const size_t dataSize = rowPitch * static_cast<size_t>(d.Extent.Height);
                TextureReadback readback;
                readback.Extent = d.Extent;
                readback.TextureFormat = d.TextureFormat;
                readback.RowPitchBytes = static_cast<u32>(rowPitch);
                readback.Data.resize(dataSize);
                std::memcpy(readback.Data.data(), mapped, dataSize);
                out = std::move(readback);
                return true;
            }
            CompletionToken Submit(CommandList& commandList) override
            {
                return Submit(commandList, {});
            }
            CompletionToken Submit(CommandList& commandList, const std::vector<CompletionToken>& dependencies) override
            {
                auto* list = dynamic_cast<VulkanCommandList*>(&commandList);
                if (!m_Device || !m_CompletionDevice || !list || !list->Ready() || !list->ValidateExpectedStates())
                    return {};
                const SubmissionDependencyError dependencyError = ValidateSubmissionDependencies(
                    m_CompletionDeviceId, m_NextCompletionSubmissionId, dependencies,
                    [this](const CompletionToken& dependency) { return FindCompletionEntry(dependency) != m_CompletionEntries.end(); });
                if (dependencyError != SubmissionDependencyError::None)
                    return {};
                if (std::any_of(list->GetOwnershipOperations().begin(), list->GetOwnershipOperations().end(), [&](const auto& operation) { return !m_BufferOwnership.ValidateSubmission(operation, dependencies); }))
                    return {};
                if (std::any_of(list->GetTextureOwnershipOperations().begin(), list->GetTextureOwnershipOperations().end(), [&](const auto& operation) { return !m_TextureOwnership.ValidateSubmission(operation, dependencies); }))
                    return {};
                const nvrhi::CommandQueue executionQueue = ConvertQueue(list->GetQueueType());
                for (const CompletionToken& dependency : dependencies)
                {
                    const auto found = FindCompletionEntry(dependency);
                    if (found != m_CompletionEntries.end() && found->second.Queue != executionQueue)
                        m_CompletionDevice->queueWaitForCommandList(executionQueue, found->second.Queue, found->second.NativeSubmissionId);
                }
                const u64 nativeSubmissionId = m_Device->executeCommandList(list->Native(), executionQueue);
                if (nativeSubmissionId == 0)
                    return {};
                const CompletionToken token { m_CompletionDeviceId, m_NextCompletionSubmissionId++ };
                m_CompletionEntries.emplace(token.SubmissionId, CompletionEntry { executionQueue, nativeSubmissionId });
                if (!list->MarkSubmitted(token))
                    return {};
                auto completion = m_CompletionEntries.find(token.SubmissionId);
                if (completion == m_CompletionEntries.end())
                    return {};
                completion->second.TimestampStates = list->TakePublishedTimestampStates();
                for (const auto& ownership : list->GetOwnershipOperations())
                {
                    const bool published = ownership.Type == BufferOwnershipOperationType::Release
                        ? m_BufferOwnership.PublishRelease(ownership, token) : m_BufferOwnership.PublishAcquire(ownership);
                    if (!published) return {};
                }
                for (const auto& ownership : list->GetTextureOwnershipOperations())
                {
                    const bool published = ownership.Type == TextureOwnershipOperationType::Release
                        ? m_TextureOwnership.PublishRelease(ownership, token) : m_TextureOwnership.PublishAcquire(ownership);
                    if (!published) return {};
                }
                return token;
            }
            CompletionStatus QueryCompletion(const CompletionToken& token) override
            {
                const auto found = FindCompletionEntry(token);
                if (found == m_CompletionEntries.end())
                    return CompletionStatus::Invalid;
                if (found->second.TerminalStatus != CompletionStatus::Incomplete)
                    return found->second.TerminalStatus;
                if (!m_CompletionDevice)
                {
                    found->second.TerminalStatus = CompletionStatus::Failed;
                    return found->second.TerminalStatus;
                }
                if (m_CompletionDevice->queueGetCompletedInstance(found->second.Queue) < found->second.NativeSubmissionId)
                    return CompletionStatus::Incomplete;
                const CompletionStatus queryStatus = FinalizeTimestampQueries(found->second, token);
                if (queryStatus != CompletionStatus::Incomplete)
                    found->second.TerminalStatus = queryStatus;
                return queryStatus;
            }
            bool WaitForCompletion(const CompletionToken& token, u32 timeoutMilliseconds) override
            {
                const auto found = FindCompletionEntry(token);
                if (found == m_CompletionEntries.end() || timeoutMilliseconds == 0 || !m_CompletionDevice)
                    return false;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
                do
                {
                    if (QueryCompletion(token) == CompletionStatus::Complete)
                        return true;
                    std::this_thread::yield();
                } while (std::chrono::steady_clock::now() < deadline);
                return QueryCompletion(token) == CompletionStatus::Complete;
            }
            bool SubmitAndWait(CommandList& commandList) override
            {
                const CompletionToken token = Submit(commandList);
                const bool completed = token.IsValid() && WaitForCompletion(token, std::numeric_limits<u32>::max());
                if (m_Device)
                    m_Device->runGarbageCollection();
                return completed;
            }
            void WaitIdle() override { if (m_Device) { m_Device->waitForIdle(); m_Device->runGarbageCollection(); } }
        private:
            DeviceDescription m_Description;
            DeviceCapabilities m_Capabilities;
            nvrhi::IDevice* m_Device = nullptr;
            nvrhi::vulkan::IDevice* m_CompletionDevice = nullptr;
            NVRHIVulkanQueueTopology m_QueueTopology;
            u64 m_CompletionDeviceId = 0;
            const u64 m_ResourceOwnerId;
            TimestampQueryRetirementQueue m_TimestampRetirements;
            VkDevice m_VulkanDevice = VK_NULL_HANDLE;
            u32 m_GraphicsTimestampValidBits = 0;
            double m_TimestampPeriodNanoseconds = 0.0;
            u64 m_NextCompletionSubmissionId = 1;
            struct CompletionEntry
            {
                nvrhi::CommandQueue Queue = nvrhi::CommandQueue::Graphics;
                u64 NativeSubmissionId = 0;
                std::vector<NativeQueryState> TimestampStates;
                CompletionStatus TerminalStatus = CompletionStatus::Incomplete;
            };
            std::unordered_map<u64, CompletionEntry> m_CompletionEntries;
            BufferOwnershipTracker m_BufferOwnership;
            TextureOwnershipTracker m_TextureOwnership;

            bool CanShareResource(QueueType source, QueueType destination) const
            {
                return VulkanQueuesMayShareResources(m_QueueTopology, source, destination);
            }
            u32 QueueFamily(QueueType queue) const
            {
                const QueueType effective = ResolveQueue(queue).Effective;
                return effective == QueueType::Compute ? m_QueueTopology.ComputeFamily
                    : effective == QueueType::Copy ? m_QueueTopology.CopyFamily : m_QueueTopology.GraphicsFamily;
            }
            bool RecoverAbandonedBufferOwnership(Buffer& resource, const CompletionToken& releaseToken)
            {
                PendingBufferOwnershipTransfer pending;
                if (!m_BufferOwnership.QueryPending(&resource, pending) || pending.ReleaseToken.DeviceId != releaseToken.DeviceId
                    || pending.ReleaseToken.SubmissionId != releaseToken.SubmissionId || QueryCompletion(releaseToken) != CompletionStatus::Complete)
                    return false;
                auto* native = dynamic_cast<VulkanBuffer*>(&resource);
                if (!native)
                    return false;
                if (QueueFamily(pending.Source) != QueueFamily(pending.Destination))
                {
                    const RecordedBufferOwnershipOperation acquire { BufferOwnershipOperationType::Acquire, &resource, pending.Source,
                        pending.Destination, pending.Before, pending.After, pending.ReleaseToken };
                    const RecordedBufferOwnershipOperation release { BufferOwnershipOperationType::Release, &resource, pending.Destination,
                        pending.Source, pending.After, pending.Before, {} };
                    const RecordedBufferOwnershipOperation returnAcquire { BufferOwnershipOperationType::Acquire, &resource, pending.Destination,
                        pending.Source, pending.After, pending.Before, {} };
                    auto submit = [this](QueueType queue, std::string_view name, const RecordedBufferOwnershipOperation& operation,
                        const std::vector<CompletionToken>& dependencies)
                    {
                        Scope<CommandList> list = CreateCommandList(queue, name);
                        auto* nativeList = dynamic_cast<VulkanCommandList*>(list.get());
                        if (!nativeList || !nativeList->Begin() || !nativeList->RecordNativeRecoveryBufferBarrier(operation) || !nativeList->End())
                            return CompletionToken {};
                        return Submit(*nativeList, dependencies);
                    };
                    const CompletionToken acquired = submit(pending.Destination, "Vulkan Buffer Recovery Acquire", acquire, { releaseToken });
                    const CompletionToken released = acquired.IsValid()
                        ? submit(pending.Destination, "Vulkan Buffer Recovery Release", release, { acquired }) : CompletionToken {};
                    const CompletionToken returned = released.IsValid()
                        ? submit(pending.Source, "Vulkan Buffer Recovery Return", returnAcquire, { released }) : CompletionToken {};
                    if (!returned.IsValid() || !WaitForCompletion(returned, 5000))
                        return false;
                }
                if (!m_BufferOwnership.Recover(resource, releaseToken, CompletionStatus::Complete))
                    return false;
                native->SetCurrentState(pending.Before);
                return true;
            }
            bool RecoverAbandonedTextureOwnership(Texture& resource, const CompletionToken& releaseToken)
            {
                PendingTextureOwnershipTransfer pending;
                if (!m_TextureOwnership.QueryPending(&resource, pending) || pending.ReleaseToken.DeviceId != releaseToken.DeviceId
                    || pending.ReleaseToken.SubmissionId != releaseToken.SubmissionId || QueryCompletion(releaseToken) != CompletionStatus::Complete)
                    return false;
                auto* native = dynamic_cast<VulkanTexture*>(&resource);
                if (!native)
                    return false;
                if (QueueFamily(pending.Source) != QueueFamily(pending.Destination))
                {
                    const RecordedTextureOwnershipOperation acquire { TextureOwnershipOperationType::Acquire, &resource, pending.Source,
                        pending.Destination, pending.Before, pending.After, pending.ReleaseToken };
                    const RecordedTextureOwnershipOperation release { TextureOwnershipOperationType::Release, &resource, pending.Destination,
                        pending.Source, pending.After, pending.Before, {} };
                    const RecordedTextureOwnershipOperation returnAcquire { TextureOwnershipOperationType::Acquire, &resource, pending.Destination,
                        pending.Source, pending.After, pending.Before, {} };
                    auto submit = [this](QueueType queue, std::string_view name, const RecordedTextureOwnershipOperation& operation,
                        const std::vector<CompletionToken>& dependencies)
                    {
                        Scope<CommandList> list = CreateCommandList(queue, name);
                        auto* nativeList = dynamic_cast<VulkanCommandList*>(list.get());
                        if (!nativeList || !nativeList->Begin() || !nativeList->RecordNativeRecoveryTextureBarrier(operation) || !nativeList->End())
                            return CompletionToken {};
                        return Submit(*nativeList, dependencies);
                    };
                    const CompletionToken acquired = submit(pending.Destination, "Vulkan Texture Recovery Acquire", acquire, { releaseToken });
                    const CompletionToken released = acquired.IsValid()
                        ? submit(pending.Destination, "Vulkan Texture Recovery Release", release, { acquired }) : CompletionToken {};
                    const CompletionToken returned = released.IsValid()
                        ? submit(pending.Source, "Vulkan Texture Recovery Return", returnAcquire, { released }) : CompletionToken {};
                    if (!returned.IsValid() || !WaitForCompletion(returned, 5000))
                        return false;
                }
                if (!m_TextureOwnership.Recover(resource, releaseToken, CompletionStatus::Complete))
                    return false;
                native->SetCurrentState(pending.Before);
                return true;
            }
            bool CanUseBufferOnQueue(const Buffer* resource, QueueType queue) const
            {
                QueueType owner;
                return resource && m_BufferOwnership.CanUse(resource) && m_BufferOwnership.QueryOwner(resource, owner)
                    && CanShareResource(owner, queue);
            }
            bool CanUseTextureOnQueue(const Texture* resource, QueueType queue) const
            {
                QueueType owner;
                return resource && m_TextureOwnership.CanUse(resource) && m_TextureOwnership.QueryOwner(resource, owner)
                    && CanShareResource(owner, queue);
            }
            CompletionStatus FinalizeTimestampQueries(CompletionEntry& entry, const CompletionToken& token)
            {
                if (entry.TimestampStates.empty())
                    return CompletionStatus::Complete;
                struct CollectedResult
                {
                    NativeQueryState State;
                    CompletionStatus Status = CompletionStatus::Failed;
                    std::vector<u64> Values;
                };
                std::vector<CollectedResult> collected;
                collected.reserve(entry.TimestampStates.size());
                bool nativeSuccess = true;
                for (const NativeQueryState& nativeState : entry.TimestampStates)
                {
                    CollectedResult result;
                    result.State = nativeState;
                    const auto state = std::static_pointer_cast<VulkanTimestampQueryState>(nativeState);
                    if (state && state->Pool != VK_NULL_HANDLE && state->Count != 0)
                    {
                        result.Values.resize(state->Count);
                        const VkResult queryResult = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetQueryPoolResults(
                            state->Device, state->Pool, 0, state->Count,
                            result.Values.size() * sizeof(u64), result.Values.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
                        if (queryResult == VK_NOT_READY)
                            return CompletionStatus::Incomplete;
                        if (queryResult == VK_SUCCESS)
                        {
                            if (state->ValidBits < 64)
                            {
                                const u64 mask = (u64 { 1 } << state->ValidBits) - 1;
                                for (u64& value : result.Values)
                                    value &= mask;
                            }
                            result.Status = CompletionStatus::Complete;
                        }
                        else
                        {
                            result.Values.clear();
                            nativeSuccess = false;
                        }
                    }
                    else
                    {
                        nativeSuccess = false;
                    }
                    collected.push_back(std::move(result));
                }

                bool lifecycleSuccess = true;
                for (const CollectedResult& result : collected)
                    if (!m_TimestampRetirements.Complete(result.State, token, result.Status, result.Values))
                        lifecycleSuccess = false;
                const CompletionStatus terminal = nativeSuccess && lifecycleSuccess
                    ? CompletionStatus::Complete : CompletionStatus::Failed;
                const bool retired = m_TimestampRetirements.Retire(token, terminal);
                entry.TimestampStates.clear();
                return retired && terminal == CompletionStatus::Complete ? CompletionStatus::Complete : CompletionStatus::Failed;
            }

            std::unordered_map<u64, CompletionEntry>::iterator FindCompletionEntry(const CompletionToken& token)
            {
                if (!token.IsValid() || token.DeviceId != m_CompletionDeviceId)
                    return m_CompletionEntries.end();
                return m_CompletionEntries.find(token.SubmissionId);
            }
            std::unordered_map<u64, CompletionEntry>::const_iterator FindCompletionEntry(const CompletionToken& token) const
            {
                if (!token.IsValid() || token.DeviceId != m_CompletionDeviceId)
                    return m_CompletionEntries.end();
                return m_CompletionEntries.find(token.SubmissionId);
            }
        };
    }

    Scope<Device> CreateNVRHIVulkanDevice(DeviceDescription description, const DeviceCapabilities& capabilities, nvrhi::IDevice* nativeDevice,
        nvrhi::vulkan::IDevice* completionDevice, NVRHIVulkanQueueTopology queueTopology, void* vulkanDevice,
        u32 graphicsTimestampValidBits, double timestampPeriodNanoseconds)
    {
        return nativeDevice && completionDevice
            ? CreateScope<VulkanDevice>(std::move(description), capabilities, nativeDevice, completionDevice, queueTopology,
                reinterpret_cast<VkDevice>(vulkanDevice), graphicsTimestampValidBits, timestampPeriodNanoseconds) : nullptr;
    }

    NVRHIVulkanTextureNativeHandles GetNVRHIVulkanTextureNativeHandles(Texture& texture)
    {
        auto* nativeTexture = dynamic_cast<VulkanTexture*>(&texture);
        if (!nativeTexture)
            return {};

        return {
            nativeTexture->Native()->getNativeObject(nvrhi::ObjectTypes::VK_Image),
            nativeTexture->Native()->getNativeView(nvrhi::ObjectTypes::VK_ImageView)
        };
    }
#else
    Scope<Device> CreateNVRHIVulkanDevice(DeviceDescription, const DeviceCapabilities&, nvrhi::IDevice*, nvrhi::vulkan::IDevice*, NVRHIVulkanQueueTopology, void*, u32, double) { return nullptr; }
    NVRHIVulkanTextureNativeHandles GetNVRHIVulkanTextureNativeHandles(Texture&) { return {}; }
#endif
}
