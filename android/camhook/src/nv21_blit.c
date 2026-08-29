// Fast CPU bilinear NV21 blit.
// Integer math with 16.16 fixed-point interpolators keeps it branch-lean:
//   step_x = (src_w << 16) / dst_w     (Q16)
//   for each dst row y:
//     sy = (y * src_h) / dst_h
//     for each dst col x (Q16 accumulator):
//       sx = accumulator >> 16
//       fx = accumulator & 0xffff
//       sample <- blend(src[sy][sx], src[sy][sx+1], fx)
// UV plane is half-res → same math with dst_w/2 and dst_h/2.

#include "nv21_blit.h"
#include <string.h>

static inline uint8_t blend(uint8_t a, uint8_t b, uint32_t fx_q16)
{
  // fx in [0,65535], blend = a + (b-a)*fx>>16.
  int32_t d = (int32_t) b - (int32_t) a;
  return (uint8_t) ((int32_t) a + ((d * (int32_t) fx_q16) >> 16));
}

// Must be ≥ the `W > 8192` guard in hook_main.cpp::overwrite_nv21. A Redmi
// Note 8T in 48 MP still capture mode hands us an 8000-px-wide buffer —
// the original 4096 stack arrays here overflowed, tripped
// `-fstack-protector`, and SIGABRT'd cameraserver. Bumped to 8192.
#define CR_BLIT_MAX_DST_W 8192

static void scale_plane_bilinear(const uint8_t* src, int src_w, int src_h, int src_stride, uint8_t* dst, int dst_w, int dst_h, int dst_stride)
{
  if (dst_w <= 0 || dst_w > CR_BLIT_MAX_DST_W)
    return;
  // Need at least 2x2 source samples for bilinear taps. Anything smaller
  // would force `sx+1` past the buffer and read 1 byte off the end into
  // the chroma plane — see the bug we hit at right-edge clamp.
  if (src_w < 2 || src_h < 2)
    return;

  const uint32_t step_x = ((uint64_t) src_w << 16) / (uint32_t) dst_w;
  const uint32_t step_y = ((uint64_t) src_h << 16) / (uint32_t) dst_h;

  // Per-column precomputed source x + fractional offset on the stack.
  // 64 KB combined; cameraserver pthreads have at least 1 MB stack so
  // this is comfortably safe. We tried `static __thread` but bionic's
  // lazy DTV allocation for dlopen'd-into-already-running-process libs
  // is fragile for large TLS sections (>= 64 KB) — observed cameraserver
  // SIGSEGV on first proxy call into the blit when TLS-allocated.
  // Stack is the boring-but-bulletproof choice.
  // Right-edge clamp: clamp `sx` to `src_w - 2` (not `src_w - 1`) so
  // `sx+1` never lands one byte past the plane. When clamped we also
  // force `fx = 0xffff` so the bilinear blend collapses to "use the
  // right neighbour" — visually equivalent to the old "sample[sx]"
  // behaviour, but without the OOB read.
  uint32_t cols_sx[CR_BLIT_MAX_DST_W];
  uint32_t cols_fx[CR_BLIT_MAX_DST_W];
  for (int x = 0; x < dst_w; ++x)
  {
    uint32_t ax = x * step_x;
    uint32_t sx = ax >> 16;
    uint32_t fx = ax & 0xffffu;
    if (sx >= (uint32_t) (src_w - 1))
    {
      sx = (uint32_t) (src_w - 2);
      fx = 0xffffu;
    }
    cols_sx[x] = sx;
    cols_fx[x] = fx;
  }

  for (int y = 0; y < dst_h; ++y)
  {
    uint32_t ay = y * step_y;
    uint32_t sy = ay >> 16;
    uint32_t fy = ay & 0xffffu;
    // Same clamp applies vertically — sy+1 must stay inside the plane.
    if (sy >= (uint32_t) (src_h - 1))
    {
      sy = (uint32_t) (src_h - 2);
      fy = 0xffffu;
    }

    const uint8_t* r0 = src + (size_t) sy * src_stride;
    const uint8_t* r1 = src + (size_t) (sy + 1) * src_stride;
    uint8_t* drow = dst + (size_t) y * dst_stride;

    for (int x = 0; x < dst_w; ++x)
    {
      uint32_t sx = cols_sx[x];
      uint32_t fx = cols_fx[x];
      uint8_t p00 = r0[sx];
      uint8_t p01 = r0[sx + 1];
      uint8_t p10 = r1[sx];
      uint8_t p11 = r1[sx + 1];
      uint8_t top = blend(p00, p01, fx);
      uint8_t bot = blend(p10, p11, fx);
      drow[x] = blend(top, bot, fy);
    }
  }
}

