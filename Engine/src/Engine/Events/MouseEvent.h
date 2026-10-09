#pragma once

#include "Engine/Events/Event.h"

#include <sstream>

namespace Engine
{
    class MouseMovedEvent final : public Event
    {
    public:
        MouseMovedEvent(float x, float y)
            : m_MouseX(x), m_MouseY(y)
        {
        }

        float GetX() const { return m_MouseX; }
        float GetY() const { return m_MouseY; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "MouseMovedEvent: " << m_MouseX << ", " << m_MouseY;
            return ss.str();
        }

        static EventType GetStaticType() { return EventType::MouseMoved; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "MouseMoved"; }
        u32 GetCategoryFlags() const override { return EventCategoryMouse | EventCategoryInput; }

    private:
        float m_MouseX;
        float m_MouseY;
    };

    class MouseScrolledEvent final : public Event
    {
    public:
        MouseScrolledEvent(float xOffset, float yOffset)
            : m_XOffset(xOffset), m_YOffset(yOffset)
        {
        }

        float GetXOffset() const { return m_XOffset; }
        float GetYOffset() const { return m_YOffset; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "MouseScrolledEvent: " << GetXOffset() << ", " << GetYOffset();
            return ss.str();
        }

        static EventType GetStaticType() { return EventType::MouseScrolled; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "MouseScrolled"; }
        u32 GetCategoryFlags() const override { return EventCategoryMouse | EventCategoryInput; }

    private:
        float m_XOffset;
        float m_YOffset;
    };

    class MouseButtonEvent : public Event
    {
    public:
        int GetMouseButton() const { return m_Button; }
        InputModifiers GetModifiers() const { return m_Modifiers; }
        u32 GetCategoryFlags() const override { return EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton; }

    protected:
        MouseButtonEvent(int button, InputModifiers modifiers)
            : m_Button(button), m_Modifiers(modifiers)
        {
        }

        int m_Button;
        InputModifiers m_Modifiers;
    };

    class MouseButtonPressedEvent final : public MouseButtonEvent
    {
    public:
        explicit MouseButtonPressedEvent(int button, InputModifiers modifiers = InputModifierNone)
            : MouseButtonEvent(button, modifiers)
        {
        }

        static EventType GetStaticType() { return EventType::MouseButtonPressed; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "MouseButtonPressed"; }
    };

    class MouseButtonReleasedEvent final : public MouseButtonEvent
    {
    public:
        explicit MouseButtonReleasedEvent(int button, InputModifiers modifiers = InputModifierNone)
            : MouseButtonEvent(button, modifiers)
        {
        }

        static EventType GetStaticType() { return EventType::MouseButtonReleased; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "MouseButtonReleased"; }
    };

    class CursorEnterEvent final : public Event
    {
    public:
        explicit CursorEnterEvent(bool entered)
            : m_Entered(entered)
        {
        }

        bool Entered() const { return m_Entered; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "CursorEnterEvent: " << (m_Entered ? "entered" : "left");
            return ss.str();
        }

        static EventType GetStaticType() { return EventType::CursorEnter; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "CursorEnter"; }
        u32 GetCategoryFlags() const override { return EventCategoryMouse | EventCategoryInput; }

    private:
        bool m_Entered;
    };
}
