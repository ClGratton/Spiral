#include "Engine/RHI/Capability.h"

#include <algorithm>

namespace Engine::RHI
{
    namespace
    {
        const FormatCapability* FindFormat(const AdapterCandidate& candidate, Format format)
        {
            for (const FormatCapability& capability : candidate.Formats)
            {
                if (capability.Value == format)
                    return &capability;
            }
            return nullptr;
        }

        bool ApiMeetsMinimum(const AdapterCandidate& candidate, const CapabilityProfile& profile)
        {
            return candidate.ApiMajor > profile.MinimumApiMajor
                || (candidate.ApiMajor == profile.MinimumApiMajor && candidate.ApiMinor >= profile.MinimumApiMinor);
        }

        bool IsSoftware(AdapterType type)
        {
            return type == AdapterType::Cpu || type == AdapterType::Software;
        }

        bool IsPreferred(const AdapterCandidate& candidate, std::string_view preferredAdapter)
        {
            return !preferredAdapter.empty()
                && (candidate.Identity.Name == preferredAdapter || candidate.Identity.StableId == preferredAdapter);
        }

        bool IsStableTieBreakBetter(const AdapterCandidate& candidate, const AdapterCandidate& current)
        {
            if (candidate.Identity.Name != current.Identity.Name)
                return candidate.Identity.Name < current.Identity.Name;
            if (candidate.Identity.StableId != current.Identity.StableId)
                return candidate.Identity.StableId < current.Identity.StableId;
            if (candidate.Identity.VendorId != current.Identity.VendorId)
                return candidate.Identity.VendorId < current.Identity.VendorId;
            return candidate.Identity.DeviceId < current.Identity.DeviceId;
        }
    }

    bool CapabilityState::IsValid() const
    {
        return (!Enabled || Advertised)
            && (!Exercised || (Advertised && Enabled && Implemented));
    }

    bool CapabilityGroupState::IsValid() const
    {
        if (Group == CapabilityGroupId::Count || ProfileName.empty()
            || PreferredPath == CapabilityPath::None || PreferredPath == CapabilityPath::Count
            || SelectedPath == CapabilityPath::Count)
            return false;

        if (SelectedPath == CapabilityPath::None)
            return !Implemented && !Exercised && Qualification == QualificationLevel::None;

        if (!Implemented || (Exercised && Qualification == QualificationLevel::None))
            return false;

        return Qualification == QualificationLevel::None || Exercised;
    }

    CapabilityState MakeCapabilityState(bool advertised, bool enabled, bool implemented, bool exercised, std::string detail)
    {
        CapabilityState result;
        result.Advertised = advertised;
        result.Enabled = advertised && enabled;
        result.Implemented = implemented;
        result.Exercised = result.Enabled && implemented && exercised;
        result.Detail = std::move(detail);
        return result;
    }

    AdapterSelectionResult EvaluateAdapterCandidates(
        const CapabilityProfile& profile,
        const std::vector<AdapterCandidate>& candidates,
        std::string_view preferredAdapter,
        bool requirePreferredAdapter)
    {
        AdapterSelectionResult result;
        result.Evaluations.reserve(candidates.size());

        std::int64_t selectedScore = std::numeric_limits<std::int64_t>::min();
        for (size_t index = 0; index < candidates.size(); ++index)
        {
            const AdapterCandidate& candidate = candidates[index];
            AdapterEvaluation evaluation;
            evaluation.CandidateIndex = index;

            if (requirePreferredAdapter && !IsPreferred(candidate, preferredAdapter))
                evaluation.RejectionReasons.emplace_back("adapter does not match the strict preference");

            if (!ApiMeetsMinimum(candidate, profile))
                evaluation.RejectionReasons.emplace_back("API version is below the profile minimum");
            if (profile.RequireGraphics && !candidate.Queues.Graphics)
                evaluation.RejectionReasons.emplace_back("graphics queue is unavailable");
            if (profile.RequirePresent && !candidate.Queues.Present)
                evaluation.RejectionReasons.emplace_back("presentation support is unavailable");
            if (profile.RequireTimelineSynchronization && !candidate.TimelineSynchronization)
                evaluation.RejectionReasons.emplace_back("timeline synchronization is unavailable");
            if (profile.RequireDynamicRendering && !candidate.DynamicRendering)
                evaluation.RejectionReasons.emplace_back("Vulkan dynamic rendering is unavailable");
            if (profile.RequireSynchronization2 && !candidate.Synchronization2)
                evaluation.RejectionReasons.emplace_back("Vulkan synchronization2 is unavailable");
            if (profile.MinimumTextureDimension2D > candidate.MaximumTextureDimension2D)
                evaluation.RejectionReasons.emplace_back("maximum 2D texture dimension is below the profile minimum");
            if (!profile.AllowSoftwareAdapter && IsSoftware(candidate.Identity.Type))
                evaluation.RejectionReasons.emplace_back("software adapters are disabled by the profile");

            if (profile.RequireCompute && !candidate.Queues.Compute)
            {
                if (profile.AllowGraphicsQueueFallback && candidate.Queues.Graphics)
                    evaluation.Fallbacks.emplace_back("compute work aliases the graphics queue");
                else
                    evaluation.RejectionReasons.emplace_back("compute queue is unavailable");
            }
            if (profile.RequireCopy && !candidate.Queues.Copy)
            {
                if (profile.AllowGraphicsQueueFallback && candidate.Queues.Graphics)
                    evaluation.Fallbacks.emplace_back("copy work aliases the graphics queue");
                else
                    evaluation.RejectionReasons.emplace_back("copy queue is unavailable");
            }

            for (const FormatRequirement& requirement : profile.RequiredFormats)
            {
                const FormatCapability* format = FindFormat(candidate, requirement.Value);
                if (!format || !HasAllFormatUsages(format->Usages, requirement.Usages))
                    evaluation.RejectionReasons.emplace_back(std::string("required format usage is unavailable: ") + ToString(requirement.Value));
            }

            evaluation.Accepted = evaluation.RejectionReasons.empty();
            if (evaluation.Accepted)
            {
                evaluation.Score = candidate.PerformanceScore;
                if (!IsSoftware(candidate.Identity.Type))
                    evaluation.Score += 1000000000ll;
                if (IsPreferred(candidate, preferredAdapter))
                    evaluation.Score += 4000000000ll;

                if (!result.HasSelection() || evaluation.Score > selectedScore
                    || (evaluation.Score == selectedScore && IsStableTieBreakBetter(candidate, candidates[result.SelectedIndex])))
                {
                    result.SelectedIndex = index;
                    selectedScore = evaluation.Score;
                }
            }

            result.Evaluations.push_back(std::move(evaluation));
        }

        return result;
    }

