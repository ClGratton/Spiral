#include "Engine/Platform/Headless/HeadlessWindow.h"

#include "Engine/Core/Log.h"

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

    void HeadlessWindow::OnUpdate()
    {
    }
}