// The VU plane is two interleaved chroma bytes per sample, so scale them as
// a single 16-bit quantity (conceptually one "VU pixel"). `dst_swap` flips
// the two bytes per pair — use it when the destination is NV12 (U first)
// while our source is NV21 (V first).
static void scale_vu_bilinear(const uint8_t* src, int src_w_pairs, int src_h, uint8_t* dst, int dst_w_pairs, int dst_h, int dst_stride_bytes, int dst_swap)
{
  // Treat the source and destination as arrays of VU pairs (2 bytes each).
  // Source stride is src_w_pairs*2 bytes; dst stride is dst_stride_bytes.
  if (dst_w_pairs <= 0 || dst_w_pairs > CR_BLIT_MAX_DST_W)
    return;
  if (src_w_pairs < 2 || src_h < 2)
    return;

  const uint32_t step_x = ((uint64_t) src_w_pairs << 16) / (uint32_t) dst_w_pairs;
  const uint32_t step_y = ((uint64_t) src_h << 16) / (uint32_t) dst_h;

  uint32_t cols_sx[CR_BLIT_MAX_DST_W];
  uint32_t cols_fx[CR_BLIT_MAX_DST_W];
  for (int x = 0; x < dst_w_pairs; ++x)
  {
    uint32_t ax = x * step_x;
    uint32_t sx = ax >> 16;
    uint32_t fx = ax & 0xffffu;
    if (sx >= (uint32_t) (src_w_pairs - 1))
    {
      sx = (uint32_t) (src_w_pairs - 2);
      fx = 0xffffu;
    }
    cols_sx[x] = sx;
    cols_fx[x] = fx;
  }

  const int off_v = dst_swap ? 1 : 0;
  const int off_u = dst_swap ? 0 : 1;

  for (int y = 0; y < dst_h; ++y)
  {
    uint32_t ay = y * step_y;
    uint32_t sy = ay >> 16;
    uint32_t fy = ay & 0xffffu;
    if (sy >= (uint32_t) (src_h - 1))
    {
      sy = (uint32_t) (src_h - 2);
      fy = 0xffffu;
    }

    const uint8_t* r0 = src + (size_t) sy * src_w_pairs * 2;
    const uint8_t* r1 = src + (size_t) (sy + 1) * src_w_pairs * 2;
    uint8_t* drow = dst + (size_t) y * dst_stride_bytes;

    for (int x = 0; x < dst_w_pairs; ++x)
    {
      uint32_t sx = cols_sx[x];
      uint32_t fx = cols_fx[x];
      uint8_t v00 = r0[sx * 2 + 0], u00 = r0[sx * 2 + 1];
      uint8_t v01 = r0[sx * 2 + 2], u01 = r0[sx * 2 + 3];
      uint8_t v10 = r1[sx * 2 + 0], u10 = r1[sx * 2 + 1];
      uint8_t v11 = r1[sx * 2 + 2], u11 = r1[sx * 2 + 3];
      uint8_t vT = blend(v00, v01, fx), vB = blend(v10, v11, fx);
      uint8_t uT = blend(u00, u01, fx), uB = blend(u10, u11, fx);
      drow[x * 2 + off_v] = blend(vT, vB, fy);
      drow[x * 2 + off_u] = blend(uT, uB, fy);
    }
  }
}

void cr_nv21_blit(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped)
{
  // Stride-aware equal-resolution fast path. The original gate required
  // `dst_y_stride == dst_w && dst_uv_stride == dst_w` which is rarely
  // true on Qualcomm gralloc — it pads strides up to 64-byte boundaries
  // (e.g. dst_w=1280 → dst_y_stride=1344). The padded case used to
  // fall through to the bilinear scaler even though source and dest
  // dimensions matched, wasting CPU on a no-op resize.
  // New gate: same resolution, no chroma swap. Per-row memcpy works
  // regardless of how big the stride padding is — we just memcpy
  // `dst_w` (or `dst_w` for the VU pair = 2 * (dst_w/2)) bytes per row
  // and skip the rest of the stride padding.
  if (src_w == dst_w && src_h == dst_h && !dst_uv_swapped)
  {
    for (int y = 0; y < dst_h; ++y)
    {
      memcpy(dst_y + (size_t) y * dst_y_stride, src + (size_t) y * src_w, (size_t) src_w);
    }
    const uint8_t* src_vu = src + (size_t) src_w * src_h;
    // VU plane is half-height, full-width-bytes (= dst_w bytes per
    // row, since each row holds dst_w/2 VU pairs × 2 bytes).
    const int vu_row_bytes = src_w;
    for (int y = 0; y < dst_h / 2; ++y)
    {
      memcpy(dst_vu + (size_t) y * dst_uv_stride, src_vu + (size_t) y * vu_row_bytes, (size_t) vu_row_bytes);
    }
    return;
  }

  // Y plane.
  scale_plane_bilinear(src, src_w, src_h, src_w, dst_y, dst_w, dst_h, dst_y_stride);

  // VU plane — half the pixel count horizontally/vertically, 2 bytes each.
  const uint8_t* src_vu = src + (size_t) src_w * src_h;
  scale_vu_bilinear(src_vu, src_w / 2, src_h / 2, dst_vu, dst_w / 2, dst_h / 2, dst_uv_stride, dst_uv_swapped);
}
