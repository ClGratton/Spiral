#include "Engine/Core/Window.h"

#include "Engine/Platform/GLFW/GLFWWindow.h"
#include "Engine/Platform/Headless/HeadlessWindow.h"

namespace Engine
{
    Scope<Window> Window::Create(const WindowSpecification& specification)
    {
        if (!specification.Headless)
            return CreateScope<GLFWWindow>(specification);

        return CreateScope<HeadlessWindow>(specification);
    }
}
