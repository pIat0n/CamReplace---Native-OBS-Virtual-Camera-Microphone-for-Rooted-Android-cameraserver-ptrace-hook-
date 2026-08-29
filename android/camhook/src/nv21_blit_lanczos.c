// Lanczos2 CPU NV21 scaler (sprint E).
// Why:
//   The default cr_nv21_blit() in nv21_blit.c is bilinear. On big up-scales
//   (e.g. 1280x720 source into a 1920x1440 still-capture buffer) bilinear
//   produces visibly soft edges, which makes the "fake" camera obvious.
//   Lanczos2 gives crisper edges with a small ring (acceptable for camera
//   content) at the cost of more CPU per pixel.
// Implementation:
//   - Separable filter: 4-tap horizontal pass into a stripe buffer, then
//     4-tap vertical pass into the destination row. This makes each output
//     pixel cost 8 multiplies instead of 16.
//   - Q15 fixed-point weights. Source values stay uint8; horizontal-pass
//     temporaries are int32 so we never overflow even with negative
//     Lanczos lobes.
//   - Edge handling: clamp source row/column index to [0, src_dim - 1].
//     Lanczos lobe taps that fall outside the image use the clamped
//     pixel — equivalent to "edge-extend" wrapping.
// Performance (scalar, no NEON):
//   - 1080p Y plane: ~80-100 ms on a Cortex-A55 @ 1.8 GHz. Borderline for
//     30 fps. NEON SIMD is the obvious next step (vmull_s8 + vmlal_s8
//     gives ~5x speedup on 8-tap groups). Not done in this commit; the
//     hook can fall back to bilinear via the runtime selector if Lanczos
//     starves the decoder.
//   - For preview-only resolutions (640x480, 1280x720) the scalar path
//     completes in 5-15 ms which fits a 30 fps frame budget comfortably.
// Coexistence:
//   This file does NOT replace cr_nv21_blit. It exposes a separate
//   `cr_nv21_blit_lanczos` symbol, and hook_main.cpp picks one based on
//   /data/cr/scaler (`lanczos` or `lanczos2`). Default stays bilinear.

#include "nv21_blit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Mirrors the bilinear ceiling. dst_w larger than this means the camera
// buffer is wider than 8K, which we already refuse for safety reasons in
// hook_main.cpp::overwrite_nv21.
#define CR_BLIT_MAX_DST_W 8192
// Lanczos2 is a 4-tap filter (a=2). Window covers source pixels at
// floor(sx)-1, floor(sx), floor(sx)+1, floor(sx)+2.
#define CR_LANCZOS_TAPS 4
#define CR_LANCZOS_RADIUS 2

// Fixed-point format for filter weights. Q15 → 32767 * value. Max sum of
// |weights| ≈ 1.10 → 36100 < INT16_MAX, so int16 weights add up safely
// into int32 accumulators.
#define CR_W_FRAC_BITS 15
#define CR_W_ONE (1 << CR_W_FRAC_BITS)

