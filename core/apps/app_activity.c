/* Daily IMU cadence and active time. Distance/calories use explicit estimates. */
#include "apps.h"
#include "../font.h"
#include <stdio.h>

#define PI 3.14159265f

/* ---- tunable model constants ---- */
#define KCAL_PER_STEP     0.04f
#define METERS_PER_STEP   0.762f
#define STEP_GOAL         10000
#define EX_GOAL_MIN       30
#define DISTANCE_GOAL_M 5000

typedef struct {
    int   steps;
    int   calories;   /* kcal */
    float dist_m;     /* metres */
    int   ex_min;     /* measured cadence duration */
    float move_frac;  /* 0..1+ (unclamped) */
    float ex_frac;
    float distance_frac;
} activity_metrics;

/* Pure, side-effect-free: everything the app displays derives from `steps`.
 * Kept isolated so the SELFTEST build can exercise it without any HAL/GFX. */
static void compute_metrics(int steps, unsigned active_seconds, activity_metrics *m)
{
    if (steps < 0) steps = 0;
    m->steps      = steps;
    m->calories   = (int)(steps * KCAL_PER_STEP);
    m->dist_m     = steps * METERS_PER_STEP;
    m->ex_min     = (int)(active_seconds / 60);
    m->move_frac  = (float)steps / (float)STEP_GOAL;
    m->ex_frac    = (float)m->ex_min / (float)EX_GOAL_MIN;
    m->distance_frac = m->dist_m / DISTANCE_GOAL_M;
}

#ifndef SELFTEST

/* 0 = rings view, 1 = stats list */
static int s_view;

static float clamp01(float f)
{
    if (f < 0.0f) return 0.0f;
    if (f > 1.0f) return 1.0f;
    return f;
}

/* one activity ring: grey track + colored progress arc from 12 o'clock */
static void draw_ring(gfx_fb *fb, float r_mid, float th, float frac, uint16_t col)
{
    gfx_ring(fb, GFX_CX, GFX_CY, r_mid, th, GFX_DGREY);
    frac = clamp01(frac);
    float a1 = frac * 2.0f * PI;
    if (a1 > 0.001f)
        gfx_arc(fb, GFX_CX, GFX_CY, r_mid, th, 0.0f, a1, col);
}

static void draw_rings_view(gfx_fb *fb, const activity_metrics *m)
{
    /* nested radii on the 240px design grid, scaled to the panel */
    draw_ring(fb, GFX_S(96), GFX_S(13), m->move_frac,  GFX_PINK);   /* Move    */
    draw_ring(fb, GFX_S(78), GFX_S(13), m->ex_frac,    GFX_GREEN);  /* Exercise*/
    draw_ring(fb, GFX_S(60), GFX_S(13), m->distance_frac, GFX_ACCENT); /* Estimated distance */

    char buf[24];
    snprintf(buf, sizeof buf, "%d", m->steps);
    font_draw_centered(fb, &font_big, (int)GFX_CX,
                       (int)(GFX_CY - GFX_S(8)), buf, GFX_WHITE);
    font_draw_centered(fb, &font_small, (int)GFX_CX,
                       (int)(GFX_CY + GFX_S(8)), "STEPS TODAY", GFX_GREY);

    snprintf(buf, sizeof buf, "~%d KCAL", m->calories);
    font_draw_centered(fb, &font_small, (int)GFX_CX,
                       (int)(GFX_CY + GFX_S(24)), buf, GFX_PINK);
    snprintf(buf, sizeof buf, "~%.2f KM", m->dist_m / 1000.0f);
    font_draw_centered(fb, &font_small, (int)GFX_CX,
                       (int)(GFX_CY + GFX_S(38)), buf, GFX_ACCENT);
}

/* one right-aligned-ish stat row: colored label left, value centered-right */
static void stat_row(gfx_fb *fb, int y, uint16_t col,
                     const char *label, const char *value)
{
    font_draw(fb, &font_small, (int)GFX_S(46), y, label, col);
    int vw = font_text_width(&font_small, value);
    font_draw(fb, &font_small, (int)(GFX_W - GFX_S(46)) - vw, y, value, GFX_WHITE);
}

