# DirectX Shader Compiler Notice Audit

The official pinned binary package contains `LICENSE-LLVM.txt`,
`LICENSE-MIT.txt`, and `LICENSE-MS.txt`; all three files are retained unchanged
in the ignored, versioned extraction directory. The Microsoft source
repository additionally publishes `LICENSE.TXT` and `ThirdPartyNotices.txt`.
Runtime staging copies the three archive-specific files beside the compiler
binaries as `DXC-LICENSE-LLVM.txt`, `DXC-LICENSE-MIT.txt`, and
`DXC-LICENSE-MS.txt`.

`LICENSE-MS.txt` is not an open-source license: it permits installation/use
solely on Windows and imposes separate requirements and restrictions on any
redistribution of listed distributable object code. In particular, an
application must add significant primary functionality and impose protective
terms on distributors/end users. This summary is not a substitute for the
retained license text.

DXC is an Editor/Sandbox runtime dependency on Windows because Slang invokes it
in process. Any distribution of `dxcompiler.dll` or `dxil.dll` must include the
exact three archive license notices and satisfy all application, distributor,
and end-user requirements in `LICENSE-MS.txt`. Staging the notices makes build
output self-describing; it is not by itself redistribution clearance.
