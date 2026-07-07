# Editor Ownership

The editor is a client of the engine. It owns panels, workflows, inspectors, viewports, guided creation, and AI-assisted tooling.

The editor may ask engine diagnostics APIs for renderer and asset data, but it must not reach around public engine boundaries to mutate private runtime state.
