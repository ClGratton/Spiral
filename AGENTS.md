# Workspace Agent Instructions

## Verification

- When a completed feature can be exercised locally, test the new behavior itself rather than relying only on compilation.
- For editor-facing changes, run the editor and inspect a screenshot when practical. Use an existing automated smoke test when it covers the interaction; otherwise add focused coverage that does.
- Report any verification that cannot be performed, together with the reason.

## Roadmap Integrity

- In `PLAN.md`, `[x]` means the described behavior is implemented, integrated into its real workflow, and verified. Compilation alone is not enough for a runtime or editor-facing feature.
- Never check an item whose delivered artifact is only a stub, placeholder, skeleton, plan, scaffold, or interface without its claimed behavior. Split partial work into a precisely worded completed item and an unchecked follow-up.
- Before changing a roadmap checkbox, update the current-state prose, add or identify focused verification, run `Scripts/CheckCodeStyle`, and confirm the checked wording does not overstate platform or backend coverage.
- Phase completion means its exit criteria are demonstrably met. A phase may have useful checked foundations without being complete.

## Agentic Workflow

- For requests to continue the roadmap, read all workspace-authored Markdown guidance, inspect the first unchecked `PLAN.md` item in order, and implement that item rather than substituting an invented slice. If the item must be split for honest platform or verification scope, preserve the original intent with a precisely worded checked result and an adjacent unchecked remainder.
- Use parallel agents when the user asks for agentic work and the task has independent bounded concerns. Typical renderer assignments are architecture/backend audit, editor/runtime integration audit, and verification/roadmap audit. The primary agent owns the implementation, reconciles findings, prevents overlapping edits, and remains responsible for the final result.
- Prefer GPT-5.6-Terra for agentic engine work when model choice is available: it retains the long context and current multi-agent workflow while reducing cost relative to the frontier model. Use Luna only for bounded, low-risk, mechanical work; do not use it as the lead for renderer architecture, cross-platform backend work, roadmap governance, or broad refactors. Use the frontier model when Terra cannot safely carry the complexity.
- Agents may accelerate analysis and verification, but their reports are not evidence by themselves. Inspect the real call sites, run the behavior, and apply the Verification and Roadmap Integrity rules before claiming completion.
- Roadmap-continuation requests authorize the agent to finish required hosted verification without asking again: stage the scoped workspace changes, create an appropriate commit on the current branch, push it to the existing configured remote, monitor the resulting CI jobs, and fix/retry failures that remain within the requested roadmap item. Do not use this authorization for unrelated changes, destructive history edits, releases, or deployment outside the repository's existing CI workflow.

## Renderer Portability

- Keep gameplay, scene, editor, and backend-neutral renderer code on `Engine::RHI`. NVRHI is the first implementation backend; it has not been replaced by raw Vulkan or raw D3D12.
- Vulkan follows NVRHI's required ownership model: the engine/platform layer creates the native instance, surface, physical/logical device, and queues, then must wrap the device with `nvrhi::vulkan::createDevice` and use the returned `nvrhi::DeviceHandle` for renderer resources and command submission.
- Native API use is allowed only in explicit backend escape hatches. Window-system swapchain/presentation and Dear ImGui backend integration may consume native handles because NVRHI does not own presentation and Dear ImGui has no NVRHI renderer backend. Do not let that bridge expand into scene rendering.
- Backend selection and fallback happen before native device creation. A strict backend request must fail clearly when unavailable; an ordinary portable launch may select a supported fallback. Never silently report one backend while running another.
- Preserve multi-device and multi-vendor behavior: enumerate adapters, select by required capabilities rather than vendor identity, avoid unconditional optional extensions, and keep platform/backend coverage accurately stated in `PLAN.md`.
