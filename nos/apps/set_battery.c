/* set_battery.c — Battery: the level and what the watch knows about its power.
 *
 * Opened from Settings > Battery and from the control centre's header (time · %).
 * The hero is the level in dot numerals over ten rounded-square cells (prog.h) that
 * sweep in; under it the state (CHARGING / ON BATTERY / ON USB POWER) and the facts
 * behind it: the system rail voltage and how old the last good sample is.
 *
 * The board senses the system rail, not a fuel gauge: on USB the rail reads the
 * charger, so the percentage is only an estimate while unplugged (battery_valid). The
 * page says so instead of showing a made-up number. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "prog.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>

#define BH_Y    104.0f          /* hero top */
#define BC_N    10              /* ten 12 x 12 cells, 5 px gaps */
#define BC_SLOT 17.0f
#define BC_Y    208.0f
#define BC_IN   3               /* the cells start sweeping 3 frames into the build */

static struct {
    nos_time_t t_enter;
    int pct, cells, charging, external, valid, mv;
    uint32_t age_ms;
    char hero[8];
} BT;

static void bt_sample(void)
{
    const svc_sys_t *s = svc_sys();
    hal_power p;
    hal_power_get(&p);
    BT.pct = s->battery_pct > 0 ? s->battery_pct : hal_battery_percent();
    BT.charging = s->charging;
    BT.external = p.external_power;
    BT.valid = p.battery_valid;
    BT.mv = p.rail_mv ? p.rail_mv : s->rail_mv;
    BT.age_ms = p.age_ms;
    BT.cells = !BT.valid || BT.pct <= 0 ? 0 : (BT.pct + 5) / 10 < 1 ? 1 : (BT.pct + 5) / 10 > BC_N ? BC_N : (BT.pct + 5) / 10;
    if (BT.valid) snprintf(BT.hero, sizeof BT.hero, "%d%%", BT.pct);
    else snprintf(BT.hero, sizeof BT.hero, "--");
}

/* the cells sweep in: EASE_STEADY over 14 frames plus one per lit cell */
static int bt_cells_n(void) { return 14 + BT.cells; }
static float bt_cells_u(void)
{
    int k = m_frame_k(BT.t_enter) - BC_IN, n = bt_cells_n();
    if (k <= 0) return 0;
    return k >= n ? 1.0f : bezier_ease(&EASE_STEADY, (float)k / (float)n);
}

static void bt_enter(void) { BT.t_enter = nos_enter_t(); bt_sample(); }
static void bt_tick(float dt)
{
    (void)dt;
    bt_sample();
    nos_motion_probe("battery.cells.fill", MC_FILL, (float)BT.cells * BC_SLOT * bt_cells_u());
}

static void bt_draw(gfx_fb *fb)
{
    ui_title(fb, "BATTERY", 0);
    if (nos_t < BT.t_enter) return;
    float rp = m_clamp((nos_t - BT.t_enter) / 0.40f, 0, 1);
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, BH_Y, BT.hero, 1, rp, DOT_REVEAL_RANDOM, 9u);
    float kt = m_ink(m_clamp((nos_t - BT.t_enter) / M_FRAMES(6), 0, 1));
    if (kt > 0)
        prog_bar(fb, PROG_SQUARES, UI_CX - BC_SLOT * BC_N * 0.5f, BC_Y, BC_SLOT * BC_N, 12.0f,
                 BC_N, (float)BT.cells / BC_N * bt_cells_u(), C_PRI, ui_dim(C_LINE, kt));
    /* the facts fade in after the hero, in perceptual steps */
    float k = m_ink((nos_t - BT.t_enter - 0.25f) / M_FRAMES(5));
    if (k <= 0) return;
    const char *state = BT.external > 0 ? "ON USB POWER" :
                        BT.external == 0 ? "ON BATTERY" : "POWER SOURCE UNKNOWN";
    ui_state(fb, UI_CX, 268, state, ui_dim(C_PRI, k));
    char b[40];
    if (BT.mv > 0) snprintf(b, sizeof b, "RAIL %.2f V", BT.mv / 1000.0f);
    else snprintf(b, sizeof b, "RAIL %s", UI_NONE);
    ui_label(fb, UI_CX, 318, b, ui_dim(C_SEC, k), UI_CENTER);
    if (!BT.valid) snprintf(b, sizeof b, BT.charging ? "LEVEL SHOWN WHEN UNPLUGGED" : "LEVEL SETTLING");
    else if (BT.age_ms < 2000) snprintf(b, sizeof b, "UPDATED JUST NOW");
    else snprintf(b, sizeof b, "UPDATED %u S AGO", (unsigned)(BT.age_ms / 1000));
    ui_label(fb, UI_CX, 350, b, ui_dim(C_DIS, k), UI_CENTER);
}

const nos_screen scr_battery = { "BATTERY", bt_enter, 0, bt_tick, bt_draw, 0, 0 };
