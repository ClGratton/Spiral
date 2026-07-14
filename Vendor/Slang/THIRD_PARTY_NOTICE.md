# Slang Runtime Notice And Distribution Gate

The installed Slang release package preserves its upstream `LICENSE` file, and
runtime staging copies it beside the compiler as `Slang-LICENSE.txt`. The
official binary package does not include a consolidated third-party-notices or
binary bill-of-materials file.

Slang is `Apache-2.0 WITH LLVM-exception`. The pinned upstream tag's
`.gitmodules`, release workflow, and source tree identify many possible
source/build inputs, but they do not prove which components are linked into
each official host binary. `Docs/DEPENDENCIES.md` links those pinned sources;
it intentionally does not label an inferred component list as a completed
notice audit.

Slang is an Editor/Sandbox runtime dependency because shaders are compiled in
process. The staging scripts copy the package license and this gate notice next
to the minimized compiler runtime, but redistribution remains blocked until a
release owner establishes the exact binary component/license closure and adds
all required notices to the product's third-party notice bundle. A generated
build output is not a cleared redistribution package.
