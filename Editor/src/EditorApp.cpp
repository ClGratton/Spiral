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
    if (args.HasFlag("--renderer-vulkan") || args.HasFlag("--vulkan-render-smoke"))
        specification.Window.GraphicsAPI = WindowGraphicsAPI::None;
    const bool extendedSmoke = args.HasFlag("--asset-watch-smoke")
        || args.HasFlag("--gltf-import-smoke")
        || args.HasFlag("--material-asset-smoke")
        || args.HasFlag("--project-save-smoke")
        || args.HasFlag("--undo-redo-smoke")
        || args.HasFlag("--frame-task-graph-smoke")
        || args.HasFlag("--frame-lifecycle-telemetry-smoke")
        || args.HasFlag("--scene-authoring-smoke")
        || args.HasFlag("--scene-render-snapshot-smoke")
        || args.HasFlag("--scene-origin-raster-smoke");
    specification.MaxFrames = args.HasFlag("--vulkan-render-smoke")
        ? 60
        : (args.HasFlag("--frame-lifecycle-telemetry-smoke") ? 12
            : (args.HasFlag("--scene-origin-raster-smoke") ? 5 : (extendedSmoke ? 4 : (args.HasFlag("--smoke-test") ? 2 : 0))));

    return new EditorApplication(specification);
}
