/* orrery_events.c — the ORRERY's events list (the face's chip opens it with an iris).
 *
 * What's coming, from now: the face's alignment scan (oppositions, conjunctions, greatest
 * elongations, line-ups), plus mission milestones, asteroids ~25 m and up passing inside
 * 1.4 lunar distances, and bright comets at perihelion, from the sky pack. One row each:
 * the date and how far off in mono, the event in the grotesk. The first row carries the
 * screen's one red dot. Tap a row and the face flies its time there (orrery_jump), and
 * this screen goes back. The house list (ui_list) scrolls it. */
#include "faces/orrery.h"
#include "ui.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NITEM 48
typedef struct { double jd; char title[40]; } item_t;
static struct { item_t it[NITEM]; int n; ui_list list; unsigned generation; } E;

static const char *const NAME[EP_N] = { "Mercury", "Venus", "Earth", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune" };
static const char *const MON[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

static void title_of(const ep_event *e, char *t, int n)
{
    switch (e->kind) {
    case EV_OPPOSITION:  snprintf(t, (size_t)n, "%s at opposition", NAME[e->a]); break;
    case EV_CONJUNCTION: snprintf(t, (size_t)n, "%s meets %s", NAME[e->a], NAME[e->b]); break;
    case EV_ELONGATION:  snprintf(t, (size_t)n, "%s at elongation", NAME[e->a]); break;
    case EV_SOLAR:       snprintf(t, (size_t)n, "%s %s conjunction", NAME[e->a], e->inferior ? "inferior" : "superior"); break;
    default:             snprintf(t, (size_t)n, "%d-planet %s line-up", e->n, e->evening ? "evening" : "morning"); break;
    }
}
static void add(double jd, const char *t)
{
    if (E.n >= NITEM || jd < EP_JD_MIN || jd > EP_JD_MAX) return;
    E.it[E.n].jd = jd; snprintf(E.it[E.n].title, sizeof E.it[0].title, "%s", t); E.n++;
}
static int cmp(const void *a, const void *b)
{
    double d = ((const item_t *)a)->jd - ((const item_t *)b)->jd;
    return d < 0 ? -1 : d > 0;
}
static void build(void)
{
    /* Keep the visible anchor's identity as chronological results are published.
     * Publication is deferred during finger/scroll ownership, so a tap can never
     * select a newly inserted row at the old index. */
    int anchor = (int)floorf(fmaxf(0, E.list.sc.x - E.list.top) / E.list.row_h);
    int keep = E.list.sc.x > 0 && anchor < E.n;
    item_t held;
    if (keep) held = E.it[anchor];
    double now = orrery_now();
    int n; const ep_event *ev = orrery_events(&n);
    E.n = 0;
    for (int i = 0; i < n; i++) if (ev[i].jd >= now - 0.5) { char t[40]; title_of(&ev[i], t, sizeof t); add(ev[i].jd, t); }
    /* BepiColombo's Mercury orbit insertion (planned 21 Nov 2026) */
    double bepi = 2461365.5;
    if (bepi >= now - 0.5 && bepi < now + 460) add(bepi, "BepiColombo reaches Mercury");
    for (int i = 0; i < od_flybys_n; i++) {          /* car-sized rocks pass weekly: ~25 m and up (H < 26) */
        const od_flyby *f = &od_flybys[i];
        if (f->au < 0.0035f && f->h > 0 && f->h < 26 && f->jd >= now - 0.5) { char t[40]; snprintf(t, sizeof t, "Asteroid %s flies by", f->des); add(f->jd, t); }
    }
    for (int i = 0; i < od_comets_n; i++) {
        const od_comet *c = &od_comets[i];
        if (c->m1 < 12 && c->el.tp >= now - 0.5 && c->el.tp < now + 460) {
            char nm[24], t[40]; snprintf(nm, sizeof nm, "%s", c->name);
            char *paren = strchr(nm, '('); if (paren && paren > nm) paren[-1] = 0;
            snprintf(t, sizeof t, "Comet %s at perihelion", nm); add(c->el.tp, t);
        }
    }
    qsort(E.it, (size_t)E.n, sizeof E.it[0], cmp);
    if (keep) for (int i = 0; i < E.n; i++)
        if (fabs(held.jd - E.it[i].jd) < 1e-7 && !strcmp(held.title, E.it[i].title)) {
            float delta = (float)((i - anchor) * E.list.row_h);
            E.list.sc.x += delta; E.list.sc.sp.x += delta; E.list.sc.sp.target += delta;
            E.list.sc.drag_off0 += delta; E.list.fb_prev += delta;
            break;
        }
    ui_fblur_invalidate(&E.list, 0, 0x7fffffff);
    ui_list_set_n(&E.list, E.n);
    E.generation = orrery_events_generation();
}

static void civil_of(double jd, int *d, int *m)
{
    long z = (long)floor(jd + 0.5), a = z + 32044, b = (4 * a + 3) / 146097, c = a - 146097 * b / 4, dd = (4 * c + 3) / 1461,
         e = c - 1461 * dd / 4, mm = (5 * e + 2) / 153;
    *d = (int)(e - (153 * mm + 2) / 5 + 1); *m = (int)(mm + 3 - 12 * (mm / 10));
}
static void row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    if (i < 0 || i >= E.n) return;
    const item_t *it = &E.it[i];
    uint16_t pri = ui_dim(C_PRI, fade), sec = ui_dim(C_SEC, fade);
    /* Keep the middle rows aligned; the list supplies narrower chords near
     * the top and bottom of the glass. */
    if (x0 < UI_ROW_X0) x0 = UI_ROW_X0;
    if (x1 > UI_ROW_X1) x1 = UI_ROW_X1;
    if (x1 - x0 < 180) return;  /* omit unreadable slivers at the round bezel */
    if (pressed) ui_rrect(fb, (float)x0 - 8, (float)y + 2, (float)(x1 - x0) + 16, (float)h - 6, 24, C_SURF2);
    int d, m; civil_of(it->jd, &d, &m);
    int days = (int)floor(it->jd - orrery_now() + 0.5);
    char when[32];
    if (days == 0) snprintf(when, sizeof when, "%02d %s · TODAY", d, MON[m - 1]);
    else snprintf(when, sizeof when, "%02d %s · IN %d D", d, MON[m - 1], days);
    int lx = x0;
    if (i == 0) { dot_disc(fb, (float)x0 + 4, (float)y + 14, 7, ui_dim(C_RED, fade)); lx += 14; }
    ui_text_fit(fb, UI_F_SMALL, lx, y + 19, when, sec, UI_LEFT, x1 - lx);
    ui_text_wrap(fb, UI_F_BODY, x0, y + 48, x1 - x0, 25, 2, it->title, pri, UI_LEFT);
}
static void on_tap(int i)
{
    if (i < 0 || i >= E.n) return;
    orrery_jump(E.it[i].jd);
    nos_feedback(FB_TAP);
    nos_pop();
}

static void enter(void)
{
    if (nos_revealed()) return;
    ui_list_init(&E.list, 0, UI_ROW_H, 104, row, on_tap);
    build();
}
static void tick(float dt)
{
    orrery_events_step();
    if (E.generation != orrery_events_generation() && !E.list.sc.dragging &&
        !E.list.dragging && !E.list.sc.moving && E.list.pressed < 0) build();
    ui_list_tick(&E.list, dt);
}
static void draw(gfx_fb *fb)
{
    ui_list_draw(fb, &E.list);
    ui_title(fb, "Events", ui_list_offset(&E.list));
    int progress = orrery_events_progress();
    if (progress < 100) {
        char status[24]; snprintf(status, sizeof status, "SCANNING %d%%", progress);
        ui_label(fb, UI_CX, 99, status, C_SEC, UI_CENTER);
    }
    if (!E.n) ui_state(fb, UI_CX, UI_CY, progress < 100 ? "SCANNING" : "NO UPCOMING EVENTS", C_SEC);
}
static int input(const nos_input *in) { return ui_list_input(&E.list, in); }

const nos_screen scr_orrery_events = { "ORRERY EVENTS", enter, 0, tick, draw, input, 0 };
void orrery_open_events(int x, int y) { nos_open(&scr_orrery_events, x, y); }
