#include "Engine/Platform/MonotonicDeadlineWaiter.h"

#include <algorithm>
#include <ctime>
#include <thread>

#if defined(GE_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

namespace Engine::Platform
{
    namespace
    {
        constexpr auto kMaximumActiveTail = std::chrono::microseconds(500);

        double Milliseconds(std::chrono::steady_clock::duration duration)
        {
            return std::chrono::duration<double, std::milli>(duration).count();
        }

        class NativeDeadlineWaiterPlatform final : public DeadlineWaiterPlatform
        {
        public:
            bool SupportsHighResolutionTimer() const override
            {
#if defined(GE_PLATFORM_WINDOWS)
                return true;
#else
                return false;
#endif
            }

            void* CreateHighResolutionTimer() override
            {
#if defined(GE_PLATFORM_WINDOWS)
                return CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                    TIMER_MODIFY_STATE | SYNCHRONIZE);
#else
                return nullptr;
#endif
            }

            void CloseTimer(void* timer) override
            {
#if defined(GE_PLATFORM_WINDOWS)
                if (timer)
                    CloseHandle(static_cast<HANDLE>(timer));
#else
                (void)timer;
#endif
            }

            bool ArmTimer(void* timer, std::chrono::steady_clock::duration relativeDelay) override
            {
#if defined(GE_PLATFORM_WINDOWS)
                const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(relativeDelay).count();
                LARGE_INTEGER dueTime {};
                dueTime.QuadPart = -std::max<LONGLONG>(1, nanoseconds / 100);
                return SetWaitableTimer(static_cast<HANDLE>(timer), &dueTime, 0, nullptr, nullptr, FALSE) != FALSE;
#else
                (void)timer;
                (void)relativeDelay;
                return false;
#endif
            }

            bool WaitTimer(void* timer) override
            {
#if defined(GE_PLATFORM_WINDOWS)
                return WaitForSingleObject(static_cast<HANDLE>(timer), INFINITE) == WAIT_OBJECT_0;
#else
                (void)timer;
                return false;
#endif
            }

            std::chrono::steady_clock::time_point Now() override { return std::chrono::steady_clock::now(); }

            double ProcessCpuMilliseconds() override
            {
                return 1000.0 * static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
            }

            void PortableWaitUntil(std::chrono::steady_clock::time_point deadline) override
            {
                std::this_thread::sleep_until(deadline);
            }

