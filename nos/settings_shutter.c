#include <stddef.h>
#include "settings_shutter.h"
#include "gfx_cost.h"
static int floor256(int x) {
  int q = x / 256, r = x % 256;
  return q - (r < 0);
}
static uint32_t tent_area(int t) {
  if (t <= -256)
    return 0;
  if (t < 0)
    return (uint32_t)((t + 256) * (t + 256));
  if (t < 256)
    return 131072u - (uint32_t)((256 - t) * (256 - t));
  return 131072u;
}
int settings_shutter_kernel_make(settings_shutter_kernel *out, int exposure,
                                 int pose) {
  if (!out || exposure < -12288 || exposure > 12288 || pose < -4096 ||
      pose > 4096)
    return 0;
  settings_shutter_kernel k = {0};
  k.pose = pose;
  k.zero = exposure == 0;
  if (k.zero) {
    *out = k;
    return 1;
  }
  unsigned length = exposure < 0 ? (unsigned)-exposure : (unsigned)exposure;
  int a = -pose + (exposure < 0 ? exposure : 0), b = a + (int)length;
  k.full0 = -floor256(-a - 256);
  k.full1 = floor256(b - 256);
  k.den = 512u * length;
  k.reciprocal = (uint32_t)(((UINT64_C(1) << 32) + k.den - 1) / k.den);
  for (int j = floor256(a); j <= floor256(b) + 1; j++) {
    if (j >= k.full0 && j <= k.full1)
      continue;
    uint32_t w = tent_area(b - j * 256) - tent_area(a - j * 256);
    if (w) {
      if (k.n >= 4)
        return 0;
      k.offset[k.n] = j;
      k.weight[k.n++] = w;
    }
  }
  *out = k;
  return 1;
}
static unsigned tent_div(uint32_t n, const settings_shutter_kernel *k) {
  unsigned q = (unsigned)(((uint64_t)n * k->reciprocal) >> 32);
  /* Valid kernels bound den <= 6,291,456 and the rounded average <= 255.
   * The ceil reciprocal can overshoot by one, so q <= 256 and q*den fits
   * in 32 bits (<= 1,610,612,736). Only n*reciprocal needs a high word. */
  if (q * k->den > n)
    q--;
  return q;
}
void settings_shutter_prefix_rows(const uint8_t *s, uint16_t *p, int w, int h,
                                  int first, int end) {
  if (!s || !p || w <= 0 || h <= 0 || h > 120 || first < 0 || end > h ||
      first >= end)
    return;
  if (!first)
    for (int x = 0; x < w; x++)
      p[x] = 0;
  for (int y = first; y < end; y++)
    for (int x = 0; x < w; x++)
      p[(y + 1) * w + x] = (uint16_t)(p[y * w + x] + s[y * w + x]);
}
/* Frozen kernel/pose/exposure/palette revision are a caller-owned frame
 * snapshot. Prefix lifetime equals sharp source lifetime; neither can publish
 * independently. */
