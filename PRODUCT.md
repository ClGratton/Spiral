# Product

## Register

product

## Users

Small game teams and ambitious independent developers working for long sessions in a desktop editor. They need to build scenes, inspect entities, manage assets, diagnose rendering, and package projects without learning internal engine boundaries first.

## Product Purpose

Spiral is a modern C++ game engine and editor focused on sharp motion, measured materials, automation, and transparent performance. The editor should make the common scene-authoring loop fast while keeping advanced renderer and profiling controls discoverable but out of the way.

## Brand Personality

Precise, capable, and calm. The engine should feel technically serious without becoming intimidating or visually theatrical.

## Anti-references

- Patchwork tool panels where global settings appear inside unrelated selected objects, or duplicate standalone settings panels repeat top-bar Settings authority.
- Marketing-style dashboards, decorative cards, oversized typography, and saturated sci-fi decoration.
- Hidden state, ambiguous checkboxes, and controls that imply unsupported behavior.
- Dense expert interfaces that omit search, hierarchy, clear selection context, or sensible defaults.

## Design Principles

1. Selection determines context: the Inspector describes the selected entity or asset, never unrelated global state.
2. Stable workspace geography: hierarchy, content, viewport, properties, and diagnostics keep predictable homes. The default layout is that geography; users may close, rearrange, save, and (where supported) detach panels, and Reset Layout and Dock Back always return to it.
3. Progressive depth: common authoring controls stay immediate; the top-bar Settings menu separates Project Settings (serialized project frame pacing) from Engine Settings (global renderer backend and viewport-navigation preset) and Editor Preferences (workspace-global, non-serialized editor state), while Profiler owns non-serialized runtime experiments.
4. Claims match behavior: labels, enabled states, roadmap status, and feedback must reflect what the engine actually supports.
5. Automation validates workflows: editor features receive focused smoke coverage when practical.
6. Project shape controls world shape: projects may choose bounded authored terrain, large streamed terrain, deterministic unbounded generation, learned-assisted generation, or hybrid geometry without inheriting an unnecessary infinite-world runtime.
7. AI assists intent, not authority: AI planning is optional and deterministic guided workflows remain available. Project-local mutations are permission-scoped, inspectable, transactional and undoable, validated, and attributable (the one accepted exception is a committed on-disk project transaction such as a Fab import, which is an explicit, visible undo barrier rather than a hidden non-undoable edit); external effects declare compensation or irreversibility, require the corresponding approval, and record their actual outcome.

## Accessibility & Inclusion

Maintain readable contrast, keyboard-accessible menus and common commands, clear disabled states, non-color-only status communication, and layouts that remain usable at typical laptop and desktop resolutions. Avoid decorative motion and preserve reduced-distraction workflows.

## Amendments 2026-10-09

Made to record the Editor UI project-owner decisions in `Docs/Architecture/EDITOR_UI_ARCHITECTURE.md`; unrelated text is unchanged.

- Principle 2: the default layout is the stable geography, and user-initiated closing, workspaces, and detaching are allowed on top of it with Reset Layout and Dock Back as the way back.
- Principle 3: Editor Preferences is named as the third Settings entry, for workspace-global non-serialized editor state.
- Principle 7: the committed Fab import is recorded as the one explicit undo barrier.
