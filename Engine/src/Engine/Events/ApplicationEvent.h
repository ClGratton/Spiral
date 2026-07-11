#pragma once

#include "Engine/Events/Event.h"

#include <sstream>
#include <utility>
#include <vector>

namespace Engine
{
    class WindowResizeEvent final : public Event
    {
    public:
        WindowResizeEvent(u32 width, u32 height)
            : m_Width(width), m_Height(height)
        {
        }

        u32 GetWidth() const { return m_Width; }
        u32 GetHeight() const { return m_Height; }

        std::string ToString() const override
        {
            std::stringstream ss;
            ss << "WindowResizeEvent: " << m_Width << ", " << m_Height;
            return ss.str();
        }

        static EventType GetStaticType() { return EventType::WindowResize; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "WindowResize"; }
        u32 GetCategoryFlags() const override { return EventCategoryApplication; }

    private:
        u32 m_Width;
        u32 m_Height;
    };

    class WindowCloseEvent final : public Event
    {
    public:
        static EventType GetStaticType() { return EventType::WindowClose; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "WindowClose"; }
        u32 GetCategoryFlags() const override { return EventCategoryApplication; }
    };

    class FileDropEvent final : public Event
    {
    public:
        explicit FileDropEvent(std::vector<std::string> paths)
            : m_Paths(std::move(paths))
        {
        }

        const std::vector<std::string>& GetPaths() const { return m_Paths; }

        static EventType GetStaticType() { return EventType::FileDrop; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "FileDrop"; }
        u32 GetCategoryFlags() const override { return EventCategoryApplication | EventCategoryInput; }

    private:
        std::vector<std::string> m_Paths;
    };

    class AppTickEvent final : public Event
    {
    public:
        static EventType GetStaticType() { return EventType::AppTick; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "AppTick"; }
        u32 GetCategoryFlags() const override { return EventCategoryApplication; }
    };

    class AppUpdateEvent final : public Event
    {
    public:
        static EventType GetStaticType() { return EventType::AppUpdate; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "AppUpdate"; }
        u32 GetCategoryFlags() const override { return EventCategoryApplication; }
    };

    class AppRenderEvent final : public Event
    {
    public:
        static EventType GetStaticType() { return EventType::AppRender; }
        EventType GetEventType() const override { return GetStaticType(); }
        std::string_view GetName() const override { return "AppRender"; }
        u32 GetCategoryFlags() const override { return EventCategoryApplication; }
    };
}
