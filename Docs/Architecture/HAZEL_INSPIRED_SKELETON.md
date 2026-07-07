# Hazel-Inspired Engine Skeleton

Status: Draft v0.1
Date: 2026-07-06

Purpose: define a coherent project skeleton, naming style, folder layout, app/editor split, module boundaries, and coding conventions inspired by public Hazel/The Cherno patterns, without committing to a Hazel fork or copying Hazel's renderer.

## Position

Use Hazel as a coherence scaffold.

Do not use Hazel as the engine foundation.

Hazel's public value for this project is not its renderer or current feature set. Its value is that it is small, readable, teachable, and organized around a few simple ideas:

- A single engine library.
- A client-owned application type.
- An engine-owned entry point.
- A layer stack for editor/game/debug behavior.
- Events that flow through the layer stack.
- Platform-specific code hidden behind stable interfaces.
- A separate editor application.
- A sandbox/example application that proves the engine is usable from the outside.
- A public umbrella include for application authors.

That shape is useful because this project will be touched by humans and AI agents. The skeleton should make incoherent additions feel obviously out of place.

## The Cherno / Hazel Lessons To Adopt

These are the Hazel-style lessons worth adopting:

1. **Start with a working application shell.**
   The engine should always boot, open a window, process events, update layers, render a frame, and shut down cleanly.

2. **Keep the engine/editor/client split visible.**
   Hazel has `Hazel`, `Hazelnut`, and `Sandbox`. We should keep the same clarity: engine library, editor tool, and small client/game examples.

3. **Let clients define the application, but let the engine own `main`.**
   Client code should implement `CreateApplication(args)`. The engine entry point initializes logging/profiling, creates the application, runs it, and shuts it down.

4. **Use layers for composition.**
   Layers are not the whole engine architecture, but they are an excellent application-level composition tool for editor UI, game simulation, debug views, overlays, and tools.

5. **Dispatch events from top-most layer downward.**
   Overlays and editor tools should get first chance to consume input. Gameplay should not receive editor shortcuts or UI-click events after they are handled.

6. **Keep platform code behind interfaces.**
   Windowing, input, file dialogs, timers, threads, and dynamic libraries should live behind platform abstractions. The rest of the engine should not know whether it is running through Win32, SDL, GLFW, or something else.

7. **Use thin renderer interfaces early, but do not freeze the renderer around them.**
   Hazel's early renderer abstraction is clean for teaching. Our renderer needs a deeper RHI/render-graph/visibility-buffer architecture, so the same clarity should be applied at a lower level.

8. **Keep a sandbox project forever.**
   The sandbox is not a toy. It is a quick compile/run target for testing the public engine API outside the editor.

9. **Make code navigable before it is clever.**
   Prefer explicit modules, predictable names, and small files over compressed cleverness. This matters even more because AI agents will contribute code.

10. **Use generated project/setup scripts.**
    Hazel uses setup and project-generation scripts so users do not manually wire everything. Our engine should go further: setup scripts, SDK checks, asset-pipeline checks, shader compiler checks, and first-project generation.

## Proposed Repository Shape

```text
/
  Engine/
    src/
      Engine/
        Core/
        Application/
        Events/
        Platform/
        RHI/
        Renderer/
        RenderGraph/
        Scene/
        Assets/
        Animation/
        Physics/
        Jobs/
        Scripting/
        Graphs/
        Audio/
        UI/
        Automation/
        Diagnostics/
        Project/
        Serialization/
    include/
      Engine.h
    shaders/
    resources/

  ScriptCore/
    src/
      Engine/
        Entity.cs
        Components.cs
        Input.cs
        Jobs.cs
        Math.cs
        Attributes.cs

  ScriptTemplates/
    Component/
    System/
    Job/
    GameplayGraph/

  Editor/
    src/
      EditorApp.cpp
      EditorLayer.h
      EditorLayer.cpp
      Panels/
      Workflows/
      Viewports/
      Gizmos/
      Inspectors/
      Automation/
      Style/

  Player/
    src/
      PlayerApp.cpp

  Sandbox/
    src/
      SandboxApp.cpp
      ExampleLayer.h
      ExampleLayer.cpp

  Tools/
    AssetCompiler/
    ShaderCompiler/
    ScriptCompiler/
    BindingGenerator/
    GraphCompiler/
    ProjectGenerator/
    MotionPackBuilder/
    TexturePacker/
    MeshClusterBuilder/
    Validation/

  Tests/
    Unit/
    Integration/
    RenderGolden/
    AssetPipeline/

  Docs/
  Resources/
  Vendor/
  Scripts/
  Build/
```

Hazel uses top-level `Hazel`, `Hazelnut`, `Sandbox`, `Hazel-ScriptCore`, `vendor`, `scripts`, and `Resources`. We keep that readability, but expand it for this project's needs: renderer research, automation, guided workflows, importers, validation, and AI tooling.

