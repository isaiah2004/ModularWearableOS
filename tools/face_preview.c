/* face_preview.c — standalone previewer for a single watch face, so a face
 * can be rendered to a PNG-able BMP without the whole sim. Compile with the
 * face's draw function name in FACEFN:
 *
 *   gcc tools/face_preview.c core/gfx.c core/font.c core/icons.c core/layer.c \
 *       core/pathtext.c core/fonts/*.c core/apps/face_myface.c \
 *       -DFACEFN=face_myface_draw -o preview.exe -lm
 *   ./preview.exe            # writes preview0.bmp .. preview3.bmp (animation)
 *
 * All HAL functions are stubbed here with pleasant showcase values. The output
 * is circular-masked to match the round 466x466 panel. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../core/gfx.h"
#include "../core/hal.h"
#include "weather_preview.h"

void FACEFN(gfx_fb *fb, const hal_datetime *t);

static uint32_t g_ms = 0;
static int g_battery = 72, g_steps = 6234, g_hour = 10, g_minute = 9, g_day = 12;
uint32_t wos_frame_id = 1;
int g_brightness=180;

uint32_t hal_millis(void) { return g_ms; }
uint32_t hal_micros(void) { return g_ms * 1000; }
void hal_frame_begin(void) {}
void hal_frame_end(void) {}
void hal_get_datetime(hal_datetime *o) {
    o->year = 2026; o->month = 7; o->day = g_day; o->weekday = 0;
    o->hour = g_hour; o->minute = g_minute; o->second = (g_ms / 1000) % 60;
    o->millis = g_ms % 1000;
}
int  hal_battery_percent(void) { return g_battery; }
void hal_power_get(hal_power *out) {*out=(hal_power){3900,g_battery,1,0,1,0};}
void hal_activity_get(hal_activity *out) {*out=(hal_activity){(uint32_t)g_steps,2520,0,1};}
void hal_set_backlight(uint8_t l) { (void)l; }
void hal_log(const char *m) { (void)m; }
int  hal_wifi_scan(hal_wifi_ap *o, int m) { (void)o; (void)m; return 0; }
int  hal_imu_read(hal_imu *o) {
    float s = g_ms / 1000.0f;
    o->ax = 0.2f * sinf(s); o->ay = 0.2f * cosf(s * 0.8f); o->az = 1.0f;
    o->gx = o->gy = o->gz = 0; return 1;
}
int  hal_audio_bars(uint8_t *l, int n) {
    float s = g_ms / 1000.0f;
    for (int i = 0; i < n; i++) { float v = 0.5f + 0.5f * sinf(s * 5 + i); l[i] = (uint8_t)(v * 255); }
    return n;
}
int  hal_ble_scan(hal_ble_dev *o, int m) { (void)o; (void)m; return 0; }
int  hal_step_count(void) { return g_steps; }
void hal_haptic(int ms) { (void)ms; }
void *hal_big_alloc(uint32_t b) { return malloc(b); }

static void save_bmp(const char *path, const uint16_t *px) {
    int stride = (GFX_W * 3 + 3) & ~3;
    static uint8_t row[GFX_W * 3 + 4];
    uint8_t fh[14] = { 0 }, ih[40] = { 0 };
    int fsz = 54 + stride * GFX_H;
    fh[0] = 'B'; fh[1] = 'M';
    fh[2] = fsz; fh[3] = fsz >> 8; fh[4] = fsz >> 16; fh[5] = fsz >> 24;
    fh[10] = 54;
    ih[0] = 40; ih[4] = (uint8_t)GFX_W; ih[5] = GFX_W >> 8;
    int nh = -GFX_H; ih[8] = nh; ih[9] = nh >> 8; ih[10] = nh >> 16; ih[11] = nh >> 24;
    ih[12] = 1; ih[14] = 24;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(fh, 14, 1, f); fwrite(ih, 40, 1, f);
    for (int y = 0; y < GFX_H; y++) {
        for (int x = 0; x < GFX_W; x++) {
            uint16_t p = px[y * GFX_W + x];
            row[x*3+2] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            row[x*3+1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
            row[x*3+0] = (uint8_t)((p & 0x1F) * 255 / 31);
        }
        fwrite(row, stride, 1, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

int main(int argc, char **argv) {
    /* Optional positional showcase values: hour minute battery steps day. */
    if (argc != 1 && argc != 6) {
        fprintf(stderr, "Usage: %s [hour minute battery steps day]\n", argv[0]);
        return 1;
    }
    if (argc == 6) {
        g_hour = atoi(argv[1]); g_minute = atoi(argv[2]);
        g_battery = atoi(argv[3]); g_steps = atoi(argv[4]); g_day = atoi(argv[5]);
        if (g_hour < 0 || g_hour > 23 || g_minute < 0 || g_minute > 59 ||
            g_battery < 0 || g_battery > 100 || g_steps < 0 || g_day < 1 || g_day > 31)
            return 1;
    }
    static uint16_t buf[GFX_W * GFX_H];
    gfx_fb fb = { .px = buf, .h = GFX_H };
    hal_datetime t;
    float rad = GFX_W / 2.0f;
    /* four frames spread across a minute so animation/seconds are visible */
    uint32_t times[4] = { 0, 8000, 23000, 47000 };
    for (int k = 0; k < 4; k++) {
        g_ms = times[k];
        wos_frame_id++;
        hal_get_datetime(&t);
        FACEFN(&fb, &t);
        for (int y = 0; y < GFX_H; y++)          /* round mask */
            for (int x = 0; x < GFX_W; x++) {
                float dx = x + 0.5f - rad, dy = y + 0.5f - rad;
                if (dx*dx + dy*dy > rad*rad) buf[y*GFX_W+x] = 0;
            }
        char nm[24]; snprintf(nm, sizeof nm, "preview%d.bmp", k);
        save_bmp(nm, buf);
    }
    return 0;
}
