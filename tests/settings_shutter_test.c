#include "../nos/render_exposure.h"
#include "../nos/render_history.h"
/* Actual production Settings cache+renderer integration against independent
 * scalar. */
#define main settings_cache_baseline_main
#include "settings_cache_test.c"
#undef main
#include <assert.h>
#include <limits.h>
static unsigned sample(const uint8_t *s, int w, int h, int x, int y) {
  return y >= 0 && y < h ? s[y * w + x] : 0;
}
static unsigned rng = 71;
static unsigned rnd(void) {
  rng = rng * 1664525u + 1013904223u;
  return rng;
}
/* Independent integration by linear segments, not triangular basis
 * coefficients. */
static unsigned reference(const uint8_t *s, int w, int h, int x, int y, int exp,
                          int pose) {
  double a = y - pose / 256.0 + (exp < 0 ? exp / 256.0 : 0),
         b = a + fabs(exp / 256.0);
  if (!exp) {
    int j = (int)floor(a);
    double f = a - j;
    return (unsigned)floor(sample(s, w, h, x, j) * (1 - f) +
                           sample(s, w, h, x, j + 1) * f + .5);
  }
  double total = 0;
  for (int j = (int)floor(a); j < (int)ceil(b); j++) {
    double lo = fmax(a, j) - j, hi = fmin(b, j + 1) - j;
    if (hi <= lo)
      continue;
    double left = sample(s, w, h, x, j),
           delta = sample(s, w, h, x, j + 1) - left;
    total += left * (hi - lo) + delta * (hi * hi - lo * lo) * .5;
  }
  return (unsigned)floor(total / (b - a) + .5 + 1e-9);
}
static settings_shutter_kernel make_kernel(int exposure, int pose) {
  settings_shutter_kernel k;
  assert(settings_shutter_kernel_make(&k, exposure, pose));
  return k;
}
typedef struct {
  int e, kind, owner, w, h, x, y;
  unsigned old, now;
} fixture;
static fixture f[36];
static uint16_t palettes[2][17][256], pre[343 * 121], A[GFX_W * GFX_H + 2],
    B[GFX_W * GFX_H + 2], tile[GFX_W * 64 + 2];