## Application Split

### Engine Library

The engine library owns:

- Core types, logging, asserts, profiling, memory helpers.
- Native job system and frame task graph.
- Application lifecycle.
- Window abstraction.
- Event system.
- Layer stack.
- RHI/render graph/renderer.
- Scene/runtime systems.
- Asset database and import pipeline APIs.
- Scripting host.
- Automation and diagnostics hooks.

The engine library must not depend on the editor.

### Editor App

The editor app is a client of the engine.

It owns:

- Main editor layer.
- Panels, inspectors, viewports, gizmos.
- Guided workflows.
- AI-agent UI and task orchestration.
- Import/setup wizards.
- Project templates.
- Debug/profiling surfaces.

The editor can depend on engine internals only through explicit editor-facing APIs. If a panel needs private renderer data, expose that through `Diagnostics` or `RendererDebug`, not by poking random internals.

### Player App

The player app is the shipping runtime shell.

It owns:

- Project loading.
- Game startup.
- Runtime-only layers.
- Platform packaging.
- Crash/error reporting.

The player must not depend on editor code.

### Sandbox

The sandbox is a small public API test.

It owns:

- Minimal examples.
- Rendering experiments.
- Public API sanity checks.
- Reproduction cases.

If a feature cannot be demonstrated from the sandbox or an integration test, the public API is probably too editor-dependent.

## Entry Point Pattern

Use a Hazel-style client hook:

```cpp
namespace Engine
{
    Application* CreateApplication(ApplicationCommandLineArgs args);
}
```

Engine-owned entry point:

```text
init logging
init profiler
parse command line
create application
run application
shutdown application
flush profiler/logs
```

Each app supplies only its specialization:

```cpp
class EditorApplication final : public Engine::Application
{
public:
    EditorApplication(const Engine::ApplicationSpecification& spec)
        : Engine::Application(spec)
    {
        PushLayer(new EditorLayer());
    }
};
```

Keep the entry point boring and stable. Do not put editor startup logic, renderer feature selection, asset import, or project loading directly in `main`.

## Layer Model

Use layers for app-level orchestration, not for low-level engine ownership.

Recommended layer types:

| Layer | Owner | Purpose |
| --- | --- | --- |
| `GameLayer` | Player/Sandbox | Game simulation entry point. |
| `EditorLayer` | Editor | Editor viewports, panels, gizmos, scene state. |
| `ImGuiLayer` or `ToolUiLayer` | Editor/Debug | Immediate-mode tools and debug UI. |
| `AutomationLayer` | Editor | Guided workflows and AI-agent task execution. |
| `DebugOverlayLayer` | Any app | Metrics, debug views, profiler HUD. |
| `CaptureLayer` | Tools | Screenshots, golden-image testing, capture automation. |

Layer lifecycle:

```cpp
OnAttach()
OnDetach()
OnUpdate(Timestep ts)
OnFixedUpdate(FixedTimestep ts)
OnRender(RenderContext& ctx)
OnUiRender()
OnEvent(Event& event)
```

Hazel has a simpler `OnUpdate`, `OnImGuiRender`, and `OnEvent` model. We should keep the simplicity but split rendering from simulation because this engine has a heavier renderer and guided tooling.

Rules:

- Layers are ordered.
- Overlays sit above normal layers.
- Events propagate from top to bottom.
- A handled event stops propagation.
- Layers should not own global engine systems.
- Long-running automation tasks should schedule jobs, not block the layer update.

## Event System

Start with blocking immediate events like Hazel, then evolve to queues where needed.

Immediate events are good for:

- Window close/resize.
- Keyboard/mouse/gamepad input.
- Editor UI routing.
- Shortcut handling.

Queued events are better for:

- Asset import completion.
- Shader compilation completion.
- AI-agent task progress.
- Background validation.
- File watcher updates.
- Network/collaboration events.

Keep event categories bitmasked:

```text
Application
Input
Keyboard
Mouse
Gamepad
Window
Editor
Asset
Renderer
Automation
```

Avoid a single global event bus becoming a dumping ground. Prefer explicit queues owned by subsystems, with editor-visible diagnostics.

## Module Boundaries

### Core

Allowed dependencies: C++ standard library, platform detection, logging, asserts, profiling.

Must not depend on renderer, scene, editor, scripting, or assets.

Contains:

- `Base.h`
- `Assert.h`
- `Log.h`
- `Profiler.h`
- `Memory.h`
- `UUID.h`
- `Timestep.h`
- `CommandLine.h`

### Application

Depends on Core, Events, Platform, UI layer interface.

Contains:

- `Application`
- `ApplicationSpecification`
- `Layer`
- `LayerStack`
- `Window`
- `Input`