            void ActiveTailUntil(std::chrono::steady_clock::time_point deadline) override
            {
                while (Now() < deadline)
                    std::this_thread::yield();
            }
        };
    }

    const char* ToString(DeadlineWaitPrimitive primitive)
    {
        switch (primitive)
        {
            case DeadlineWaitPrimitive::PortableSteadyClock: return "PortableSteadyClock";
            case DeadlineWaitPrimitive::WindowsHighResolutionWaitableTimer: return "WindowsHighResolutionWaitableTimer";
            case DeadlineWaitPrimitive::WindowsHighResolutionTimerFallback: return "WindowsHighResolutionTimerFallback";
        }
        return "Unknown";
    }

    const char* ToString(DeadlineWaitFallbackReason reason)
    {
        switch (reason)
        {
            case DeadlineWaitFallbackReason::None: return "None";
            case DeadlineWaitFallbackReason::TimerUnavailable: return "TimerUnavailable";
            case DeadlineWaitFallbackReason::CreationFailed: return "CreationFailed";
            case DeadlineWaitFallbackReason::ArmFailed: return "ArmFailed";
            case DeadlineWaitFallbackReason::WaitFailed: return "WaitFailed";
        }
        return "Unknown";
    }

    MonotonicDeadlineWaiter::MonotonicDeadlineWaiter()
        : m_OwnedPlatform(std::make_unique<NativeDeadlineWaiterPlatform>())
        , m_Platform(m_OwnedPlatform.get())
    {
        if (m_Platform->SupportsHighResolutionTimer())
        {
            m_WindowsTimer = m_Platform->CreateHighResolutionTimer();
            if (!m_WindowsTimer)
                m_CreationFailure = DeadlineWaitFallbackReason::CreationFailed;
        }
        else
        {
            m_CreationFailure = DeadlineWaitFallbackReason::TimerUnavailable;
        }
    }

    MonotonicDeadlineWaiter::MonotonicDeadlineWaiter(DeadlineWaiterPlatform& platform)
        : m_Platform(&platform)
    {
        if (m_Platform->SupportsHighResolutionTimer())
        {
            m_WindowsTimer = m_Platform->CreateHighResolutionTimer();
            if (!m_WindowsTimer)
                m_CreationFailure = DeadlineWaitFallbackReason::CreationFailed;
        }
        else
        {
            m_CreationFailure = DeadlineWaitFallbackReason::TimerUnavailable;
        }
    }

    MonotonicDeadlineWaiter::~MonotonicDeadlineWaiter()
    {
        if (m_WindowsTimer)
            m_Platform->CloseTimer(m_WindowsTimer);
    }

    DeadlineWaitTelemetry MonotonicDeadlineWaiter::WaitUntil(std::chrono::steady_clock::time_point deadline)
    {
        DeadlineWaitTelemetry telemetry;
        const auto wallStart = m_Platform->Now();
        const double cpuStart = m_Platform->ProcessCpuMilliseconds();

        if (!m_WindowsTimer)
        {
            telemetry.Primitive = m_Platform->SupportsHighResolutionTimer()
                ? DeadlineWaitPrimitive::WindowsHighResolutionTimerFallback
                : DeadlineWaitPrimitive::PortableSteadyClock;
            telemetry.FellBack = true;
            telemetry.FallbackReason = m_CreationFailure;
            const auto portableStart = m_Platform->Now();
            m_Platform->PortableWaitUntil(deadline);
            telemetry.PortableWaitMilliseconds = std::max(0.0, Milliseconds(m_Platform->Now() - portableStart));
        }
        else
        {
            telemetry.Primitive = DeadlineWaitPrimitive::WindowsHighResolutionWaitableTimer;
            const auto beforeTimer = m_Platform->Now();
            const auto remaining = deadline > beforeTimer ? deadline - beforeTimer : std::chrono::steady_clock::duration::zero();
            const auto tailBudget = std::min(kMaximumActiveTail, std::chrono::duration_cast<std::chrono::microseconds>(remaining));
            const auto timerDeadline = deadline - tailBudget;
            telemetry.ActiveTailBudgetMilliseconds = Milliseconds(tailBudget);

            bool timerSucceeded = true;
            while (m_Platform->Now() < timerDeadline)
            {
                const auto relativeDelay = timerDeadline - m_Platform->Now();
                if (!m_Platform->ArmTimer(m_WindowsTimer, relativeDelay))
                {
                    telemetry.FallbackReason = DeadlineWaitFallbackReason::ArmFailed;
                    timerSucceeded = false;
                    break;
                }
                const auto timerWaitStart = m_Platform->Now();
                if (!m_Platform->WaitTimer(m_WindowsTimer))
                {
                    telemetry.FallbackReason = DeadlineWaitFallbackReason::WaitFailed;
                    timerSucceeded = false;
                    break;
                }
                telemetry.TimerWaitMilliseconds += std::max(0.0, Milliseconds(m_Platform->Now() - timerWaitStart));
            }

            if (!timerSucceeded)
            {
                telemetry.Primitive = DeadlineWaitPrimitive::WindowsHighResolutionTimerFallback;
                telemetry.FellBack = true;
                telemetry.ActiveTailBudgetMilliseconds = 0.0;
                const auto portableStart = m_Platform->Now();
                m_Platform->PortableWaitUntil(deadline);
                telemetry.PortableWaitMilliseconds = std::max(0.0, Milliseconds(m_Platform->Now() - portableStart));
            }
            else
            {
                const auto tailStart = m_Platform->Now();
                m_Platform->ActiveTailUntil(deadline);
                telemetry.ActiveTailMilliseconds = std::max(0.0, Milliseconds(m_Platform->Now() - tailStart));
            }
        }

        telemetry.CpuTimeMilliseconds = std::max(0.0, m_Platform->ProcessCpuMilliseconds() - cpuStart);
        telemetry.WallTimeMilliseconds = std::max(0.0, Milliseconds(m_Platform->Now() - wallStart));
        return telemetry;
    }
}
