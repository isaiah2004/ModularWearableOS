/* Rendering regressions: exact live values, frame latching, cached hands,
 * translated/striped composition, and both static font instances. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/apps/apps.h"
#include "../core/layer.h"
#include "../core/pathtext.h"

uint32_t wos_frame_id = 1;
int g_brightness=180;
static int battery = 72, steps = 6234, battery_reads, step_reads;
static hal_weather weather;
static int external;
void hal_power_get(hal_power *out) {battery_reads++;*out=(hal_power){3900,battery,!external,external,1,0};}
void hal_activity_get(hal_activity *out) {step_reads++;*out=(hal_activity){(uint32_t)steps,0,0,1};}
void hal_weather_get(hal_weather *out) {*out=weather;}
int hal_location(char *city, int n, float *lat, float *lon)
{ (void)city; (void)n; (void)lat; (void)lon; return 0; }
int hal_battery_percent(void) { battery_reads++; return battery; }
int hal_step_count(void) { step_reads++; return steps; }
void *hal_big_alloc(uint32_t size) { return malloc(size); }
extern int layer_dbg;
static uint16_t pixels[GFX_W * GFX_H], previous[GFX_W * GFX_H];
static uint16_t stripe[GFX_W * 16];
static gfx_fb fb = { .px = pixels, .h = GFX_H };
static hal_datetime now = { .year=2026, .month=7, .day=12, .hour=10, .minute=9, .second=23 };

static void changed_value(void)
{
    int count = layer_dbg;
    memcpy(previous, pixels, sizeof pixels);
    wos_frame_id++;
    face_ultra_draw(&fb, &now);
    assert(layer_dbg == count + 1);
    assert(memcmp(previous, pixels, sizeof pixels) != 0);
}

static void transparent_hands(void)
{
    /* Look through both apertures at three distinct positions over the dial.
     * Compare against the cached artwork itself, including grid/text pixels:
     * a black or background-colored imitation of a hole cannot pass. The
     * chosen hands and second needle are well separated at these times. */
    const int times[][2] = {{10,9}, {2,42}, {7,18}};
    gfx_fb base = { .px = previous, .h = GFX_H };
    int detail_samples[2] = {0};
    for (int t = 0; t < 3; t++) {
        now.hour = times[t][0]; now.minute = times[t][1];
        now.second = 23; now.millis = 0;
        wos_frame_id++;
        face_ultra_draw(&fb, &now);
        layer_blit(&base, LAYER_TAG_ULTRA);
        float angles[2] = {
            (now.hour % 12 + now.minute / 60.0f) * (6.2831853f / 12),
            (now.minute + now.second / 60.0f) * (6.2831853f / 60)
        };
        for (int h = 0; h < 2; h++) {
            int distinct = 0;
            uint16_t first = 0;
            for (int r = h ? 70 : 35; r <= (h ? 87 : 47); r++) {
                int x = (int)(GFX_CX + sinf(angles[h]) * GFX_S(r));
                int y = (int)(GFX_CY - cosf(angles[h]) * GFX_S(r));
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int p = (y + dy) * GFX_W + x + dx;
                        assert(pixels[p] == previous[p]);
                        if (previous[p] != first) { distinct++; first = previous[p]; }
                    }
            }
            detail_samples[h] += distinct;
        }
    }
    /* Some positions cover plain charcoal; across the three positions each
     * hand must also have preserved the contrasting dial details. */
    assert(detail_samples[0] > 3 && detail_samples[1] > 3);
}

