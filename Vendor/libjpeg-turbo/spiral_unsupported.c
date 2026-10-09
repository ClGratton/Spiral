/*
 * spiral_unsupported.c
 *
 * Spiral-authored file; not part of libjpeg-turbo. The decoder master in
 * jdmaster.c unconditionally references the 9-to-12-bit module initializers.
 * Spiral compiles only the 8-bit sample path, so each initializer is replaced
 * by a stub that rejects the stream through the normal libjpeg error manager.
 * The signatures come from jpegint.h, so the compiler checks every stub
 * against upstream's prototype.
 */

#include "jinclude.h"
#include "jpeglib.h"
#include "jpegint.h"
#include "jerror.h"

#define SPIRAL_UNSUPPORTED_PRECISION(name) \
  void name(j_decompress_ptr cinfo) \
  { \
    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision); \
  }

#define SPIRAL_UNSUPPORTED_PRECISION_BUFFERED(name) \
  void name(j_decompress_ptr cinfo, boolean need_full_buffer) \
  { \
    (void)need_full_buffer; \
    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision); \
  }

SPIRAL_UNSUPPORTED_PRECISION(j12init_1pass_quantizer)
SPIRAL_UNSUPPORTED_PRECISION(j12init_2pass_quantizer)
SPIRAL_UNSUPPORTED_PRECISION(j12init_color_deconverter)
SPIRAL_UNSUPPORTED_PRECISION(j12init_inverse_dct)
SPIRAL_UNSUPPORTED_PRECISION(j12init_merged_upsampler)
SPIRAL_UNSUPPORTED_PRECISION(j12init_upsampler)
SPIRAL_UNSUPPORTED_PRECISION_BUFFERED(j12init_d_coef_controller)
SPIRAL_UNSUPPORTED_PRECISION_BUFFERED(j12init_d_main_controller)
SPIRAL_UNSUPPORTED_PRECISION_BUFFERED(j12init_d_post_controller)
