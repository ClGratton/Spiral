#pragma once

#include "Engine/Platform/MonotonicDeadlineWaiter.h"

#include <cmath>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

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

    // SubmissionGate is retained for explicit benchmark/smoke CLI conditions.
    // It is not a product presentation setting; product/game Smooth Frametime
    // always resolves to InterFrame.
    enum class SmoothFrametimeCandidate
    {
        InterFrame,
        SubmissionGate
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
        SmoothFrametimeCandidate Candidate = SmoothFrametimeCandidate::InterFrame;
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

    inline const char* ToString(SmoothFrametimeCandidate candidate)
    {
        switch (candidate)
        {
            case SmoothFrametimeCandidate::InterFrame: return "InterFrame";
            case SmoothFrametimeCandidate::SubmissionGate: return "SubmissionGate";
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
        {
            resolved.SmoothTargetFramesPerSecond = projectPolicy.SmoothTargetFramesPerSecond;
            resolved.Candidate = SmoothFrametimeCandidate::InterFrame;
            resolved.Behavior = "inter-frame";
        }
        else
            resolved.Behavior = "no-intentional-wait";
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
            ResolvedFramePacingPolicy resolved = ResolveFramePacingPolicy(projectPolicy, m_RuntimeOverride);
            return resolved;
        }

    private:
        FramePacingOverride m_RuntimeOverride = FramePacingOverride::InheritProject;
    };

    // A deliberately small, backend-neutral deadline state machine. The system
    // clock delegates OS waiting to Platform; tests inject this interface and never
    // sleep. A late frame advances from its observed release time, never drops
    // work or fabricates a cadence sample.
    class FramePacingClock
    {
    public:
        virtual ~FramePacingClock() = default;
        virtual double NowMilliseconds() = 0;
        virtual void WaitUntilMilliseconds(double deadlineMilliseconds) = 0;
        virtual Platform::DeadlineWaitTelemetry LastWaitTelemetry() const { return {}; }
    };

    class SystemFramePacingClock final : public FramePacingClock
    {
    public:
        double NowMilliseconds() override
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_Origin).count();
        }

        void WaitUntilMilliseconds(double deadlineMilliseconds) override
        {
            m_LastTelemetry = m_Waiter.WaitUntil(m_Origin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double, std::milli>(deadlineMilliseconds)));
        }

        Platform::DeadlineWaitTelemetry LastWaitTelemetry() const override { return m_LastTelemetry; }

    private:
        std::chrono::steady_clock::time_point m_Origin = std::chrono::steady_clock::now();
        Platform::MonotonicDeadlineWaiter m_Waiter;
        Platform::DeadlineWaitTelemetry m_LastTelemetry;
    };

    struct FramePacingWaitResult
    {
        bool Applied = false;
        bool DeadlineMissed = false;
        double RequestedDeadlineMilliseconds = 0.0;
        double ActualReleaseMilliseconds = 0.0;
        double WaitMilliseconds = 0.0;
        Platform::DeadlineWaitTelemetry Telemetry;
    };

    class SmoothFrametimePacer
    {
    public:
        FramePacingWaitResult Apply(const ResolvedFramePacingPolicy& policy, SmoothFrametimeCandidate controlPoint, FramePacingClock& clock)
        {
            FramePacingWaitResult result;
            if (policy.EffectiveMode != FramePacingMode::SmoothFrametime || policy.Candidate != controlPoint
                || !policy.SmoothTargetFramesPerSecond || !IsValidSmoothTargetFramesPerSecond(*policy.SmoothTargetFramesPerSecond))
            {
                if (policy.EffectiveMode == FramePacingMode::Responsive)
                    Reset();
                return result;
            }

            const double now = clock.NowMilliseconds();
            const double cadence = 1000.0 / *policy.SmoothTargetFramesPerSecond;
            DeadlineState& state = controlPoint == SmoothFrametimeCandidate::InterFrame
                ? m_InterFrame : m_SubmissionGate;
            const bool candidateChanged = m_LastCandidate && *m_LastCandidate != policy.Candidate;
            const bool targetChanged = state.TargetFramesPerSecond && *state.TargetFramesPerSecond != *policy.SmoothTargetFramesPerSecond;
            if (candidateChanged || targetChanged)
                state.NextDeadlineMilliseconds.reset();
            m_LastCandidate = policy.Candidate;
            state.TargetFramesPerSecond = *policy.SmoothTargetFramesPerSecond;

            if (!state.NextDeadlineMilliseconds)
            {
                // A changed policy never reuses an old deadline. When this
                // control point has a valid release phase, retain it; otherwise
                // establish phase from the next valid observation.
                if (!state.LastReleaseMilliseconds)
                {
                    state.NextDeadlineMilliseconds = now + cadence;
                    result.ActualReleaseMilliseconds = now;
                    state.LastReleaseMilliseconds = now;
                    return result;
                }
                state.NextDeadlineMilliseconds = *state.LastReleaseMilliseconds + cadence;
            }

            result.RequestedDeadlineMilliseconds = *state.NextDeadlineMilliseconds;
            result.DeadlineMissed = now >= *state.NextDeadlineMilliseconds;
            if (!result.DeadlineMissed)
            {
                clock.WaitUntilMilliseconds(*state.NextDeadlineMilliseconds);
                result.Telemetry = clock.LastWaitTelemetry();
                const double released = clock.NowMilliseconds();
                result.Applied = true;
                result.ActualReleaseMilliseconds = released;
                result.WaitMilliseconds = released - now;
                state.LastReleaseMilliseconds = released;
                state.NextDeadlineMilliseconds = released + cadence;
            }
            else
            {
                result.ActualReleaseMilliseconds = now;
                state.LastReleaseMilliseconds = now;
                state.NextDeadlineMilliseconds = now + cadence;
            }
            return result;
        }

        void Reset()
        {
            m_InterFrame = {};
            m_SubmissionGate = {};
            m_LastCandidate.reset();
        }

    private:
        struct DeadlineState
        {
            std::optional<double> NextDeadlineMilliseconds;
            std::optional<double> LastReleaseMilliseconds;
            std::optional<double> TargetFramesPerSecond;
        };
        DeadlineState m_InterFrame;
        DeadlineState m_SubmissionGate;
        std::optional<SmoothFrametimeCandidate> m_LastCandidate;
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
        description += ", candidate=";
        description += ToString(policy.Candidate);
        description += ", behavior=";
        description += policy.Behavior;
        return description;
    }
}
