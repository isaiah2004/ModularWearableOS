/* gfx_bench.c — cost of each drawing primitive per covered pixel, measured the
 * way the UI really renders: a full frame as 16-row stripes, each primitive
 * clipped per stripe. These numbers are the CPU side of the 60 fps budget (the
 * bus side is ~18.6 MB/s on the 40 MHz QSPI panel).
 *
 * Coverage is counted, not estimated: each primitive is first drawn once into a
 * full-frame scratch buffer over a sentinel colour and the changed pixels counted. */
#include "gfx_bench.h"
#include "font.h"
#include "icons.h"
#include "hal.h"
#include "sprite.h"
#include <stdio.h>
#include <string.h>

#define SPR 200                          /* sprite / mask edge (px) */
#define SPR_X ((GFX_W - SPR) / 2)
static uint16_t *s_spr;                  /* RGB565 sprite in PSRAM (like a cached layer) */
static uint8_t *s_mask;                  /* 8-bit coverage mask in PSRAM (like an icon mask) */
static uint8_t *s_spr8;                  /* RGB332-index sprite in PSRAM (sprite8) */
static uint16_t s_lut[256];              /* its colour table */
static const uint16_t C = GFX_RGB(255, 140, 40);

/* Rows [y0, y0+n) of the stripe that overlap logical rows [a, b). */
static int span(const gfx_fb *fb, int a, int b, int *r0, int *r1)
{
    int lo = fb->y_base > a ? fb->y_base : a, hi = fb->y_base + fb->h < b ? fb->y_base + fb->h : b;
    *r0 = lo; *r1 = hi;
    return hi > lo;
}

static void p_clear(gfx_fb *fb)  { gfx_clear(fb, C); }
static void p_rect(gfx_fb *fb)   { gfx_fill_rect(fb, 83, 83, 300, 300, C); }
static void p_circle(gfx_fb *fb) { gfx_fill_circle(fb, GFX_CX, GFX_CY, 120, C); }
static void p_ring(gfx_fb *fb)   { gfx_ring(fb, GFX_CX, GFX_CY, 180, 14, C); }
static void p_arc(gfx_fb *fb)    { gfx_arc(fb, GFX_CX, GFX_CY, 180, 14, 0.3f, 2.8f, C); }
static void p_pill(gfx_fb *fb)   { gfx_fill_pill(fb, 120, GFX_CY, 346, 34, C); }
static void p_line(gfx_fb *fb)   { gfx_line(fb, 110, 110, 356, 356, 8, C); }
static void p_text(gfx_fb *fb)   { font_draw(fb, &font_medium, 70, 245, "Quick brown fox", C); }
static void p_digits(gfx_fb *fb) { font_draw_centered(fb, &font_big, (int)GFX_CX, 270, "12:34", C); }
static void p_icon(gfx_fb *fb)   { icon_glyph(fb, ICON_WEATHER, GFX_CX, GFX_CY, 70, C); }
static void p_blit(gfx_fb *fb)
{
    int r0, r1;
    if (!span(fb, SPR_X, SPR_X + SPR, &r0, &r1)) return;
    for (int y = r0; y < r1; y++)
        memcpy(gfx_row(fb, y - fb->y_base) + SPR_X, &s_spr[(y - SPR_X) * SPR], SPR * 2);
}
static void p_blit8(gfx_fb *fb)          /* RGB332 indices through a colour table (sprite8_blit) */
{
    sprite8 s = { s_spr8, SPR, SPR };
    sprite8_blit(fb, &s, SPR_X, SPR_X, s_lut, 0);
}
static void p_blend(gfx_fb *fb)          /* 50% alpha sprite over the destination */
{
    int r0, r1;
    if (!span(fb, SPR_X, SPR_X + SPR, &r0, &r1)) return;
    for (int y = r0; y < r1; y++) {
        uint16_t *d = gfx_row(fb, y - fb->y_base) + SPR_X;
        const uint16_t *s = &s_spr[(y - SPR_X) * SPR];
        for (int x = 0; x < SPR; x++) d[x] = gfx_blend565(d[x], s[x], 128);
    }
}
static void p_mask(gfx_fb *fb)           /* coloured 8-bit coverage mask (icon cache) */
{
    int r0, r1;
    if (!span(fb, SPR_X, SPR_X + SPR, &r0, &r1)) return;
    for (int y = r0; y < r1; y++) {
        uint16_t *d = gfx_row(fb, y - fb->y_base) + SPR_X;
        const uint8_t *a = &s_mask[(y - SPR_X) * SPR];
        for (int x = 0; x < SPR; x++) {
            if (a[x] == 255) d[x] = C;
            else if (a[x]) d[x] = gfx_blend565(d[x], C, a[x]);
        }
    }
}
static void p_none(gfx_fb *fb) { (void)fb; }   /* per-stripe loop overhead only */

