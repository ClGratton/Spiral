# Editor Ownership

The editor is a client of the engine. It owns panels, workflows, inspectors, viewports, guided creation, and AI-assisted tooling.

The editor may ask engine diagnostics APIs for renderer and asset data, but it must not reach around public engine boundaries to mutate private runtime state.

The opt-in control mailbox is Editor-owned verification tooling. Its accepted schema exposes only fixed material inspect and select-plus-surface-patch actions; the ordered version-2 extension remains pending and may add only fixed entity inspect/select, complete Transform, typed Light, complete project color-pipeline, and main-camera viewport-pose actions. The accepted shared foundation puts the complete project color-pipeline value through normal Editor history validation/restore and Renderer republication. Mailbox work executes on the Editor main thread through normal Scene, MaterialLibrary, renderer-publication, selection/pivot/camera, and history authorities and publishes bounded terminal receipts. Every mutation is explicitly session-only/not-saved. It must not become arbitrary dispatch, a network service, implicit save authority, entity/component lifecycle authority, or a substitute for the future model-neutral Automation boundary.
