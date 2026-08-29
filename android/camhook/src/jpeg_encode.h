#pragma once

// NV21 → JPEG encoder for photo-mode BLOB substitution. Backed by
// stb_image_write (single-header public-domain). Encodes RGB-encoded
// JPEG at the requested quality.
// Returns 0 on success, negative error code on failure:
//   -1: invalid args (null buffers, zero dims, dst too small)
//   -2: malloc failed for the RGB scratch buffer
//   -3: stb encoder failed or output overflowed dst capacity

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  int cr_nv21_to_jpeg(const uint8_t* nv21, int src_w, int src_h, uint8_t* dst, size_t dst_cap, int quality, size_t* out_written);

#ifdef __cplusplus
}
#endif
