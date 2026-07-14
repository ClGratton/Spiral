# Current Handoff

Updated 2026-07-14. This file is a recovery aid, not a roadmap authority; `PLAN.md` remains authoritative.

## Current Slice

Phase 3C's shared asynchronous scene-shader portability item remains checked in `PLAN.md`. This follow-up repairs portability defects exposed by hosted Actions run `29350139365`; it does not begin Vulkan scene work.

- Pinned Slang `v2026.13.1` and matching DXC `v1.9.2602` with exact archive hashes, fail-closed fetching, validated extraction, manifests, and a minimized staged runtime.
- Added a backend-neutral shader request/package contract, stable SHA-256 cache keys, transactional versioned caching, structured diagnostics, and reflected layout/convention validation.
- Added an in-process Slang compiler path producing paired DXIL and SPIR-V artifacts from one request. D3D12 consumes validated DXIL; SPIR-V is artifact evidence only until the Vulkan scene viewport consumes it.
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

Hosted Actions run `29350139365` completed with Windows D3D12 success but portable failures: Ubuntu `EngineTests` could not load staged `libslang-compiler.so.0.2026.13.1`, and macOS libc++ rejected `std::atomic<std::shared_ptr<const SceneRenderSnapshot/SceneRasterFrame>>` during the build. The local disposable WSL loader probe confirmed an ELF binary with `RUNPATH=$ORIGIN` resolves the staged versioned Slang compiler without `LD_LIBRARY_PATH`; its files were removed. Replacement CI must build and execute both portable jobs before Linux/macOS qualification is claimed.

## Limits And Deferred Work

- Vulkan scene rendering is not implemented or qualified; current SPIR-V is compile/reflection/convention package evidence only.
- Linux and macOS portable-host qualification remains pending the replacement Actions run; Windows ARM64 and physical-device breadth remain unqualified until matching executed evidence exists.
- Redistribution remains blocked pending the admitted Slang binary-component/notice audit.
- Live D3D12 shader-source rebuild remains a later Phase 3C item.

## Next Ordered Work

After the replacement hosted run verifies this repair, the first unchecked `PLAN.md` item is Vulkan `Engine::RHI::Device` scene-viewport resources and command submission through the wrapped `nvrhi::DeviceHandle`, while keeping raw Vulkan confined to bootstrap, WSI/presentation, and ImGui.

Before implementing it, confirm that a real Vulkan execution path is available for focused runtime verification. Do not substitute source inspection or compilation for backend behavior.

## Working State

The implementation commits above were pushed and the working tree was clean before adding this handoff document and its catalog entry. Inspect `git status --short` before continuing; do not discard unrelated user changes.
