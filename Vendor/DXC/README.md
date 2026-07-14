# Pinned DirectX Shader Compiler

`Scripts/FetchDXC.ps1` and `Scripts/FetchDXC.sh` acquire the exact official
Microsoft DXC package used by Slang v2026.13.1's own build configuration:
DXC v1.9.2602, archive `dxc_2026_02_20.zip`, SHA-256
`a1e89031421cf3c1fca6627766ab3020ca4f962ac7e2caa7fab2b33a8436151e`.

The hash-verified package is extracted under
`v1.9.2602/windows-x86_64/`. Compiler binaries, downloaded archives, and
caches are intentionally ignored. Archive member paths are checked before
extraction, and an installed manifest carrying the admitted archive digest plus
the exact compiler, validator, and notice files is revalidated on later setup
runs. There is no system, Visual Studio, or Windows SDK fallback.

The compiler and validator are runtime dependencies of in-process Editor and
Sandbox shader compilation. Runtime staging copies their exact archive notices
beside the DLLs; `NOTICE.md` records the remaining redistribution requirements.
