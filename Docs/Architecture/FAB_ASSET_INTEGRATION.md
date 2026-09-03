# Fab Asset Acquisition And Import Integration

Status: Accepted planning contract
Date: 2026-09-04

This contract defines the pre-demo Fab workflow requested for Spiral. It governs how the Editor hands off acquisition to Epic's supported surfaces, imports a user-selected local package, records provenance and license metadata, and proves the result in the actual Scene. `PLAN.md` alone controls implementation order and completion status.

## Evidence And Decision Levels

### Current official source claims

- Epic's [Purchasing and Downloading Assets](https://dev.epicgames.com/documentation/en-us/fab/purchasing-and-downloading-assets-in-fab) guide says Fab.com supports discovery, acquisition, library access, and direct download for applicable products. Adding a product to the library requires an Epic account sign-in; free products may also offer anonymous website download after the applicable EULA acknowledgement. A listing exposes its license, available formats, and technical details.
- Epic's [Fab in Launcher export guide](https://dev.epicgames.com/documentation/en-us/fab/exporting-assets-from-fab-in-launcher) says Fab in Launcher is available only on Windows and macOS. It supports local download plus exchange formats including FBX, glTF/GLB, OBJ, and USD/USDZ, and offers custom disk and local-socket export workflows. These launcher features are not evidence of a supported Linux launcher integration.
- The [Fab Standard License](https://www.fab.com/eula) summary permits use with compatible tools and incorporation into projects, but forbids redistributing the asset on a standalone basis. Reference-Only provides referenced content rather than source format. CC-BY content retains its own attribution requirement. The binding license text and the license attached to the acquired product remain authoritative.

These statements describe Epic's current public surfaces and terms. They do not establish a stable public entitlement/download API for Spiral, and they may change; implementation work must recheck the official pages before relying on them.

### Project inference

On Linux, the smallest supported workflow is a user-controlled Fab.com session followed by explicit local-package import. Spiral must not emulate the Windows/macOS launcher, scrape the site, extract browser cookies, or infer entitlement from a URL. A local package can be validated and cooked deterministically without giving the engine custody of Epic credentials or purchase authority.

### Accepted project decision

The first integration is a portable local-package importer with a Linux website handoff:

1. The Editor opens the official Fab website in a normal user-controlled browser. Search, sign-in, EULA acceptance, license/tier choice, purchase, and download remain visible human actions on Epic's surface.
2. Spiral receives only a path the user explicitly selects after download. It never reads browser profiles, cookies, passwords, session tokens, payment state, or private Fab endpoints and never automates checkout or EULA acceptance.
3. The initial admitted content route is an archive or directory containing a glTF/GLB source plus texture payloads supported by the importer at implementation time. Unsupported engine-native, plugin, executable, script, encrypted, Reference-Only, or incomplete packages fail visibly; they are not silently converted by invoking another engine.
4. Import stages extraction, path and size validation, source hashing, glTF/texture validation, cooking, registry changes, and optional Scene assignment before one atomic project commit. Failure or cancellation preserves the prior registry, assets, Scene, and receipt set and removes owned staging files.
5. The durable receipt is versioned and records the Fab listing/product identifier or canonical URL supplied by the user, publisher, selected product/version or download label when available, file format, declared license family and tier, required attribution text/link, source archive SHA-256, importer/cooker versions, resulting stable asset handles, and diagnostic acquisition time. The source hash plus declared product/version/format—not path or timestamp—is the reimport identity. Fields unavailable from the package are explicit user-confirmed metadata, never guessed or scraped.
6. Duplicate import, same-product update, reimport, and source replacement are explicit outcomes. Same receipt identity and source hash reuse the accepted result; changed bytes create a new provenance generation and retain the old result until the replacement transaction succeeds.
7. Licensed raw downloads and extraction caches stay outside public engine artifacts by default and are ignored from source control unless the project owner deliberately chooses a license-compatible private storage policy. Spiral may ship cooked content only as incorporated project/game content under the user's applicable license; it does not claim to decide legal compliance. CC-BY attribution metadata must remain exportable into future packaging receipts.

Windows/macOS launcher custom-disk or explicit loopback-socket ingestion is a later transport adapter over the same local-package transaction. It must be capability-gated, opt-in, authenticated to one user-visible import session, bounded in payload/path/size, and unable to mutate a project without the same preview and atomic commit. It is not required for the first Linux/Vulkan integration and must not create a second importer authority.

## Ownership And Security Boundaries

- `Editor` owns the browser handoff, import preview, user-confirmed metadata, file picker, progress, cancellation, and optional Scene assignment.
- `Engine::Assets` owns package validation, safe extraction policy, semantic import/cooking, stable handles, provenance generations, and transactional publication. It does not own browser authentication or commerce.
- Existing glTF, material, texture, and registry contracts remain the only content authorities. Fab origin is provenance, not a special renderer path.
- Renderer and Scene consume normal cooked assets and stable handles; neither receives Fab URLs, credentials, archive paths, or license policy.
- Archive input is hostile until validated. Reject absolute/parent-traversal paths, links escaping staging, duplicate normalized paths, unsupported nested archives, executable/script payloads, excessive file count, per-file or aggregate expanded-size overflow, malformed encodings, and package changes during hashing/import.
- Logs, crash reports, receipts, and automation output redact user paths when practical and never contain credentials, cookies, authorization headers, or browser storage.

## Verification Contract

Deterministic tests must cover a minimal valid package, missing/ambiguous glTF roots, corrupt GLB, missing and role-mismatched textures, traversal and symlink escape, duplicate normalized names, count/size/decompression limits, unsupported payloads, changed-on-read input, cancellation at each publication boundary, duplicate import, identical reimport, changed-source replacement, corrupt prior receipt, missing required license/attribution metadata, and save/reopen. Every failure must preserve the prior project state and remove only transaction-owned staging data.

The headed Linux gate uses the actual current Debug Editor and native Vulkan backend. The Editor opens Fab.com in the user-visible browser; the user alone completes any Epic sign-in, EULA, acquisition, and download. The agent then resumes automatically, imports the explicitly selected source-format package through the real UI, confirms the receipt and stable asset entries, assigns the imported mesh/material to the Scene, saves, closes, reopens, and verifies the same asset visibly renders. Reimport with identical bytes must reuse identity; one controlled invalid or incomplete package must fail without changing the accepted Scene.

The gate records the exact Editor executable, backend/device, listing URL/identifier, selected format, source hash, declared license/tier/attribution state, resulting handles, save/reopen result, and visible/capture evidence. It does not record credentials or prove Epic entitlement beyond the user's acquisition action. Automated local fixtures do not substitute for one actual Fab-acquired asset, and a browser download alone does not prove Editor integration.

## Explicit Non-Goals For The First Integration

- No private or reverse-engineered Fab API, headless login, cookie reuse, automated purchasing, or background entitlement polling.
- No Linux claim for Fab in Launcher, and no Wine/Proton dependency.
- No direct import of Unreal `.uasset`, `.uproject`, or `.uplugin` packages.
- No marketplace publishing, resale, license adjudication, automatic update service, or standalone redistribution of source assets.
- No new renderer-only Fab path; the demo Scene must exercise ordinary Spiral asset, material, Scene, and renderer contracts.

