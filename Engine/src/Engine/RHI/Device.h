#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/Buffer.h"
#include "Engine/RHI/Capability.h"
#include "Engine/RHI/CommandList.h"
#include "Engine/RHI/CompletionToken.h"
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
        std::vector<CapabilityGroupState> CapabilityGroups;
        std::vector<FormatCapability> Formats;
        std::vector<std::string> Fallbacks;
        std::vector<AdapterCandidate> AdapterCandidates;
        AdapterSelectionResult AdapterSelection;

        const CapabilityState& GetFeature(DeviceFeature feature) const
        {
            return Features[static_cast<size_t>(feature)];
        }

        CapabilityState& GetFeature(DeviceFeature feature)
        {
            return Features[static_cast<size_t>(feature)];
        }

        const CapabilityGroupState* GetCapabilityGroup(CapabilityGroupId group) const
        {
            for (const CapabilityGroupState& state : CapabilityGroups)
            {
                if (state.Group == group)
                    return &state;
            }
            return nullptr;
        }

        CapabilityGroupState* GetCapabilityGroup(CapabilityGroupId group)
        {
            for (CapabilityGroupState& state : CapabilityGroups)
            {
                if (state.Group == group)
                    return &state;
            }
            return nullptr;
        }

        const AdapterCandidate* GetSelectedAdapter() const
        {
            return AdapterSelection.HasSelection() && AdapterSelection.SelectedIndex < AdapterCandidates.size()
                ? &AdapterCandidates[AdapterSelection.SelectedIndex]
                : nullptr;
        }

        const AdapterEvaluation* GetSelectedAdapterEvaluation() const
        {
            if (!AdapterSelection.HasSelection() || AdapterSelection.SelectedIndex >= AdapterCandidates.size())
                return nullptr;

            for (const AdapterEvaluation& evaluation : AdapterSelection.Evaluations)
            {
                if (evaluation.CandidateIndex == AdapterSelection.SelectedIndex)
                    return &evaluation;
            }
            return nullptr;
        }
    };

    struct QueueResolution
    {
        QueueType Requested = QueueType::Graphics;
        QueueType Effective = QueueType::Graphics;
        bool Independent = false;
    };

    class Device
    {
    public:
        virtual ~Device() = default;

        virtual const DeviceDescription& GetDescription() const = 0;
        virtual const DeviceCapabilities& GetCapabilities() const = 0;
        virtual QueueResolution ResolveQueue(QueueType requested) const { return { requested, QueueType::Graphics, requested == QueueType::Graphics }; }

        virtual Scope<Buffer> CreateBuffer(const BufferDescription& description) = 0;
        virtual Scope<Texture> CreateTexture(const TextureDescription& description) = 0;
        // Returns true only for a live wrapper created by this exact Engine::RHI
        // device instance. Null, foreign-backend, and cross-device resources fail.
        virtual bool OwnsResource(const Buffer* resource) const = 0;
        virtual bool OwnsResource(const Texture* resource) const = 0;
        // Observes only the last state accepted by this exact device for a live
        // wrapper. It never exposes native state values or adopts caller state.
        virtual bool QueryResourceState(const Buffer* resource, ResourceState& state) const = 0;
        virtual bool QueryResourceState(const Texture* resource, ResourceState& state) const = 0;
        virtual Scope<Shader> CreateShader(const ShaderDescription& description) = 0;
        virtual Scope<Pipeline> CreatePipeline(const PipelineDescription& description) = 0;
        virtual Scope<QueryPool> CreateQueryPool(const QueryPoolDescription& description) = 0;
        virtual Scope<CommandList> CreateCommandList(QueueType queueType, std::string_view debugName) = 0;
        virtual bool UploadBuffer(Buffer& destination, const void* sourceData, u64 sizeBytes, u64 destinationOffset = 0) = 0;
        virtual bool ReadbackTexture(Texture& source, TextureReadback& destination) = 0;
        // Returns after the queue accepts closed work. The opaque token is
        // subsequently owned by this Device for query/wait/reuse decisions.
        virtual CompletionToken Submit(CommandList& commandList) = 0;
        virtual CompletionToken Submit(CommandList& commandList, const std::vector<CompletionToken>& dependencies)
        {
            (void)commandList; (void)dependencies; return {};
        }
        virtual CompletionStatus QueryCompletion(const CompletionToken& token) = 0;
        // A finite timeout is required; false reports invalid/stale/cross-device
        // tokens, device failure, or an incomplete timeout.
        virtual bool WaitForCompletion(const CompletionToken& token, u32 timeoutMilliseconds) = 0;
        virtual bool SubmitAndWait(CommandList& commandList) = 0;

        virtual void WaitIdle() = 0;
    };
}
