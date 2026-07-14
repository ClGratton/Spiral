# Pinned Slang Toolchain

This directory is populated only by `Scripts/FetchSlang.ps1` or
`Scripts/FetchSlang.sh`. It is intentionally not a user-installed compiler
fallback and no compiler archive, binary, cache, or generated shader artifact
is committed.

The scripts install exactly one official `shader-slang/slang` release package
per host under `v2026.13.1/<platform>-<architecture>/`, verify the declared
SHA-256 and archive member paths before extraction, and record that digest in
an installed manifest. A subsequent setup accepts the package only when that
manifest and the exact host header, import/shared libraries, compiler runtime,
standard module, and `LICENSE` still exist. The current third-party provenance
and redistribution gate are recorded in `THIRD_PARTY_NOTICE.md`.
This release's compiler ABI is pinned with the package; do not substitute a
system Slang/DXC installation.

`Scripts/ShaderToolchainPins.env` is the machine-readable pin ledger used by
the fetch scripts and Premake. The authoritative admitted versions, URLs,
license obligations, and host matrix are recorded in `Docs/DEPENDENCIES.md`.
