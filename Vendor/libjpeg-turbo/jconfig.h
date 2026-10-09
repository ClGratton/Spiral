/*
 * Hand-written for Spiral from libjpeg-turbo 3.2.0 src/jconfig.h.in using the
 * default CMake configuration (no JPEG7/JPEG8 API emulation, no SIMD, no
 * arithmetic coding). See SPIRAL_NOTES.md.
 */

/* Version ID for the JPEG library. */
#define JPEG_LIB_VERSION  62

/* libjpeg-turbo version */
#define LIBJPEG_TURBO_VERSION  3.2.0

/* libjpeg-turbo version in integer form */
#define LIBJPEG_TURBO_VERSION_NUMBER  3002000

/* Arithmetic encoding and decoding are intentionally not built. */
/* #undef C_ARITH_CODING_SUPPORTED */
/* #undef D_ARITH_CODING_SUPPORTED */

/* Support in-memory source/destination managers */
#define MEM_SRCDST_SUPPORTED  1

/* No SIMD extensions are built. */
/* #undef WITH_SIMD */

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8
#endif

#ifdef _WIN32

#undef RIGHT_SHIFT_IS_UNSIGNED

/* Define "boolean" as unsigned char, not int, per Windows custom */
#ifndef __RPCNDR_H__            /* don't conflict if rpcndr.h already read */
typedef unsigned char boolean;
#endif
#define HAVE_BOOLEAN            /* prevent jmorecfg.h from redefining it */

/* Define "INT32" as int, not long, per Windows custom */
#if !(defined(_BASETSD_H_) || defined(_BASETSD_H))   /* don't conflict if basetsd.h already read */
typedef short INT16;
typedef signed int INT32;
#endif
#define XMD_H                   /* prevent jmorecfg.h from redefining it */

#else

/* All supported compilers use arithmetic right shifts of signed values. */
/* #undef RIGHT_SHIFT_IS_UNSIGNED */

#endif
