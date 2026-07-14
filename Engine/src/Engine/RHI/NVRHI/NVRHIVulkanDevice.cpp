#include "Engine/RHI/NVRHI/NVRHIVulkanDevice.h"

#include "Engine/Core/Log.h"

#if defined(GE_HAS_NVRHI_VULKAN)
    #include <nvrhi/nvrhi.h>
    #include <cstring>
    #include <limits>
    #include <vector>
#endif

namespace Engine::RHI
{
#if defined(GE_HAS_NVRHI_VULKAN)
    namespace
    {
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

        class VulkanBuffer final : public Buffer
        {
        public:
            VulkanBuffer(BufferDescription description, nvrhi::BufferHandle buffer)
                : m_Description(std::move(description)), m_Buffer(std::move(buffer)) {}
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
            nvrhi::IBuffer* Native() const { return m_Buffer; }
        private:
            BufferDescription m_Description;
            nvrhi::BufferHandle m_Buffer;
            nvrhi::IDevice* m_Device = nullptr;
            bool m_Mapped = false;
        };

        class VulkanTexture final : public Texture
        {
        public:
            VulkanTexture(TextureDescription description, nvrhi::TextureHandle texture)
                : m_Description(std::move(description)), m_Texture(std::move(texture)) {}
            const TextureDescription& GetDescription() const override { return m_Description; }
            nvrhi::ITexture* Native() const { return m_Texture; }
        private:
            TextureDescription m_Description;
            nvrhi::TextureHandle m_Texture;
        };

        class VulkanCommandList final : public CommandList
        {
        public:
            VulkanCommandList(QueueType queueType, std::string name, nvrhi::CommandListHandle list)
                : m_QueueType(queueType), m_Name(std::move(name)), m_List(std::move(list)) {}
            QueueType GetQueueType() const override { return m_QueueType; }
            bool Begin() override
            {
                if (m_State == State::Recording || !m_List)
                    return false;
                m_List->open();
                m_DebugMarkerNames.clear();
                m_Color = nullptr;
                m_Depth = nullptr;
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
            bool BindViewportOutputs(Texture& color, Texture* depth) override { m_Color = dynamic_cast<VulkanTexture*>(&color); m_Depth = depth ? dynamic_cast<VulkanTexture*>(depth) : nullptr; return m_Color && (!depth || m_Depth); }
            bool ClearViewportOutputs(const ViewportClear& clear) override
            {
                if (m_State != State::Recording || !m_Color) return false;
                if (clear.ClearColor) m_List->clearTextureFloat(m_Color->Native(), nvrhi::AllSubresources, nvrhi::Color(clear.Color[0], clear.Color[1], clear.Color[2], clear.Color[3]));
                if (clear.ClearDepth && m_Depth) m_List->clearDepthStencilTexture(m_Depth->Native(), nvrhi::AllSubresources, true, clear.Depth, false, clear.Stencil);
                return true;
            }
            bool TransitionTexture(Texture& texture, ResourceState state) override
            {
                auto* native = dynamic_cast<VulkanTexture*>(&texture);
                if (m_State != State::Recording || !native) return false;
                m_List->setTextureState(native->Native(), nvrhi::AllSubresources, ConvertState(state)); m_List->commitBarriers(); return true;
            }
            void SetGraphicsPipeline(Pipeline&) override { Log::Error("Vulkan RHI graphics pipelines are deferred to the next Phase 3C item"); }
            void SetGraphicsConstantBuffer(u32, Buffer&) override { Log::Error("Vulkan RHI graphics bindings are deferred to the next Phase 3C item"); }
            void SetViewport(const Viewport&) override {}
            void SetScissorRect(const ScissorRect&) override {}
            void SetVertexBuffer(u32, Buffer&) override {}
            void SetIndexBuffer(Buffer&, IndexFormat) override {}
            bool CopyBuffer(Buffer&, u64, Buffer&, u64, u64) override { Log::Error("Vulkan RHI buffer copy recording is not implemented"); return false; }
            void DrawIndexed(u32, u32, u32, int, u32) override { Log::Error("Vulkan RHI indexed drawing is deferred to the next Phase 3C item"); }
            void ResetQueryPool(QueryPool&, u32, u32) override {}
            void WriteTimestamp(QueryPool&, u32) override {}
            void ResolveQueryPool(QueryPool&, u32, u32) override {}
            bool Ready() const { return m_State == State::Closed; }
            bool MarkSubmitted()
            {
                if (!Ready())
                    return false;
                m_State = State::Submitted;
                return true;
            }
            nvrhi::ICommandList* Native() const { return m_List; }
        private:
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
            VulkanTexture* m_Color = nullptr;
            VulkanTexture* m_Depth = nullptr;
            std::vector<std::string> m_DebugMarkerNames;
            State m_State = State::Ready;
        };

