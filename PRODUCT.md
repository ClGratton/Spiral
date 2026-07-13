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

- Patchwork tool panels where global settings appear inside unrelated selected objects.
- Marketing-style dashboards, decorative cards, oversized typography, and saturated sci-fi decoration.
- Hidden state, ambiguous checkboxes, and controls that imply unsupported behavior.
- Dense expert interfaces that omit search, hierarchy, clear selection context, or sensible defaults.

## Design Principles

1. Selection determines context: the Inspector describes the selected entity or asset, never unrelated global state.
2. Stable workspace geography: hierarchy, content, viewport, properties, and diagnostics keep predictable homes.
3. Progressive depth: common authoring controls stay immediate; renderer and project configuration live in dedicated settings surfaces.
4. Claims match behavior: labels, enabled states, roadmap status, and feedback must reflect what the engine actually supports.
5. Automation validates workflows: editor features receive focused smoke coverage when practical.
6. Project shape controls world shape: projects may choose bounded authored terrain, large streamed terrain, deterministic unbounded generation, learned-assisted generation, or hybrid geometry without inheriting an unnecessary infinite-world runtime.
7. AI assists intent, not authority: AI planning is optional and deterministic guided workflows remain available. Project-local mutations are permission-scoped, inspectable, transactional and undoable, validated, and attributable; external effects declare compensation or irreversibility, require the corresponding approval, and record their actual outcome.

## Accessibility & Inclusion

Maintain readable contrast, keyboard-accessible menus and common commands, clear disabled states, non-color-only status communication, and layouts that remain usable at typical laptop and desktop resolutions. Avoid decorative motion and preserve reduced-distraction workflows.
