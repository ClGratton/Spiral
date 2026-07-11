#include "EditorLayer.h"

#include <Engine.h>
#include <Engine/Core/EntryPoint.h>

class EditorApplication final : public Engine::Application
{
public:
    explicit EditorApplication(const Engine::ApplicationSpecification& specification)
        : Engine::Application(specification)
    {
        if (!specification.Window.Headless)
            SetImGuiLayer(static_cast<Engine::ImGuiLayer*>(PushOverlay(Engine::CreateScope<Engine::ImGuiLayer>())));

        PushLayer(Engine::CreateScope<EditorLayer>());
    }
};

Engine::Application* Engine::CreateApplication(ApplicationCommandLineArgs args)
{
    ApplicationSpecification specification;
    specification.Name = "Spiral Editor";
    specification.CommandLineArgs = args;
    specification.Window.Width = 1600;
    specification.Window.Height = 900;
    specification.Window.Headless = args.HasFlag("--headless");
    const bool extendedSmoke = args.HasFlag("--asset-watch-smoke")
        || args.HasFlag("--gltf-import-smoke")
        || args.HasFlag("--material-asset-smoke")
        || args.HasFlag("--project-save-smoke");
    specification.MaxFrames = extendedSmoke ? 4 : (args.HasFlag("--smoke-test") ? 2 : 0);

    return new EditorApplication(specification);
}
