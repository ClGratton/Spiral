#include "Engine/Platform/GLFW/GLFWWindow.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Events/ApplicationEvent.h"
#include "Engine/Events/KeyEvent.h"
#include "Engine/Events/MouseEvent.h"

#include <GLFW/glfw3.h>

#include <vector>

namespace Engine
{
    namespace
    {
        u32 s_GLFWWindowCount = 0;

        void GLFWErrorCallback(int error, const char* description)
        {
            Log::Error("GLFW error ", error, ": ", description);
        }
    }

    GLFWWindow::GLFWWindow(WindowSpecification specification)
        : m_Specification(std::move(specification))
    {
        Initialize();
    }

    GLFWWindow::~GLFWWindow()
    {
        Shutdown();
    }

    void GLFWWindow::Initialize()
    {
        m_Data.Title = m_Specification.Title;
        m_Data.Width = m_Specification.Width;
        m_Data.Height = m_Specification.Height;

        if (s_GLFWWindowCount == 0)
        {
            const int success = glfwInit();
            GE_CORE_ASSERT(success, "Could not initialize GLFW");
            glfwSetErrorCallback(GLFWErrorCallback);
        }

        WindowGraphicsAPI graphicsAPI = m_Specification.GraphicsAPI;
        if (graphicsAPI == WindowGraphicsAPI::Default)
        {
#if defined(GE_HAS_NVRHI_D3D12)
            graphicsAPI = WindowGraphicsAPI::None;
#else
            graphicsAPI = WindowGraphicsAPI::OpenGL;
#endif
        }

        m_UsesOpenGLContext = graphicsAPI == WindowGraphicsAPI::OpenGL;
        if (m_UsesOpenGLContext)
        {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        }
        else
        {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        }

        m_Window = glfwCreateWindow(static_cast<int>(m_Data.Width), static_cast<int>(m_Data.Height), m_Data.Title.c_str(), nullptr, nullptr);
        GE_CORE_ASSERT(m_Window, "Could not create GLFW window");
        ++s_GLFWWindowCount;

        if (m_UsesOpenGLContext)
        {
            glfwMakeContextCurrent(m_Window);
            glfwSwapInterval(1);
        }

        glfwSetWindowUserPointer(m_Window, &m_Data);

        glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window)
        {
            WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            WindowCloseEvent event;
            if (data.EventCallback)
                data.EventCallback(event);
        });

        glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height)
        {
            WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            data.Width = static_cast<u32>(width);
            data.Height = static_cast<u32>(height);

            WindowResizeEvent event(data.Width, data.Height);
            if (data.EventCallback)
                data.EventCallback(event);
        });

        glfwSetDropCallback(m_Window, [](GLFWwindow* window, int pathCount, const char* paths[])
        {
            WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            if (!data.EventCallback)
                return;

            std::vector<std::string> droppedPaths;
            droppedPaths.reserve(static_cast<size_t>(pathCount));
            for (int pathIndex = 0; pathIndex < pathCount; ++pathIndex)
                droppedPaths.emplace_back(paths[pathIndex]);

            FileDropEvent event(std::move(droppedPaths));
            data.EventCallback(event);
        });

        glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int scancode, int action, int mods)
        {
            (void)scancode;
            (void)mods;

            WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            if (!data.EventCallback)
                return;

            if (action == GLFW_PRESS)
            {
                KeyPressedEvent event(key, false);
                data.EventCallback(event);
            }
            else if (action == GLFW_RELEASE)
            {
                KeyReleasedEvent event(key);
                data.EventCallback(event);
            }
            else if (action == GLFW_REPEAT)
            {
                KeyPressedEvent event(key, true);
                data.EventCallback(event);
            }
        });

        glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int mods)
        {
            (void)mods;

            WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            if (!data.EventCallback)
                return;

            if (action == GLFW_PRESS)
            {
                MouseButtonPressedEvent event(button);
                data.EventCallback(event);
            }
            else if (action == GLFW_RELEASE)
            {
                MouseButtonReleasedEvent event(button);
                data.EventCallback(event);
            }
        });

        glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset)
        {
            WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            MouseScrolledEvent event(static_cast<float>(xOffset), static_cast<float>(yOffset));
            if (data.EventCallback)
                data.EventCallback(event);
        });

        glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double x, double y)
        {
            WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
            MouseMovedEvent event(static_cast<float>(x), static_cast<float>(y));
            if (data.EventCallback)
                data.EventCallback(event);
        });

        Log::Info("Created GLFW window: ", m_Data.Title, " (", m_Data.Width, "x", m_Data.Height, ")");
    }

    void GLFWWindow::Shutdown()
    {
        if (!m_Window)
            return;

        glfwDestroyWindow(m_Window);
        m_Window = nullptr;

        GE_CORE_ASSERT(s_GLFWWindowCount > 0);
        --s_GLFWWindowCount;
        if (s_GLFWWindowCount == 0)
            glfwTerminate();

        Log::Info("Destroyed GLFW window: ", m_Data.Title);
    }

    void GLFWWindow::OnUpdate()
    {
        glfwPollEvents();
    }

    bool GLFWWindow::ShouldClose() const
    {
        return m_Window && glfwWindowShouldClose(m_Window);
    }

    void GLFWWindow::RequestClose()
    {
        if (m_Window)
            glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
    }

    void GLFWWindow::SwapBuffers()
    {
        if (m_Window && m_UsesOpenGLContext)
            glfwSwapBuffers(m_Window);
    }
}
