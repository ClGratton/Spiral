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
        u32 GetCategoryFlags() const override { return EventCategoryMouse | EventCategoryInput | EventCategoryMouseButton; }

    protected:
        explicit MouseButtonEvent(int button)
            : m_Button(button)
        {
        }

        int m_Button;
    };

    class MouseButtonPressedEvent final : public MouseButtonEvent
    {
    public:
        explicit MouseButtonPressedEvent(int button)
            : MouseButtonEvent(button)
        {
        }

        static EventType GetStaticType() { return EventType::MouseButtonPressed; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "MouseButtonPressed"; }
    };

    class MouseButtonReleasedEvent final : public MouseButtonEvent
    {
    public:
        explicit MouseButtonReleasedEvent(int button)
            : MouseButtonEvent(button)
        {
        }

        static EventType GetStaticType() { return EventType::MouseButtonReleased; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "MouseButtonReleased"; }
    };
}
