# Roadmap Governance

`PLAN.md` is an execution contract, not a progress mood board. Its checkboxes must remain useful to someone deciding what the engine can do today.

## Status Rules

A roadmap item may use `[x]` only when all of these are true:

1. The exact behavior on the line exists in engine-owned code or a deliberately delivered repository artifact.
2. It is connected to the real runtime, editor, build, or automation workflow named by the item.
3. Focused verification exercises the behavior. Compilation is sufficient only for an item that explicitly claims compilation or project generation.
4. Platform and backend wording matches the tested coverage.
5. Known limitations are represented by separate unchecked items, not hidden behind broad completed wording.

The words `stub`, `placeholder`, `skeleton`, `scaffold`, `pending`, and `not implemented` are incompatible with a checked implementation item. A research or architecture document may be complete, but it belongs in the documentation inventory rather than masquerading as a runtime feature.

## Change Procedure

Before changing `[ ]` to `[x]`:

1. Re-read the item and its phase exit criteria.
2. Link the implementation in the commit or pull request description.
3. Add or identify a focused test or smoke workflow.
4. Run the behavior locally when practical.
5. Run `Scripts/CheckCodeStyle.ps1` or `Scripts/CheckCodeStyle.sh`.
6. Update the Current State section without claiming the whole phase is complete unless every exit criterion is met.

If only part is complete, rewrite it as two lines. The narrow completed behavior gets `[x]`; the remaining behavior stays `[ ]`.

## Batched Roadmap Work

Agents may integrate up to three small roadmap slices before a shared build and CI run when doing so avoids repeating an expensive build. A valid batch preserves roadmap order: the first unchecked item is included, every additional slice is immediately eligible, and no selected slice depends on unfinished work inside or before the batch.

Batch completion is evaluated per checkbox, not per build. Each slice must have distinct implementation ownership, focused behavior evidence, accurate current-state wording, and platform/backend scope. One passing build cannot check three items by itself. If only part of a batch meets its gate, check only that part and leave precise follow-up wording for the rest.

The primary agent must record why the items were safe to batch, prevent overlapping edits, reconcile the integrated result, and run the shared regression suite. The detailed agent and build coordination contract lives in [../AGENTS.md](../AGENTS.md); verification selection lives in [VERIFICATION.md](VERIFICATION.md).

## Review Rule

Code review must treat an unsupported checkmark as a correctness defect. Fix the wording or implementation before merge. CI validates obvious status-language violations, while reviewers remain responsible for whether the evidence actually supports the claim.
