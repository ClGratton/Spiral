# libjpeg-turbo 3.2.0 in Spiral

Source: official tag archive `https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.2.0.tar.gz`,
SHA-256 `980dd81f425082aa6d7c9e47fef27554ce7a9ffc8e2f6e863b97d263c5c50858`. Only the 8-bit libjpeg
decompression path is vendored. Every upstream file below is byte-identical to the tag's `src/`
(or repository root for the license files) except `jmorecfg.h`.

## Spiral-authored or modified files

| File | Reason |
| --- | --- |
| `jconfig.h`, `jconfigint.h`, `jversion.h` | Hand-written from `src/jconfig.h.in`, `src/jconfigint.h.in`, `src/jversion.h.in` with upstream's default CMake values (`JPEG_LIB_VERSION 62`, `MEM_SRCDST_SUPPORTED`, no SIMD, no arithmetic coding). `jconfigint.h` also defines `NO_GETENV`/`NO_PUTENV` so no environment variable can change decoder behavior, empties `HIDDEN`/`THREAD_LOCAL`, and derives `SIZEOF_SIZE_T` from `SIZE_MAX` for MSVC/GCC/Clang. |
| `jmorecfg.h` | One edit: the `D_LOSSLESS_SUPPORTED` capability line is replaced by a comment. This is upstream's documented compile-time switch; it removes 8/12/16-bit lossless decoding, so `jdlossls`, `jddiffct`, `jdlhuff` and the 16-bit paths are not needed. An SOF3 stream fails with `JERR_NOT_COMPILED`/`JERR_SOF_UNSUPPORTED`. |
| `spiral_unsupported.c` | `jdmaster.c` references the nine 9-to-12-bit module initializers unconditionally. Rather than compile the 12-bit wrappers, these stubs raise `JERR_BAD_PRECISION` through the normal error manager. Signatures are checked by the compiler against `jpegint.h`. |

Arithmetic decoding is disabled by leaving `D_ARITH_CODING_SUPPORTED` undefined in `jconfig.h`; `jaricom.c`
and `jdarith.c` are not vendored. The Spiral wrapper rejects `cinfo.arith_code` after `jpeg_read_header`.

## Compiled translation units (all listed explicitly in `Engine/premake5.lua`)

Direct: `jcomapi.c` (common destroy/abort), `jdapimin.c` (decompress object lifecycle, header reading),
`jdatasrc.c` (`jpeg_mem_src`), `jdhuff.c` (sequential Huffman; includes `jstdhuff.c`), `jdinput.c`,
`jdmarker.c`, `jdmaster.c`, `jdphuff.c` (progressive Huffman), `jerror.c`, `jmemmgr.c`,
`jmemnobs.c` (no backing store: a temp file can never be created), `spiral_unsupported.c`.

Precision wrappers (`wrapper/*-8.c`, each defines `BITS_IN_JSAMPLE 8` and includes the sibling source):
`jdapistd`, `jdcoefct`, `jdcolor` (includes `jdcolext.c`, `jdcol565.c`), `jddctmgr`, `jdmainct`,
`jdmerge` (includes `jdmrgext.c`, `jdmrg565.c`), `jdpostct`, `jdsample`, `jidctflt`, `jidctfst`,
`jidctint`, `jidctred`, `jquant1`, `jquant2`, `jutils`. The wrapper pattern is upstream's, so the
sibling sources are never compiled directly.

Headers kept because those units include them: `jdcoefct.h jdct.h jdhuff.h jdmainct.h jdmaster.h jdmerge.h
jdsample.h jerror.h jinclude.h jmemsys.h jmorecfg.h jpegapicomp.h jpegint.h jpeglib.h jsamplecomp.h`.
`LICENSE.md` (IJG + BSD-3-Clause + zlib terms) and `README.ijg` (the IJG license text) are kept.

## Deliberately not vendored

All compression sources (`jc*`, `jf*`), the TurboJPEG API and its helpers, `jdtrans.c` (coefficient
transcoding), `jdicc.c`/`jcicc.c`, `jdarith.c`/`jaricom.c`/`jcarith.c`, lossless decode
(`jdlossls.c`, `jddiffct.c`, `jdlhuff.c`), 12-bit and 16-bit wrappers, `jpeg_nbits.c`, every SIMD
directory and NASM input, command-line tools, test images, and the CMake build system.

Behavior for Spiral callers lives in `Engine/src/Engine/Assets/CommonImage.cpp`; this directory is
unmodified upstream apart from the files listed above.
