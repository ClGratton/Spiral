# Hazel Engine Evaluation

Status: Draft v0.1
Date: 2026-07-06

Purpose: decide whether Hazel Engine should be used as the legal/technical base for this engine.

## Short Decision

Do not use Hazel as the main engine base.

Use Hazel as a reference for education, editor organization, project structure, C++/C# scripting integration ideas, and small reusable Apache-2.0 pieces only after dependency/license audit.

## What Hazel Is

Hazel is a C++ 3D game/application engine created by Studio Cherno/The Cherno. It started in 2018 as a public educational YouTube game-engine series and has since become a more serious Studio Cherno product.

Current public-facing Hazel direction:

- C++ engine.
- C# .NET scripting.
- Vulkan renderer with an API-agnostic renderer direction.
- Multi-threaded runtime with main/render thread split.
- Physically based Forward+ HDR renderer.
- Windows support, Linux experimental.
- Source access to the current product is through Patreon Supporter III or higher.
- Studio Cherno says the long-term plan is free prebuilt binaries, while source/development branches remain Patreon-funded.

There are effectively two Hazel meanings:

1. **Old public GitHub Hazel**: `TheCherno/Hazel`, educational, early-stage, Apache-2.0.
2. **Current Studio Cherno Hazel**: active product/development repository, source available through Patreon access, with its own access terms that must be reviewed separately before reuse.

## Legal Assessment

Not legal advice, but the practical reading is:

### Public GitHub Hazel

The public `TheCherno/Hazel` repository is licensed under Apache License 2.0.

That means we can generally:

- Use the code commercially.
- Modify it.
- Distribute source or binary derivatives.
- Sublicense our modifications under our own terms, as long as Apache-2.0 obligations are preserved.

Obligations and risks:

- Preserve license and copyright notices.
- Mark modified files.
- Include NOTICE attribution if the project has one.
- Do not imply trademark/name rights to "Hazel", "Studio Cherno", or related branding.
- Audit all third-party dependencies and submodules separately. Their licenses may differ.
- Keep a clean attribution ledger for any copied code.

### Current Patreon/Studio Cherno Hazel

Do not assume the current private/source-available Hazel code can be used as our engine base just because the old public repo is Apache-2.0.

Before using current Patreon-source Hazel code, we would need:

- The exact license text for the current repository.
- Patreon/subscription terms affecting source access and redistribution.
- Confirmation whether commercial derivative engine redistribution is allowed.
- Third-party dependency and asset license audit.

Until that is reviewed, the current private/source-available Hazel should be treated as reference-only, not copyable engine foundation.

## Technical Fit

Hazel has good engineering taste for a learning-oriented C++ engine:

- Simple, readable architecture.
- Editor/runtime split via Hazelnut/Sandbox style.
- C# scripting direction similar to Unity familiarity.
- Vulkan experience.
- Forward+ PBR renderer.
- Useful educational value.

But it is not aligned enough with this engine's north star to become the base:

| Area | Hazel Direction | Our Direction | Fit |
| --- | --- | --- | --- |
| Renderer | Forward+ HDR PBR. | Visibility buffer + compact G-buffer + clustered lighting + sparse ray residuals. | Weak. |
| Image philosophy | Conventional real-time PBR direction. | No temporal baseline, Callisto/Proxima BRDF, sharp motion. | Weak. |
| Geometry | Conventional engine path, not a Nanite-like cluster foundation. | Offline cluster hierarchy, anti-subpixel policy, analytic coverage. | Weak. |
| Product goal | Capable indie engine/product plus education. | Automation-first Unreal/Unity alternative for amateurs and small teams. | Partial. |
| Scripting | C# .NET. | Still plausible. | Good reference. |
| Editor architecture | Hazelnut-style editor. | Guided workflows, AI agent, automation, expert diagnostics. | Partial. |
| Legal clarity | Public old repo Apache-2.0; current source Patreon-gated. | Need clean commercial base. | Mixed. |

## Recommendation

Build our engine foundation independently.

Do use Hazel as:

- A study/reference engine.
- A comparison point for beginner-friendly C++ architecture.
- Inspiration for C# scripting integration.
- Inspiration for editor/runtime/project layout.
- Potential source for small Apache-2.0 utility patterns after copying is explicitly tracked.

Do not use Hazel as:

- The rendering foundation.
- The virtual geometry foundation.
- The asset pipeline foundation.
- The automation/AI workflow foundation.
- A legal base unless every copied file and dependency is audited.

## If We Borrow Anything

Use this checklist before copying code:

1. Confirm the code comes from the public Apache-2.0 repository, not a private Patreon-only branch.
2. Record source URL, commit hash, file path, and license.
3. Preserve required notices.
4. Mark modifications.
5. Audit submodule/dependency licenses.
6. Avoid Hazel branding, logos, and trademarks.
7. Prefer reimplementing ideas cleanly unless the code saves meaningful time.

## Final Position

Hazel is useful to learn from, but not the correct base for this project.

The project is trying to beat legacy engines on image stability, automation, workflow, BRDF/material quality, and low-level renderer architecture. Starting from Hazel would mostly give us a conventional C++/Vulkan/editor skeleton and then force us to replace the renderer, geometry path, asset pipeline, editor workflows, and product assumptions. That is not leverage; it is inherited refactor work.

The better path is:

```text
custom core renderer + custom asset automation + guided editor + optional C# scripting layer
```

Hazel can still be a valuable reference for how to keep C++ engine code readable and approachable.

## Sources

- Public Hazel GitHub repository: https://github.com/TheCherno/Hazel
- Public Hazel Apache-2.0 license: https://github.com/TheCherno/Hazel/blob/master/LICENSE
- Public Hazel README: https://github.com/TheCherno/Hazel/blob/master/README.md
- Hazel official site: https://hazelengine.com/
- Hazel getting-started docs: https://docs.hazelengine.com/Welcome/GettingStarted
- Hazel developer guide: https://docs.hazelengine.com/HazelForEngineers/DeveloperGuide