typedef struct { const char *name; void (*fn)(gfx_fb *); } prim;
static const prim PRIMS[] = {
    { "clear", p_clear },   { "fill_rect", p_rect }, { "blit_psram", p_blit }, { "blit8_lut", p_blit8 },
    { "blend50", p_blend }, { "mask_blit", p_mask }, { "circle_aa", p_circle },
    { "ring_aa", p_ring },  { "arc_aa", p_arc },     { "pill_aa", p_pill },
    { "line_aa", p_line },  { "text_med", p_text },  { "digits_big", p_digits },
    { "icon_vec", p_icon }, { "none", p_none },
};

static uint32_t coverage(uint16_t *full, void (*fn)(gfx_fb *))
{
    const uint16_t sentinel = 0x0841;
    for (int i = 0; i < GFX_W * GFX_H; i++) full[i] = sentinel;
    gfx_fb fb = { full, 0, 0, 0, GFX_H, 0, GFX_W, 0, 0, 0 };
    fn(&fb);
    uint32_t n = 0;
    for (int i = 0; i < GFX_W * GFX_H; i++) n += full[i] != sentinel;
    return n;
}

void gfx_bench_run(uint16_t *stripe, int stripe_h, void (*yield)(void))
{
    if (!s_spr) s_spr = hal_big_alloc(SPR * SPR * 2);
    if (!s_mask) s_mask = hal_big_alloc(SPR * SPR);
    if (!s_spr8) s_spr8 = hal_big_alloc(SPR * SPR);
    uint16_t *full = hal_big_alloc(GFX_W * GFX_H * 2);
    if (!s_spr || !s_mask || !s_spr8 || !full) { printf("GFXB ERR no memory\n"); return; }
    for (int i = 0; i < SPR * SPR; i++) {
        s_spr[i] = (uint16_t)(i * 2654435761u >> 16);
        s_spr8[i] = (uint8_t)(s_spr[i] | 1);         /* never black: coverage counts every pixel */
        int x = i % SPR - SPR / 2, y = i / SPR - SPR / 2, d = x * x + y * y;   /* soft disc */
        s_mask[i] = d < 80 * 80 ? 255 : d < 100 * 100 ? (uint8_t)((100 * 100 - d) * 255 / (100 * 100 - 80 * 80)) : 0;
    }
    sprite8_lut_identity(s_lut);
    const int reps = 8;
    for (unsigned k = 0; k < sizeof PRIMS / sizeof PRIMS[0]; k++) {
        uint32_t px = coverage(full, PRIMS[k].fn);
        uint32_t t0 = hal_micros();
        for (int r = 0; r < reps; r++)
            for (int y = 0; y < GFX_H; y += stripe_h) {
                gfx_fb fb = { stripe, 0, 0, y, GFX_H - y < stripe_h ? GFX_H - y : stripe_h, 0, GFX_W, 0, 0, 0 };
                PRIMS[k].fn(&fb);
            }
        uint32_t us = (hal_micros() - t0) / reps;
        printf("GFXB %-11s px=%6u frame_us=%6u ns_per_px=%u\n", PRIMS[k].name, (unsigned)px, (unsigned)us,
               px ? (unsigned)((uint64_t)us * 1000u / px) : 0u);
        if (yield) yield();
    }
    printf("GFXB done\n");
}
