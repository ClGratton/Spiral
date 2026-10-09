# Pinned Chromium Embedded Framework

This directory is populated only by `Scripts/FetchCEF.sh` (Linux) or
`Scripts/FetchCEF.ps1` (Windows). It is intentionally not a system-installed
CEF/Chromium fallback and no CEF archive, binary, cache, or generated object is
committed. Only this file and `THIRD_PARTY_NOTICE.md` are tracked.

The scripts install exactly one official CEF minimal distribution per host under
`v154.0.34/<platform>-<architecture>/` from `https://cef-builds.spotifycdn.com`,
verify its pinned SHA-256 and archive member paths before extraction, and record
the digest in an installed manifest. On Linux the installer also checks the
shipped `libcef.so` against its recorded pre-strip digest, runs
`strip --strip-all` (about 1.4 GB to about 270 MB), records the stripped digest,
and deletes `chrome-sandbox`: a setuid-root helper is never installed. A
subsequent run accepts the package only when that manifest, the headers, the
runtime files, and the stripped library digest still match.

`Scripts/BrowserRuntimePins.env` is the machine-readable pin ledger used by the
fetch scripts and Premake. The authoritative admitted version, license
obligations, and integration boundary are recorded in `Docs/DEPENDENCIES.md`.

Premake builds the `CEFWrapper` static library and the `SpiralBrowserHelper`
executable only when this directory holds a package whose installed manifest
matches the pin; otherwise the Editor builds without any CEF input. CEF is
Editor-private: `Engine` and `Sandbox` never include its headers or link it.

CEF stable branches receive Chromium security rebuilds every few days to weeks.
Bumping the pin means re-reading the cef-builds index, recomputing the archive
and `libcef.so` digests, regenerating the explicit wrapper source list in
`Vendor/CEF.premake.lua`, and rerunning the helper launch check.
