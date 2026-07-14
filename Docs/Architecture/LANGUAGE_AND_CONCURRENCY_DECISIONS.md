# Language And Concurrency Decisions

Status: Draft v0.1
Date: 2026-07-06

Purpose: choose the engine implementation language, user-facing scripting language, visual scripting model, shader language, and multithreaded runtime architecture.

## Short Decisions

| Area | Decision |
| --- | --- |
| Engine core | Modern C++23/26-style C++ with strict engine conventions. |
| User gameplay scripting | C# on .NET 10 LTS, hosted by the C++ engine through `hostfxr`/CoreCLR. |
| Hot data systems | DOTS-like C++/C# restricted jobs over chunk/archetype data, not arbitrary object callbacks. |
| Visual scripting | Blueprint-like graph editor over the same C#/IR system, not a separate slow VM silo. |
| Verse-like direction | Adopt concepts later: transactions, safe concurrency, persistence, command buffers. Do not build a custom Verse first. |
| Shader language | Slang-first, HLSL-compatible shader authoring, with generated targets for D3D12/Vulkan through NVRHI. |
| Tools/offline automation | C# and Python are allowed for tools. Runtime/gameplay defaults to C# plus native engine APIs. |
| ECS/DOTS | Use DOTS principles: archetype/chunk storage, job scheduling, data-oriented transforms. Do not copy Unity DOTS wholesale. |

The current workspace compiles as C++20. “C++23/26-style” describes the long-term language direction, not permission to raise the repository toolchain standard without an explicit build/CI decision in `PLAN.md` and the dependency/toolchain documentation.

## Core Engine Language

Decision:

```text
Engine core: C++.
```

Reasons:

- NVRHI, NRI, mesh/RT/GPU vendor APIs, physics libraries, profilers, asset libraries, and OS/platform APIs are all native-first.
- The renderer needs deterministic memory, explicit lifetime, SIMD-friendly data layouts, and no GC pauses.
- The engine must own resource lifetime, streaming, descriptors, command recording, and render graph behavior.
- C++ lets the engine integrate third-party native libraries without constant FFI cost.

Rules:

- Use modern C++ with narrow module boundaries and clear ownership.
- Avoid template metaprogramming cleverness in gameplay-facing APIs.
- Avoid global singletons except for controlled engine services.
- Use handles/IDs for assets, entities, jobs, GPU resources, and scripts.
- Keep scripting/editor APIs stable and generated from engine metadata.

Rejected:

- **Rust core first**: attractive for safety, but too much friction with current graphics/game middleware and C++ ecosystem for this first engine.
- **C# core first**: good productivity, but wrong for the renderer/RHI/streaming/RT foundation.
- **Zig/Odin/Jai-style core first**: interesting, but toolchain/ecosystem risk is too high for the first production-oriented architecture.

## User Gameplay Language

Decision:

```text
Primary user-facing gameplay language: C# on .NET 10 LTS.
```

Why:

- Familiar to Unity developers, small studios, students, and solo creators.
- Strong static typing and excellent IDE/debugger support.
- Good AI-code assistance because it is textual, structured, and mainstream.
- Faster and safer than Lua/Python-style dynamic scripting for gameplay.
- More approachable than C++ for most creators.
- .NET has official native-hosting support through `hostfxr`.
- .NET 10 is the current LTS line as of 2026 and is supported to November 2028.

User workflow:

```text
create component/script -> edit C# -> compile in editor -> hot reload assembly -> run in editor/player
```

Example style:

```csharp
using Engine;

public sealed partial class DoorController : Component
{
    public Entity Door;
    public float OpenAngle = 90.0f;

    protected override void OnStart()
    {
        Log.Info("Door ready");
    }

    protected override void OnUpdate(float dt)
    {
        if (Input.Pressed(Key.E))
            Door.RotateLocalY(OpenAngle);
    }
}
```

The C# API should feel like Unity/Godot simplicity, but underneath it should write command buffers and typed engine handles rather than poking unsafe engine internals.

## Embedding .NET

Implementation path:

1. C++ engine hosts .NET through `hostfxr`.
2. Editor uses Roslyn/MSBuild to compile project scripts.
3. Scripts compile to managed assemblies.
4. Assemblies load through collectible `AssemblyLoadContext` so editor hot reload is possible.
5. Source generators produce:
   - component registration,
   - reflection metadata,
   - native binding glue,
   - serialization descriptors,
   - job-system access metadata.
6. Engine exposes native APIs through generated bindings and stable handles.
7. Shipping builds are self-contained .NET runtime bundles by default.
8. NativeAOT is optional for tools/server/static bundles, not the first hot-reload gameplay path.

Rules:

- Editor hot reload must shadow-copy assemblies so source builds can replace binaries.
- Script unloading must be tested; no static event leaks or unmanaged references keeping old assemblies alive.
- Reflection is allowed in editor tooling, but runtime should use generated metadata.
- C# object references are not entity identity. Entity handles are stable IDs.
- Managed scripts may schedule jobs only through safe engine APIs.

## Verse-Like Language Direction