    const char* ToString(DeviceFeature feature)
    {
        switch (feature)
        {
            case DeviceFeature::RayTracing: return "Ray Tracing";
            case DeviceFeature::MeshShaders: return "Mesh Shaders";
            case DeviceFeature::WorkGraphs: return "Work Graphs";
            case DeviceFeature::NeuralShaders: return "Neural Shaders";
            case DeviceFeature::Timestamps: return "Timestamps";
            case DeviceFeature::TimelineSynchronization: return "Timeline Synchronization";
            case DeviceFeature::DynamicRendering: return "Dynamic Rendering";
            case DeviceFeature::Synchronization2: return "Synchronization2";
            case DeviceFeature::BufferDeviceAddress: return "Buffer Device Address";
            case DeviceFeature::PlacedResources: return "Placed Resources";
            case DeviceFeature::AliasingBarriers: return "Aliasing Barriers";
            case DeviceFeature::Count: break;
        }
        return "Unknown";
    }

    const char* ToString(CapabilityGroupId group)
    {
        switch (group)
        {
            case CapabilityGroupId::Phase3FrameTimingV1: return "Phase3FrameTimingV1";
            case CapabilityGroupId::Phase3TransientResourcesV1: return "Phase3TransientResourcesV1";
            case CapabilityGroupId::Count: break;
        }
        return "Unknown";
    }

    const char* ToString(CapabilityPath path)
    {
        switch (path)
        {
            case CapabilityPath::None: return "None";
            case CapabilityPath::CpuSteadyClock: return "CpuSteadyClock";
            case CapabilityPath::GpuTimestamps: return "GpuTimestamps";
            case CapabilityPath::PlacedAliasedTransient: return "PlacedAliasedTransient";
            case CapabilityPath::NonAliasedGpuRetiredPool: return "NonAliasedGpuRetiredPool";
            case CapabilityPath::Count: break;
        }
        return "Unknown";
    }

    const char* ToString(QualificationLevel level)
    {
        switch (level)
        {
            case QualificationLevel::None: return "None";
            case QualificationLevel::Build: return "Build";
            case QualificationLevel::Bootstrap: return "Bootstrap";
            case QualificationLevel::Presentation: return "Presentation";
            case QualificationLevel::Scene: return "Scene";
            case QualificationLevel::Production: return "Production";
        }
        return "Unknown";
    }

    const char* ToString(AdapterType type)
    {
        switch (type)
        {
            case AdapterType::Unknown: return "Unknown";
            case AdapterType::Discrete: return "Discrete";
            case AdapterType::Integrated: return "Integrated";
            case AdapterType::Virtual: return "Virtual";
            case AdapterType::Cpu: return "CPU";
            case AdapterType::Software: return "Software";
        }
        return "Unknown";
    }

    std::string FormatUsagesToString(FormatUsage usages)
    {
        std::string result;
        const auto append = [&result](const char* name)
        {
            if (!result.empty())
                result += ", ";
            result += name;
        };

        if (HasAllFormatUsages(usages, FormatUsage::Sampled)) append("sampled");
        if (HasAllFormatUsages(usages, FormatUsage::Storage)) append("storage");
        if (HasAllFormatUsages(usages, FormatUsage::ColorAttachment)) append("color attachment");
        if (HasAllFormatUsages(usages, FormatUsage::DepthStencil)) append("depth/stencil");
        if (HasAllFormatUsages(usages, FormatUsage::CopySource)) append("copy source");
        if (HasAllFormatUsages(usages, FormatUsage::CopyDestination)) append("copy destination");
        return result.empty() ? "none" : result;
    }
}
