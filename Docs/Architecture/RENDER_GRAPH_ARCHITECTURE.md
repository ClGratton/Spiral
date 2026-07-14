# Render Graph Architecture

**Status:** Required design contract
**Date:** 2026-07-12

## Why This Is A Separate Contract

The render graph is shared infrastructure for most renderer phases, not one renderer pass. Keeping its construction, execution, synchronization, and verification contract here prevents the Phase 3 checklist from implying that lifetime data alone is an executable graph.

## Current Foundation

The checked Phase 3 construction item is a compiler-only logical graph. A pass declares read, write, or read-write access with queue, required state, and shader-stage intent; compilation validates those declarations, derives RAW/WAR/WAW edges and declared ordering constraints, rejects invalid handles, ambiguous duplicate uses, transient read-before-write, and cycles, then produces stable topological order. Resource first/last-use intervals, state barriers, and cross-queue transition records are calculated from that order, not registration order.

The compiler deliberately does not:

- bind declared resources to imported or graph-owned RHI resources;
- record and execute pass callbacks;
- turn abstract transitions into RHI barriers or queue signal/wait commands;
- retire per-frame graph state and transient resources against GPU completion;
- drive the real scene viewport.

Those belong to the following ordered Phase 3 execution-core, cross-queue, and real-workflow integration items. The construction compiler is an authority for intended logical dependencies and state; it is not an execution path.

## Construction And Scheduling Contract

Each frame builds a graph from pass registrations. Every pass declares its resource reads and writes, required states, and queue class before compilation. Compilation must:

1. Validate resource handles, states, imported-resource contracts, and read/write hazards.
2. Derive dependency edges from declared data hazards rather than relying on registration order.
3. Reject cycles and unresolved read-before-write dependencies with actionable diagnostics.
4. Produce a deterministic topological pass order.
5. Compute first and last use in that resolved order.
6. Derive same-queue barriers and cross-queue ownership/synchronization transitions.

Execution must bind logical resources to physical `Engine::RHI` resources, invoke pass callbacks with a restricted execution context, record the compiled barriers and commands, and submit work in dependency order. Imported resources must declare initial and required final states. Queue transitions must include the required signal/wait relationship; a queue-name change alone is not synchronization.

Construction, single-queue execution core, cross-queue execution, and real-workflow adoption are separate roadmap gates. A compiled dependency/barrier plan does not complete execution. The core first binds real physical/imported resources, records same-queue work, and retires frame contexts by GPU completion; the next gate turns queue-dependency records into explicit signal/wait submission; the final gate moves a representative multi-pass Scene viewport onto that path and proves output equivalence before its bootstrap path is removed.

GPU-retired frame-context reuse depends on a backend-neutral completion boundary that did not exist when construction was completed: the original device contract exposed only synchronous `SubmitAndWait`, with no opaque submission token, completion query, or reusable recording lifecycle. Phase 3 therefore requires an explicit RHI prerequisite before the executor. `Device::Submit` returns after queue acceptance with a device-owned monotonic `CompletionToken`; `QueryCompletion` is nonblocking and `WaitForCompletion` has an explicit finite deadline. Tokens contain only opaque RHI device/submission identities, while each backend retains the corresponding native primitive. Tokens with zero identities, another device identity, or an unissued submission identity are invalid; completed tokens remain queryable for the current device lifetime.

D3D12 assigns each submitted list a per-queue fence value and retains that mapping behind the token. Vulkan submits through the existing NVRHI device and maps the returned NVRHI graphics submission instance to the token; completion is observed through NVRHI Vulkan's existing graphics timeline counter. Its bounded wait polls that counter only, so the adapter creates no second raw-Vulkan command or submission path. In both adapters, a command list records its own last token and `Begin` rejects allocator/recording reset until that token reports complete. `SubmitAndWait` is a compatibility wrapper over submit plus bounded completion wait so bootstrap consumers do not fork submission policy. CPU frame numbers and synchronous destroy-and-recreate are not acceptable substitutes because neither proves GPU retirement before reuse.

## Barrier Authority

The graph is the authority for intended logical resource states, pass dependencies, and queue ownership. `Engine::RHI` translates compiled transitions into backend commands. NVRHI automatic state tracking may remain a bootstrap implementation aid only under this rule:

