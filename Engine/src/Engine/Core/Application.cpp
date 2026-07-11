#include "Engine/Core/Application.h"

#include "Engine/Core/Log.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Renderer/Renderer.h"
#include "Engine/UI/ImGuiLayer.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace Engine
{
    Application* Application::s_Instance = nullptr;

    const char* ApplicationCommandLineArgs::operator[](int index) const
    {
        if (index < 0 || index >= Count)
            throw std::out_of_range("Application command-line argument index is out of range");
        return Args[index];
    }

    bool ApplicationCommandLineArgs::HasFlag(std::string_view flag) const
    {
        for (int i = 0; i < Count; ++i)
        {
            if (Args[i] == flag)
                return true;
        }

        return false;
    }

    Application::Application(ApplicationSpecification specification)
        : m_Specification(std::move(specification))
    {
        if (s_Instance)
            throw std::logic_error("Application already exists");
        s_Instance = this;

        try
        {
            if (!m_Specification.WorkingDirectory.empty())
                std::filesystem::current_path(m_Specification.WorkingDirectory);

            m_Specification.Window.Title = m_Specification.Name;
            m_Window = Window::Create(m_Specification.Window);
            m_Window->SetEventCallback(GE_BIND_EVENT_FN(Application::OnEvent));

            Renderer::Initialize();
        }
        catch (...)
        {
            s_Instance = nullptr;
            throw;
        }
    }

    Application::~Application()
    {
        Renderer::Shutdown();
        s_Instance = nullptr;
    }

    Application& Application::Get()
    {
        if (!s_Instance)
            throw std::logic_error("Application has not been created");
        return *s_Instance;
    }

    void Application::Run()
    {
        Log::Info("Running application: ", m_Specification.Name);

        auto lastFrameTime = std::chrono::steady_clock::now();

        while (m_Running && !m_Window->ShouldClose())
        {
            const auto now = std::chrono::steady_clock::now();
            const std::chrono::duration<float> delta = now - lastFrameTime;
            lastFrameTime = now;
            Timestep timestep(delta.count());

            if (!m_Minimized)
            {
                Renderer::BeginFrame();

                for (auto& layer : m_LayerStack)
                    layer->OnUpdate(timestep);

                for (auto& layer : m_LayerStack)
                    layer->OnRender();

                Renderer::EndFrame();
            }

            if (m_ImGuiLayer && !m_Minimized)
                m_ImGuiLayer->Begin();

            for (auto& layer : m_LayerStack)
                layer->OnUiRender();

            if (m_ImGuiLayer && !m_Minimized)
                m_ImGuiLayer->End();

            m_Window->OnUpdate();
            JobSystem::Get().WaitIdle();

            ++m_FrameIndex;
            if (m_Specification.MaxFrames != 0 && m_FrameIndex >= m_Specification.MaxFrames)
                Close();
        }

        Log::Info("Application stopped after ", m_FrameIndex, " frame(s)");
    }

    void Application::OnEvent(Event& event)
    {
        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<WindowCloseEvent>(GE_BIND_EVENT_FN(Application::OnWindowClose));
        dispatcher.Dispatch<WindowResizeEvent>(GE_BIND_EVENT_FN(Application::OnWindowResize));

        for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
        {
            if (event.Handled)
                break;

            (*it)->OnEvent(event);
        }
    }

    Layer* Application::PushLayer(Scope<Layer> layer)
    {
        return m_LayerStack.PushLayer(std::move(layer));
    }

    Layer* Application::PushOverlay(Scope<Layer> layer)
    {
        return m_LayerStack.PushOverlay(std::move(layer));
    }

    void Application::SetImGuiLayer(ImGuiLayer* layer)
    {
        m_ImGuiLayer = layer;
    }

    void Application::Close()
    {
        m_Running = false;
        m_Window->RequestClose();
    }

    bool Application::OnWindowClose(WindowCloseEvent& event)
    {
        (void)event;
        Close();
        return true;
    }

    bool Application::OnWindowResize(WindowResizeEvent& event)
    {
        m_Minimized = event.GetWidth() == 0 || event.GetHeight() == 0;
        return false;
    }
}
