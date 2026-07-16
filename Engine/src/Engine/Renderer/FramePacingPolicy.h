#pragma once

#include <cmath>
#include <optional>
#include <string>

namespace Engine
{
    // This is configuration intent only. It deliberately has no sleep, wait,
    // present, submission, or frame-replacement behavior.
    enum class FramePacingMode
    {
        Responsive,
        SmoothFrametime
    };

    enum class FramePacingOverride
    {
        InheritProject,
        Responsive,
        SmoothFrametime
    };

    struct FramePacingPolicy
    {
        FramePacingMode Mode = FramePacingMode::Responsive;
        double SmoothTargetFramesPerSecond = 60.0;
    };

    struct ResolvedFramePacingPolicy
    {
        FramePacingMode ProjectMode = FramePacingMode::Responsive;
        FramePacingOverride RuntimeOverride = FramePacingOverride::InheritProject;
        FramePacingMode EffectiveMode = FramePacingMode::Responsive;
        std::optional<double> SmoothTargetFramesPerSecond;
        const char* Behavior = "policy-only";
    };

    constexpr double kMinimumSmoothTargetFramesPerSecond = 1.0;
    constexpr double kMaximumSmoothTargetFramesPerSecond = 1000.0;

    inline const char* ToString(FramePacingMode mode)
    {
        switch (mode)
        {
            case FramePacingMode::Responsive: return "Responsive";
            case FramePacingMode::SmoothFrametime: return "Smooth Frametime";
        }

        return "Unknown";
    }

    inline const char* ToString(FramePacingOverride overrideMode)
    {
        switch (overrideMode)
        {
            case FramePacingOverride::InheritProject: return "Inherit Project";
            case FramePacingOverride::Responsive: return "Responsive";
            case FramePacingOverride::SmoothFrametime: return "Smooth Frametime";
        }

        return "Unknown";
    }

    inline bool IsValidSmoothTargetFramesPerSecond(double target)
    {
        return std::isfinite(target)
            && target >= kMinimumSmoothTargetFramesPerSecond
            && target <= kMaximumSmoothTargetFramesPerSecond;
    }

    inline bool IsValidFramePacingPolicy(const FramePacingPolicy& policy)
    {
        return policy.Mode == FramePacingMode::Responsive
            || (policy.Mode == FramePacingMode::SmoothFrametime
                && IsValidSmoothTargetFramesPerSecond(policy.SmoothTargetFramesPerSecond));
    }

    inline ResolvedFramePacingPolicy ResolveFramePacingPolicy(
        const FramePacingPolicy& projectPolicy,
        FramePacingOverride runtimeOverride = FramePacingOverride::InheritProject)
    {
        ResolvedFramePacingPolicy resolved;
        resolved.ProjectMode = projectPolicy.Mode;
        resolved.RuntimeOverride = runtimeOverride;
        resolved.EffectiveMode = runtimeOverride == FramePacingOverride::InheritProject
            ? projectPolicy.Mode
            : (runtimeOverride == FramePacingOverride::Responsive
                ? FramePacingMode::Responsive
                : FramePacingMode::SmoothFrametime);
        if (resolved.EffectiveMode == FramePacingMode::SmoothFrametime)
            resolved.SmoothTargetFramesPerSecond = projectPolicy.SmoothTargetFramesPerSecond;
        return resolved;
    }

    class GameFramePacingSettings
    {
    public:
        FramePacingOverride GetRuntimeOverride() const { return m_RuntimeOverride; }

        bool SetRuntimeOverride(FramePacingOverride runtimeOverride, const FramePacingPolicy& projectPolicy)
        {
            if (runtimeOverride == FramePacingOverride::SmoothFrametime
                && !IsValidSmoothTargetFramesPerSecond(projectPolicy.SmoothTargetFramesPerSecond))
                return false;

            m_RuntimeOverride = runtimeOverride;
            return true;
        }

        void ClearRuntimeOverride() { m_RuntimeOverride = FramePacingOverride::InheritProject; }

        ResolvedFramePacingPolicy Resolve(const FramePacingPolicy& projectPolicy) const
        {
            return ResolveFramePacingPolicy(projectPolicy, m_RuntimeOverride);
        }

    private:
        FramePacingOverride m_RuntimeOverride = FramePacingOverride::InheritProject;
    };

    inline std::string DescribeFramePacingPolicy(const ResolvedFramePacingPolicy& policy)
    {
        std::string description = "project=";
        description += ToString(policy.ProjectMode);
        description += ", runtimeOverride=";
        description += ToString(policy.RuntimeOverride);
        description += ", effective=";
        description += ToString(policy.EffectiveMode);
        if (policy.SmoothTargetFramesPerSecond)
        {
            description += ", targetFps=" + std::to_string(*policy.SmoothTargetFramesPerSecond);
        }
        description += ", behavior=";
        description += policy.Behavior;
        return description;
    }
}
