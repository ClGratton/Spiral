# Current Handoff

Updated 2026-07-14. This file is a recovery aid, not a roadmap authority; `PLAN.md` remains authoritative.

## Current Slice

Phase 3C's shared asynchronous scene-shader portability item remains checked in `PLAN.md`. This follow-up corrects the host target contract exposed by hosted Actions replacement run `29352633796`; it does not begin Vulkan scene work.

- Pinned Slang `v2026.13.1` and matching DXC `v1.9.2602` with exact archive hashes, fail-closed fetching, validated extraction, manifests, and a minimized staged runtime.
- Added a backend-neutral shader request/package contract, stable SHA-256 cache keys, transactional versioned caching, structured diagnostics, and reflected layout/convention validation.
- Added an in-process Slang compiler path with an exact admitted host target set: Windows x86_64 produces paired DXIL+SPIR-V through pinned DXC, while Linux/macOS produce SPIR-V-only packages. D3D12 consumes validated DXIL; SPIR-V is artifact evidence only until the Vulkan scene viewport consumes it. Non-Windows DXIL requests fail before compile with a clear unavailable-target diagnostic.
- Added job-system fire-and-poll compilation with deduplicated requests, immutable publication, retryable terminal failure, and retention of the last valid pipeline. Smoke tests use deterministic inline execution.
- Removed legacy D3D12 source compilation and added strict terminal/pipeline evidence markers to the render smoke test.
- Generated Linux Editor/Sandbox/EngineTests executables now use `RUNPATH=$ORIGIN`; generated macOS counterparts use `LC_RPATH @loader_path`, allowing Slang's staged proxy to resolve its versioned compiler beside the executable without host-library paths.
- Immutable scene snapshot/raster-frame publication retains release-store/acquire-load semantics through `std::atomic<std::shared_ptr<const T>>` only when the C++ library advertises that specialization, otherwise through the standard atomic `shared_ptr` free functions.

Commits pushed to `main`:

- `6edc856 Build a portable asynchronous shader pipeline`
- `81579f1 Fix clean PowerShell toolchain setup`

## Evidence

Completed locally:

- MSVC Debug build: passed with zero warnings/errors.
- MSVC `EngineTests`: 35/35 passed.
- Windows MinGW Debug build: passed; `EngineTests`: 35/35 passed.
- Strict D3D12 render test: passed, including cold compile and warm disk-cache paths, deterministic A/C captures, and translated B capture.
- Parallel and deterministic frame-task/snapshot smoke tests: passed.
- Code style, PowerShell parsing, Bash syntax, dependency JSON, Markdown links, `git diff --check`, unsafe-ZIP rejection, and minimized-runtime tests: passed.

Hosted Actions run `29350139365` completed with Windows D3D12 success but exposed the Linux loader and macOS atomic-specialization failures; commit `5df91bb` repaired both. Replacement run `29352633796` then built and launched both portable jobs but failed exactly 1/35 tests because it requested DXIL+SPIR-V without an admitted non-Windows DXC. Commit `2c29fbe` made the host target set explicit, and `0f02ac5` made cache loading and async publication validate every artifact requested. Final-head run `29354068102` (<https://github.com/ClGratton/Spiral/actions/runs/29354068102>) passed Code Style, Windows D3D12, Ubuntu portable/Vulkan, and macOS portable/MoltenVK jobs. Ubuntu and macOS each passed all 35 `EngineTests` with real SPIR-V compilation, reflection/layout/convention validation, deterministic cache/input invalidation, and explicit DXIL rejection. This closes the portable-host repair; it is not Vulkan scene-rendering qualification.

## Limits And Deferred Work

- Vulkan scene rendering is not implemented or qualified; current SPIR-V is compile/reflection/convention package evidence only.
- Linux and macOS x86_64 portable build/test paths are qualified by run `29354068102`; Windows ARM64, Apple Silicon, Vulkan scene rendering, and physical-device breadth remain unqualified until matching executed evidence exists.
- Local WSL cannot replace hosted Linux verification: its installed glibc is older than the vendored Premake executable's required `GLIBC_2.38`, so `Scripts/Build.sh Debug gmake` stops during generation. No system compiler/tool substitution was used.
- Redistribution remains blocked pending the admitted Slang binary-component/notice audit.
- Live D3D12 shader-source rebuild remains a later Phase 3C item.

## Next Ordered Work

Evaluation of the former next Vulkan scene item found a contract prerequisite: `Engine::RHI::CommandList` cannot bind or clear a renderer-owned color/depth target. The current D3D12 prototype compensates with native D3D12 presentation-command access, which cannot be duplicated for Vulkan without violating the required native-boundary rule. `PLAN.md` now places the smallest prerequisite immediately first: define and execute backend-neutral viewport-output recording plus its narrow NVRHI-output-to-native-presentation/ImGui handoff. It must retain raw API ownership solely in bootstrap, WSI/presentation, and ImGui.

Only after that contract has real focused output evidence should the Vulkan `Engine::RHI::Device` scene-viewport item proceed. Run `29354068102` confirms real Linux Vulkan and macOS MoltenVK presentation paths are available; local Windows Vulkan smoke is also available. Neither is Vulkan scene-execution evidence.

## Working State

Commits through `0f02ac5` are pushed to `main`. Inspect `git status --short` before continuing; do not discard unrelated user changes.
