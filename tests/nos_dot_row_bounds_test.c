/* Original per-glyph row calculation versus the shared per-call limits. */
#include "../nos/dot.c"
#include "nos_dot_row_reference.inc"
#include <assert.h>
#include <stdio.h>
static int allocations;
void *(hal_big_alloc)(uint32_t n) { allocations++; return malloc(n); }
static unsigned rng = 73117;
static unsigned random_word(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint16_t actual[GFX_W * 32 + 8], expected[GFX_W * 32 + 8];
static void fill(void) {
    for (unsigned i = 0; i < sizeof actual / sizeof *actual; i++) actual[i] = expected[i] = (uint16_t)random_word();
}
static gfx_fb buffer(int i) {
    gfx_fb f = {0};
    f.h = i % 3 == 0 ? 1 : i % 3 == 1 ? 16 : 31;
    f.y_base = (int)(random_word() % 600) - 67;
    f.ox = (int)(random_word() % 65) - 32;
    f.oy = (int)(random_word() % 65) - 32;
    f.dim = i % 3 == 0 ? 13 : 0;
    f.cx0 = (int)(random_word() % 300);
    f.cx1 = f.cx0 + 1 + (int)(random_word() % 166);
    if (i % 2) { f.x_base = f.cx0; f.stride = f.cx1 - f.cx0; }
    return f;
}
static void direct_cells(void)
{
    for (int i = 0; i < 20000; i++) {
        gfx_fb f = buffer(i); fill();
        dclip k; clip_init(&k, &f);
        float pitch = 0.75f + (random_word() % 61) * 0.25f;
        float oy = k.ylo - (random_word() % 181) * 0.25f;
        float ox = k.c0 + (random_word() % 81) * 0.25f;
        dglyph g = {0}; g.w = 5; g.rows = i % 12;
        for (int r = 0; r < 11; r++) g.bits[r] = (uint16_t)random_word();
        drawctx c = { &k, pitch, pitch * 0.8f, 0xffff, i % 2 ? 0x2104 : 0,
                      (i % 7) / 6.0f, i % 5, (unsigned)i, 7, 11, pitch * 0.4f + 1, 0, 0 };
        draw_row_bounds(&c, oy);
        int lo = ifloor(((float)k.ylo - c.ext - oy) / pitch - 0.5f);
        int hi = iceil(((float)k.yhi + c.ext - oy) / pitch + 0.5f);
        assert(c.r0 == (lo < 0 ? 0 : lo) && c.r1 == hi);
        f.px = expected;
        row_reference_draw_cells(&c, i % 9 ? &g : NULL, ox, oy, 1, 0, 7, 9, (unsigned)i);
        f.px = actual;
        draw_cells(&c, i % 9 ? &g : NULL, ox, oy, 1, 0, 7, 9, (unsigned)i);
        assert(!memcmp(actual, expected, sizeof actual));
    }
}
static void entry_points(void)
{
    static const char *texts[] = { "1234", "gypjq", "09:58", "--", "iIl.,", "steps", "\xC2\xB0", "", "A?[]" };
    for (int i = 0; i < 12000; i++) {
        gfx_fb f = buffer(i); fill();
        float pitch = 0.75f + (random_word() % 49) * 0.25f;
        float d = pitch * (i % 4 ? 0.8f : 1.3f);
        float x = f.cx0 - f.ox + (f.cx1 - f.cx0) * 0.5f;
        float y = f.y_base - f.oy - (random_word() % 81) * 0.25f;
        float p = (i % 11 - 1) * 0.125f;
        int mode = i % 5, align = i % 3;
        unsigned seed = random_word();
        dot_style st = { i % DOT_FONT_COUNT, pitch, d, (uint16_t)random_word(),
                         i % 3 ? 0 : 0x2104, i % 2 ? 0 : DOT_GAP_NONE, i % 2 };
        const char *text = texts[i % (sizeof texts / sizeof *texts)];
        f.px = expected;
        int advance = row_reference_text_impl(&f, &st, x, y, text, align, p, mode, seed);
        f.px = actual;
        assert(dot_text_anim(&f, &st, x, y, text, align, p, mode, seed) == advance);
        assert(!memcmp(actual, expected, sizeof actual));
        gfx_rec ref = {0}, got = {0};
        f.rec = &ref; row_reference_text_impl(&f, &st, x, y, text, align, p, mode, seed);
        f.rec = &got; dot_text_anim(&f, &st, x, y, text, align, p, mode, seed);
        assert(!memcmp(&ref, &got, sizeof ref));
        f.rec = NULL; fill();
        int icon = 1 + i % (DOT_ICON_COUNT - 1);
        f.px = expected;
        row_reference_dot_icon_anim(&f, icon, x, y, pitch, d, st.color, p, mode);
        f.px = actual;
        dot_icon_anim(&f, icon, x, y, pitch, d, st.color, p, mode);
        assert(!memcmp(actual, expected, sizeof actual));
        memset(&ref, 0, sizeof ref); memset(&got, 0, sizeof got);
        f.rec = &ref; row_reference_dot_icon_anim(&f, icon, x, y, pitch, d, st.color, p, mode);
        f.rec = &got; dot_icon_anim(&f, icon, x, y, pitch, d, st.color, p, mode);
        assert(!memcmp(&ref, &got, sizeof ref));
    }
}
int main(void)
{
    assert(tables()); dot_prebake_all();
    int before = allocations;
    direct_cells(); entry_points();
    assert(allocations == before);
    puts("PASS dot rows: 20000 direct clamps +12000 text/icon pixel/record cases, ghosts/descenders/AA/transforms/packed/dim, no added allocation");
    return 0;
}