### Platform

Implements OS-specific services.

Contains:

- `WindowsWindow`
- `WindowsInput`
- `FileDialog`
- `DynamicLibrary`
- `Threading`
- `Process`
- `Filesystem`

Do not leak Win32/GLFW/SDL headers through public engine headers.

### Jobs

Owns native multithreading.

Contains:

- Worker threads.
- Work stealing queues.
- Frame task graph.
- Job dependencies.
- Fiber/coroutine integration if adopted.
- Async asset/shader/build task queues.
- Profiling lanes.

Jobs must not know about editor panels, gameplay entities, or renderer internals. Those systems submit work through public job/task APIs.

### RHI

Low-level GPU abstraction.

Contains:

- Device.
- Queues.
- Swapchain.
- Buffers/images/samplers.
- Descriptors.
- Pipelines.
- Command lists.
- Barriers.
- Timing queries.

RHI is not the renderer. It should not know about scenes, entities, materials, or lights.

### RenderGraph

Owns frame pass scheduling.

Contains:

- Pass declaration.
- Resource lifetime.
- Barriers.
- Transient attachments.
- Async compute scheduling.
- Debug capture names.

### Renderer

Owns high-level render systems.

Contains:

- Visibility buffer.
- Compact G-buffer.
- Clustered lighting.
- Ray residual passes.
- Shadow maps.
- Material resolve.
- Post/AA.
- Debug views.

Renderer depends on RHI and RenderGraph. Scene submits render packets to Renderer; Renderer should not own gameplay entities.

### Scene

Owns runtime world data.

Contains:

- ECS/entity wrapper.
- Components.
- Archetype/chunk runtime storage.
- Scene-object authoring facade.
- Bake/convert path from editor objects to runtime chunks.
- Scene serialization.
- Runtime state.
- Scene update/simulation hooks.

Scene should not call editor panels. Editor inspects scene through public APIs.

### Assets

Owns asset identity, metadata, import settings, cooked outputs, and dependency tracking.

Contains:

- Asset registry.
- Import settings.
- Cooked asset database.
- Texture/material/mesh/animation importers.
- Versioned build artifacts.
- Reimport and validation.

Assets should not render. Renderer consumes cooked GPU-ready data.

### Scripting

Owns user-facing C# scripting and native bindings.

Contains:

- .NET host integration through `hostfxr`.
- C# assembly loading and hot reload.
- Script lifecycle.
- Source-generated bindings.
- Component metadata.
- Restricted C# job API.
- Script diagnostics and profiler hooks.

Scripting should expose stable engine handles and generated APIs. It must not expose raw native pointers or private renderer/RHI internals to gameplay code.

### Graphs

Owns visual scripting front-ends.

Contains:

- Gameplay graph compiler.
- Animation graph compiler.
- Material graph to Slang compiler.
- Automation workflow graph.
- Source maps from generated code/IR to graph nodes.

Graphs are editor/user-facing front-ends. Hot runtime behavior compiles to C#, native IR, Slang, or job graph nodes rather than a slow graph interpreter.

### Automation

Owns guided workflows and AI-agent bridge points.

Contains:

- Workflow definitions.
- Task graph for editor automation.
- Agent tool descriptions.
- Validation checkpoints.
- Explainability output.
- Undoable generated changes.

Automation must use normal editor/engine APIs. It should not secretly mutate project files in ways the editor cannot explain.

## Naming Conventions

Hazel-style naming to adopt:

- Types: `PascalCase`.
- Functions: `PascalCase`.
- Member variables: `m_Name`.
- Static variables: `s_Name`.
- Constants: `kName` or `constexpr Name` consistently; choose one per module.
- Namespaces: `Engine`, then subsystem namespaces only when they reduce ambiguity.
- Files: one primary type per `.h/.cpp` pair where practical.
- Public engine include: `Engine.h`.
- Platform implementation names: `WindowsWindow`, `VulkanDevice`, `D3D12Device`.
- Events: `WindowResizeEvent`, `KeyPressedEvent`, `AssetImportedEvent`.
- Components: `TransformComponent`, `CameraComponent`, `MeshRendererComponent`.
- Systems: `AnimationSystem`, `PhysicsSystem`, `RenderSubmissionSystem`.

Macro prefix should be project-specific, not `HZ_`.

Example:

```cpp
#define GE_ASSERT(...)
#define GE_CORE_ASSERT(...)
#define GE_PROFILE_FUNCTION()
#define GE_BIND_EVENT_FN(fn)
```

Choose the final prefix before code starts and use it everywhere.

## Ownership Helpers

Hazel uses `Scope` and `Ref` aliases for `unique_ptr` and `shared_ptr`. Adopt that readability, but keep ownership stricter:

```cpp
template<typename T>
using Scope = std::unique_ptr<T>;

template<typename T>
using Ref = std::shared_ptr<T>;
```

Rules:

- Prefer `Scope` for owned engine systems.
- Use `Ref` for shared asset/resource handles only when lifetime is truly shared.
- Use explicit handles/IDs for assets, GPU resources, entities, and jobs.
- Avoid raw owning pointers.
- Non-owning raw pointers are allowed only when lifetime is obvious and documented by ownership context.

## Public Include Style

Have one umbrella include for client apps:

```cpp
#include <Engine.h>
#include <Engine/Core/EntryPoint.h>
```

`Engine.h` should expose stable public API only:

- Application.
- Layer.
- Input.
- Events.
- Scene/entity public API.
- Components.
- Asset handles.
- Renderer public handles/debug toggles.
- Scripting public hooks.

Do not include heavy private renderer/RHI internals in `Engine.h`.

## Editor Structure

Use a Hazel/Hazelnut-style `EditorLayer`, but do not let one file become the whole editor.

Editor modules:

```text
EditorLayer
Panels/
  SceneHierarchyPanel
  InspectorPanel
  ContentBrowserPanel
  ProfilerPanel
  RenderDebugPanel
  AssetImportPanel
Workflows/
  NewProjectWorkflow
  FirstPlayableWorkflow
  VisualStyleWorkflow
  PerformanceWorkflow
Viewports/
  SceneViewport
  GameViewport
Gizmos/
Inspectors/
Automation/
Style/
```

Rule: `EditorLayer` coordinates. It should not contain all editor behavior.

## Coding Style

Use a simple Hazel-like style:

- C++20 or newer.
- `.h` and `.cpp` pairs.
- `#pragma once`.
- LF line endings.
- Tabs for indentation if we want to stay Hazel-like; spaces are acceptable only if chosen globally before code starts.
- Opening braces on the next line for namespaces/classes/functions.
- Keep includes ordered: local header, engine headers, third-party headers, standard headers.
- Keep public headers light.
- Use forward declarations in headers when practical.
- Avoid large source files. Split by responsibility once a file becomes hard to scan.
- Use assertions aggressively in engine code.
- Add profiler scopes to app loop, asset import, render passes, job systems, and editor workflows.
- Prefer explicit initialization defaults in structs.

## Build And Scripts

Hazel uses Premake and setup scripts. We can copy the philosophy, not necessarily the exact build system.

Required scripts:

```text
Scripts/
  Setup.ps1
  Setup.bat
  GenerateProjects.ps1
  Build.ps1
  Test.ps1
  Format.ps1
  ValidateAssets.ps1
  CompileShaders.ps1
```

Build-system requirements:

- One command prepares the repository.
- One command regenerates projects.
- One command builds editor/player/tools/tests.
- One command runs validation.
- Missing SDKs produce clear repair instructions.
- Asset compiler, shader compiler, and project generator are first-class build targets.

## AI-Agent Guardrails

Because this project expects AI-assisted development, the skeleton must include guardrails:

- Every module has an `OWNERSHIP.md` explaining responsibilities and forbidden dependencies.
- Every public subsystem has a short `README.md`.
- New files must land in an existing module or create a module with ownership docs.
- Cross-module calls should go through public interfaces.
- Generated code must be labeled and regenerable.
- AI workflow changes must be undoable and visible in editor history.
- Render/asset/automation changes need validation hooks from day one.

This is the real reason to use Hazel's style: not nostalgia, but coherence.

## First Implementation Order

1. Root workspace and build scripts.
2. Engine library with Core, Application, Events, Platform.
3. Editor app with `EditorLayer`.
4. Sandbox app with `ExampleLayer`.
5. Logging, asserts, profiling, command-line args.
6. Window/input/event path.
7. Native job system and frame task graph.
8. Minimal archetype/chunk scene runtime.
9. RHI stub and renderer interface.
10. Asset registry stub.
11. C#/.NET host and `ScriptCore`.
12. Source-generated bindings and C# component lifecycle.
13. Automation workflow stub.
14. Graph compiler stub.
15. Render architecture prototype from `LOW_LEVEL_ARCHITECTURE.md`.

Do not begin with the advanced renderer before the app/editor/sandbox shell exists. The shell is what keeps the project coherent while the hard systems grow.

## Sources

- Public Hazel GitHub repository: https://github.com/TheCherno/Hazel
- Hazel public README: https://github.com/TheCherno/Hazel/blob/master/README.md
- Hazel official site: https://hazelengine.com/
- Hazel getting-started docs: https://docs.hazelengine.com/Welcome/GettingStarted
- Hazel game engine code review syllabus: https://www.classcentral.com/course/youtube-hazel-my-game-engine-code-review-140820
- The Cherno Game Engine series: https://thecherno.com/engine
