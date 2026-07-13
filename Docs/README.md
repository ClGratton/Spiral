# Project Documentation Map

Status: Authoritative catalog
Date: 2026-07-13

This file tells contributors and AI agents where project knowledge lives. It catalogs every workspace-authored Markdown document, identifies its role, and prevents implementation order or completion state from depending on chat context.

## Authority Order

When documents overlap, use this order:

1. `AGENTS.md` for repository workflow, scope, and documentation-maintenance rules.
2. `PLAN.md` for current state, execution order, required work, and checkboxes.
3. `PRODUCT.md` and `DESIGN.md` for product and editor-experience requirements.
4. Accepted ADRs and required architecture contracts for technical decisions.
5. Living dependency, verification, roadmap-governance, UI-review, and ownership documents for their named domains.
6. Draft research, evaluations, and historical skeletons as rationale/reference only.

Code, tests, captures, and completed CI jobs are evidence of actual behavior. If evidence contradicts prose, correct the authoritative document instead of preserving a false completion claim.

## Core Documents

| File | Purpose | Update when |
| --- | --- | --- |
| [Docs/README.md](README.md) | Complete Markdown catalog, authority order, and documentation update matrix. | Any Markdown file, document role, authority, or update responsibility changes. |
| [AGENTS.md](../AGENTS.md) | Persistent agent workflow, authority, scopes, verification, roadmap, portability, and documentation rules. | Workflow, scopes, authority, model policy, or required documents change. |
| [PLAN.md](../PLAN.md) | Only execution-order and completion-status authority. | Behavior, prerequisites, order, current state, verification, or completion changes. |
| [PRODUCT.md](../PRODUCT.md) | Product users, purpose, principles, and accessibility goals. | Product direction or user promises change. |
| [DESIGN.md](../DESIGN.md) | Editor visual system and interaction contracts. | Editor layout, styling, or interaction rules change. |
| [README.md](../README.md) | Build/run quick start and concise current implementation summary. | Commands, dependencies, platform status, or primary navigation changes. |
| [Docs/ROADMAP_GOVERNANCE.md](ROADMAP_GOVERNANCE.md) | Checkmark and phase-completion rules. | Roadmap evidence policy changes. |
| [Docs/VERIFICATION.md](VERIFICATION.md) | Local and hosted verification matrix. | Test commands, smoke coverage, platform evidence, or required gates change. |
| [Docs/DEPENDENCIES.md](DEPENDENCIES.md) | Dependency/version/license/integration ledger. | A dependency is admitted, removed, upgraded, repurposed, or packaged differently. |
| [Docs/EDITOR_UI_REVIEW.md](EDITOR_UI_REVIEW.md) | Current editor UX review, resolved issues, and follow-ups. | Editor-facing behavior or review findings change. |

## Architecture Documents

Technical documents are indexed in recommended reading order by [Architecture/README.md](Architecture/README.md).

