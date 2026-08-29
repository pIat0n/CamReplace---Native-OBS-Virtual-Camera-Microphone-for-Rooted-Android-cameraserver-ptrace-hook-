// NV21 → JPEG encoder used by photo-mode BLOB substitution.
// We bundle stb_image_write (single-header, public domain). It's about
// 70KB of source and adds ~50KB to the stripped libcr_camhook.so. No
// external link dependency, which matters because cameraserver's
// process is locked down — bringing in libjpeg.so via dlopen works on
// most Androids but is fragile across vendor builds.
// Pipeline per encode call:
//   1. Allocate w*h*3 byte RGB scratch on heap (~6 MB at 1080p, ~36 MB
//      at 4K — fine inside cameraserver, freed immediately).
//   2. NV21 → RGB888 conversion (BT.601 limited-range Y'CbCr math).
//   3. stbi_write_jpg_to_func writes JPEG bytes via callback directly
//      into the destination buffer (the BLOB camera buffer), tracking
//      offset and refusing to overflow.
// Returns 0 on success, negative on failure. *out_written gets the
// JPEG payload size on success (trailer written separately by caller).

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
// We don't need most of the formats — drop everything except JPEG to
// shave the binary. PNG/BMP/TGA/HDR exports out, JPEG stays in.
#define STBIW_ASSERT(x) ((void) 0)
#include "../../../third_party/stb/stb_image_write.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg_encode.h"
struct EncodeCtx
{
  uint8_t* dst;
  size_t cap;
  size_t off;
  int overflow;
};

static void write_cb(void* user, void* data, int size)
{
  struct EncodeCtx* ctx = (struct EncodeCtx*) user;
  if (ctx->overflow)
    return;
  if ((size_t) size <= 0)
    return;
  if (ctx->off + (size_t) size > ctx->cap)
  {
    ctx->overflow = 1;
    return;
  }
  memcpy(ctx->dst + ctx->off, data, (size_t) size);
  ctx->off += (size_t) size;
}

static inline int clampi(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// NV21 → RGB888. NV21 is Y plane (sw*sh bytes) followed by interleaved
// V,U pairs at half-resolution ((sw/2)*(sh/2) pairs * 2 bytes). The
// chroma row index is y/2; the chroma byte pair starts at (x/2)*2.
static void nv21_to_rgb24(const uint8_t* nv21, int sw, int sh, uint8_t* rgb)
{
  const uint8_t* y_plane = nv21;
  const uint8_t* vu_plane = nv21 + (size_t) sw * sh;
  for (int y = 0; y < sh; ++y)
  {
    const int sy_uv = y >> 1;
    const uint8_t* yr = y_plane + (size_t) y * sw;
    const uint8_t* vu = vu_plane + (size_t) sy_uv * sw;
    uint8_t* drow = rgb + (size_t) y * sw * 3;
    for (int x = 0; x < sw; ++x)
    {
      const int sx_uv = (x >> 1) << 1;
      const int Y = yr[x];
      const int V = vu[sx_uv + 0];
      const int U = vu[sx_uv + 1];
      // BT.601 limited-range YUV → RGB.
      const int C = Y - 16;
      const int D = U - 128;
      const int E = V - 128;
      int R = (298 * C + 409 * E + 128) >> 8;
      int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
      int B = (298 * C + 516 * D + 128) >> 8;
      drow[x * 3 + 0] = (uint8_t) clampi(R, 0, 255);
      drow[x * 3 + 1] = (uint8_t) clampi(G, 0, 255);
      drow[x * 3 + 2] = (uint8_t) clampi(B, 0, 255);
    }
  }
}

int cr_nv21_to_jpeg(const uint8_t* nv21, int sw, int sh, uint8_t* dst, size_t dst_cap, int quality, size_t* out_written)
{
  if (!nv21 || sw < 8 || sh < 8 || !dst || dst_cap < 1024)
    return -1;
  if (quality < 1 || quality > 100)
    quality = 85;

  uint8_t* rgb = (uint8_t*) malloc((size_t) sw * sh * 3);
  if (!rgb)
    return -2;
  nv21_to_rgb24(nv21, sw, sh, rgb);

  struct EncodeCtx ctx;
  ctx.dst = dst;
  ctx.cap = dst_cap;
  ctx.off = 0;
  ctx.overflow = 0;

  int ok = stbi_write_jpg_to_func(write_cb, &ctx, sw, sh, 3, rgb, quality);
  free(rgb);

  if (!ok || ctx.overflow || ctx.off == 0)
    return -3;
  if (out_written)
    *out_written = ctx.off;
  return 0;
}