- a graph-owned command list uses one declared mode for its whole recording lifetime;
- in automatic mode, the backend may ask NVRHI to emit barriers, but it must validate that the observed transitions match the compiled graph;
- in explicit mode, the graph-derived RHI transition batches are recorded and NVRHI automatic barriers are disabled for graph-owned resources/commands;
- a pass must never mix automatic and explicit ownership in a way that can emit duplicate, reordered, or contradictory transitions;
- imported-resource initial/final states and cross-queue signal/wait edges remain explicit regardless of backend automation.

Execution covers both textures and buffers. Because construction already emits buffer-state transitions, the backend-neutral command-list contract must expose explicit buffer transitions on D3D12 and Vulkan/NVRHI before the execution core can truthfully consume every compiled resource class. A texture-only executor is not an acceptable silent fallback; unsupported state/usage combinations must fail before submission with actionable diagnostics.

`CommandList::TransitionBuffer` records a whole-buffer transition from a compiled `ResourceState`; it accepts neither an inferred pass use nor a native state value. GPU-only buffers may transition to `Common`, compatible copy states, read-only `ShaderResource`, or storage `UnorderedAccess`; CPU-visible upload/readback buffers, `Unknown`, and texture-only states fail. D3D12 owns the native whole-resource barrier and tracked state, mapping vertex/index/constant read-only use to `GENERIC_READ`; Vulkan owns NVRHI `setBufferState` plus `commitBarriers` and tracks no-op repeats. These are backend translation details, not graph policy. Existing upload/copy helpers retain their scoped bootstrap barriers; when a destination is already explicitly `CopyDest`, they emit no duplicate transition.

The transition from bootstrap automatic barriers to explicit graph barriers is measured per backend. It does not change renderer pass declarations or permit native API state policy to leak above `Engine::RHI`.

## Transient Resource Contract

Transient allocation depends on the compiled graph above. Reuse is legal only when lifetimes do not overlap in resolved execution order and resource compatibility requirements are satisfied. Cross-frame reuse must be gated by GPU retirement, not CPU frame number. Aliasing or ownership barriers required by the backend must be emitted by the compiled graph.

Heap placement/aliasing is an optimization, not a correctness requirement. A backend/device that cannot safely use aliasing heaps must fall back to separately allocated or non-aliased pooled resources while preserving the same graph lifetimes and results. Diagnostics must report the selected allocation mode and its memory cost; the hosted MoltenVK `MTLHeap` workaround must not become an assumed production capability.

This is why the Phase 3 transient-allocation item follows frame/render graph construction.

## Downstream Dependencies

No roadmap items were reordered beyond inserting the missing prerequisite immediately before transient allocation. The following unchecked work implicitly consumes the graph and must not create independent pass schedulers:

- Phase 3: scene-viewport resource/command integration, mip generation, Forward+/clustered lighting, PBR, lights, shadows, sky/atmosphere, debug overlays, and GPU timing/capture scopes.
- Phase 4: the multi-pass coverage, antialiasing, filtering, and native-resolution validation pipeline.
- Phase 6: visibility, occluder, HZB, material-resolve, compact G-buffer, and debug-view passes.
- Phase 7: runtime GPU culling, LOD selection, transitions, and optional runtime geometry paths. Offline meshlet/cluster cooking does not depend on the frame graph.
- Phase 8: runtime probe, lightmap, sky, reflection, and indirect-lighting passes.
- Phase 9: the full sparse-ray classification, tracing, reconstruction, composition, and debug pipeline.
- Phase 15: GPU timing, memory/transient-pool inspection, render debug views, captures, and golden render tests.
- Phase 16: optional upscaler integrations when they are expressed as renderer passes.

These are dependency flags, not claims that every listed item requires a unique graph feature.

## Completion And Verification

The Phase 3 construction item is complete when focused deterministic tests cover declared-use validation, dependency ordering and independent-pass stability, cycles, read-before-write, lifetime calculation in resolved order, barriers, and cross-queue dependency records, with the normal build/test suite and code style passing.

The following execution-core item separately requires imported and explicitly supplied physical resources to bind to real RHI resources, callbacks and graph-derived same-queue RHI barriers/commands to execute, deterministic submission, GPU retirement, and focused real-resource evidence. The next cross-queue item requires compiled queue dependencies to become explicit RHI signal/wait submission and ownership transitions; a queue-transition record is intentionally not an emitted synchronization primitive. The following real-workflow item then requires a representative multi-pass Scene-viewport smoke or capture on every backend it claims, with graph/pass capture labels and output equivalence against the bootstrap path.

Transient allocation remains a separate unchecked item until physical reuse and GPU-safe retirement are themselves integrated and exercised.
