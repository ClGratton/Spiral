#pragma once

#include "Engine/Core/Base.h"

#include <string>
#include <string_view>

namespace Engine
{
    enum class EventType
    {
        None = 0,
        WindowClose,
        WindowResize,
        FileDrop,
        WindowFocus,
        WindowLostFocus,
        AppTick,
        AppUpdate,
        AppRender,
        KeyPressed,
        KeyReleased,
        MouseButtonPressed,
        MouseButtonReleased,
        MouseMoved,
        MouseScrolled,
        CharTyped,
        CursorEnter,
        WindowContentScale
    };

    enum EventCategory : u32
    {
        EventCategoryNone = 0,
        EventCategoryApplication = 1u << 0u,
        EventCategoryInput = 1u << 1u,
        EventCategoryKeyboard = 1u << 2u,
        EventCategoryMouse = 1u << 3u,
        EventCategoryMouseButton = 1u << 4u
    };

    // Engine-owned modifier state. Platform backends translate their native
    // bits into these flags; native constants never leave Engine/Platform.
    enum InputModifier : u32
    {
        InputModifierNone = 0,
        InputModifierShift = 1u << 0u,
        InputModifierControl = 1u << 1u,
        InputModifierAlt = 1u << 2u,
        InputModifierSuper = 1u << 3u,
        InputModifierCapsLock = 1u << 4u,
        InputModifierNumLock = 1u << 5u
    };

    using InputModifiers = u32;

    class Event
    {
    public:
        virtual ~Event() = default;

        bool Handled = false;

        virtual EventType GetEventType() const = 0;
        virtual std::string_view GetName() const = 0;
        virtual u32 GetCategoryFlags() const = 0;

        virtual std::string ToString() const
        {
            return std::string(GetName());
        }

        bool IsInCategory(EventCategory category) const
        {
            return (GetCategoryFlags() & category) != 0;
        }
    };

    class EventDispatcher
    {
    public:
        explicit EventDispatcher(Event& event)
            : m_Event(event)
        {
        }

        template<typename T, typename F>
        bool Dispatch(const F& function)
        {
            if (m_Event.GetEventType() == T::GetStaticType())
            {
                m_Event.Handled = function(static_cast<T&>(m_Event)) || m_Event.Handled;
                return true;
            }

            return false;
        }

    private:
        Event& m_Event;
    };
}
