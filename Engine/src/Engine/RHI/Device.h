#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Buffer.h"
#include "Engine/RHI/Capability.h"
#include "Engine/RHI/CommandList.h"
#include "Engine/RHI/Pipeline.h"
#include "Engine/RHI/Query.h"
#include "Engine/RHI/Shader.h"
#include "Engine/RHI/Swapchain.h"
#include "Engine/RHI/Texture.h"

#include <array>
#include <string_view>

namespace Engine::RHI
{
    struct DeviceCapabilities
    {
        Backend ActiveBackend = Backend::None;
        std::string ProfileName;
        AdapterIdentity Identity;
        QueueCapabilities Queues;
        QualificationLevel Qualification = QualificationLevel::None;
        std::array<CapabilityState, static_cast<size_t>(DeviceFeature::Count)> Features;
        std::vector<FormatCapability> Formats;
        std::vector<std::string> Fallbacks;

        const CapabilityState& GetFeature(DeviceFeature feature) const
        {
            return Features[static_cast<size_t>(feature)];
        }

        CapabilityState& GetFeature(DeviceFeature feature)
        {
            return Features[static_cast<size_t>(feature)];
        }
    };

    class Device
    {
    public:
        virtual ~Device() = default;

        virtual const DeviceDescription& GetDescription() const = 0;
        virtual const DeviceCapabilities& GetCapabilities() const = 0;

        virtual Scope<Buffer> CreateBuffer(const BufferDescription& description) = 0;
        virtual Scope<Texture> CreateTexture(const TextureDescription& description) = 0;
        virtual Scope<Shader> CreateShader(const ShaderDescription& description) = 0;
        virtual Scope<Pipeline> CreatePipeline(const PipelineDescription& description) = 0;
        virtual Scope<QueryPool> CreateQueryPool(const QueryPoolDescription& description) = 0;
        virtual Scope<CommandList> CreateCommandList(QueueType queueType, std::string_view debugName) = 0;
        virtual bool UploadBuffer(Buffer& destination, const void* sourceData, u64 sizeBytes, u64 destinationOffset = 0) = 0;
        virtual bool SubmitAndWait(CommandList& commandList) = 0;

        virtual void WaitIdle() = 0;
    };
}
