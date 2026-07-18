#pragma once

#include <chrono>
#include <memory>

namespace Engine::Platform
{
    enum class DeadlineWaitPrimitive
    {
        PortableSteadyClock,
        WindowsHighResolutionWaitableTimer,
        WindowsHighResolutionTimerFallback
    };

    enum class DeadlineWaitFallbackReason
    {
        None,
        TimerUnavailable,
        CreationFailed,
        ArmFailed,
        WaitFailed
    };

    const char* ToString(DeadlineWaitPrimitive primitive);
    const char* ToString(DeadlineWaitFallbackReason reason);

    // This is observation of one intentional deadline wait, not a claim about
    // display timing or energy use. CPU/wall deltas are a bounded cost proxy.
    struct DeadlineWaitTelemetry
    {
        DeadlineWaitPrimitive Primitive = DeadlineWaitPrimitive::PortableSteadyClock;
        DeadlineWaitFallbackReason FallbackReason = DeadlineWaitFallbackReason::None;
        bool FellBack = false;
        double TimerWaitMilliseconds = 0.0;
        double PortableWaitMilliseconds = 0.0;
        double ActiveTailMilliseconds = 0.0;
        double ActiveTailBudgetMilliseconds = 0.0;
        double CpuTimeMilliseconds = 0.0;
        double WallTimeMilliseconds = 0.0;
    };

    // This narrow seam exists solely to exercise timer failure/early-wake paths
    // deterministically. Production uses the private native implementation.
    class DeadlineWaiterPlatform
    {
    public:
        virtual ~DeadlineWaiterPlatform() = default;
        virtual bool SupportsHighResolutionTimer() const = 0;
        virtual void* CreateHighResolutionTimer() = 0;
        virtual void CloseTimer(void* timer) = 0;
        virtual bool ArmTimer(void* timer, std::chrono::steady_clock::duration relativeDelay) = 0;
        virtual bool WaitTimer(void* timer) = 0;
        virtual std::chrono::steady_clock::time_point Now() = 0;
        virtual double ProcessCpuMilliseconds() = 0;
        virtual void PortableWaitUntil(std::chrono::steady_clock::time_point deadline) = 0;
        virtual void ActiveTailUntil(std::chrono::steady_clock::time_point deadline) = 0;
    };

    // Owns the platform primitive and its lifetime. The caller supplies a
    // monotonic absolute deadline; no renderer policy lives at this boundary.
    class MonotonicDeadlineWaiter
    {
    public:
        MonotonicDeadlineWaiter();
        explicit MonotonicDeadlineWaiter(DeadlineWaiterPlatform& platform);
        ~MonotonicDeadlineWaiter();
        MonotonicDeadlineWaiter(const MonotonicDeadlineWaiter&) = delete;
        MonotonicDeadlineWaiter& operator=(const MonotonicDeadlineWaiter&) = delete;

        DeadlineWaitTelemetry WaitUntil(std::chrono::steady_clock::time_point deadline);

    private:
        std::unique_ptr<DeadlineWaiterPlatform> m_OwnedPlatform;
        DeadlineWaiterPlatform* m_Platform = nullptr;
        void* m_WindowsTimer = nullptr;
        DeadlineWaitFallbackReason m_CreationFailure = DeadlineWaitFallbackReason::None;
    };
}
