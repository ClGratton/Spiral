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

Hosted Actions run `29350139365` completed with Windows D3D12 success but portable failures: Ubuntu `EngineTests` could not load staged `libslang-compiler.so.0.2026.13.1`, and macOS libc++ rejected `std::atomic<std::shared_ptr<const SceneRenderSnapshot/SceneRasterFrame>>` during the build. Commit `5df91bb` repaired both. Replacement run `29352633796` (<https://github.com/cla20/Game-Engine/actions/runs/29352633796>) built and launched both portable jobs, but each failed exactly 1/35 tests: `Slang compiler emits validated portable shader packages` requested DXIL+SPIR-V, then Slang reported a failed downstream DXC/DXIL library/pass-through compiler. That is expected from the existing dependency ledger: DXC v1.9.2602 is admitted only for Windows x86_64. The portable integration test now compiles and validates SPIR-V-only packages, exercises deterministic cache/input invalidation/reflection/conventions, and asserts that a DXIL request fails clearly. Commit `2c29fbe` is pushed to `main`; queued hosted runs `29353654307` (<https://github.com/ClGratton/Spiral/actions/runs/29353654307>) and `29353654316` (<https://github.com/ClGratton/Spiral/actions/runs/29353654316>) are the pending Linux/macOS execution gate. Do not convert their queued/build result into Vulkan scene-rendering qualification.

## Limits And Deferred Work

- Vulkan scene rendering is not implemented or qualified; current SPIR-V is compile/reflection/convention package evidence only.
- Linux and macOS portable-host qualification remains pending the replacement Actions run; Windows ARM64 and physical-device breadth remain unqualified until matching executed evidence exists.
- Local WSL cannot replace hosted Linux verification: its installed glibc is older than the vendored Premake executable's required `GLIBC_2.38`, so `Scripts/Build.sh Debug gmake` stops during generation. No system compiler/tool substitution was used.
- Redistribution remains blocked pending the admitted Slang binary-component/notice audit.
- Live D3D12 shader-source rebuild remains a later Phase 3C item.

## Next Ordered Work

After the replacement hosted run verifies this repair, the first unchecked `PLAN.md` item is Vulkan `Engine::RHI::Device` scene-viewport resources and command submission through the wrapped `nvrhi::DeviceHandle`, while keeping raw Vulkan confined to bootstrap, WSI/presentation, and ImGui.

Before implementing it, confirm that a real Vulkan execution path is available for focused runtime verification. Do not substitute source inspection or compilation for backend behavior.

## Working State

The implementation commits above were pushed and the working tree was clean before adding this handoff document and its catalog entry. Inspect `git status --short` before continuing; do not discard unrelated user changes.
