#include "gfx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t direct[GFX_W * GFX_H];
static uint16_t split[GFX_W * GFX_H];
static uint16_t expected[GFX_W * GFX_H];

static void fill(uint16_t *pixels, uint16_t color)
{
    for (int i = 0; i < GFX_W * GFX_H; i++) pixels[i] = color;
}

static void check(float end, int y_base, int rows, int x0, int x1)
{
    const uint16_t background = GFX_RGB(7, 11, 19);
    fill(direct, background);
    fill(split, background);
    gfx_fb a = { direct + y_base * GFX_W, 0, 0, y_base, rows, x0, x1, 0, 0, 0 };
    gfx_fb b = { split + y_base * GFX_W, 0, 0, y_base, rows, x0, x1, 0, 0, 0 };
    gfx_arc(&a, GFX_CX, GFX_CY, GFX_S(100), GFX_S(4), 0.0f, end, GFX_ACCENT);
    gfx_sweep(&b, GFX_CX, GFX_CY, GFX_S(100), GFX_S(4), end,
              6.28318531f / 60.0f, GFX_ACCENT);
    assert(memcmp(direct, split, sizeof direct) == 0);
}

static void record(float end, gfx_rec *rec)
{
    gfx_fb fb = { 0 };
    fb.h = GFX_H;
    fb.cx1 = GFX_W;
    fb.rec = rec;
    gfx_sweep(&fb, GFX_CX, GFX_CY, GFX_S(100), GFX_S(4), end,
              6.28318531f / 60.0f, GFX_ACCENT);
}

static void render_full(uint16_t *pixels, float end, uint16_t background)
{
    fill(pixels, background);
    gfx_fb fb = { pixels, 0, 0, 0, GFX_H, 0, GFX_W, 0, 0, 0 };
    gfx_sweep(&fb, GFX_CX, GFX_CY, GFX_S(100), GFX_S(4), end,
              6.28318531f / 60.0f, GFX_ACCENT);
}

/* Repaint the old and new boxes of every changed recorder command, as the
 * retained renderer does. Drawing the whole current scene in each clipped box
 * includes its background clear, which erases the previous moving tip. */
static void repaint_changed(float end, const gfx_rec *old_rec,
                            const gfx_rec *new_rec, uint16_t background)
{
    /* A tip appears/disappears exactly on a snap boundary. The production
     * damage engine treats a command-count change as a full redraw. */
    if (old_rec->n != new_rec->n) {
        render_full(split, end, background);
        return;
    }
    for (int i = 0; i < new_rec->n; i++) {
        const gfx_cmd *old = &old_rec->c[i], *now = &new_rec->c[i];
        if (old->hash == now->hash && old->x0 == now->x0 && old->y0 == now->y0 &&
            old->x1 == now->x1 && old->y1 == now->y1)
            continue;
        const gfx_cmd *boxes[2] = { old, now };
        for (int k = 0; k < 2; k++) {
            int x0 = boxes[k]->x0, y0 = boxes[k]->y0;
            int x1 = boxes[k]->x1, y1 = boxes[k]->y1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > GFX_W) x1 = GFX_W;
            if (y1 > GFX_H) y1 = GFX_H;
            /* Match compute_damage(): panel sub-rects are expanded to even
             * bounds for the 32-bit QSPI transfer. */
            x0 &= ~1;
            y0 &= ~1;
            x1 = (x1 + 1) & ~1;
            y1 = (y1 + 1) & ~1;
            if (x1 > GFX_W) x1 = GFX_W;
            if (y1 > GFX_H) y1 = GFX_H;
            if (x1 <= x0 || y1 <= y0) continue;
            gfx_fb fb = { split + y0 * GFX_W, 0, 0, y0, y1 - y0, x0, x1, 0, 0, 0 };
            gfx_clear(&fb, background);
            gfx_sweep(&fb, GFX_CX, GFX_CY, GFX_S(100), GFX_S(4), end,
                      6.28318531f / 60.0f, GFX_ACCENT);
        }
    }
}

