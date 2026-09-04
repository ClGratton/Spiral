# Editor Ownership

The editor is a client of the engine. It owns panels, workflows, inspectors, viewports, guided creation, and AI-assisted tooling.

The editor may ask engine diagnostics APIs for renderer and asset data, but it must not reach around public engine boundaries to mutate private runtime state.

The opt-in control mailbox is Editor-owned verification tooling. Its accepted schema 2 retains fixed material inspect and select-plus-surface-patch actions and adds only fixed entity inspect/select, complete Transform, typed Light, complete project color-pipeline, and dedicated main-camera viewport-pose actions. The shared foundation puts complete Scene values and the complete project color pipeline through normal validation, Editor history/restore, and Renderer republication where applicable. Mailbox work executes on the Editor main thread through normal Scene, MaterialLibrary, renderer-publication, selection/pivot/camera, and history authorities and publishes bounded terminal receipts. Every mutation is explicitly session-only/not-saved. It must not become arbitrary dispatch, a network service, implicit save authority, entity/component lifecycle authority, or a substitute for the future model-neutral Automation boundary.