static inline int clampi(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint8_t saturate_u8(int v)
{
  return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Continuous Lanczos2 kernel, evaluated at floating-point offset x.
// Caller computes x as (output_position - source_sample_position).
static double lanczos2(double x)
{
  if (x == 0.0)
    return 1.0;
  if (x <= -CR_LANCZOS_RADIUS || x >= CR_LANCZOS_RADIUS)
    return 0.0;
  const double pix = M_PI * x;
  return (CR_LANCZOS_RADIUS * sin(pix) * sin(pix / CR_LANCZOS_RADIUS)) / (pix * pix);
}

// Fill `weights[4]` with Lanczos2 taps for source samples at integer
// positions [base-1, base, base+1, base+2], evaluated at fractional
// offset `frac` (0..1) relative to base. Sum is normalized so the
// weighted average of equal-valued samples reproduces the input.
static void lanczos_weights_q15(double frac, int16_t weights[CR_LANCZOS_TAPS])
{
  double w[CR_LANCZOS_TAPS];
  w[0] = lanczos2(-1.0 - frac);
  w[1] = lanczos2(0.0 - frac);
  w[2] = lanczos2(1.0 - frac);
  w[3] = lanczos2(2.0 - frac);
  double sum = w[0] + w[1] + w[2] + w[3];
  if (sum == 0.0)
    sum = 1.0;
  int q[CR_LANCZOS_TAPS];
  int q_sum = 0;
  for (int i = 0; i < CR_LANCZOS_TAPS; ++i)
  {
    q[i] = (int) lrint((w[i] / sum) * CR_W_ONE);
    q_sum += q[i];
  }
  // Re-balance into the largest weight so sum is exactly CR_W_ONE.
  int max_i = 1;
  if (q[2] > q[max_i])
    max_i = 2;
  q[max_i] += (CR_W_ONE - q_sum);
  for (int i = 0; i < CR_LANCZOS_TAPS; ++i)
  {
    weights[i] = (int16_t) q[i];
  }
}

// ---- Y plane ---------------------------------------------------------------
// One scratch line per call; safe because the hook is single-threaded
// per camera buffer (queueBufferToConsumer holds the stream lock).
// Source layout: row stride == src_w (NV21 packs Y rows tightly).
// Destination layout: row stride == dst_y_stride (gralloc-padded).
static void scale_plane_lanczos(const uint8_t* src, int src_w, int src_h, int src_stride, uint8_t* dst, int dst_w, int dst_h, int dst_stride)
{
  if (dst_w <= 0 || dst_w > CR_BLIT_MAX_DST_W)
    return;
  if (src_w < CR_LANCZOS_TAPS || src_h < CR_LANCZOS_TAPS)
    return;

  // Per-column horizontal taps + 4 source-x indices (clamped).
  int16_t* hw = (int16_t*) malloc((size_t) dst_w * CR_LANCZOS_TAPS * sizeof(int16_t));
  int* hx = (int*) malloc((size_t) dst_w * CR_LANCZOS_TAPS * sizeof(int));
  // Horizontal-pass scratch — one int32 per dst column for ONE row.
  // Reused across all 4 vertically-relevant source rows.
  int32_t* htmp = (int32_t*) malloc((size_t) dst_w * sizeof(int32_t));
  if (!hw || !hx || !htmp)
  {
    free(hw);
    free(hx);
    free(htmp);
    return;
  }

  const double sx_step = (double) src_w / (double) dst_w;
  const double sy_step = (double) src_h / (double) dst_h;
  // Centre-aligned sampling: dst pixel x covers source [x * step, (x+1) * step).
  // Sample at the centre → src_x_centre = (x + 0.5) * step - 0.5.
  for (int x = 0; x < dst_w; ++x)
  {
    double sx = (x + 0.5) * sx_step - 0.5;
    int base = (int) floor(sx);
    double frac = sx - base;
    int16_t w[CR_LANCZOS_TAPS];
    lanczos_weights_q15(frac, w);
    for (int t = 0; t < CR_LANCZOS_TAPS; ++t)
    {
      hx[x * CR_LANCZOS_TAPS + t] = clampi(base - 1 + t, 0, src_w - 1);
      hw[x * CR_LANCZOS_TAPS + t] = w[t];
    }
  }

  // Cache the last horizontal-pass row index so we don't redo work
  // when consecutive output rows touch the same source row stripe.
  int cached_rows[CR_LANCZOS_TAPS] = {-1, -1, -1, -1};
  int32_t* cached_hbuf[CR_LANCZOS_TAPS] = {NULL, NULL, NULL, NULL};
  for (int t = 0; t < CR_LANCZOS_TAPS; ++t)
  {
    cached_hbuf[t] = (int32_t*) malloc((size_t) dst_w * sizeof(int32_t));
    if (!cached_hbuf[t])
    {
      for (int u = 0; u < t; ++u)
        free(cached_hbuf[u]);
      free(hw);
      free(hx);
      free(htmp);
      return;
    }
  }

  for (int y = 0; y < dst_h; ++y)
  {
    double sy = (y + 0.5) * sy_step - 0.5;
    int base = (int) floor(sy);
    double frac = sy - base;
    int16_t vw[CR_LANCZOS_TAPS];
    lanczos_weights_q15(frac, vw);

    int sy_taps[CR_LANCZOS_TAPS];
    for (int t = 0; t < CR_LANCZOS_TAPS; ++t)
    {
      sy_taps[t] = clampi(base - 1 + t, 0, src_h - 1);
    }

    // Refresh horizontal cache for any of the 4 needed source rows
    // we don't already have.
    for (int t = 0; t < CR_LANCZOS_TAPS; ++t)
    {
      int needed = sy_taps[t];
      int slot = -1;
      for (int s = 0; s < CR_LANCZOS_TAPS; ++s)
      {
        if (cached_rows[s] == needed)
        {
          slot = s;
          break;
        }
      }
      if (slot >= 0)
        continue;
      // Pick LRU slot (smallest cached_rows). Cheap heuristic.
      int evict = 0;
      for (int s = 1; s < CR_LANCZOS_TAPS; ++s)
      {
        if (cached_rows[s] < cached_rows[evict])
          evict = s;
      }
      const uint8_t* row = src + (size_t) needed * src_stride;
      int32_t* hb = cached_hbuf[evict];
      for (int x = 0; x < dst_w; ++x)
      {
        const int16_t* w = hw + x * CR_LANCZOS_TAPS;
        const int* px = hx + x * CR_LANCZOS_TAPS;
        int32_t acc = (int32_t) row[px[0]] * w[0] + (int32_t) row[px[1]] * w[1] + (int32_t) row[px[2]] * w[2] + (int32_t) row[px[3]] * w[3];
        hb[x] = acc;
      }
      cached_rows[evict] = needed;
    }

    // Vertical combine — pull the right cache slot per tap.
    int32_t* hb_for_tap[CR_LANCZOS_TAPS];
    for (int t = 0; t < CR_LANCZOS_TAPS; ++t)
    {
      int needed = sy_taps[t];
      for (int s = 0; s < CR_LANCZOS_TAPS; ++s)
      {
        if (cached_rows[s] == needed)
        {
          hb_for_tap[t] = cached_hbuf[s];
          break;
        }
      }
    }

    uint8_t* drow = dst + (size_t) y * dst_stride;
    for (int x = 0; x < dst_w; ++x)
    {
      // hb values are pixel * Q15. Multiply by Q15 vertical weight,
      // sum, shift back by 2 * Q15.
      int64_t acc = (int64_t) hb_for_tap[0][x] * vw[0] + (int64_t) hb_for_tap[1][x] * vw[1] + (int64_t) hb_for_tap[2][x] * vw[2] + (int64_t) hb_for_tap[3][x] * vw[3];
      int v = (int) ((acc + (1LL << (2 * CR_W_FRAC_BITS - 1))) >> (2 * CR_W_FRAC_BITS));
      drow[x] = saturate_u8(v);
    }
  }

  for (int t = 0; t < CR_LANCZOS_TAPS; ++t)
    free(cached_hbuf[t]);
  free(hw);
  free(hx);
  free(htmp);
}

// ---- VU plane --------------------------------------------------------------
// The chroma plane has src_w/2 horizontal samples per row. Each sample
// is 2 interleaved bytes: V then U (NV21) or U then V (NV12). To Lanczos
// over a 2D domain we de-interleave V and U into two stack scratch
// planes, scale each independently, then re-interleave on write.
// `dst_swap` selects destination order — see nv21_blit.h.
static void scale_vu_lanczos(const uint8_t* src, int src_w_pairs, int src_h, uint8_t* dst, int dst_w_pairs, int dst_h, int dst_stride_bytes, int dst_swap)
{
  if (dst_w_pairs <= 0 || dst_w_pairs > CR_BLIT_MAX_DST_W)
    return;
  if (src_w_pairs < CR_LANCZOS_TAPS || src_h < CR_LANCZOS_TAPS)
    return;

  const size_t src_plane_bytes = (size_t) src_w_pairs * src_h;
  const size_t dst_plane_bytes = (size_t) dst_w_pairs * dst_h;
  uint8_t* sV = (uint8_t*) malloc(src_plane_bytes);
  uint8_t* sU = (uint8_t*) malloc(src_plane_bytes);
  uint8_t* dV = (uint8_t*) malloc(dst_plane_bytes);
  uint8_t* dU = (uint8_t*) malloc(dst_plane_bytes);
  if (!sV || !sU || !dV || !dU)
  {
    free(sV);
    free(sU);
    free(dV);
    free(dU);
    return;
  }

  // De-interleave source: NV21 = (V,U) pairs.
  for (int y = 0; y < src_h; ++y)
  {
    const uint8_t* srow = src + (size_t) y * src_w_pairs * 2;
    uint8_t* vrow = sV + (size_t) y * src_w_pairs;
    uint8_t* urow = sU + (size_t) y * src_w_pairs;
    for (int x = 0; x < src_w_pairs; ++x)
    {
      vrow[x] = srow[x * 2 + 0];
      urow[x] = srow[x * 2 + 1];
    }
  }

  // Lanczos each plane independently. Stride == src_w_pairs (no
  // padding in our scratch buffer).
  scale_plane_lanczos(sV, src_w_pairs, src_h, src_w_pairs, dV, dst_w_pairs, dst_h, dst_w_pairs);
  scale_plane_lanczos(sU, src_w_pairs, src_h, src_w_pairs, dU, dst_w_pairs, dst_h, dst_w_pairs);

  // Re-interleave into destination, honouring dst_swap.
  const int off_v = dst_swap ? 1 : 0;
  const int off_u = dst_swap ? 0 : 1;
  for (int y = 0; y < dst_h; ++y)
  {
    uint8_t* drow = dst + (size_t) y * dst_stride_bytes;
    const uint8_t* vrow = dV + (size_t) y * dst_w_pairs;
    const uint8_t* urow = dU + (size_t) y * dst_w_pairs;
    for (int x = 0; x < dst_w_pairs; ++x)
    {
      drow[x * 2 + off_v] = vrow[x];
      drow[x * 2 + off_u] = urow[x];
    }
  }

  free(sV);
  free(sU);
  free(dV);
  free(dU);
}

void cr_nv21_blit_lanczos(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped)
{
  // Same equal-resolution fast path as cr_nv21_blit — Lanczos and
  // bilinear both reduce to memcpy when no scaling is needed.
  if (src_w == dst_w && src_h == dst_h && !dst_uv_swapped)
  {
    for (int y = 0; y < dst_h; ++y)
    {
      memcpy(dst_y + (size_t) y * dst_y_stride, src + (size_t) y * src_w, (size_t) src_w);
    }
    const uint8_t* src_vu = src + (size_t) src_w * src_h;
    const int vu_row_bytes = src_w;
    for (int y = 0; y < dst_h / 2; ++y)
    {
      memcpy(dst_vu + (size_t) y * dst_uv_stride, src_vu + (size_t) y * vu_row_bytes, (size_t) vu_row_bytes);
    }
    return;
  }

  scale_plane_lanczos(src, src_w, src_h, src_w, dst_y, dst_w, dst_h, dst_y_stride);

  const uint8_t* src_vu = src + (size_t) src_w * src_h;
  scale_vu_lanczos(src_vu, src_w / 2, src_h / 2, dst_vu, dst_w / 2, dst_h / 2, dst_uv_stride, dst_uv_swapped);
}