Verse is important because Epic is moving Unreal's future gameplay programming model toward it, with transactional concurrency and large persistent worlds as explicit goals.

Do not build a custom Verse clone first.

Reasons:

- A new language is a multi-year toolchain project.
- It needs compiler, debugger, formatter, language server, package system, docs, learning material, and AI/tooling integration.
- C# already gives us adoption, tooling, and scripts quickly.

Adopt Verse-like ideas instead:

- Transactional world mutations through command buffers.
- Deterministic async tasks.
- Safe concurrency by default.
- Persistent-world APIs.
- Event streams and resumable gameplay flows.
- Clear failure/rollback semantics.

Future path:

```text
C# gameplay API
  -> graph compiler / gameplay IR
  -> optional Verse-like domain language later
```

## Visual Scripting

Decision:

```text
Support visual scripting, but make it a graph editor over real generated code/IR.
```

What this means:

- Beginners can use Blueprint-like nodes.
- Designers can create gameplay, animation, AI, triggers, UI, and automation flows.
- Graphs compile to C# partial classes, native IR, or job graph nodes depending on domain.
- Hot paths do not run in a slow interpreted visual scripting VM.
- Generated code/IR has source maps back to graph nodes.
- AI agent can edit graphs and/or generated text safely.

Graph categories:

| Graph | Compile target |
| --- | --- |
| Gameplay Flow Graph | C# partial class or gameplay IR. |
| Behavior/AI Graph | Behavior IR plus C# extension points. |
| Animation Graph | Native animation task graph. |
| Material/Shader Graph | Slang shader modules. |
| VFX Graph | GPU/CPU simulation graph. |
| Automation Workflow Graph | Editor task graph. |

Why not Blueprint-only:

- Harder to diff, merge, search, test, and AI-edit than text.
- VM overhead can become a gameplay tax.
- Logic can become opaque to programmers and automation tools.

Why not code-only:

- The engine's product goal is to win amateurs and small studios.
- Guided visual workflows are essential.

Rule:

```text
Visual scripting is a front-end. The runtime contract is generated, typed, inspectable code or IR.
```

## Shader Language

Decision:

```text
Use Slang-first shader authoring with HLSL-compatible style.
```

Reasons:

- Slang is HLSL-compatible and fits D3D12/Vulkan targets.
- It supports large modular shader codebases.
- It can generate multiple backends including HLSL, SPIR-V, Metal, WGSL, CUDA, C++, and CPU targets.
- It supports modern features useful to this engine: generics/interfaces, ray tracing, differentiable/neural graphics paths.
- Khronos hosts the Slang initiative, reducing single-vendor risk.

Rules:

- Author engine shaders in Slang/HLSL style.
- Material graph emits Slang modules.
- Validate generated shader layouts against C++/C# structs.
- Keep backend-specific escape hatches for shader features not expressible cleanly yet.

Current implementation scope: the Windows x86_64/MSVC D3D12 viewport compiles its disk-backed Slang/HLSL-style source with pinned Slang v2026.13.1 and pinned downstream DXC v1.9.2602 into one validated DXIL+SPIR-V package per stage. D3D12 consumes DXIL; SPIR-V is retained as reflected and convention-validated portability evidence until the Vulkan scene-RHI path consumes it. Normal runtime compilation is asynchronous through the job system, while deterministic-inline mode is reserved for smoke tests. Live source-change pipeline rebuild, non-MSVC host qualification, Vulkan scene execution, and redistribution clearance remain separate pending work.

## DOTS Assessment

Unity DOTS is the right family of ideas:

- ECS data layout.
- Jobs.
- Burst-compiled high-performance code.
- Deterministic, memory-aware systems.
- Explicit system dependencies.

But DOTS is not the exact thing to copy.

Problems with copying DOTS directly:

- It is deeply tied to Unity's editor, packages, Burst compiler, Entities package, baking workflow, and GameObject bridge.
- It can be hard for beginners when exposed raw.
- It pushes users into data-oriented thinking even when a simple object workflow would be enough.

Decision:

```text
Use a hybrid DOTS-like runtime: object/scene facade for users, archetype/chunk ECS and jobs underneath.
```

This gives:

- Beginner-friendly authoring.
- High-performance runtime.
- Automatic conversion/baking to data-oriented storage.
- AI/guided workflow compatibility.
- Escape hatches for expert data systems.

## ECS And Job Runtime

Core runtime model:

```text
Scene objects/components in editor
  -> bake/convert
  -> archetype/chunk runtime world
  -> scheduled systems/jobs
  -> command buffers publish structural changes
```

Runtime ECS rules:

- Store high-scale entity data in archetype/chunk SoA-style storage.
- Structural changes happen through command buffers.
- Systems declare read/write component access.
- Scheduler runs non-conflicting systems in parallel.
- Transform, animation, physics, visibility, audio, AI, and particles use data-oriented pipelines.
- Renderer consumes immutable frame snapshots.
- Gameplay scripts can use object-style facades, but hot systems compile down to data jobs where possible.

Do not put everything into ECS:

- Renderer has its own GPU data model.
- Physics has islands/broadphase/narrowphase data.
- Animation has skeleton/clip/pose caches.
- Asset database has dependency graphs.
- Editor UI has document/panel state.

