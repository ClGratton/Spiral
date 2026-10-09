#pragma once

#include "Engine/Core/Base.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SpiralEditor
{
    enum class NotificationSeverity : Engine::u8
    {
        Info,
        Warning,
        Error
    };

    // Monotonic milliseconds. Injected so expiry is deterministic under test.
    using NotificationClock = std::function<Engine::u64()>;
    Engine::u64 SteadyMillisecondClock();

    struct NotificationSpec
    {
        NotificationSeverity Severity = NotificationSeverity::Info;
        std::string Message;
        // Info 4000 ms, Warning 8000 ms, Error stays until dismissed. 0 means stay until dismissed.
        std::optional<Engine::u32> DurationMs;
        // Both set or both empty, for example "Undo: Move Cube" and "edit.undo".
        std::string ActionLabel;
        std::string ActionCommandId;
    };

    struct Notification
    {
        Engine::u64 Id = 0; // 1-based, strictly increasing, never reused
        NotificationSeverity Severity = NotificationSeverity::Info;
        std::string Message;
        std::string ActionLabel;
        std::string ActionCommandId;
        Engine::u32 Count = 1;        // identical posts folded into this toast, saturating at kMaximumCount
        bool Sticky = false;          // no deadline
        bool Held = false;            // paused, for example while hovered
        Engine::u64 ExpiresAtMs = 0;  // meaningful only when !Sticky && !Held
    };

    struct PostResult
    {
        Engine::u64 Id = 0; // 0 when the spec was rejected
        bool Deduplicated = false;
    };

    // A short stack of transient toasts. Posting an identical active toast restarts its timer and bumps its
    // count instead of stacking a copy. A toast is visible while now < ExpiresAtMs; the clock is clamped to be
    // non-decreasing. When more than `maximumActive` toasts are live the oldest is dropped. Dismissal and
    // expiry are not a record: callers that need one also write to the log buffer. Main thread only.
    class Notifications
    {
    public:
        static constexpr size_t kMaximumMessageBytes = 512;
        static constexpr size_t kMaximumActionBytes = 64;
        static constexpr Engine::u32 kMaximumDurationMs = 10 * 60 * 1000;
        static constexpr Engine::u32 kMaximumCount = 999;

        // A null clock selects SteadyMillisecondClock. maximumActive is clamped to [1, 32].
        explicit Notifications(NotificationClock clock = {}, size_t maximumActive = 5);

        PostResult Post(NotificationSpec spec);

        // Live toasts, oldest first, after dropping everything that has expired.
        std::vector<Notification> Active();

        bool Dismiss(Engine::u64 id);
        // Pauses or resumes a toast's countdown; resuming restores exactly the time that was left.
        bool Hold(Engine::u64 id, bool held);
        // The toast's action command id, dismissing the toast so a double click cannot run it twice.
        std::optional<std::string> TakeAction(Engine::u64 id);
        void Clear();

    private:
        struct Item
        {
            Notification View;
            Engine::u32 DurationMs = 0;
            Engine::u64 HeldRemainingMs = 0;
        };

        Engine::u64 Now();
        void Expire(Engine::u64 now);
        Item* FindItem(Engine::u64 id);

        NotificationClock m_Clock;
        size_t m_MaximumActive;
        Engine::u64 m_LastNow = 0;
        Engine::u64 m_NextId = 1;
        std::vector<Item> m_Items;
    };
}
