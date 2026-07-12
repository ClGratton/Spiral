#include "ExampleLayer.h"

#include <Engine.h>
#include <Engine/Core/EntryPoint.h>

class SandboxApplication final : public Engine::Application
{
public:
    explicit SandboxApplication(const Engine::ApplicationSpecification& specification)
        : Engine::Application(specification)
    {
        PushLayer(Engine::CreateScope<ExampleLayer>());
    }
};

Engine::Application* Engine::CreateApplication(ApplicationCommandLineArgs args)
{
    ApplicationSpecification specification;
    specification.Name = "Spiral Sandbox";
    specification.CommandLineArgs = args;
    specification.Window.Width = 1280;
    specification.Window.Height = 720;
    specification.Window.Headless = args.HasFlag("--headless");
    if (args.HasFlag("--renderer-vulkan") || args.HasFlag("--vulkan-render-smoke"))
        specification.Window.GraphicsAPI = WindowGraphicsAPI::None;
    specification.MaxFrames = args.HasFlag("--vulkan-render-smoke") ? 4 : (args.HasFlag("--smoke-test") ? 3 : 0);

    return new SandboxApplication(specification);
}
