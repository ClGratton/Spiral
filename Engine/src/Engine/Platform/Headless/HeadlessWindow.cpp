#include "Engine/Platform/Headless/HeadlessWindow.h"

#include "Engine/Core/Log.h"
#include "Engine/Events/ApplicationEvent.h"

#include <chrono>
#include <thread>

namespace Engine
{
    HeadlessWindow::HeadlessWindow(WindowSpecification specification)
        : m_Specification(std::move(specification))
    {
        Log::Info("Created headless window: ", m_Specification.Title, " (", m_Specification.Width, "x", m_Specification.Height, ")");
    }

    HeadlessWindow::~HeadlessWindow()
    {
        Log::Info("Destroyed headless window: ", m_Specification.Title);
    }

    void HeadlessWindow::PollEvents()
    {
        // Take the batch first: a callback may queue another resize.
        std::vector<std::pair<u32, u32>> pending;
        pending.swap(m_PendingResizes);
        for (const auto& [width, height] : pending)
        {
            m_Specification.Width = width;
            m_Specification.Height = height;
            WindowResizeEvent event(width, height);
            if (m_EventCallback)
                m_EventCallback(event);
        }
    }

    void HeadlessWindow::WaitEvents(double timeoutSeconds)
    {
        // Like glfwWaitEventsTimeout: return at once when events are pending,
        // otherwise let the whole interval elapse (no other event source exists).
        const bool hadPending = !m_PendingResizes.empty();
        PollEvents();
        if (!hadPending && timeoutSeconds > 0.0)
            std::this_thread::sleep_for(std::chrono::duration<double>(timeoutSeconds));
    }
}
