/* pip_preview.c — renders the Pip creature face (face_pip.c) in staged moods
 * to pip_previewN.bmp for visual iteration without the sim or hardware.
 *
 *   gcc tools/pip_preview.c core/gfx.c core/font.c core/layer.c \
 *       core/fonts/*.c core/apps/face_pip.c -o build/pip_preview.exe -lm
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../core/gfx.h"
#include "../core/hal.h"
#include "../core/wos.h"

void face_pip_draw(gfx_fb *fb, const hal_datetime *t);
int  face_pip_event(const wos_event *ev);

/* ---- stub world ---- */
static uint32_t g_ms = 5000;
static int      g_mic = 10;
static int      g_pitch = 0;
static int      g_batt = 85;
static hal_datetime g_dt = { 2026, 8, 11, 2, 10, 9, 30, 0 };
uint32_t wos_frame_id = 1;

uint32_t hal_millis(void) { return g_ms; }
uint32_t hal_micros(void) { return g_ms * 1000; }
void hal_get_datetime(hal_datetime *o) { *o = g_dt; }
int  hal_mic_level(void) { return g_mic; }
int  hal_mic_pitch(void) { return g_pitch; }
int  hal_battery_percent(void) { return g_batt; }
void hal_play(int s) { (void)s; }
void hal_haptic(int ms) { (void)ms; }
void hal_log(const char *m) { (void)m; }
void wos_callout(const char *m, int ms) { (void)m; (void)ms; }
void *hal_big_alloc(uint32_t b) { return malloc(b); }

static void save_bmp(const char *path, const uint16_t *px)
{
    int stride = (GFX_W * 3 + 3) & ~3;
    static uint8_t row[GFX_W * 3 + 4];
    uint8_t fh[14] = { 0 }, ih[40] = { 0 };
    int fsz = 54 + stride * GFX_H;
    fh[0] = 'B'; fh[1] = 'M';
    fh[2] = fsz; fh[3] = fsz >> 8; fh[4] = fsz >> 16; fh[5] = fsz >> 24;
    fh[10] = 54;
    ih[0] = 40; ih[4] = GFX_W & 0xFF; ih[5] = GFX_W >> 8;
    int nh = -GFX_H; ih[8] = nh; ih[9] = nh >> 8; ih[10] = nh >> 16; ih[11] = nh >> 24;
    ih[12] = 1; ih[14] = 24;
    FILE *f = fopen(path, "wb");
    if (!f) return;
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

static uint16_t buf[GFX_W * GFX_H];

/* run n animation frames (16ms apart) so eased poses settle, then snapshot */
static void settle_and_shot(const char *name, int frames)
{
    gfx_fb fb = { .px = buf, .h = GFX_H };
    for (int i = 0; i < frames; i++) {
        g_ms += 16;
        wos_frame_id++;
        face_pip_draw(&fb, &g_dt);
    }
    /* don't snapshot mid-blink (eyes fully shuttered = misleading frame) */
    while ((g_ms % 3400u) < 700u) {
        g_ms += 16;
        wos_frame_id++;
        face_pip_draw(&fb, &g_dt);
    }
    float rad = GFX_W / 2.0f;
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) {
            float dx = x + 0.5f - rad, dy = y + 0.5f - rad;
            if (dx * dx + dy * dy > rad * rad) buf[y * GFX_W + x] = 0;
        }
    save_bmp(name, buf);
}

int main(void)
{
    /* 0: idle ping-pong, healthy battery, daytime (eyes mid-sweep) */
    g_batt = 85; g_mic = 10; g_dt.hour = 10;
    settle_and_shot("pip_preview0.bmp", 30);

    /* 1: dodging a tap on the left side (eyes flinch right) */
    wos_event tap = { WOS_EV_TAP, 120, 233 };
    face_pip_event(&tap);
    settle_and_shot("pip_preview1.bmp", 8);

    /* 2: startled by a clap */
    settle_and_shot("pip_scratch.bmp", 80);      /* let the dodge fade out */
    g_mic = 220;
    settle_and_shot("pip_scratch.bmp", 2);       /* spike registers */
    g_mic = 10;
    settle_and_shot("pip_preview2.bmp", 8);

    /* 3: vibing to high-pitched music (loud + high pitch: wide, lifted eyes) */
    settle_and_shot("pip_scratch.bmp", 60);      /* settle back to idle */
    g_mic = 90; g_pitch = 800;
    settle_and_shot("pip_preview3.bmp", 140);    /* > 1.5s sustained */

    /* 4: being yelled at -> sad (heavy lids, sunken dim eyes) */
    g_mic = 200; g_pitch = 300;
    settle_and_shot("pip_preview4.bmp", 60);     /* > 600ms sustained loud */

    /* 5: tired (12% battery) */
    g_mic = 10; g_pitch = 0;
    settle_and_shot("pip_scratch.bmp", 260);     /* let sadness pass */
    g_batt = 12;
    settle_and_shot("pip_preview5.bmp", 80);

    /* 6: asleep at night */
    g_batt = 70; g_dt.hour = 23;
    settle_and_shot("pip_preview6.bmp", 80);

    /* 7: mid ping-pong rally — ball in flight, striker mid-lunge */
    g_batt = 85; g_mic = 10; g_dt.hour = 10;
    settle_and_shot("pip_scratch.bmp", 40);              /* wake up + settle */
    g_ms = ((g_ms / 22000u) + 1) * 22000u;               /* jump to rally start */
    settle_and_shot("pip_preview7.bmp", 12);             /* ~190ms into pass 0 */

    /* 8: dancing AND rallying at once (noisy-environment idle) */
    g_mic = 90; g_pitch = 500;
    settle_and_shot("pip_scratch.bmp", 140);             /* get the groove going */
    g_ms = ((g_ms / 22000u) + 1) * 22000u;
    settle_and_shot("pip_preview8.bmp", 12);

    return 0;
}
