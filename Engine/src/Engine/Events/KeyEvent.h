#pragma once

#include "Engine/Events/Event.h"

#include <sstream>

namespace Engine
{
    class KeyEvent : public Event
    {
    public:
        int GetKeyCode() const { return m_KeyCode; }
        // Platform scancode of the physical key (GLFW scancode; the X keycode on X11). 0 when unknown.
        int GetScancode() const { return m_Scancode; }
        InputModifiers GetModifiers() const { return m_Modifiers; }
        u32 GetCategoryFlags() const override { return EventCategoryKeyboard | EventCategoryInput; }

    protected:
        KeyEvent(int keyCode, int scancode, InputModifiers modifiers)
            : m_KeyCode(keyCode), m_Scancode(scancode), m_Modifiers(modifiers)
        {
        }

        int m_KeyCode;
        int m_Scancode;
        InputModifiers m_Modifiers;
    };

    class KeyPressedEvent final : public KeyEvent
    {
    public:
        KeyPressedEvent(int keyCode, bool repeat, int scancode = 0, InputModifiers modifiers = InputModifierNone)
            : KeyEvent(keyCode, scancode, modifiers), m_Repeat(repeat)
        {
        }

        bool IsRepeat() const { return m_Repeat; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyPressedEvent: " << m_KeyCode << " (repeat = " << m_Repeat
               << ", scancode = " << m_Scancode << ", mods = " << m_Modifiers << ")";
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
        explicit KeyReleasedEvent(int keyCode, int scancode = 0, InputModifiers modifiers = InputModifierNone)
            : KeyEvent(keyCode, scancode, modifiers)
        {
        }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "KeyReleasedEvent: " << m_KeyCode << " (scancode = " << m_Scancode << ", mods = " << m_Modifiers << ")";
            return ss.str();
        }

        static EventType GetStaticType() { return EventType::KeyReleased; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "KeyReleased"; }
    };

    // One Unicode scalar value of committed text (after layout, dead keys and compose).
    // Delivered separately from KeyPressedEvent: there is no 1:1 key/character mapping.
    class CharTypedEvent final : public Event
    {
    public:
        explicit CharTypedEvent(u32 codePoint)
            : m_CodePoint(codePoint)
        {
        }

        u32 GetCodePoint() const { return m_CodePoint; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "CharTypedEvent: U+" << std::hex << std::uppercase << m_CodePoint;
            return ss.str();
        }

        static EventType GetStaticType() { return EventType::CharTyped; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "CharTyped"; }
        u32 GetCategoryFlags() const override { return EventCategoryKeyboard | EventCategoryInput; }

    private:
        u32 m_CodePoint;
    };
}