int main(void)
{
    /* Reproduce the gallery order that exposed the original tag collision:
     * another face has already populated tag 10 before Ultra is first shown. */
    face_nothing_draw(&fb, &now);
    battery_reads = step_reads = 0;
    int expected_renders = layer_dbg + 1;
    face_ultra_draw(&fb, &now);
    assert(layer_dbg == expected_renders && battery_reads == 1 && step_reads == 1);
    memcpy(previous, pixels, sizeof pixels);
    face_ultra_draw(&fb, &now);
    assert(layer_dbg == expected_renders && battery_reads == 1 && step_reads == 1);
    assert(memcmp(previous, pixels, sizeof pixels) == 0);
    /* A one-step change within the former 50-step bucket must appear. */
    steps++;
    changed_value();
    steps = 0;
    changed_value();
    steps = 100000;
    changed_value();
    battery = 100;
    changed_value();
    battery = 0;
    changed_value();
    now.day = 31;
    changed_value();
    external=1;changed_value();
    external=0;changed_value();
    weather.valid=1;weather.generation=1;weather.feels=26;
    changed_value();
    weather.stale=1;changed_value();
    g_brightness=5;changed_value();
    g_brightness=128;changed_value();
    g_brightness=255;changed_value();
    int count = layer_dbg;
    memcpy(previous, pixels, sizeof pixels);
    wos_frame_id++;
    now.millis = 500;
    face_ultra_draw(&fb, &now);
    assert(layer_dbg == count); /* moving hands never rerasterize dial text */
    assert(memcmp(previous, pixels, sizeof pixels) != 0);
    /* Device-sized stripes reproduce the exact full frame. */
    for (int y = 0; y < GFX_H; y += 16) {
        int rows = GFX_H - y < 16 ? GFX_H - y : 16;
        gfx_fb part = { .px = stripe, .h = rows, .y_base = y };
        face_ultra_draw(&part, &now);
        assert(memcmp(stripe, pixels + y * GFX_W, rows * GFX_W * 2) == 0);
    }
    /* A slide shifts the cached dial and live hands together. */
    memcpy(previous, pixels, sizeof pixels);
    gfx_fb shifted = fb;
    shifted.ox = 37;
    face_ultra_draw(&shifted, &now);
    for (int y = 0; y < GFX_H; y++)
        assert(memcmp(pixels + y * GFX_W + 37, previous + y * GFX_W,
                      (GFX_W - 37) * 2) == 0);
    layer_invalidate(-1);
    face_ultra_draw(&fb, &now);
    assert(layer_dbg == count + 1);
    assert(memcmp(pixels, previous, sizeof pixels) == 0);

    /* A face tour can recycle every cache slot. Ultra must then rebuild its
     * own complete dial even if the former conflicting tag is populated. */
    count = layer_dbg;
    for (int tag = 20; tag < 24; tag++) {
        gfx_fb *tour = layer_need_render(tag);
        assert(tour);
        gfx_clear(tour, (uint16_t)tag);
    }
    face_nothing_draw(&fb, &now);
    battery_reads = step_reads = 0;
    wos_frame_id++;
    face_ultra_draw(&fb, &now);
    assert(layer_dbg == count + 6);
    assert(memcmp(pixels, previous, sizeof pixels) == 0);
    count = layer_dbg;

    /* Framed hands must stay within the damage command budget used on device. */
    gfx_rec face_rec = {0};
    gfx_fb face_recorded = { .rec = &face_rec, .h = GFX_H };
    face_ultra_draw(&face_recorded, &now);
    assert(face_rec.n > 0 && face_rec.n < 64 && !face_rec.overflow);
    assert(layer_dbg == count);

    transparent_hands();

    for (int font = 0; font < PATHTEXT_FONT_COUNT; font++) {
        gfx_clear(&fb, GFX_BLACK);
        pathtext_draw(&fb, font, "H", 50, 100, 28, GFX_WHITE, 0);
        int top = GFX_H, bottom = -1;
        for (int y = 0; y < GFX_H; y++)
            for (int x = 0; x < GFX_W; x++) if (pixels[y * GFX_W + x]) {
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        assert(top >= 71 && top <= 73 && bottom >= 98 && bottom <= 100);
    }
    assert(pathtext_width(PATHTEXT_CONDENSED, "0123456789", 20) <
           pathtext_width(PATHTEXT_BOLD, "0123456789", 20));
    /* Lower-arc text puts the cap inward, rather than below its baseline.
     * Compare radial bounds, then check stripe composition of a longer arc. */
    gfx_clear(&fb, GFX_BLACK);
    pathtext_arc_inward(&fb, PATHTEXT_BOLD, "H", 100, 3.14159265f, 20, GFX_WHITE);
    int top = GFX_H, bottom = -1;
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) if (pixels[y * GFX_W + x]) {
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    assert(top >= 312 && top <= 314 && bottom >= 331 && bottom <= 333);
    gfx_clear(&fb, GFX_BLACK);
    pathtext_arc_inward(&fb, PATHTEXT_CONDENSED, "Feels like temp. 23", 124, 2.34f, 14, GFX_WHITE);
    for (int y = 0; y < GFX_H; y += 16) {
        int rows = GFX_H - y < 16 ? GFX_H - y : 16;
        gfx_fb part = { .px = stripe, .h = rows, .y_base = y };
        gfx_clear(&part, GFX_BLACK);
        pathtext_arc_inward(&part, PATHTEXT_CONDENSED, "Feels like temp. 23", 124, 2.34f, 14, GFX_WHITE);
        assert(memcmp(stripe, pixels + y * GFX_W, rows * GFX_W * 2) == 0);
    }
    gfx_rec rec = {0};
    gfx_fb recorded = { .rec = &rec, .h = GFX_H };
    pathtext_draw(&recorded, PATHTEXT_BOLD, "12", 233, 100, 54, GFX_WHITE, 1);
    pathtext_arc(&recorded, PATHTEXT_CONDENSED, "02", 190, 1.0f, 17, GFX_WHITE);
    pathtext_arc_inward(&recorded, PATHTEXT_CONDENSED, "89 bpm", 84, 2.22f, 15, GFX_WHITE);
    assert(rec.n == 0); /* expensive pathtext remains raster-only */
    puts("PASS Ultra: live values, cached/transparent hands, stripe/slide equivalence, eviction, font metrics, inward arc orientation and record mode");
    return 0;
}