| File | Classification | Scope |
| --- | --- | --- |
| [Docs/Architecture/README.md](Architecture/README.md) | Architecture index | Recommended reading order and link into the technical decision set. |
| [Docs/Architecture/ENGINE_INSTRUCTIONS.md](Architecture/ENGINE_INSTRUCTIONS.md) | Vision contract | Renderer/product north star, hard rules, pass layout, quality gates, and profiling goals. |
| [Docs/Architecture/ARCHITECTURE_COHERENCE_AUDIT.md](Architecture/ARCHITECTURE_COHERENCE_AUDIT.md) | Living audit | Cross-document conflicts, missing prerequisites, and intentional open design work. |
| [Docs/Architecture/LOW_LEVEL_ARCHITECTURE.md](Architecture/LOW_LEVEL_ARCHITECTURE.md) | Design contract | Renderer shape, buffers, textures, geometry, descriptors, and pass graph. |
| [Docs/Architecture/RENDERER_IMPLEMENTATION_CONTRACTS.md](Architecture/RENDERER_IMPLEMENTATION_CONTRACTS.md) | Required contract | Exact visibility, culling, streaming, AA, lighting, and runtime implementation rules. |
| [Docs/Architecture/RENDER_GRAPH_ARCHITECTURE.md](Architecture/RENDER_GRAPH_ARCHITECTURE.md) | Required contract | Graph construction, execution, state ownership, synchronization, transient lifetime, and verification. |
| [Docs/Architecture/RENDERER_CAPABILITY_CONTRACT.md](Architecture/RENDERER_CAPABILITY_CONTRACT.md) | Required contract | Adapter selection, required/optional capabilities, enabled features, fallbacks, and qualification evidence. |
| [Docs/Architecture/TECHNICAL_ROADMAP_COVERAGE.md](Architecture/TECHNICAL_ROADMAP_COVERAGE.md) | Required traceability contract | Maps accepted renderer/frame-stability research to ordered roadmap infrastructure and feature phases. |
| [Docs/Architecture/RHI_AND_LIGHTING_DECISIONS.md](Architecture/RHI_AND_LIGHTING_DECISIONS.md) | Decision contract | NVRHI/RHI boundary, presentation ownership, uploads, daylight, lighting data, and pacing. |
| [Docs/Architecture/MACOS_RENDERER_BACKEND_DECISION.md](Architecture/MACOS_RENDERER_BACKEND_DECISION.md) | Accepted ADR | MoltenVK/NVRHI choice, measured portability gaps, packaging, and macOS completion gates. |
| [Docs/Architecture/PROBE_LIGHTING_AND_GI_DECISIONS.md](Architecture/PROBE_LIGHTING_AND_GI_DECISIONS.md) | Decision contract | Probe/light-field GI, static/dynamic consistency, zones, AO, volumetrics, and debug requirements. |
| [Docs/Architecture/RENDERING_FEATURE_AND_PERFORMANCE_DECISIONS.md](Architecture/RENDERING_FEATURE_AND_PERFORMANCE_DECISIONS.md) | Decision contract | Reflections, AO, shadows, optional accelerators, profiling, batching, and topology. |
| [Docs/Architecture/LOD_TRANSITIONS_AND_MULTITHREADING_DECISIONS.md](Architecture/LOD_TRANSITIONS_AND_MULTITHREADING_DECISIONS.md) | Decision contract | Stable LOD transitions, scan conversion, job/task architecture, and compiled graphs. |
| [Docs/Architecture/LANGUAGE_AND_CONCURRENCY_DECISIONS.md](Architecture/LANGUAGE_AND_CONCURRENCY_DECISIONS.md) | Decision contract | C++ core, C# host, visual graphs, Slang direction, ECS, and jobs. |
| [Docs/Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md](Architecture/TERRAIN_ARCHITECTURE_AND_RESEARCH.md) | Accepted planning contract | Project-selectable terrain profiles, source/artifact contracts, generation, streaming, LOD, collision, edits, provenance, Terrain Diffusion evaluation, and verification. |
| [Docs/Architecture/PHYSICS_ARCHITECTURE_AND_RESEARCH.md](Architecture/PHYSICS_ARCHITECTURE_AND_RESEARCH.md) | Accepted planning contract | Physics authority, fixed stepping, backend bake-off, collision cooking, determinism levels, solver research tiers, GPU synchronization, fallbacks, and qualification. |
| [Docs/Architecture/KTX2_BASIS_TEXTURE_IMPORT_PLAN.md](Architecture/KTX2_BASIS_TEXTURE_IMPORT_PLAN.md) | Accepted import contract | KTX2/Basis roles, cooking, streaming, libktx boundary, and validation. |
| [Docs/Architecture/MISSING_RESEARCH_AUDIT_2026.md](Architecture/MISSING_RESEARCH_AUDIT_2026.md) | Research addendum | Optional accelerators, future GPU execution, geometry compression, and standards. |
| [Docs/Architecture/RESEARCH_DECISIONS.md](Architecture/RESEARCH_DECISIONS.md) | Research rationale | Broad renderer/physics discoveries and prototype questions. |
| [Docs/Architecture/HAZEL_ENGINE_EVALUATION.md](Architecture/HAZEL_ENGINE_EVALUATION.md) | Reference evaluation | Why Hazel is a reference rather than the engine base. |
| [Docs/Architecture/HAZEL_INSPIRED_SKELETON.md](Architecture/HAZEL_INSPIRED_SKELETON.md) | Historical/reference structure | Naming and modularity guidance. Its proposed tree and implementation order are not the current repository map or roadmap. |

Only `PLAN.md` controls implementation order. Any “first implementation order,” “prototype order,” or proposed directory tree inside an architecture/research document is rationale unless copied into `PLAN.md`.

## Ownership Documents

| File | Scope |
| --- | --- |
| [Engine/src/Engine/OWNERSHIP.md](../Engine/src/Engine/OWNERSHIP.md) | Engine module responsibilities and dependency direction. |
| [Engine/src/Engine/Core/OWNERSHIP.md](../Engine/src/Engine/Core/OWNERSHIP.md) | Stricter Core boundary. |
| [Engine/src/Engine/Jobs/OWNERSHIP.md](../Engine/src/Engine/Jobs/OWNERSHIP.md) | CPU worker, frame-task dependency, publication, and scheduling boundary. |
| [Editor/OWNERSHIP.md](../Editor/OWNERSHIP.md) | Editor-as-client boundary. |
| [Sandbox/OWNERSHIP.md](../Sandbox/OWNERSHIP.md) | Public API proving-ground boundary. |

If a subsystem grows rules that cannot be stated clearly in the engine-wide ownership table, add a local `OWNERSHIP.md` and list it here.

## Documentation Change Matrix

| Change | Required documentation |
| --- | --- |
| Runtime/editor behavior | `PLAN.md` current state and relevant checklist wording; relevant contract if behavior changes a rule. |
| Roadmap prerequisite/order | `PLAN.md` plus the architecture document explaining the dependency. |
| Backend/device capability | Renderer capability contract, relevant backend ADR, `PLAN.md` coverage wording, and verification matrix. |
| Dependency/version/license/package | `Docs/DEPENDENCIES.md`; architecture ADR if the choice changes. |
| Module/file responsibility | Nearest `OWNERSHIP.md`, `AGENTS.md` scope map, and this catalog. |
| Editor UX | `PRODUCT.md`/`DESIGN.md` as applicable and `Docs/EDITOR_UI_REVIEW.md`. |
| Build/test/CI command | Root `README.md`, `Docs/VERIFICATION.md`, and reusable scripts/workflows. |
| New Markdown file | This catalog; also `Docs/Architecture/README.md` when architectural. |

Do not duplicate volatile facts in multiple documents unless each copy is necessary for its reader. Prefer a concise link to the authoritative owner.
