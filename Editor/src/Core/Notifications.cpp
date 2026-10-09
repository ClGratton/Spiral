#include "Notifications.h"

#include "TextLimits.h"

#include <algorithm>
#include <chrono>

namespace SpiralEditor
{
    namespace
    {
        Engine::u32 DefaultDurationMs(NotificationSeverity severity)
        {
            switch (severity)
            {
            case NotificationSeverity::Info: return 4000;
            case NotificationSeverity::Warning: return 8000;
            case NotificationSeverity::Error: return 0;
            }
            return 0;
        }
    }

    Engine::u64 SteadyMillisecondClock()
    {
        return static_cast<Engine::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    Notifications::Notifications(NotificationClock clock, size_t maximumActive)
        : m_Clock(clock ? std::move(clock) : NotificationClock(SteadyMillisecondClock))
        , m_MaximumActive(std::clamp<size_t>(maximumActive, 1, 32))
    {
    }

    Engine::u64 Notifications::Now()
    {
        m_LastNow = std::max(m_LastNow, m_Clock());
        return m_LastNow;
    }

    void Notifications::Expire(Engine::u64 now)
    {
        std::erase_if(m_Items, [now](const Item& item)
        {
            return !item.View.Sticky && !item.View.Held && now >= item.View.ExpiresAtMs;
        });
    }

    Notifications::Item* Notifications::FindItem(Engine::u64 id)
    {
        const auto it = std::find_if(m_Items.begin(), m_Items.end(), [id](const Item& item) { return item.View.Id == id; });
        return it == m_Items.end() ? nullptr : &*it;
    }

    PostResult Notifications::Post(NotificationSpec spec)
    {
        if (static_cast<size_t>(spec.Severity) > static_cast<size_t>(NotificationSeverity::Error) || spec.Message.empty()
            || spec.ActionLabel.empty() != spec.ActionCommandId.empty() || spec.ActionLabel.size() > kMaximumActionBytes
            || spec.ActionCommandId.size() > kMaximumActionBytes)
            return {};
        spec.Message = std::string(TruncateUtf8(spec.Message, kMaximumMessageBytes));
        if (spec.Message.empty())
            return {};

        const Engine::u64 now = Now();
        Expire(now);
        const Engine::u32 duration = std::min(spec.DurationMs.value_or(DefaultDurationMs(spec.Severity)), kMaximumDurationMs);

        for (Item& item : m_Items)
        {
            const Notification& view = item.View;
            if (view.Severity != spec.Severity || view.Message != spec.Message || view.ActionLabel != spec.ActionLabel
                || view.ActionCommandId != spec.ActionCommandId)
                continue;
            item.View.Count = std::min(item.View.Count + 1, kMaximumCount);
            item.DurationMs = duration;
            item.View.Sticky = duration == 0;
            item.View.ExpiresAtMs = now + duration;
            item.HeldRemainingMs = duration;
            return { view.Id, true };
        }

        Item item;
        item.View.Id = m_NextId++;
        item.View.Severity = spec.Severity;
        item.View.Message = std::move(spec.Message);
        item.View.ActionLabel = std::move(spec.ActionLabel);
        item.View.ActionCommandId = std::move(spec.ActionCommandId);
        item.View.Sticky = duration == 0;
        item.View.ExpiresAtMs = now + duration;
        item.DurationMs = duration;
        item.HeldRemainingMs = duration;
        const Engine::u64 id = item.View.Id;
        m_Items.push_back(std::move(item));
        if (m_Items.size() > m_MaximumActive)
            m_Items.erase(m_Items.begin());
        return { id, false };
    }

    std::vector<Notification> Notifications::Active()
    {
        Expire(Now());
        std::vector<Notification> views;
        views.reserve(m_Items.size());
        for (const Item& item : m_Items)
            views.push_back(item.View);
        return views;
    }

    bool Notifications::Dismiss(Engine::u64 id)
    {
        Expire(Now());
        return std::erase_if(m_Items, [id](const Item& item) { return item.View.Id == id; }) != 0;
    }

    bool Notifications::Hold(Engine::u64 id, bool held)
    {
        const Engine::u64 now = Now();
        Expire(now);
        Item* item = FindItem(id);
        if (!item)
            return false;
        if (item->View.Held == held)
            return true;
        if (!item->View.Sticky)
        {
            if (held)
                item->HeldRemainingMs = item->View.ExpiresAtMs - now;
            else
                item->View.ExpiresAtMs = now + item->HeldRemainingMs;
        }
        item->View.Held = held;
        return true;
    }

    std::optional<std::string> Notifications::TakeAction(Engine::u64 id)
    {
        Expire(Now());
        Item* item = FindItem(id);
        if (!item || item->View.ActionCommandId.empty())
            return std::nullopt;
        std::string command = std::move(item->View.ActionCommandId);
        std::erase_if(m_Items, [id](const Item& candidate) { return candidate.View.Id == id; });
        return command;
    }

    void Notifications::Clear()
    {
        m_Items.clear();
    }
}