settings_shutter_coverage
settings_shutter_blit(gfx_fb *fb, const uint8_t *s, const uint16_t *p, int w,
                      int h, int x, int y, const uint16_t *palette,
                      const settings_shutter_kernel *k, int exposure, int pose,
                      int owner0, int owner1, unsigned key) {
  settings_shutter_coverage c = {0};
  if (!s || !p || !palette || !k || w <= 0 || w > 343 || h <= 0 || h > 120 ||
      exposure < -12288 || exposure > 12288 || pose < -4096 || pose > 4096 ||
      k->pose != pose || k->zero != (exposure == 0) || k->n < 0 || k->n > 4)
    return c;
  int a = -pose + (exposure < 0 ? exposure : 0),
      b = -pose + (exposure > 0 ? exposure : 0);
  int off0 = floor256(a), off1 = -floor256(-b);
  if (!exposure)
    off1 = off0 + 1;
  int top = y - off1, bottom = y + h - off0;
  if (top < owner0)
    top = owner0;
  if (bottom > owner1)
    bottom = owner1;
  if (top >= bottom)
    return c;
  key = gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_u(key, (unsigned)exposure),
                                         (unsigned)pose),
                              (unsigned)x),
                   (unsigned)y);
  key = gfx_hash_u(gfx_hash_u(key, (unsigned)owner0), (unsigned)owner1);
  if (gfx_rec_cmd(fb, x, top, x + w, bottom, key))
    return c;
  int cx0 = fb->cx0, cx1 = fb->cx1;
  if (cx1 <= cx0) {
    cx0 = 0;
    cx1 = GFX_W;
  }
  if (cx0 < 0)
    cx0 = 0;
  if (cx1 > GFX_W)
    cx1 = GFX_W;
  if (cx0 < fb->x_base)
    cx0 = fb->x_base;
  int endx = fb->x_base + (fb->stride ? fb->stride : GFX_W);
  if (cx1 > endx)
    cx1 = endx;
  int bx = x + fb->ox, left = cx0 - bx, right = cx1 - bx;
  if (left < 0)
    left = 0;
  if (right > w)
    right = w;
  int first = top - y, end = bottom - y, by = y + fb->oy - fb->y_base;
  if (first < -by)
    first = -by;
  if (end > fb->h - by)
    end = fb->h - by;
  if (first < -y - fb->oy)
    first = -y - fb->oy;
  if (end > GFX_H - y - fb->oy)
    end = GFX_H - y - fb->oy;
  if (left >= right || first >= end)
    return c;
  c = (settings_shutter_coverage){x + left, y + first, x + right, y + end,
                                  (unsigned)(right - left) *
                                      (unsigned)(end - first)};
  /* Placeholder mask-class accounting, NOT a measured shutter target cost. */
  GFX_COST(GFX_COST_MASK, c.pixels);
  for (int yy = first; yy < end; yy++) {
    const uint16_t *p0 = NULL, *p1 = NULL;
    const uint8_t *edge[4];
    uint32_t weight[4];
    int n = 0;
    if (k->zero) {
      int j = floor256(-k->pose), f = -k->pose - j * 256;
      for (int side = 0; side < 2; side++) {
        int sy = yy + j + side;
        unsigned wt = side ? (unsigned)f : 256u - (unsigned)f;
        if (sy >= 0 && sy < h && wt) {
          edge[n] = s + sy * w;
          weight[n++] = wt;
        }
      }
    } else {
      if (k->full0 <= k->full1) {
        int lo = yy + k->full0, hi = yy + k->full1 + 1;
        if (lo > h)
          lo = h;
        if (hi > h)
          hi = h;
        if (lo > 0)
          p0 = p + lo * w;
        if (hi > 0)
          p1 = p + hi * w;
      }
      for (int e = 0; e < k->n; e++) {
        int sy = yy + k->offset[e];
        if (sy >= 0 && sy < h) {
          edge[n] = s + sy * w;
          weight[n++] = k->weight[e];
        }
      }
    }
    uint16_t *d = gfx_row(fb, by + yy);
#define SHUTTER_ROW(STORE)                                                     \
  do {                                                                         \
    for (int xx = left; xx < right; xx++) {                                    \
      unsigned sum = (p1 ? p1[xx] : 0) - (p0 ? p0[xx] : 0);                    \
      uint32_t v = sum * 131072u;                                              \
      for (int e = 0; e < n; e++)                                              \
        v += (uint32_t)edge[e][xx] * weight[e];                                \
      unsigned intensity =                                                     \
          k->zero ? (v + 128u) >> 8 : tent_div(v + k->den / 2, k);             \
      uint16_t color = palette[intensity];                                     \
      d[bx + xx] = (STORE);                                                    \
    }                                                                          \
  } while (0)
    if (!fb->dim)
      SHUTTER_ROW(color);
    else
      SHUTTER_ROW(gfx_ink(fb, color));
#undef SHUTTER_ROW
  }
  return c;
}

