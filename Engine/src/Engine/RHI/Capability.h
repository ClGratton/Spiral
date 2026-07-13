#pragma once

#include "Engine/Core/Base.h"
#include "Engine/RHI/RHICommon.h"

#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Engine::RHI
{
    enum class DeviceFeature : u32
    {
        RayTracing,
        MeshShaders,
        WorkGraphs,
        NeuralShaders,
        Timestamps,
        TimelineSynchronization,
        BufferDeviceAddress,
        Count
    };

    enum class QualificationLevel : u32
    {
        None,
        Build,
        Bootstrap,
        Presentation,
        Scene,
        Production
    };

    enum class AdapterType : u32
    {
        Unknown,
        Discrete,
        Integrated,
        Virtual,
        Cpu,
        Software
    };

    enum class FormatUsage : u32
    {
        None = 0,
        Sampled = 1u << 0u,
        Storage = 1u << 1u,
        ColorAttachment = 1u << 2u,
        DepthStencil = 1u << 3u,
        CopySource = 1u << 4u,
        CopyDestination = 1u << 5u
    };

    constexpr FormatUsage operator|(FormatUsage left, FormatUsage right)
    {
        return static_cast<FormatUsage>(static_cast<u32>(left) | static_cast<u32>(right));
    }

    constexpr bool HasAllFormatUsages(FormatUsage value, FormatUsage required)
    {
        return (static_cast<u32>(value) & static_cast<u32>(required)) == static_cast<u32>(required);
    }

    struct CapabilityState
    {
        bool Advertised = false;
        bool Enabled = false;
        bool Implemented = false;
        bool Exercised = false;
        std::string Detail;

        bool IsUsable() const { return Advertised && Enabled && Implemented; }
        bool IsValid() const;
    };

    struct AdapterIdentity
    {
        std::string Name;
        std::string StableId;
        std::string DriverVersion;
        u32 VendorId = 0;
        u32 DeviceId = 0;
        AdapterType Type = AdapterType::Unknown;
        u64 DedicatedVideoMemoryBytes = 0;
    };

    struct QueueCapabilities
    {
        bool Graphics = false;
        bool Compute = false;
        bool Copy = false;
        bool Present = false;
        bool DedicatedCompute = false;
        bool DedicatedCopy = false;
    };

    struct FormatCapability
    {
        Format Value = Format::Unknown;
        FormatUsage Usages = FormatUsage::None;
        u32 SampleCountMask = 1;
    };

    struct FormatRequirement
    {
        Format Value = Format::Unknown;
        FormatUsage Usages = FormatUsage::None;
    };

    struct AdapterCandidate
    {
        Backend CandidateBackend = Backend::None;
        AdapterIdentity Identity;
        QueueCapabilities Queues;
        std::vector<FormatCapability> Formats;
        u32 ApiMajor = 0;
        u32 ApiMinor = 0;
        u32 MaximumTextureDimension2D = 0;
        bool TimelineSynchronization = false;
        std::int64_t PerformanceScore = 0;
    };

    struct CapabilityProfile
    {
        std::string Name;
        u32 MinimumApiMajor = 0;
        u32 MinimumApiMinor = 0;
        u32 MinimumTextureDimension2D = 0;
        bool RequireGraphics = true;
        bool RequirePresent = false;
        bool RequireCompute = false;
        bool RequireCopy = false;
        bool AllowGraphicsQueueFallback = true;
        bool RequireTimelineSynchronization = false;
        bool AllowSoftwareAdapter = true;
        std::vector<FormatRequirement> RequiredFormats;
    };

    struct AdapterEvaluation
    {
        size_t CandidateIndex = 0;
        bool Accepted = false;
        std::int64_t Score = std::numeric_limits<std::int64_t>::min();
        std::vector<std::string> RejectionReasons;
        std::vector<std::string> Fallbacks;
    };

    struct AdapterSelectionResult
    {
        size_t SelectedIndex = std::numeric_limits<size_t>::max();
        std::vector<AdapterEvaluation> Evaluations;

        bool HasSelection() const { return SelectedIndex != std::numeric_limits<size_t>::max(); }
    };

    CapabilityState MakeCapabilityState(bool advertised, bool enabled, bool implemented, bool exercised, std::string detail = {});
    AdapterSelectionResult EvaluateAdapterCandidates(
        const CapabilityProfile& profile,
        const std::vector<AdapterCandidate>& candidates,
        std::string_view preferredAdapter = {},
        bool requirePreferredAdapter = false);
    const char* ToString(DeviceFeature feature);
    const char* ToString(QualificationLevel level);
    const char* ToString(AdapterType type);
    std::string FormatUsagesToString(FormatUsage usages);
}