        class VulkanDevice final : public Device
        {
        public:
            VulkanDevice(DeviceDescription description, DeviceCapabilities capabilities, nvrhi::IDevice* device)
                : m_Description(std::move(description)), m_Capabilities(std::move(capabilities)), m_Device(device) {}
            const DeviceDescription& GetDescription() const override { return m_Description; }
            const DeviceCapabilities& GetCapabilities() const override { return m_Capabilities; }
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
                auto result = CreateScope<VulkanBuffer>(description, native); result->SetDevice(m_Device); return result;
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
                nvrhi::TextureHandle native = m_Device->createTexture(d); return native ? CreateScope<VulkanTexture>(description, native) : nullptr;
            }
            Scope<Shader> CreateShader(const ShaderDescription&) override { Log::Error("Vulkan RHI shaders are deferred to the next Phase 3C item"); return nullptr; }
            Scope<Pipeline> CreatePipeline(const PipelineDescription&) override { Log::Error("Vulkan RHI graphics pipelines are deferred to the next Phase 3C item"); return nullptr; }
            Scope<QueryPool> CreateQueryPool(const QueryPoolDescription&) override { Log::Error("Vulkan RHI query pools are not implemented"); return nullptr; }
            Scope<CommandList> CreateCommandList(QueueType type, std::string_view name) override
            {
                if (!m_Device || type != QueueType::Graphics) return nullptr;
                nvrhi::CommandListHandle list = m_Device->createCommandList(); return list ? CreateScope<VulkanCommandList>(type, std::string(name), list) : nullptr;
            }
            bool UploadBuffer(Buffer& destination, const void* data, u64 size, u64 offset) override
            {
                auto* buffer = dynamic_cast<VulkanBuffer*>(&destination); if (!buffer || !data || !size || offset > destination.GetDescription().SizeBytes || size > destination.GetDescription().SizeBytes - offset) return false;
                Scope<CommandList> list = CreateCommandList(QueueType::Graphics, "Vulkan RHI Buffer Upload"); if (!list || !list->Begin()) return false;
                static_cast<VulkanCommandList*>(list.get())->Native()->writeBuffer(buffer->Native(), data, size, offset); return list->End() && SubmitAndWait(*list);
            }
            bool ReadbackTexture(Texture& source, TextureReadback& out) override
            {
                auto* texture = dynamic_cast<VulkanTexture*>(&source); const auto& d = source.GetDescription(); if (!texture || d.TextureFormat != Format::R8G8B8A8Unorm || !HasTextureUsage(d.Usage, TextureUsage::CopySource)) return false;
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
            bool SubmitAndWait(CommandList& commandList) override
            {
                auto* list = dynamic_cast<VulkanCommandList*>(&commandList);
                if (!m_Device || !list || !list->MarkSubmitted())
                    return false;
                m_Device->executeCommandList(list->Native());
                const bool ok = m_Device->waitForIdle();
                m_Device->runGarbageCollection();
                return ok;
            }
            void WaitIdle() override { if (m_Device) { m_Device->waitForIdle(); m_Device->runGarbageCollection(); } }
        private:
            DeviceDescription m_Description;
            DeviceCapabilities m_Capabilities;
            nvrhi::IDevice* m_Device = nullptr;
        };
    }

    Scope<Device> CreateNVRHIVulkanDevice(DeviceDescription description, const DeviceCapabilities& capabilities, nvrhi::IDevice* nativeDevice)
    {
        return nativeDevice ? CreateScope<VulkanDevice>(std::move(description), capabilities, nativeDevice) : nullptr;
    }
#else
    Scope<Device> CreateNVRHIVulkanDevice(DeviceDescription, const DeviceCapabilities&, nvrhi::IDevice*) { return nullptr; }
#endif
}
