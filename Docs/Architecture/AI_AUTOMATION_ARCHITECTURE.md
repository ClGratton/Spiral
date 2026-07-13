# AI And Automation Architecture

**Status:** Accepted planning contract; product runtime not implemented
**Date:** 2026-07-13

## Decision Summary

Spiral uses an intent-first, live-steerable automation model:

```text
human-owned intent and constraints
  -> editable plan and preview
  -> deterministic permission-scoped tools through public APIs
  -> execute under declared effect and recovery semantics
  -> validation and user-visible result
  -> receipt recording commit, rollback, compensation state, or external result
  -> durable project and roadmap state
```

AI may help form the plan and select tools, but it is not a second mutation authority. The same guided workflows remain usable without AI. Model/provider integration is optional and replaceable; engine commands, transactions, validation, history, and provenance remain deterministic engine/editor capabilities.

This is a targeted hardening of the existing direction, not a wholesale redesign. No substantive product AI framework exists today. The unused `WorkflowDefinition`/`WorkflowStep` header was removed because it exposed mutable completion state without a consumer, executor, transaction, permission, validation, recovery, or test contract.

## Scope: Two Different Agent Systems

Do not conflate these systems:

1. **Repository development agents** help humans research, design, implement, test, and document Spiral itself. `AGENTS.md` owns their operating contract.
2. **The future in-product game-making agent** helps users create and modify projects through editor-owned orchestration and normal engine/editor commands. This document owns its architecture constraints; `PLAN.md` owns implementation order and completion.

Repository agents do not prove the product agent. Product-agent code must not acquire hidden privileges merely because repository agents can edit source files.

## Research Audit

The supplied Codex analysis was useful secondary material, but its numerical “85%” similarity score had no measurement basis. Its transaction, undo, and provenance recommendations are Spiral design inferences, not claims made by Salvatore Sanfilippo. The following primary-source chronology was reviewed:

| Source | Source claim | Spiral interpretation |
| --- | --- | --- |
| Sanfilippo, [“We are destroying software”](https://antirez.com/news/145) | Complexity, dependency chains, elaborate builds, premature frameworks, and systems that do not scale down damage software. | Make minimality and an actual-consumer requirement explicit. |
| Sanfilippo, [“Coding with LLMs in the summer of 2025”](https://antirez.com/news/154) | Human/LLM work benefits from broad context, goals, invariants, named bad paths, communication, and active supervision. | Preserve durable context and explicit non-goals; the article's model/UI preferences were time-specific and are not architecture. |
| Sanfilippo, [“Automatic programming”](https://antirez.com/news/159) | The human owns the product vision and continuously steers AI-assisted production; “what” to build remains fundamental. | Human-owned intent and acceptance evidence stay authoritative. |
| Sanfilippo, [Z80/ZX Spectrum experiment](https://antirez.com/news/160) | A specification, factual documentation, simplicity rules, tests, commits, a work log, rereading after compaction, and targeted steering produced a strong result. | Keep repository-readable state, verification, recovery after compaction, and mechanism-specific steering. |
| Sanfilippo, [Redis Array development](https://antirez.com/news/164) | A long specification evolved under evidence; an initial representation was replaced; tests, manual inspection, and stress testing exposed design and efficiency issues. | Contracts must be challengeable under evidence, and source inspection remains a valid risk-control tool. |
| Sanfilippo, [“Controllare le idee è più importante di guardare il codice”](https://www.youtube.com/watch?v=XZZ_ddBvELc), 2026-07-13 | The current recommendation shifts attention from exhaustive line reading to design hints, idea/mechanism questions, QA, real scenarios, and live back-and-forth steering. It also asserts that frontier models no longer make local mistakes and recommends never returning to code reading. | Adopt attention allocation, short mechanism explanations, visible progress, steering, and QA-first acceptance. Treat zero-local-error and zero-code-review as unverified source claims, not Spiral facts. |

The current video partly supersedes the author's earlier personal workflow, but it is not engine-owned evidence that local implementation mistakes no longer occur. Spiral therefore adopts the useful mechanism, not the absolute.

## Repository Development-Agent Contract

### Intent and context

Before implementation, the active task must make recoverable:

- the user-visible outcome and why it matters;
- invariants, non-goals, risky choices, and known attractive-but-wrong paths;
- affected authoritative documents and ownership boundaries;
- acceptance evidence and platform/device scope;
- the smallest coherent current slice and its next unchecked follow-up.

The repository documents are durable context. Chat summaries, model memory, and hidden reasoning are not authority when repository evidence exists.

### Live steering and explanations

Agents expose a concise editable plan, current progress, observed evidence, and decisions that can still change. A user correction interrupts or redirects the affected work without requiring a fresh session. “Explainability” means mechanisms, alternatives, actions, results, and receipts; it does not require disclosure of private chain-of-thought.

Review should begin with the idea and behavior:

- What architecture and data/control flow were selected?
- Which invariants and failure modes does the implementation preserve?
- Which alternatives were rejected and why?
- What did real tests, captures, benchmarks, or scenarios show?
- What remains unimplemented or unqualified?

Generated source is inspected when risk warrants it, when behavior and explanation disagree, or when evidence is incomplete. Synchronization/lifetimes, unsafe or native API boundaries, serialization/migration, security/permissions, persistent data loss, and performance-critical allocation or scheduling deserve direct inspection proportionate to risk. Exhaustive line-by-line reading is not a universal acceptance gate, but neither is it prohibited.

### Contract challenge

Accepted contracts remain authoritative during implementation, but they are not immutable. When evidence contradicts one:

1. Pause the affected slice; do not silently violate or work around the contract.
2. Record the conflicting observation, reproducible evidence, affected invariant, and scope.
3. Propose the smallest coherent contract change, including simpler alternatives, compatibility/migration impact, and verification changes.
4. Obtain the authority required by `AGENTS.md`; a user-directed architecture audit may accept the change by editing the authoritative document in the scoped commit.
5. Resume from the updated contract and update `PLAN.md` if order, prerequisites, state, or completion changed.

### Minimality gate

New machinery must name a concrete current requirement and consumer. Prefer the smallest mechanism satisfying the accepted behavior. Do not add:

- extension points justified only by hypothetical users;
- parallel mutable authorities or duplicate workflow state;
- a public abstraction with no behavior boundary or focused verification;
- a dependency for functionality that is smaller and safer to implement locally;
- generic infrastructure before the first vertical workflow demonstrates what must be shared.

For a new abstraction, module, or dependency, record the simpler alternative considered and how the machinery can be deleted or replaced if its consumer disappears.

### Durable handoff and commits

Before handoff, repository state must recover the objective, accepted decisions, current behavior, evidence, limitations/blockers, dirty-tree status, and next ordered work. Do not leave a future agent dependent on chat history.

Architecture-changing commit messages state what changed and why, the evidence/source that motivated it, significant rejected alternatives, compatibility or deferred work, and verification. A terse subject without rationale is insufficient for such a change.

## In-Product Automation Contract

### Ownership

- Domain modules own typed commands, preconditions, validation, and domain results.
- A future engine Automation module may own model-neutral action, transaction, receipt, and deterministic headless-runner contracts only when a real workflow consumes them.
- Editor owns workflow UX, tool registration, model/provider adapters, planning/orchestration, preview and approval UI, cancellation, and history presentation.
- AI never calls backend-native APIs, mutates private subsystem state, or writes project files through an untracked side channel.

### Action lifecycle

Every mutating workflow follows:

```text
intent brief
  -> proposed plan
  -> typed action preview
  -> permission and precondition check
  -> execution through public command APIs under declared effect semantics
  -> domain validation plus workflow acceptance checks
  -> provenance receipt recording the result and available recovery
```

Actions have stable workflow/run/action identities, typed inputs and outputs, schema/tool versions, preconditions, deterministic validation, cancellation semantics, declared effect semantics, and explicit terminal status. Retrying must be idempotent or use a recorded idempotency key. Stale preconditions, unsupported schemas, and unauthorized actions fail before execution. Partial failures remain explicit in action state and receipts; they never become unreported or unattributed effects.

### Effect and recovery semantics

Every mutating action declares exactly one class before approval or execution:

- **Transactional:** Spiral controls the affected state. The action commits atomically or completely rolls back its staged changes, retaining failure diagnostics.
- **Compensatable:** Complete rollback cannot be guaranteed, normally because an external system has accepted an effect. The action declares an explicit compensating action, its limits, and whether compensation succeeded, failed, or remains pending. Compensation is a new recorded action, not a claim that the original effect never occurred.
- **Irreversible:** No reliable rollback or compensation exists. The action requires explicit just-in-time approval immediately before execution and records the external identity/result or the best available confirmation of an indeterminate outcome.

A workflow may contain different classes, but it must not describe the whole workflow as atomic once a compensatable or irreversible action can execute. Transactional project-local preparation should occur before an external effect where safe; no receipt may imply that Spiral reversed an effect it does not control.

### Permission policy

- Read-only inspection and local preview may run without per-action approval when project policy permits.
- Reversible project-local mutations run as transactional actions inside a bounded transaction and obey the project's approval policy.
- Compensatable actions require approval at their effect boundary and preview the compensating action and its limitations.
- Destructive, irreversible, security-sensitive, credential-bearing, external-service, publishing, purchase, permission, or deployment actions require explicit just-in-time approval immediately before execution. External does not automatically mean irreversible, but the action must declare which recovery class actually applies.

Approval is scoped to the exact action and data destination. It is not a blanket grant to the model or workflow.

### Provenance receipt

A committed action records, with secret and private-data redaction:

- workflow/run/action IDs and parent relationships;
- human intent/plan revision and project revision/preconditions;
- tool/command and schema versions;
- approved normalized inputs and affected resource identities;
- produced changes/artifacts and transaction/history identity;
- validators, evidence, warnings, fallbacks, result, and timestamps;
- required approvals and whether AI or the deterministic non-AI workflow selected the action.

Receipts explain what happened; they are not hidden model reasoning and do not store credentials or raw sensitive prompts.

### AI-optional parity

The AI adapter may propose plans, ask questions, select registered tools, and explain results. It does not own the tools. Each supported product workflow has a deterministic guided UI or command path with equivalent mutation, validation, undo, and receipt semantics. Model unavailability degrades planning assistance, not the user's ability to complete the workflow.

## Options Considered

### Keep the current public workflow structs

Rejected. Two names plus a mutable `Completed` flag conflate definition and execution state, provide no stable identity or failure semantics, and have no consumer or test. Keeping them would turn an initial guess into accidental public API.

### Build a generic AI/workflow framework now

Rejected. Phase 13 has no implemented vertical workflow from which to derive shared requirements. This would violate the actual-consumer and minimality gates.

### Remove the stub and defer all decisions

Partly accepted. Executable machinery is deferred, but permission, transaction, provenance, AI-optional, steering, and evidence constraints are decided now so the first workflow cannot create an incompatible hidden mutation path.

### Intent-first hybrid over deterministic tools

Accepted. It preserves human product vision and live steering while keeping mutations testable, reversible, provider-neutral, and usable without AI.

## Verification Contract For Future Phase 13 Work

No runtime behavior is checked complete by this document. Future focused tests must carry workflow/run/action IDs through preview, apply, validate, the declared commit/rollback/compensation/external-result outcome, undo where supported, and receipt publication, and must prove:

- deterministic non-AI and AI-selected paths produce the same command semantics and user-visible result;
- unauthorized, stale, malformed, unknown-version, and unsupported actions are rejected before mutation;
- injected transactional failures roll back every staged change and retain diagnostics;
- compensatable failures publish and exercise the declared compensation path without misreporting the original effect as rolled back;
- irreversible actions cannot execute without just-in-time approval and record successful, failed, or indeterminate external outcomes honestly;
- cancellation and retry/idempotency behavior is deterministic;
- provenance is complete, versioned, tied to history, and redacts secrets;
- approval gates occur at the correct risk boundary;
- validators exercise the real edited project and do not accept a log marker or agent report as behavior evidence;
- headless and editor workflows agree where both are claimed.

Provider/model quality is evaluated separately with versioned scenario sets, repeated trials, cost/latency, unsafe-action rate, clarification quality, plan/tool accuracy, and deterministic-tool outcomes. A model benchmark never upgrades the deterministic engine workflow's qualification.

## Current State And Next Implementation Rule

There is no in-product AI runtime, provider adapter, generic workflow executor, action schema, or Automation module. Phase 2 project creation and undo/redo are ordinary editor features, not proof of Phase 13 automation.

Phase 13 begins with one real non-AI guided workflow using existing public commands, transaction/history behavior, validation, and a receipt. Reusable automation types are extracted only when that vertical slice exposes a stable boundary or a second concrete consumer. The AI adapter comes after deterministic tools and permission/receipt semantics exist.
