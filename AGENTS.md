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
