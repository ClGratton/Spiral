# Fab Asset Acquisition And Import Integration

Status: Accepted planning contract
Date: 2026-09-05

This contract defines the pre-demo Fab workflow requested for Spiral. It governs how the Editor hands off acquisition to Epic's supported surfaces, imports a user-selected local package, records provenance and license metadata, and proves the result in the actual Scene. `PLAN.md` alone controls implementation order and completion status.

## Evidence And Decision Levels

### Current official source claims

- Epic's [Purchasing and Downloading Assets](https://dev.epicgames.com/documentation/en-us/fab/purchasing-and-downloading-assets-in-fab) guide says Fab.com supports discovery, acquisition, library access, and direct download for applicable products. Adding a product to the library requires an Epic account sign-in; free products may also offer anonymous website download after the applicable EULA acknowledgement. A listing exposes its license, available formats, and technical details.
- Epic's [Fab in Launcher export guide](https://dev.epicgames.com/documentation/en-us/fab/exporting-assets-from-fab-in-launcher) says Fab in Launcher is available only on Windows and macOS. It supports local download plus exchange formats including FBX, glTF/GLB, OBJ, and USD/USDZ, and offers custom disk and local-socket export workflows. These launcher features are not evidence of a supported Linux launcher integration.
- Epic's [asset format requirements](https://dev.epicgames.com/documentation/en-us/fab/asset-file-format-and-structure-requirements-in-fab) list FBX, GLB, glTF, OBJ, USD, and USDZ among accepted exchange formats, but each listing controls which downloadable formats actually exist. UE-native content can require the UE integration or Launcher and is not an input to Spiral's portable importer.
- Epic's [licenses and pricing guide](https://dev.epicgames.com/documentation/en-us/fab/licenses-and-pricing-in-fab) distinguishes CC-BY, Fab Standard Personal, Fab Standard Professional, Personal Reference Only, and legacy UE Marketplace licensing. Standard Personal and Professional currently grant the same scope but use different eligibility/pricing tiers; Spiral records the user's acquisition-time declaration and never chooses a tier. `NoAI`, generated-with-AI, and legacy-license provenance are metadata to preserve, not permissions for the importer to adjudicate.
- The [Fab Standard License](https://www.fab.com/eula) summary permits use with compatible tools and incorporation into projects, but forbids redistributing the asset on a standalone basis. Reference-Only provides referenced content rather than source format. CC-BY content retains its own attribution requirement. The binding license text and the license attached to the acquired product remain authoritative.

These statements were rechecked against Epic's official pages on 2026-09-04. They do not establish a stable public entitlement/download API for Spiral, and they may change; implementation work must recheck the official pages before relying on them. No documented public marketplace REST/download API or CLI was found in the official material reviewed for this decision. UE Fab-plugin C++ classes are not a public marketplace-service contract.

### Project inference

Acquisition plumbing differs by host, but semantic import must not. Linux uses a user-controlled Fab.com session followed by explicit local-package selection. Windows may use the same website path or Epic's documented Fab-in-Launcher export to disk. Both yield one local input for the same importer, receipts, cooked formats, and project transaction. Spiral must not emulate an unavailable launcher, scrape the site, extract browser cookies, or infer entitlement from a URL.

### Accepted project decision

The first supported integration requires Windows-and-Linux parity through one portable local-package importer:

1. On Linux the Editor opens the official Fab website in a normal user-controlled browser. On Windows it may open Fab.com or accept a documented Fab-in-Launcher disk export. Search, sign-in, EULA acceptance, license/tier choice, purchase, export, and download remain visible human actions on Epic's surface.
2. Spiral receives only a path the user explicitly selects after download. It never reads browser profiles, cookies, passwords, session tokens, payment state, or private Fab endpoints and never automates checkout or EULA acceptance.
3. The initial admitted content route is an archive or directory containing a glTF/GLB source plus texture payloads supported by the importer at implementation time. Unsupported engine-native, plugin, executable, script, encrypted, Reference-Only, or incomplete packages fail visibly; they are not silently converted by invoking another engine.
4. Import stages extraction, path and size validation, source hashing, glTF/texture validation, cooking, registry changes, and optional Scene assignment before one atomic project commit. Failure or cancellation preserves the prior registry, assets, Scene, and receipt set and removes owned staging files.
5. The durable receipt is versioned and records the canonical `https://www.fab.com/listings/<id>` URL confirmed by the user, publisher, selected product/version or download label when available, file format, declared license family and tier, required attribution text/link, source archive SHA-256, importer/cooker versions, resulting stable asset handles, and diagnostic acquisition time. A future UI may normalize a separately entered listing ID before validation, but the persisted identity is always the canonical URL. The source hash plus declared product/version/format—not path or timestamp—is the reimport identity. Fields unavailable from the package are explicit user-confirmed metadata, never guessed or scraped.
6. Duplicate import, same-product update, reimport, and source replacement are explicit outcomes. Same receipt identity and source hash reuse the accepted result; changed bytes create a new provenance generation and retain the old result until the replacement transaction succeeds.
7. Licensed raw downloads and extraction caches stay outside public engine artifacts by default and are ignored from source control unless the project owner deliberately chooses a license-compatible private storage policy. Spiral may ship cooked content only as incorporated project/game content under the user's applicable license; it does not claim to decide legal compliance. CC-BY attribution metadata must remain exportable into future packaging receipts.

The implementation is deliberately staged without weakening that final contract. Linux directory intake is the first independently verifiable hostile-input boundary; equivalent Windows secure-directory intake is the next required parity gate. ZIP input follows only after a separately pinned archive reader and decompression-ratio policy are admitted on both hosts. A checked platform-specific directory-snapshot prerequisite therefore does not claim cross-platform Fab completion, archive import, texture decoding, project commit, or headed acceptance.

Windows Fab-in-Launcher disk export is an acquisition adapter in the first supported cross-platform feature, not a second importer. An optional explicit loopback-socket optimization and macOS support remain later adapters. Any socket path must be capability-gated, opt-in, authenticated to one user-visible import session, bounded in payload/path/size, and unable to mutate a project without the same preview and atomic commit.

## Ownership And Security Boundaries

- `Editor` owns the browser handoff, import preview, user-confirmed metadata, file picker, progress, cancellation, and optional Scene assignment.
- `Engine::Assets` owns package validation, safe extraction policy, semantic import/cooking, stable handles, provenance generations, and transactional publication. It does not own browser authentication or commerce.
- Existing glTF, material, texture, and registry contracts remain the only content authorities. Fab origin is provenance, not a special renderer path.
- Renderer and Scene consume normal cooked assets and stable handles; neither receives Fab URLs, credentials, archive paths, or license policy.
- Package input is hostile until validated. Reject every symlink/reparse point and hardlink, device, FIFO, socket, mount/device crossing, absolute/drive/UNC/parent-traversal path, backslash, empty/dot segment, colon/alternate stream, control byte, trailing dot/space, portable case-fold collision, unsupported nested archive, executable/script payload, excessive count/depth/path/segment/file/aggregate size, malformed encoding, and package change during hashing/import. The first dependency-free boundary accepts portable ASCII member names only; Unicode normalization is deferred until an explicit authority is admitted.
- Logs, crash reports, receipts, and automation output redact user paths when practical and never contain credentials, cookies, authorization headers, or browser storage.

### Immutable intake and identity

Linux directory intake inventories the tree descriptor-relatively, opens each regular file beneath a retained root with `openat`/`fstatat` no-follow semantics, compares file and mount identity before and after streaming copy plus SHA-256, and re-inventories the source before acceptance. It retains close-on-exec parent/snapshot descriptors, seals and rehashes the copied tree after the final callback, and cleans by exact device/inode identity even if the original staging pathname is replaced. A changed source fails with a stable changed-source diagnostic; the current `bool`/error API does not expose a typed status enum.

Windows intake must provide the equivalent boundary with retained handles, owner-private staging, reparse-point rejection, stable file/volume identity and link-count checks, and exact-owned cleanup without a trust-bearing pathname reopen. Linux descriptors and Windows handles feed identical canonical names, byte hashes, glTF dependency validation, receipt identity, cooking, and project-transaction logic. Semantic parsing on both hosts reads only the private copied snapshot.

Directory/expanded defaults are bounded to 4,096 regular files, depth 16, 1,024-byte canonical paths, 255-byte segments, 1 GiB per expanded file, 8 GiB aggregate expanded bytes, and 64 MiB glTF JSON. The future archive adapter additionally limits compressed input to 4 GiB, per-entry expansion to `max(1 MiB, compressed * 100)`, and aggregate expansion to `max(16 MiB, compressed * 50)`. Arithmetic is overflow-checked and tests inject small policies to exercise exact boundaries. The initial ZIP profile admits only single-disk Store/Deflate; encrypted, split, executable, nested-archive, and Zip64 inputs remain rejected until separately qualified.

The canonical expanded-tree digest is domain-separated and hashes sorted canonical path, file size, and per-file SHA-256; timestamps and the user's local path are excluded. Fab identity has two levels:

- `streamId = SHA256("SpiralFabStreamV1\n" + canonical listing URL + "\n" + declared version/download label + "\n" + format token + "\n")`;
- `generationId = SHA256("SpiralFabGenerationV1\n" + streamId + "\n" + source SHA-256 + "\n" + expanded-tree SHA-256 + "\n")`.

Stable semantic asset handles take the first eight big-endian bytes of `SHA256("SpiralFabAssetV1\n" + streamId + "\n" + asset-type token + "\n" + semantic role + "\n")`, mapping zero to one. They derive from the stream, not the source digest. Exact duplicate bytes can therefore reuse a generation, while changed bytes prepare a new immutable cooked root without breaking Scene references. The published registry snapshot contains the generation-specific cooked root and source/watch policy. Older renderer snapshots retain their old root and must never resolve through the newest global handle-derived file. Generation directories are engine-created trusted cooked output; the current resolver foundation does not claim race-resistant containment or receipt-hash enforcement against arbitrary same-user tampering after publication. The later manifest transaction owns create-once publication and reload validation.

The versioned receipt records the declared identity and license fields, source and expanded-tree digests, importer/cooker versions, generation, semantic handles, generation-relative artifact paths and hashes, raw-source policy, and diagnostic acquisition time. It never stores the selected absolute source path. Missing or corrupt prior receipts, contradictory metadata, Reference-Only content, unconfirmed declarations, or CC-BY without attribution fail closed.

### Preparation and project commit

Package snapshotting, glTF/material/texture conversion, candidate registry/material/receipt construction, cooked generation creation, and optional Scene assignment are preparation. They do not mutate live state. The first truthful glTF profile is exactly one static renderable glTF/GLB root, triangles, baked node transform, UV0, one opaque metallic-roughness material, and admitted local textures. Remote/file URIs, escaping or percent-encoded traversal, skins, animation, morphs, compressed geometry, multiple materials, unsupported alpha/sampler combinations, and missing dependencies reject instead of silently degrading.

Cooked assets and complete project state are written to immutable transaction-owned generation directories. One atomically replaced project manifest is the sole commit pointer for the Scene, registry, materials, Fab receipts, and cooked generation. Cancellation is accepted through the pre-manifest boundary and removes only transaction-owned staging. After the manifest replacement, the result is committed rather than falsely reported as rolled back. Live state and one new renderer catalog are swapped only from the reload-validated committed candidate; a post-commit live-load failure is recovery-required and reloads the committed manifest.

## Verification Contract

The accepted Linux directory-snapshot foundation covers minimal text glTF/GLB, missing/ambiguous/corrupt roots, absent or undersized external/GLB buffers, strict JSON, plain and percent-encoded traversal, root/child symlinks, hardlinks and special objects, portable case-fold duplicates, exact count/depth/path/segment/file/aggregate/JSON boundaries, unsupported payloads, changed-on-read input, every exposed cancellation point, final staged-tree tampering, staging-parent replacement, close-on-exec ownership, deterministic tree hashing, transactional output preservation, and exact-owned cleanup. Shared deterministic receipt/registry tests cover canonical serialization, independent identity vectors, license/attribution failure closure, duplicate/update/replacement classification, schema migration, watcher exclusion, atomic storage, and retained old/new rooted resolvers. These tests do not prove Windows intake, archives, image decoding, cooking, project commit, or Editor behavior.

Windows foundation qualification must execute an equivalent reparse/link/volume/race/limit/cancellation/hash corpus natively under MSVC and in the hosted Windows lane. The later cross-platform ZIP/image and prepare/cook slices add their own shared fixtures. Complete import verification must additionally cover role-mismatched textures, duplicate and identical reimport, changed-source replacement with old resolver continuity, corrupt prior state, atomic project commit, cancellation at every publication boundary, and save/reopen. Every failure must preserve prior project state and remove only transaction-owned staging.

The headed Linux gate uses the actual current Debug Editor and native Vulkan backend with Fab.com. The headed Windows gate uses the actual current Debug Editor and D3D12 backend with Fab.com or documented Fab-in-Launcher disk export. In both, the user alone completes sign-in, EULA, acquisition, and download/export; the agent then resumes automatically, imports through the same real Editor controller, confirms receipt/stable assets, assigns the mesh/material, saves, closes, reopens, and verifies the same asset visibly renders. Identical reimport must reuse identity, and one controlled invalid package must fail without changing the accepted Scene.

Each gate records the exact Editor executable, backend/device, canonical listing URL, selected format, source hash, declared license/tier/attribution state, resulting handles, save/reopen result, and visible/capture evidence. It does not record credentials or prove Epic entitlement beyond the user's acquisition action. Automated fixtures do not substitute for one actual Fab-acquired asset on each required platform, and a browser/Launcher download alone does not prove Editor integration.

## Explicit Non-Goals For The First Integration

- No private or reverse-engineered Fab API, headless login, cookie reuse, automated purchasing, or background entitlement polling.
- No Linux claim for Fab in Launcher, and no Wine/Proton dependency.
- No direct import of Unreal `.uasset`, `.uproject`, or `.uplugin` packages.
- No marketplace publishing, resale, license adjudication, automatic update service, or standalone redistribution of source assets.
- No new renderer-only Fab path; the demo Scene must exercise ordinary Spiral asset, material, Scene, and renderer contracts.
