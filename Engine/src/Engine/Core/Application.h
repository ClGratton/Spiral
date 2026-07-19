#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/LayerStack.h"
#include "Engine/Core/Timestep.h"
#include "Engine/Core/Window.h"
#include "Engine/Events/ApplicationEvent.h"

#include <string>
#include <string_view>

int main(int argc, char** argv);

namespace Engine
{
    class ImGuiLayer;

    struct ApplicationCommandLineArgs
    {
        int Count = 0;
        char** Args = nullptr;

        const char* operator[](int index) const;
        bool HasFlag(std::string_view flag) const;
        std::string_view GetOptionValue(std::string_view option) const;
    };

    struct ApplicationSpecification
    {
        std::string Name = "Spiral Application";
        std::string WorkingDirectory;
        ApplicationCommandLineArgs CommandLineArgs;
        WindowSpecification Window;
        u32 MaxFrames = 3;
    };

    class Application
    {
    public:
        explicit Application(ApplicationSpecification specification);
        virtual ~Application();

        void OnEvent(Event& event);
        Layer* PushLayer(Scope<Layer> layer);
        Layer* PushOverlay(Scope<Layer> layer);
        void SetImGuiLayer(ImGuiLayer* layer);
        void Close();

        Window& GetWindow() { return *m_Window; }
        const ApplicationSpecification& GetSpecification() const { return m_Specification; }
        u64 GetFrameIndex() const { return m_FrameIndex; }

        static Application& Get();

    private:
        void Run();
        bool OnWindowClose(WindowCloseEvent& event);
        bool OnWindowResize(WindowResizeEvent& event);

    private:
        ApplicationSpecification m_Specification;
        Scope<Window> m_Window;
        LayerStack m_LayerStack;
        ImGuiLayer* m_ImGuiLayer = nullptr;
        bool m_Running = true;
        bool m_Minimized = false;
        u64 m_FrameIndex = 0;
        bool m_FrameLifecycleTelemetrySmokeComplete = false;
        bool m_SmoothFrametimeTargetChangeApplied = false;
        double m_SmoothFrametimeOldTargetFramesPerSecond = 0.0;
        double m_SmoothFrametimeNewTargetFramesPerSecond = 0.0;
        bool m_FramePacingBenchmarkStarted = false;
        bool m_FramePacingAttachmentCheckResolved = false;
        bool m_FramePacingAttachmentReleased = false;
        std::string m_FramePacingAttachmentRunId;
        u32 m_FramePacingAttachmentProcessId = 0;
        std::string m_FramePacingAttachmentExecutablePath;
        u64 m_FramePacingAttachmentQpcFrequency = 0;
        bool m_OpticalTriggerArmed = false;
        bool m_OpticalTriggerSynthetic = false;
        bool m_OpticalCapturePublished = false;
        std::string m_OpticalTriggerId;
        u64 m_OpticalTriggerSequence = 0;

        static Application* s_Instance;

        friend int ::main(int argc, char** argv);
    };

    Application* CreateApplication(ApplicationCommandLineArgs args);
}
