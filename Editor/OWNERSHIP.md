# Editor Ownership

The editor is a client of the engine. It owns panels, workflows, inspectors, viewports, guided creation, and AI-assisted tooling.

The editor may ask engine diagnostics APIs for renderer and asset data, but it must not reach around public engine boundaries to mutate private runtime state.

The opt-in material-control mailbox is Editor-owned verification tooling. It may expose only its fixed versioned inspect and select-plus-surface-patch actions, execute them on the Editor main thread through normal Scene, MaterialLibrary, renderer-publication, selection/pivot, and history authorities, and publish bounded terminal receipts. It must not become arbitrary dispatch, a network service, implicit save authority, or a substitute for the future model-neutral Automation boundary.