static uint8_t gray[343 * 120];
static void load(int e) {
  entry_cache *r = &RC->entry[e];
  entry_image *im = &r->image[0];
  assert(r->ready && r->prefix);
  memcpy(gray, im->s.px, (size_t)im->s.w * im->s.h);
  memcpy(pre, r->prefix, r->prefix_bytes);
  for (int x = 0; x < im->s.w; x++) {
    unsigned sum = 0;
    assert(pre[x] == 0);
    for (int y = 0; y < im->s.h; y++) {
      sum += gray[y * im->s.w + x];
      assert(pre[(y + 1) * im->s.w + x] == sum);
    }
  }
}
static void setup(void) {
  set_settings_shutter_set(1);
  assert(set_settings_shutter_get());
  S.arm_e = -1;
  S.flash_e = -1;
  S.L.pressed = -1;
  S.build_t0 = nos_t - 100;
  tw_set(&S.arm_tw[0], 0);
  tw_set(&S.arm_tw[1], 0);
  tw_set(&S.grey_tw, 0);
  for (int i = 0; i < I_COUNT; i++) {
    S.vt0[i] = nos_t - 100;
    tw_set(&S.tog[i], S.tog[i].to);
  }
  for (int e = 0; e < NE; e++) {
    assert(entry_bake(e));
    entry_cache *r = &RC->entry[e];
    entry_image *im = &r->image[0];
    assert(r->prefix &&
           r->prefix_bytes == (unsigned)im->s.w * (im->s.h + 1) * 2);
    for (int k = 1; k < 7; k++)
      assert(!r->image[k].s.px);
    f[e] = (fixture){e,       E[e].kind, units_of(E[e].kind) * UNIT,
                     im->s.w, im->s.h,   im->x,
                     im->y,   0,         0};
  }
  memcpy(palettes, RC->lut, sizeof palettes);
}
static unsigned long long pixels;
static unsigned randcases(void) {
  int widths[] = {0,    1,    64,   128,  256,  384,  480,  928,   1535, 1536,
                  1537, 3583, 3584, 3585, 7167, 7168, 7169, 12287, 12288};
  for (int t = 0; t < 1200; t++) {
    int e = t % 36;
    load(e);
    fixture *q = &f[e];
    int length = widths[rnd() % (sizeof widths / sizeof *widths)];
    if (rnd() & 1)
      length = -length;
    int pose = (int)(rnd() % 1537) - 768;
    settings_shutter_kernel k = make_kernel(length, pose);
    int stride = 1 + rnd() % 466;
    gfx_fb fb = {0};
    fb.px = A + 1;
    fb.stride = stride;
    fb.x_base = (int)(rnd() % 470) - 2;
    fb.y_base = (int)(rnd() % 450) - 20;
    fb.h = 1 + rnd() % 64;
    fb.ox = (int)(rnd() % 101) - 50;
    fb.oy = (int)(rnd() % 101) - 50;
    fb.dim = rnd() % 33;
    fb.cx0 = fb.x_base;
    fb.cx1 = fb.x_base + stride;
    int x = fb.x_base - fb.ox - (int)(rnd() % 100),
        owner0 = fb.y_base - fb.oy - 20, owner1 = owner0 + q->owner,
        y = owner0 + q->y;
    const uint16_t *pal = palettes[rnd() % 2][rnd() % 17];
    for (unsigned i = 0; i < sizeof A / sizeof *A; i++)
      A[i] = B[i] = 0xa5a5;
    settings_shutter_coverage c =
        settings_shutter_blit(&fb, gray, pre, q->w, q->h, x, y, pal, &k, length,
                              pose, owner0, owner1, 10);
    unsigned touched = 0;
    int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
    double aa = (-pose + (length < 0 ? length : 0)) / 256.,
           bb = (-pose + (length > 0 ? length : 0)) / 256.;
    int off0 = (int)floor(aa), off1 = (int)ceil(bb);
    if (!length)
      off1 = off0 + 1;
    int top = y - off1, bottom = y + q->h - off0;
    if (top < owner0)
      top = owner0;
    if (bottom > owner1)
      bottom = owner1;
    for (int row = 0; row < fb.h; row++)
      for (int col = 0; col < stride; col++) {
        int sx = fb.x_base + col, sy = fb.y_base + row, lx = sx - fb.ox,
            ly = sy - fb.oy;
        int inside = sx >= 0 && sx < GFX_W && sy >= 0 && sy < GFX_H &&
                     sx >= fb.cx0 && sx < fb.cx1 && lx >= x && lx < x + q->w &&
                     ly >= top && ly < bottom;
        if (inside) {
          unsigned v =
              reference(gray, q->w, q->h, lx - x, ly - y, length, pose);
          B[1 + row * stride + col] = gfx_ink(&fb, pal[v]);
          touched++;
          pixels++;
          if (lx < minx)
            minx = lx;
          if (ly < miny)
            miny = ly;
          if (lx + 1 > maxx)
            maxx = lx + 1;
          if (ly + 1 > maxy)
            maxy = ly + 1;
        }
      }
    assert(!memcmp(A, B, sizeof A));
    assert(c.pixels == touched);
    if (touched)
      assert(c.x0 == minx && c.y0 == miny && c.x1 == maxx && c.y1 == maxy);
  }
  return 1200;
}
static void partitions(void) {
  for (int t = 0; t < 80; t++) {
    int e = t % 36;
    load(e);
    fixture *q = &f[e];
    int length = (int)(rnd() % 12289);
    if (t & 1)
      length = -length;
    int pose = (int)(rnd() % 1025) - 512;
    settings_shutter_kernel k = make_kernel(length, pose);
    for (unsigned i = 0; i < sizeof A / sizeof *A; i++)
      A[i] = B[i] = 0x6bad;
    gfx_fb fb = {0};
    fb.px = A + 1;
    fb.stride = GFX_W;
    fb.h = GFX_H;
    fb.cx1 = GFX_W;
    fb.ox = (int)(rnd() % 31) - 15;
    fb.oy = (int)(rnd() % 31) - 15;
    fb.dim = rnd() % 33;
    int x = 62 + q->x, y = 170 + q->y, owner0 = 170, owner1 = 170 + q->owner;
    const uint16_t *pal = palettes[t & 1][t % 17];
    settings_shutter_coverage full =
        settings_shutter_blit(&fb, gray, pre, q->w, q->h, x, y, pal, &k, length,
                              pose, owner0, owner1, 12);
    unsigned total = 0;
    /* Independently paint a full scalar image BEFORE testing arbitrary damage.
     */
    double aa = (-pose + (length < 0 ? length : 0)) / 256.,
           bb = (-pose + (length > 0 ? length : 0)) / 256.;
    int lo = (int)floor(aa), hi = (int)ceil(bb);
    if (!length)
      hi = lo + 1;
    int top = y - hi, bottom = y + q->h - lo;
    if (top < owner0)
      top = owner0;
    if (bottom > owner1)
      bottom = owner1;
    for (int sy = 0; sy < GFX_H; sy++)
      for (int sx = 0; sx < GFX_W; sx++) {
        int lx = sx - fb.ox, ly = sy - fb.oy;
        if (lx >= x && lx < x + q->w && ly >= top && ly < bottom) {
          unsigned v =
              reference(gray, q->w, q->h, lx - x, ly - y, length, pose);
          B[1 + sy * GFX_W + sx] = gfx_ink(&fb, pal[v]);
          pixels++;
        }
      }
    assert(!memcmp(A, B, sizeof A));
    for (unsigned i = 0; i < sizeof B / sizeof *B; i++)
      B[i] = 0x6bad;
    for (int yy = 0; yy < GFX_H;) {
      int hh = 1 + rnd() % 16;
      if (yy + hh > GFX_H)
        hh = GFX_H - yy;
      for (int xx = 0; xx < GFX_W;) {
        int ww = 1 + rnd() % 89;
        if (xx + ww > GFX_W)
          ww = GFX_W - xx;
        for (unsigned j = 0; j < sizeof tile / sizeof *tile; j++)
          tile[j] = 0x6bad;
        gfx_fb part = fb;
        part.px = tile + 1;
        part.stride = ww;
        part.x_base = xx;
        part.y_base = yy;
        part.h = hh;
        part.cx0 = xx;
        part.cx1 = xx + ww;
        settings_shutter_coverage c =
            settings_shutter_blit(&part, gray, pre, q->w, q->h, x, y, pal, &k,
                                  length, pose, owner0, owner1, 12);
        total += c.pixels;
        assert(tile[0] == 0x6bad && tile[1 + ww * hh] == 0x6bad);
        for (int row = 0; row < hh; row++)
          memcpy(B + 1 + (yy + row) * GFX_W + xx, tile + 1 + row * ww, ww * 2);
        xx += ww;
      }
      yy += hh;
    }
    assert(full.pixels == total);
    assert(!memcmp(A, B, sizeof A));
  }
}
static void recording(void) {
  load(1);
  gfx_rec rec = {0};
  gfx_fb fb = {0};
  fb.rec = &rec;
  fb.h = GFX_H;
  fb.cx1 = GFX_W;
  settings_shutter_kernel k = make_kernel(64, -37);
  settings_shutter_coverage c =
      settings_shutter_blit(&fb, gray, pre, f[1].w, f[1].h, 63, 206,
                            palettes[0][16], &k, 64, -37, 170, 260, 11);
  assert(!c.pixels && rec.n == 1);
  unsigned hash = rec.c[0].hash;
  rec.n = 0;
  k = make_kernel(64, -38);
  settings_shutter_blit(&fb, gray, pre, f[1].w, f[1].h, 63, 206,
                        palettes[0][16], &k, 64, -38, 170, 260, 11);
  assert(hash != rec.c[0].hash);
  hash = rec.c[0].hash;
  rec.n = 0;
  settings_shutter_blit(&fb, gray, pre, f[1].w, f[1].h, 63, 206,
                        palettes[0][15], &k, 64, -38, 170, 260, 12);
  assert(hash != rec.c[0].hash);
}
static void lifecycle(void) {
  int e = 1;
  entry_cache *r = &RC->entry[e];
  uint8_t *old = r->image[0].s.px;
  uint16_t *prefix = r->prefix;
  row_start(e);
  while (RC->job.active) {
    assert(r->image[0].s.px == old && r->prefix == prefix);
    row_step_job();
  }
  assert(r->image[0].s.px != old && r->prefix != prefix);
  old = r->image[0].s.px;
  prefix = r->prefix;
  row_start(e);
  while (RC->job.phase != 7)
    row_step_job();
  unsigned failures = nos_mem_failures();
  nos_mem_fail_after(0);
  row_step_job();
  nos_mem_fail_after(-1);
  assert(!RC->job.active && r->image[0].s.px == old && r->prefix == prefix &&
         nos_mem_failures() == failures + 1);
  for (int phase = 0; phase <= 8; phase++) {
    if (phase == 5 || phase == 6)
      continue;
    row_start(e);
    for (int n = 0; RC->job.active && RC->job.phase != phase && n < 1000; n++)
      row_step_job();
    assert(RC->job.active && RC->job.phase == phase);
    set_settings_shutter_set(0);
    assert(!RC && !row_bytes && !set_settings_shutter_get());
    set_settings_shutter_set(1);
    assert(entry_bake(e));
  }
  setup();
  unsigned bytes = sizeof *RC;
  for (int j = 0; j < NE; j++) {
    entry_cache *q = &RC->entry[j];
    bytes += q->prefix_bytes;
    for (int k = 0; k < 7; k++)
      bytes += (unsigned)q->image[k].s.w * q->image[k].s.h;
  }
  assert(row_bytes == bytes);
  settings_shutter_kernel k;
  assert(!settings_shutter_kernel_make(&k, INT_MIN, 0));
  assert(!settings_shutter_kernel_make(&k, 0, INT_MIN));
  k = make_kernel(0, 0);
  gfx_fb dummy = {0};
  dummy.px = A + 1;
  dummy.h = 1;
  dummy.cx1 = GFX_W;
  A[1] = 0xabcd;
  settings_shutter_coverage c = settings_shutter_blit(
      &dummy, gray, pre, 1, 1, 0, 0, palettes[0][16], &k, 0, INT_MIN, 0, 1, 1);
  assert(!c.pixels && A[1] == 0xabcd);
}
static void exposure_case(int chrome) {
  S.L.sc.x = chrome ? 10.25f : 180.25f;
  shutter_previous = S.L.sc.x - 7.25f;
  S.L.bar_alpha = chrome;
  memset(A, 0, sizeof A);
  gfx_fb full = {0};
  full.px = A + 1;
  full.h = GFX_H;
  full.stride = GFX_W;
  full.cx1 = GFX_W;
  nos_history_reset();
  nos_frame++;
  nos_history_begin(nos_frame);
  assert(nos_history_apply(&full, 0, 0, GFX_W, GFX_H, 0));
  nos_frame++;
  nos_history_begin(nos_frame);
  /* The engine advances frame, prepares tick publications, then draws every
   * compositor stripe in that same generation. The atlas is frame-owned. */
  rows_tick(1.f / 40);
  unsigned protected_lit = 0, temporal_lit = 0;
  for (int yy = 0; yy < GFX_H; yy += 16) {
    int hh = GFX_H - yy;
    if (hh > 16)
      hh = 16;
    gfx_fb stripe = full;
    stripe.px = A + 1 + yy * GFX_W;
    stripe.y_base = yy;
    stripe.h = hh;
    settings_draw(&stripe);
    uint16_t before[GFX_W * 16];
    memcpy(before, stripe.px, (size_t)hh * GFX_W * 2);
    nos_exposure_job job;
    assert(nos_exposure_prepare(&job, &stripe, 0, yy, GFX_W, yy + hh, 1, 0));
    nos_exposure_apply(&job);
    for (int row = 0; row < hh; row++) {
      int spans[64][2];
      int n = ui_fblur_row_spans(yy + row, spans, 64);
      for (int x = 0; x < GFX_W; x++) {
        int covered = 0;
        for (int i = 0; i < n; i++)
          covered |= x >= spans[i][0] && x < spans[i][1];
        unsigned at = row * GFX_W + x;
        if (covered) {
          assert(stripe.px[at] == before[at]);
          protected_lit += before[at] != 0;
        } else
          temporal_lit += stripe.px[at] != before[at];
      }
    }
  }
  printf("COVER protected=%u temporal=%u exposure=%d mode=%d dragging=%d\\n",
         protected_lit, temporal_lit, shutter_exposure, nos_mblur,
         S.L.dragging);
  assert(protected_lit > 0);
  if (chrome)
    assert(temporal_lit > 0);
  else
    assert(temporal_lit == 0);
  puts("PASS actual exposurecompositor preservesdirectionalpixels, "
       "uncoveredforegroundtemporal, title/scrollbarexclusions");
}
static void whole_settings(void) {
  S.L.pressed = -1;
  S.arm_e = -1;
  S.flash_e = -1;
  S.L.sc.v = 0;
  S.L.bar_alpha = 0;
  S.build_t0 = nos_t - 100;
  float offset = 110;
  shutter_previous = offset;
  for (int frame = 0; frame < 24; frame++) {
    offset += (frame < 8 ? .25f : frame < 16 ? 7.25f : -22.5f);
    S.L.sc.x = offset;
    rows_tick(1.f / 40);
    int exposure = shutter_exposure, phase = shutter_pose;
    memset(A, 0, sizeof A);
    memset(B, 0, sizeof B);
    gfx_fb full = {0};
    full.px = A + 1;
    full.h = GFX_H;
    full.stride = GFX_W;
    full.cx1 = GFX_W;
    settings_draw(&full);
    for (int yy = 0; yy < GFX_H; yy += 7) {
      int hh = GFX_H - yy;
      if (hh > 7)
        hh = 7;
      for (int xx = 0; xx < GFX_W; xx += 61) {
        int ww = GFX_W - xx;
        if (ww > 61)
          ww = 61;
        memset(tile, 0, sizeof tile);
        gfx_fb part = full;
        part.px = tile + 1;
        part.stride = ww;
        part.x_base = part.cx0 = xx;
        part.cx1 = xx + ww;
        part.y_base = yy;
        part.h = hh;
        settings_draw(&part);
        for (int row = 0; row < hh; row++)
          memcpy(B + 1 + (yy + row) * GFX_W + xx, tile + 1 + row * ww, ww * 2);
      }
    }
    assert(!memcmp(A, B, sizeof A));
    assert(shutter_exposure == exposure && shutter_pose == phase);
  }
  exposure_case(0);
  exposure_case(1);
  settings_shutter_stats st = set_settings_shutter_stats();
  assert(st.moving_ticks >= 24 && st.filtered_pixels > 0);
  set_settings_shutter_set(0);
  assert(!RC && !row_bytes);
  puts("PASS productionprefix "
       "transactionalreplacement/faultrollback/everyphase-toggle/"
       "24wholeSettingspackedframes/frozenkernel");
}
int main(int argc, char **argv) {
  assert(!settings_cache_baseline_main(argc, argv));
  setup();
  unsigned n = randcases();
  partitions();
  recording();
  lifecycle();
  whole_settings();
  printf("PASS%u realfixture RGB565/dim/packed/translation/ownerclip "
         "cases,%llu scalarintegral pixels,80 arbitrarytilepartition "
         "frames,counters/canaries;36actualimages and34palettes\\n",
         n, pixels);
  return 0;
}