static void check_damage_across_snap(void)
{
    const uint16_t background = GFX_RGB(7, 11, 19);
    const float snap = 6.28318531f / 60.0f;
    const float before = 17.0f * snap - 0.002f;
    const float after  = 17.0f * snap + 0.002f;
    gfx_rec old_rec = {0}, new_rec = {0};

    record(before, &old_rec);
    record(after, &new_rec);
    assert(old_rec.n == 2 && new_rec.n == 2);
    /* The settled command advances at the boundary; the moving tip also moves
     * from the end of the old bucket to the start of the new one. */
    assert(old_rec.c[0].hash != new_rec.c[0].hash);
    assert(old_rec.c[1].hash != new_rec.c[1].hash);

    render_full(split, before, background);
    render_full(expected, after, background);
    repaint_changed(after, &old_rec, &new_rec, background);
    assert(memcmp(expected, split, sizeof expected) == 0);
}

static void check_damage_sequence(void)
{
    const uint16_t background = GFX_RGB(7, 11, 19);
    const float turn = 6.28318531f;
    const int frames = 600;
    int same_bucket = 0, snap_crossings = 0, full_fallbacks = 0;

    /* Half a step avoids making every tenth sample land exactly on a snap.
     * The endpoints still cover the start and finish closely, while the
     * 600-frame walk crosses every one of the 60 settled-arc boundaries. */
    float previous = 0.5f * turn / (float)frames;
    gfx_rec previous_rec = {0};
    record(previous, &previous_rec);
    render_full(split, previous, background);

    for (int direction = 0; direction < 2; direction++) {
        for (int step = 1; step < frames; step++) {
            int index = direction == 0 ? step : frames - 1 - step;
            float current = ((float)index + 0.5f) * turn / (float)frames;
            gfx_rec current_rec = {0};
            record(current, &current_rec);
            render_full(expected, current, background);

            if (previous_rec.n != current_rec.n) {
                full_fallbacks++;
            } else if (previous_rec.n == 2 &&
                       previous_rec.c[0].hash == current_rec.c[0].hash &&
                       previous_rec.c[0].x0 == current_rec.c[0].x0 &&
                       previous_rec.c[0].y0 == current_rec.c[0].y0 &&
                       previous_rec.c[0].x1 == current_rec.c[0].x1 &&
                       previous_rec.c[0].y1 == current_rec.c[0].y1) {
                same_bucket++;
            } else {
                snap_crossings++;
            }

            repaint_changed(current, &previous_rec, &current_rec, background);
            if (memcmp(expected, split, sizeof expected) != 0) {
                int bad = 0;
                while (bad < GFX_W * GFX_H && expected[bad] == split[bad]) bad++;
                fprintf(stderr, "damage mismatch direction=%d step=%d angle=%f old_n=%d new_n=%d\n",
                        direction, step, current, previous_rec.n, current_rec.n);
                fprintf(stderr, "first pixel x=%d y=%d expected=%04x actual=%04x\n",
                        bad % GFX_W, bad / GFX_W, expected[bad], split[bad]);
                for (int q = 0; q < current_rec.n; q++)
                    fprintf(stderr, "old%d=%d,%d,%d,%d new%d=%d,%d,%d,%d\n", q,
                            previous_rec.c[q].x0, previous_rec.c[q].y0,
                            previous_rec.c[q].x1, previous_rec.c[q].y1, q,
                            current_rec.c[q].x0, current_rec.c[q].y0,
                            current_rec.c[q].x1, current_rec.c[q].y1);
                assert(0);
            }
            previous = current;
            previous_rec = current_rec;
        }
    }
    (void)previous;
    assert(same_bucket > 900);
    assert(snap_crossings >= 100);
    /* Command count changes at the first settled segment and where the final
     * near-full sector switches to its conservative unsplit recording. */
    assert(full_fallbacks <= 4);
}

int main(void)
{
    const float ends[] = { 0.01f, 0.10471976f, 0.19f, 1.2f, 3.14f, 4.9f, 6.27f };
    for (unsigned i = 0; i < sizeof ends / sizeof ends[0]; i++) {
        check(ends[i], 0, GFX_H, 0, GFX_W);
        check(ends[i], 81, 37, 29, 401);
        check(ends[i], 214, 21, 170, 310);
    }
    check_damage_across_snap();
    check_damage_sequence();
    puts("arc sweep: exact raster and 1198 incremental damage frames verified");
    return 0;
}