Use ECS where entity-scale parallelism matters.

## C# Jobs

User C# jobs should exist, but as a restricted subset.

Allowed in jobs:

- Blittable/value components.
- Engine-provided native containers.
- Entity handles.
- Read/write component access declared up front.
- Deterministic math.
- Command buffers for structural changes.

Forbidden in hot jobs:

- Managed allocations.
- Arbitrary object graph traversal.
- Reflection.
- Blocking I/O.
- Calling editor UI.
- Calling arbitrary engine APIs that are not job-safe.

Implementation:

- Source generator validates job signatures and component access.
- Scheduler builds dependencies from declared reads/writes.
- Editor shows why jobs cannot run in parallel.
- Shipping builds can ahead-of-time compile hot assemblies where practical.

## ECS Library Choice

Do not commit the engine to Flecs, EnTT, Bevy ECS, or Unity DOTS as a dependency yet.

Use them as references:

- **Flecs**: strong C/C++ ECS database model, relationships, pipelines, good reference for explorer/tooling.
- **EnTT**: small, fast, header-only C++ ECS/reference for simple native ECS patterns.
- **Bevy ECS**: excellent schedule/dependency and archetype design reference.
- **Unreal MassEntity**: useful reference for gameplay-scale data-oriented crowds/agents.
- **Unity DOTS**: strongest reference for C# jobs, Burst-like restricted code, baking, and ECS workflow.

Decision:

```text
Prototype with references, but build an engine-owned ECS/runtime once requirements are clear.
```

Why:

- We need C# bindings, editor serialization, asset baking, guided workflows, AI diagnostics, streaming, undo/redo, and renderer/physics integration.
- Those requirements will shape the data model.
- A dependency can help prototypes, but the core runtime contract should be ours.

## First Implementation Order

1. Engine core in C++ with `Core`, `Jobs`, `Application`, `Platform`.
2. C++ job system and frame task graph.
3. Minimal archetype/chunk ECS prototype.
4. C#/.NET 10 host through `hostfxr`.
5. C# project build using MSBuild/Roslyn.
6. Source-generated component registration and bindings.
7. Script hot reload through collectible `AssemblyLoadContext`.
8. C# component lifecycle: `OnStart`, `OnUpdate`, events.
9. Restricted C# job API with read/write declarations.
10. Visual gameplay graph compiling to C# partials or gameplay IR.
11. Slang shader pipeline.
12. DOTS-like bake pipeline from editor scene objects to runtime chunks.

## Final Position

The engine should be:

```text
C++ native engine
  + C#/.NET gameplay scripting
  + graph front-ends that compile to typed code/IR
  + Slang shader language
  + DOTS-like data runtime and native job system
```

This balances power and accessibility. It gives beginners a guided visual/editor path, Unity developers a familiar C# path, expert programmers native control, and the renderer/streaming systems the low-level C++ control they need.

## Sources

- .NET support policy: https://dotnet.microsoft.com/en-us/platform/support/policy/dotnet-core
- Microsoft custom .NET runtime host: https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting
- .NET native hosting design: https://github.com/dotnet/runtime/blob/main/docs/design/features/native-hosting.md
- Assembly unloadability / `AssemblyLoadContext`: https://learn.microsoft.com/en-us/dotnet/standard/assembly/unloadability
- Native AOT deployment: https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/
- Roslyn incremental generators: https://github.com/dotnet/roslyn/blob/main/docs/features/incremental-generators.md
- Godot C#/.NET docs: https://docs.godotengine.org/en/stable/tutorials/scripting/c_sharp/index.html
- Coral CoreCLR wrapper: https://github.com/StudioCherno/Coral
- Unreal Engine Blueprints docs: https://dev.epicgames.com/documentation/unreal-engine/blueprints-visual-scripting-in-unreal-engine
- Epic Road to UE6 / Verse direction: https://www.unrealengine.com/news/the-road-to-ue-6
- Epic State of Unreal 2026: https://www.unrealengine.com/news/state-of-unreal-2026-top-news-from-the-show
- Verse programming docs: https://dev.epicgames.com/documentation/fortnite/programming-with-verse-in-unreal-editor-for-fortnite
- Unity ECS overview: https://unity.com/ecs
- Unity DOTS overview: https://unity.com/dots
- Unity Entities job system docs: https://docs.unity3d.com/Packages/com.unity.entities@1.0/manual/systems-scheduling-jobs.html
- Unreal MassEntity docs: https://dev.epicgames.com/documentation/unreal-engine/mass-entity-in-unreal-engine
- Flecs docs: https://www.flecs.dev/flecs/
- EnTT ECS docs: https://github.com/skypjack/entt/wiki/Entity-Component-System
- Bevy schedule docs: https://docs.rs/bevy/latest/bevy/ecs/schedule/index.html
- Slang shader language: https://shader-slang.org/
- Slang GitHub: https://github.com/shader-slang/slang
- Khronos Slang initiative: https://www.khronos.org/news/press/khronos-group-launches-slang-initiative-hosting-open-source-compiler-contributed-by-nvidia
