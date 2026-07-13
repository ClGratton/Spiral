# Jobs Module Ownership

`Engine/src/Engine/Jobs` owns reusable CPU work scheduling and dependency execution. It must remain independent of Scene, Renderer, Assets, Editor, and backend-native graphics APIs.

Owned responsibilities:

- worker lifecycle, external job injection, worker-local queues, and work stealing;
- stable worker identity and scheduler statistics for diagnostics;
- frame-task dependency validation, execution lanes, graph-local completion, failure/skip propagation, deterministic single-thread execution, and profiler events;
- typed staged publication that exposes immutable data only after a producer succeeds.

Forbidden responsibilities:

- subsystem-specific frame phases, scene/render payload definitions, editor policy, or gameplay behavior;
- GPU render-graph scheduling, RHI queue synchronization, or backend-native work submission;
- arbitrary global frame barriers as a substitute for declared dependencies;
- allowing worker tasks to call `WaitIdle`, mutate caller-thread-only UI/window/renderer state, or publish partially written data.

The Application, Scene extraction, Physics, Terrain, Renderer, and other consumers define their own task payloads and dependencies through these generic contracts.
