/* nos_prog_lab.c — sim only: every prog.h style filling, to compare them on the glass.
 * Page 0: the six bar styles stacked. Pages 1-6: one ring style each. Tap: next page. */
#include "nos.h"
#include "ui.h"
#include "prog.h"
#include <math.h>
#include <stdio.h>

static struct { int page; float t0; } L;

#define LAB_FILL_S 1.6f        /* empty to full */
#define LAB_HOLD_S 0.8f        /* then full, then empty again */
static float lab_frac(void)
{
    float t = fmodf(nos_t - L.t0, LAB_FILL_S + LAB_HOLD_S);
    return t < LAB_FILL_S ? floorf(t / LAB_FILL_S * 96.0f) / 96.0f : 1.0f;
}

static void lab_enter(void) { L.page = 0; L.t0 = nos_t; }
static void lab_tick(float dt) { (void)dt; nos_keep_awake(); }
static void lab_draw(gfx_fb *fb)
{
    float f = lab_frac();
    if (fb->rec) gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_f(0x1AB0u + (unsigned)L.page, f));
    if (L.page == 0) {
        ui_title(fb, "PROGRESS", 0);
        static const int n[PROG_NSTYLES] = { 12, 10, 14, 14, 11, 14 };
        for (int s = 0; s < PROG_NSTYLES; s++) {
            float y = 112.0f + 44.0f * (float)s;
            ui_label(fb, 100, (int)y - 6, prog_name(s), C_DIS, UI_LEFT);
            prog_bar(fb, s, 100, y, 266, 18, n[s], f, C_WHITE, C_LINE);
        }
        ui_label(fb, UI_CX, 392, "TAP: RINGS", C_SEC, UI_CENTER);
        return;
    }
    int s = L.page - 1;
    prog_ring(fb, s, UI_CX, UI_CY, 193, 26, 48, f, C_WHITE, C_LINE);
    ui_label(fb, UI_CX, UI_CY + 6, prog_name(s), C_PRI, UI_CENTER);
    ui_label(fb, UI_CX, UI_CY + 40, "TAP: NEXT", C_DIS, UI_CENTER);
}
static int lab_input(const nos_input *in)
{
    if (in->type != IN_TAP) return 0;
    L.page = (L.page + 1) % (PROG_NSTYLES + 1);
    L.t0 = nos_t;
    return 1;
}

const nos_screen scr_prog_lab = { "PROGRESS LAB", lab_enter, 0, lab_tick, lab_draw, lab_input, SCR_KEEPAWAKE };
