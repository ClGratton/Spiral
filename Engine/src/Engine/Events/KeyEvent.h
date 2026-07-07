#pragma once

#include "Engine/Events/Event.h"

#include <sstream>

namespace Engine
{
    class KeyEvent : public Event
    {
    public:
        int GetKeyCode() const { return m_KeyCode; }
        u32 GetCategoryFlags() const override { return EventCategoryKeyboard | EventCategoryInput; }

    protected:
        explicit KeyEvent(int keyCode)
            : m_KeyCode(keyCode)
        {
        }

        int m_KeyCode;
    };

    class KeyPressedEvent final : public KeyEvent
    {
    public:
        KeyPressedEvent(int keyCode, bool repeat)
            : KeyEvent(keyCode), m_Repeat(repeat)
        {
        }

        bool IsRepeat() const { return m_Repeat; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyPressedEvent: " << m_KeyCode << " (repeat = " << m_Repeat << ")";
            return ss.str();
        }

        static EventType GetStaticType() { return EventType::KeyPressed; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "KeyPressed"; }

    private:
        bool m_Repeat;
    };

    class KeyReleasedEvent final : public KeyEvent
    {
    public:
        explicit KeyReleasedEvent(int keyCode)
            : KeyEvent(keyCode)
        {
        }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyReleasedEvent: " << m_KeyCode;
            return ss.str();
        }

        static EventType GetStaticType() { return EventType::KeyReleased; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "KeyReleased"; }
    };
}
