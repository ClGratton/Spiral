/*
 * Hand-written for Spiral from libjpeg-turbo 3.2.0 src/jconfigint.h.in for a
 * portable GCC/Clang/MSVC decompression-only build. See SPIRAL_NOTES.md.
 */

#include <stdint.h>

/* libjpeg-turbo build number */
#define BUILD  "spiral"

/* Symbol visibility is irrelevant for a statically linked private subset. */
#define HIDDEN

/* Compiler's inline keyword */
#undef inline

/* How to obtain function inlining. */
#if defined(_MSC_VER)
#define INLINE  __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define INLINE  __inline__ __attribute__((always_inline))
#else
#define INLINE  inline
#endif

/* Environment-variable overrides are not part of the Spiral decoder contract. */
#define NO_GETENV
#define NO_PUTENV

/* Thread-local storage is only used by the TurboJPEG API, which is not built. */
#define THREAD_LOCAL

/* Define to the full name of this package. */
#define PACKAGE_NAME  "libjpeg-turbo"

/* Version number of package */
#define VERSION  "3.2.0"

/* The size of `size_t', as computed by sizeof. */
#if SIZE_MAX == 0xffffffffu
#define SIZEOF_SIZE_T  4
#else
#define SIZEOF_SIZE_T  8
#endif

#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define FALLTHROUGH  __attribute__((fallthrough));
#else
#define FALLTHROUGH
#endif
#else
#define FALLTHROUGH
#endif

/*
 * Define BITS_IN_JSAMPLE as either
 *   8   for 8-bit sample values (the usual setting)
 *   12  for 12-bit sample values
 * Only 8 and 12 are legal data precisions for lossy JPEG according to the
 * JPEG standard, and the IJG code does not support anything else!
 */

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8      /* use 8 or 12 */
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD

#undef WITH_PROFILE
