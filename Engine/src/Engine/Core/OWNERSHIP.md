# Core Ownership

Core owns application lifecycle, logging, assertions, layers, windows, command-line args, and basic utility types.

Core must stay lightweight. Core headers and reusable Core services must not depend on renderer, scene, assets, scripting, or editor code.

`Application.cpp` is the engine composition root and may sequence public renderer lifecycle and timing APIs beside platform/window, layer, and job orchestration. That narrow implementation-file exception reflects the existing renderer startup/frame/shutdown integration; it must not leak renderer types into Core public headers or authorize renderer policy in other Core services. If composition grows beyond lifecycle/timing coordination, move it to a dedicated host boundary rather than broadening this exception.