static void draw_stats_view(gfx_fb *fb, const activity_metrics *m)
{
    font_draw_centered(fb, &font_medium, (int)GFX_CX,
                       (int)GFX_S(52), "ACTIVITY", GFX_WHITE);

    char v[24];
    int y = (int)GFX_S(92);
    int step = (int)GFX_S(24);

    snprintf(v, sizeof v, "%d / %d", m->steps, STEP_GOAL);
    stat_row(fb, y, GFX_GREY, "Steps", v);           y += step;

    /* Move ring is steps-based; show its percent for clarity */
    snprintf(v, sizeof v, "%d%%", (int)(clamp01(m->move_frac) * 100.0f + 0.5f));
    stat_row(fb, y, GFX_PINK, "Move", v);            y += step;

    snprintf(v, sizeof v, "%d / %d min", m->ex_min, EX_GOAL_MIN);
    stat_row(fb, y, GFX_GREEN, "Active", v);       y += step;


    snprintf(v, sizeof v, "~%d kcal", m->calories);
    stat_row(fb, y, GFX_ORANGE, "Calories", v);      y += step;

    snprintf(v, sizeof v, "~%.2f km", m->dist_m / 1000.0f);
    stat_row(fb, y, GFX_YELLOW, "Distance", v);      y += step;

    font_draw_centered(fb, &font_small, (int)GFX_CX,
                       (int)GFX_S(228), "TAP FOR RINGS", GFX_DGREY);
}

static int on_event(const wos_event *ev)
{
    if (ev->type == WOS_EV_TAP) {
        s_view = !s_view;
        return 1;
    }
    return 0;   /* TICK etc.: on_draw already reads live steps every frame */
}

static void on_draw(gfx_fb *fb)
{
    activity_metrics m;
    hal_activity activity; hal_activity_get(&activity);
    compute_metrics(activity.steps>2147483647u ? 2147483647 : (int)activity.steps, activity.active_seconds, &m);

    gfx_clear(fb, GFX_BLACK);
    if (s_view) draw_stats_view(fb, &m);
    else        draw_rings_view(fb, &m);
    if(!activity.valid) font_draw_centered(fb,&font_small,(int)GFX_CX,(int)GFX_S(214),"Motion data unavailable",GFX_GREY);
}

const wos_app app_activity = {
    "Activity", 0, on_event, on_draw, GFX_PINK, ICON_ACTIVITY
};

#else /* SELFTEST ---------------------------------------------------------- */

#include <stdlib.h>
#include <math.h>

static int check(const char *what, int ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

int main(void)
{
    int fails = 0;
    activity_metrics m;

    /* Anchor case: 10000 steps -> 400 kcal, 7620 m, Move ring 100% */
    compute_metrics(10000, 1800, &m);
    printf("10000 steps: cal=%d dist=%.1fm move=%.3f ex=%d/%d\n",
           m.calories, m.dist_m, m.move_frac,
           m.ex_min, EX_GOAL_MIN);
    fails += !check("calories == 400", m.calories == 400);
    fails += !check("distance == 7620 m", fabsf(m.dist_m - 7620.0f) < 0.5f);
    fails += !check("move_frac == 1.0", fabsf(m.move_frac - 1.0f) < 1e-4f);

    /* Half case: 5000 steps -> 200 kcal, 3810 m, Move ring 50% */
    compute_metrics(5000, 900, &m);
    printf("5000 steps:  cal=%d dist=%.1fm move=%.3f\n",
           m.calories, m.dist_m, m.move_frac);
    fails += !check("calories == 200", m.calories == 200);
    fails += !check("distance == 3810 m", fabsf(m.dist_m - 3810.0f) < 0.5f);
    fails += !check("move_frac == 0.5", fabsf(m.move_frac - 0.5f) < 1e-4f);

    /* Zero / negative guards */
    compute_metrics(-100, 0, &m);
    fails += !check("negative steps clamp to 0", m.steps == 0 && m.calories == 0);

    printf(fails ? "\nSELFTEST FAILED (%d)\n" : "\nSELFTEST OK\n", fails);
    return fails ? 1 : 0;
}

#endif
