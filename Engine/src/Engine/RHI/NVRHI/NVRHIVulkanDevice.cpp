#include "Engine/RHI/NVRHI/NVRHIVulkanDevice.h"

#include "Engine/Core/Log.h"
#include "Engine/RHI/SubmissionDependency.h"

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

        nvrhi::ShaderType ConvertShaderStage(ShaderStage stage)
        {
            switch (stage)
            {
                case ShaderStage::Vertex: return nvrhi::ShaderType::Vertex;
                case ShaderStage::Pixel: return nvrhi::ShaderType::Pixel;
                default: return nvrhi::ShaderType::None;
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
            VulkanPipeline(PipelineDescription description, nvrhi::InputLayoutHandle inputLayout, nvrhi::BindingLayoutHandle bindings, nvrhi::ShaderHandle vertex, nvrhi::ShaderHandle pixel)
                : m_Description(std::move(description)), m_InputLayout(std::move(inputLayout)), m_Bindings(std::move(bindings)), m_Vertex(std::move(vertex)), m_Pixel(std::move(pixel)) {}
            const PipelineDescription& GetDescription() const override { return m_Description; }
            nvrhi::IInputLayout* InputLayout() const { return m_InputLayout; }
            nvrhi::IBindingLayout* Bindings() const { return m_Bindings; }
            nvrhi::IGraphicsPipeline* GetOrCreateNative(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& framebufferInfo)
            {
                if (m_NativePipeline && m_FramebufferInfo == framebufferInfo)
                    return m_NativePipeline;
                if (!device)
                    return nullptr;
                nvrhi::GraphicsPipelineDesc pipeline;
                pipeline.setPrimType(nvrhi::PrimitiveType::TriangleList).setInputLayout(m_InputLayout)
                    .setVertexShader(m_Vertex).setPixelShader(m_Pixel).addBindingLayout(m_Bindings);
                pipeline.renderState.rasterState.setCullNone().setFrontCounterClockwise(false);
                pipeline.renderState.depthStencilState.depthTestEnable = m_Description.DepthTestEnable;
                pipeline.renderState.depthStencilState.depthWriteEnable = m_Description.DepthWriteEnable;
                m_NativePipeline = device->createGraphicsPipeline(pipeline, framebufferInfo);
                if (m_NativePipeline)
                    m_FramebufferInfo = framebufferInfo;
                return m_NativePipeline;
            }
        private:
            PipelineDescription m_Description;
            nvrhi::InputLayoutHandle m_InputLayout;
            nvrhi::BindingLayoutHandle m_Bindings;
            nvrhi::ShaderHandle m_Vertex;
            nvrhi::ShaderHandle m_Pixel;
            nvrhi::GraphicsPipelineHandle m_NativePipeline;
            nvrhi::FramebufferInfo m_FramebufferInfo;
        };

        class VulkanCommandList final : public CommandList
        {
        public:
            VulkanCommandList(QueueType queueType, std::string name, nvrhi::CommandListHandle list, nvrhi::IDevice* device,
                std::function<CompletionStatus(const CompletionToken&)> queryCompletion, BufferOwnershipTracker* ownershipTracker,
                TextureOwnershipTracker* textureOwnershipTracker,
                std::function<QueueType(QueueType)> resolveQueue)
                : m_QueueType(queueType), m_Name(std::move(name)), m_List(std::move(list)), m_Device(device),
                m_QueryCompletion(std::move(queryCompletion)), m_OwnershipTracker(ownershipTracker), m_TextureOwnershipTracker(textureOwnershipTracker), m_ResolveQueue(std::move(resolveQueue)) {}
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
                m_OwnershipOperation.reset();
                m_UsedTextures.clear();
                m_TextureOwnershipOperation.reset();
                m_State = State::Recording;
                return true;
            }
            bool End() override
            {
                if (m_State != State::Recording || !m_DebugMarkerNames.empty())
                    return false;
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
                    || !m_TextureOwnershipTracker || !m_TextureOwnershipTracker->CanUse(&color)
                    || !m_TextureOwnershipTracker->CanUse(depth)) return false;
                nvrhi::FramebufferDesc framebuffer;
                framebuffer.addColorAttachment(m_Color->Native()).setDepthAttachment(m_Depth->Native());
                m_Framebuffer = m_Device->createFramebuffer(framebuffer);
                return m_Framebuffer != nullptr;
            }
            bool ClearViewportOutputs(const ViewportClear& clear) override
            {
                if (m_State != State::Recording || !m_Color || !m_TextureOwnershipTracker
                    || !m_TextureOwnershipTracker->CanUse(m_Color)
                    || (m_Depth && !m_TextureOwnershipTracker->CanUse(m_Depth))) return false;
                if (clear.ClearColor) m_List->clearTextureFloat(m_Color->Native(), nvrhi::AllSubresources, nvrhi::Color(clear.Color[0], clear.Color[1], clear.Color[2], clear.Color[3]));
                if (clear.ClearDepth && m_Depth) m_List->clearDepthStencilTexture(m_Depth->Native(), nvrhi::AllSubresources, true, clear.Depth, false, clear.Stencil);
                return true;
            }
            bool TransitionTexture(Texture& texture, ResourceState state) override
            {
                auto* native = dynamic_cast<VulkanTexture*>(&texture);
                if (m_State != State::Recording || !native || state == ResourceState::Unknown
                    || (m_TextureOwnershipTracker && !m_AllowPendingTexture && !m_TextureOwnershipTracker->CanUse(&texture))) return false;
                if (GetTextureState(*native) == state) return true;
                m_List->setTextureState(native->Native(), nvrhi::AllSubresources, ConvertState(state));
                m_List->commitBarriers();
                StageTextureState(*native, state);
                return true;
            }
            bool ReleaseTextureOwnership(const TextureOwnershipRelease& release) override
            {
                if (m_State != State::Recording || !m_TextureOwnershipTracker || m_TextureOwnershipOperation
                    || std::find(m_UsedTextures.begin(), m_UsedTextures.end(), release.Resource) != m_UsedTextures.end()) return false;
                RecordedTextureOwnershipOperation operation;
                if (!m_TextureOwnershipTracker->RecordRelease(release, m_ResolveQueue(m_QueueType), m_ResolveQueue(release.SourceQueue),
                    m_ResolveQueue(release.DestinationQueue), operation)) return false;
                // Current Vulkan resolves all queues to Graphics. This remains a
                // rejection seam until multi-queue/family translation is admitted.
                return false;
            }
            bool AcquireTextureOwnership(const TextureOwnershipAcquire& acquire) override
            {
                if (m_State != State::Recording || !m_TextureOwnershipTracker || m_TextureOwnershipOperation
                    || std::find(m_UsedTextures.begin(), m_UsedTextures.end(), acquire.Resource) != m_UsedTextures.end()) return false;
                RecordedTextureOwnershipOperation operation;
                if (!m_TextureOwnershipTracker->RecordAcquire(acquire, m_ResolveQueue(m_QueueType), m_ResolveQueue(acquire.SourceQueue),
                    m_ResolveQueue(acquire.DestinationQueue), operation)) return false;
                return false;
            }
            bool TransitionBuffer(Buffer& buffer, ResourceState state) override
            {
                auto* native = dynamic_cast<VulkanBuffer*>(&buffer);
                if (m_State != State::Recording || !native
                    || !m_OwnershipTracker || !m_OwnershipTracker->CanUse(&buffer)
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
            bool ReleaseBufferOwnership(const BufferOwnershipRelease& release) override
            {
                if (m_State != State::Recording || !m_OwnershipTracker || m_OwnershipOperation
                    || std::find(m_UsedBuffers.begin(), m_UsedBuffers.end(), release.Resource) != m_UsedBuffers.end())
                    return false;
                RecordedBufferOwnershipOperation operation;
                if (!m_OwnershipTracker->RecordRelease(release, m_ResolveQueue(m_QueueType), m_ResolveQueue(release.SourceQueue),
                    m_ResolveQueue(release.DestinationQueue), operation))
                    return false;
                m_OwnershipOperation = operation;
                return true;
            }
            bool AcquireBufferOwnership(const BufferOwnershipAcquire& acquire) override
            {
                if (m_State != State::Recording || !m_OwnershipTracker || m_OwnershipOperation
                    || std::find(m_UsedBuffers.begin(), m_UsedBuffers.end(), acquire.Resource) != m_UsedBuffers.end())
                    return false;
                RecordedBufferOwnershipOperation operation;
                if (!m_OwnershipTracker->RecordAcquire(acquire, m_ResolveQueue(m_QueueType), m_ResolveQueue(acquire.SourceQueue),
                    m_ResolveQueue(acquire.DestinationQueue), operation))
                    return false;
                // Current Vulkan resolves every request to Graphics, so the
                // tracker rejects before this future translation point.
                return false;
            }
            void SetGraphicsPipeline(Pipeline& pipeline) override { m_Pipeline = dynamic_cast<VulkanPipeline*>(&pipeline); }
            void SetGraphicsConstantBuffer(u32 rootParameterIndex, Buffer& buffer) override
            {
                auto* native = dynamic_cast<VulkanBuffer*>(&buffer);
                if (!m_Pipeline || !native || rootParameterIndex != 0) { m_BindingSet = nullptr; return; }
                nvrhi::BindingSetDesc bindings;
                bindings.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(rootParameterIndex, native->Native()));
                m_BindingSet = m_Device->createBindingSet(bindings, m_Pipeline->Bindings());
            }
            void SetViewport(const Viewport& viewport) override { m_Viewport = viewport; }
            void SetScissorRect(const ScissorRect& rect) override { m_Scissor = rect; }
            void SetVertexBuffer(u32 slot, Buffer& buffer) override { if (slot == 0) m_Vertex = dynamic_cast<VulkanBuffer*>(&buffer); }
            void SetIndexBuffer(Buffer& buffer, IndexFormat format) override { m_Index = dynamic_cast<VulkanBuffer*>(&buffer); m_IndexFormat = format; }
            bool CopyBuffer(Buffer& destination, u64, Buffer& source, u64, u64) override
            {
                if (!m_OwnershipTracker || !m_OwnershipTracker->CanUse(&destination) || !m_OwnershipTracker->CanUse(&source))
                    return false;
                Log::Error("Vulkan RHI buffer copy recording is not implemented"); return false;
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
                m_List->setGraphicsState(state);
                m_List->drawIndexed(nvrhi::DrawArguments().setVertexCount(indexCount).setInstanceCount(instanceCount).setStartIndexLocation(startIndex).setStartVertexLocation(0).setStartInstanceLocation(startInstance));
            }
            void ResetQueryPool(QueryPool&, u32, u32) override {}
            void WriteTimestamp(QueryPool&, u32) override {}
            void ResolveQueryPool(QueryPool&, u32, u32) override {}
            bool Ready() const { return m_State == State::Closed; }
            bool MarkSubmitted(const CompletionToken& token)
            {
                if (!Ready() || !token.IsValid())
                    return false;
                m_LastSubmission = token;
                CommitTrackedStates();
                m_State = State::Submitted;
                return true;
            }
            nvrhi::ICommandList* Native() const { return m_List; }
            const std::optional<RecordedBufferOwnershipOperation>& GetOwnershipOperation() const { return m_OwnershipOperation; }
            const std::optional<RecordedTextureOwnershipOperation>& GetTextureOwnershipOperation() const { return m_TextureOwnershipOperation; }
        private:
            struct TextureState { VulkanTexture* Resource = nullptr; ResourceState State = ResourceState::Common; };
            struct BufferState { VulkanBuffer* Resource = nullptr; ResourceState State = ResourceState::Common; };

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

            void StageTextureState(VulkanTexture& resource, ResourceState state)
            {
                for (TextureState& pending : m_TextureStates)
                    if (pending.Resource == &resource) { pending.State = state; return; }
                m_TextureStates.push_back({ &resource, state });
                m_UsedTextures.push_back(&resource);
            }

            void StageBufferState(VulkanBuffer& resource, ResourceState state)
            {
                for (BufferState& pending : m_BufferStates)
                    if (pending.Resource == &resource) { pending.State = state; return; }
                m_BufferStates.push_back({ &resource, state });
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
            BufferOwnershipTracker* m_OwnershipTracker = nullptr;
            TextureOwnershipTracker* m_TextureOwnershipTracker = nullptr;
            std::function<QueueType(QueueType)> m_ResolveQueue;
            CompletionToken m_LastSubmission;
            std::vector<TextureState> m_TextureStates;
            std::vector<BufferState> m_BufferStates;
            std::vector<Buffer*> m_UsedBuffers;
            std::vector<Texture*> m_UsedTextures;
            std::optional<RecordedBufferOwnershipOperation> m_OwnershipOperation;
            std::optional<RecordedTextureOwnershipOperation> m_TextureOwnershipOperation;
            bool m_AllowPendingTexture = false;
            State m_State = State::Ready;
        };

        class VulkanDevice final : public Device
        {
        public:
            VulkanDevice(DeviceDescription description, DeviceCapabilities capabilities, nvrhi::IDevice* device,
                nvrhi::vulkan::IDevice* completionDevice)
                : m_Description(std::move(description)), m_Capabilities(std::move(capabilities)), m_Device(device)
                , m_CompletionDevice(completionDevice)
                , m_CompletionDeviceId(s_NextCompletionDeviceId.fetch_add(1))
                , m_ResourceOwnerId(s_NextResourceOwnerId.fetch_add(1)) {}
            const DeviceDescription& GetDescription() const override { return m_Description; }
            const DeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
            QueueResolution ResolveQueue(QueueType requested) const override
            {
                return { requested, QueueType::Graphics, requested == QueueType::Graphics };
            }
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
                return OwnsResource(&resource) && m_BufferOwnership.Recover(resource, releaseToken, QueryCompletion(releaseToken));
            }
            bool QueryResourceState(const Texture* resource, ResourceState& state) const override
            {
                const auto* texture = dynamic_cast<const VulkanTexture*>(resource);
                return texture && OwnsResource(resource) && m_TextureOwnership.QueryState(resource, state);
            }
            bool QueryTextureQueueOwner(const Texture* resource, QueueType& owner) const override { return OwnsResource(resource) && m_TextureOwnership.QueryOwner(resource, owner); }
            bool HasPendingTextureOwnershipTransfer(const Texture* resource) const override { return OwnsResource(resource) && m_TextureOwnership.HasPending(resource); }
            bool CanDestroyTexture(const Texture* resource) const override { return OwnsResource(resource) && m_TextureOwnership.CanDestroy(resource); }
            bool RecoverAbandonedTextureOwnershipTransfer(Texture& resource, const CompletionToken& token) override { return OwnsResource(&resource) && m_TextureOwnership.Recover(resource, token, QueryCompletion(token)); }
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
                auto* vertex = dynamic_cast<VulkanShader*>(description.VertexShader);
                auto* pixel = dynamic_cast<VulkanShader*>(description.PixelShader);
                if (!m_Device || description.Type != PipelineType::Graphics || !vertex || !pixel || description.VertexInputs.size() != 3 || description.ConstantBufferBindings.size() != 1
                    || description.ConstantBufferBindings[0].ShaderRegister != 0 || description.ConstantBufferBindings[0].RegisterSpace != 0) return nullptr;
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
                return inputLayout && bindings ? CreateScope<VulkanPipeline>(description, inputLayout, bindings, vertex->NativeHandle(), pixel->NativeHandle()) : nullptr;
            }
            Scope<QueryPool> CreateQueryPool(const QueryPoolDescription&) override { Log::Error("Vulkan RHI query pools are not implemented"); return nullptr; }
            Scope<CommandList> CreateCommandList(QueueType type, std::string_view name) override
            {
                if (!m_Device) return nullptr;
                nvrhi::CommandListHandle list = m_Device->createCommandList(); return list ? CreateScope<VulkanCommandList>(type, std::string(name), list, m_Device,
                    [this](const CompletionToken& token) { return QueryCompletion(token); }, &m_BufferOwnership, &m_TextureOwnership,
                    [this](QueueType requested) { return ResolveQueue(requested).Effective; }) : nullptr;
            }
            bool UploadBuffer(Buffer& destination, const void* data, u64 size, u64 offset) override
            {
                auto* buffer = dynamic_cast<VulkanBuffer*>(&destination); if (!buffer || !OwnsResource(&destination)
                    || !m_BufferOwnership.CanUse(&destination) || !data || !size || offset > destination.GetDescription().SizeBytes
                    || size > destination.GetDescription().SizeBytes - offset) return false;
                Scope<CommandList> list = CreateCommandList(QueueType::Graphics, "Vulkan RHI Buffer Upload"); if (!list || !list->Begin()) return false;
                static_cast<VulkanCommandList*>(list.get())->Native()->writeBuffer(buffer->Native(), data, size, offset); return list->End() && SubmitAndWait(*list);
            }
            bool ReadbackTexture(Texture& source, TextureReadback& out) override
            {
                auto* texture = dynamic_cast<VulkanTexture*>(&source); const auto& d = source.GetDescription(); if (!texture || !OwnsResource(&source) || !m_TextureOwnership.CanUse(&source) || d.TextureFormat != Format::R8G8B8A8Unorm || !HasTextureUsage(d.Usage, TextureUsage::CopySource)) return false;
                nvrhi::StagingTextureHandle staging = m_Device->createStagingTexture(texture->Native()->getDesc(), nvrhi::CpuAccessMode::Read); if (!staging) return false;
                Scope<CommandList> list = CreateCommandList(QueueType::Graphics, "Vulkan RHI Texture Readback"); if (!list || !list->Begin()) return false;
                static_cast<VulkanCommandList*>(list.get())->Native()->copyTexture(staging, nvrhi::TextureSlice(), texture->Native(), nvrhi::TextureSlice()); if (!list->End() || !SubmitAndWait(*list)) return false;
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
                if (!m_Device || !m_CompletionDevice || !list || !list->Ready())
                    return {};
                const SubmissionDependencyError dependencyError = ValidateSubmissionDependencies(
                    m_CompletionDeviceId, m_NextCompletionSubmissionId, dependencies,
                    [this](const CompletionToken& dependency) { return FindCompletionEntry(dependency) != m_CompletionEntries.end(); });
                if (dependencyError != SubmissionDependencyError::None)
                    return {};
                if (list->GetOwnershipOperation() && !m_BufferOwnership.ValidateSubmission(*list->GetOwnershipOperation(), dependencies))
                    return {};
                if (list->GetTextureOwnershipOperation() && !m_TextureOwnership.ValidateSubmission(*list->GetTextureOwnershipOperation(), dependencies))
                    return {};
                const u64 nativeSubmissionId = m_Device->executeCommandList(list->Native());
                if (nativeSubmissionId == 0)
                    return {};
                const CompletionToken token { m_CompletionDeviceId, m_NextCompletionSubmissionId++ };
                m_CompletionEntries.emplace(token.SubmissionId, nativeSubmissionId);
                if (!list->MarkSubmitted(token))
                    return {};
                if (const auto& ownership = list->GetOwnershipOperation())
                {
                    const bool published = ownership->Type == BufferOwnershipOperationType::Release
                        ? m_BufferOwnership.PublishRelease(*ownership, token) : m_BufferOwnership.PublishAcquire(*ownership);
                    if (!published) return {};
                }
                if (const auto& ownership = list->GetTextureOwnershipOperation())
                {
                    const bool published = ownership->Type == TextureOwnershipOperationType::Release
                        ? m_TextureOwnership.PublishRelease(*ownership, token) : m_TextureOwnership.PublishAcquire(*ownership);
                    if (!published) return {};
                }
                return token;
            }
            CompletionStatus QueryCompletion(const CompletionToken& token) override
            {
                const auto found = FindCompletionEntry(token);
                if (found == m_CompletionEntries.end())
                    return CompletionStatus::Invalid;
                if (!m_CompletionDevice)
                    return CompletionStatus::Failed;
                return m_CompletionDevice->queueGetCompletedInstance(nvrhi::CommandQueue::Graphics) >= found->second
                    ? CompletionStatus::Complete : CompletionStatus::Incomplete;
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
            u64 m_CompletionDeviceId = 0;
            const u64 m_ResourceOwnerId;
            u64 m_NextCompletionSubmissionId = 1;
            std::unordered_map<u64, u64> m_CompletionEntries;
            BufferOwnershipTracker m_BufferOwnership;
            TextureOwnershipTracker m_TextureOwnership;

            std::unordered_map<u64, u64>::const_iterator FindCompletionEntry(const CompletionToken& token) const
            {
                if (!token.IsValid() || token.DeviceId != m_CompletionDeviceId)
                    return m_CompletionEntries.end();
                return m_CompletionEntries.find(token.SubmissionId);
            }
        };
    }

    Scope<Device> CreateNVRHIVulkanDevice(DeviceDescription description, const DeviceCapabilities& capabilities, nvrhi::IDevice* nativeDevice,
        nvrhi::vulkan::IDevice* completionDevice)
    {
        return nativeDevice && completionDevice
            ? CreateScope<VulkanDevice>(std::move(description), capabilities, nativeDevice, completionDevice) : nullptr;
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
    Scope<Device> CreateNVRHIVulkanDevice(DeviceDescription, const DeviceCapabilities&, nvrhi::IDevice*, nvrhi::vulkan::IDevice*) { return nullptr; }
    NVRHIVulkanTextureNativeHandles GetNVRHIVulkanTextureNativeHandles(Texture&) { return {}; }
#endif
}
