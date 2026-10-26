/* ORRERY card: tick owns asset adoption, ephemeris and sprite rendering; draw is pure.
 * Large buffers are reusable PSRAM allocations. See docs/ORRERY_CARD.md. */
#include "faces/orrery.h"
#include "faces/face_kit.h"
#include "sprite.h"
#include "apps/nos_apps.h"
#include "gfx_cost.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define PI 3.14159265358979323846f
#define RAD (PI / 180.f)
#define AU 149597870.7
#define TEX_BYTES 1048576u
#define TEX_PAGE_BYTES 65536u
#define TEX_PAGES (TEX_BYTES / TEX_PAGE_BYTES)
#define MAX_SIDE 416
#define MAX_ITEMS 40
#include "orrery_card_data.inc"
typedef struct {
    int16_t lon, lat;
    uint16_t z, v;
} globe_lut;
typedef struct {
    float x, y, d;
    uint16_t c;
} point;
typedef struct {
    float x, y, r, lx, ly;
    int craft;
    char name[32], info[96];
} moon_item;
typedef struct {
    int ob, ready, n, minor_n, moons_state;      /* moons_state: 0 shown, 1 still loading, 2 data missing */
    double jd, dist, sun_dist;
    float olat, olon, slat, slon, lit, elong, sx, sy, ex, ey;
    char hero[20], unit[64], sub[64], count[48], bepi[64];
    point graph[53], minor[512];
    int graph_n;
    moon_item items[MAX_ITEMS], directions[2];
    int direction_n;
    float low, high, planet_r, mark;
    char range[32], next[64];
} body_state;
#include "orrery_card_coords.h"
#include "orrery_card_lighting.h"
#include "orrery_shade_q16.h"
static struct {
    int ob, page, mode, free, drag, dirty, prepared, pending, failed;
    double open_jd;
    uint64_t open_epoch;
    float pos, start_pos, spin, tilt, spin0, tilt0, vel, hold;
    float peek, anim_from, anim_to, anim_t, anim_n, page_from, page_to, page_t, page_n, angle, acc;
    int preview, commit, pressed, dial_queue;
    char info[96];
    nos_time_t info_until;
    unsigned key, preview_key;
    body_state *b, *nb;
    sprite globe, neighbour;
    globe_lut *lut;
    card_coords *coords;
    card_lighting *lighting_coeff;
    const uint32_t *shade_q16; /* permanent immutable global; borrowed, never freed here */
    int shade_q16_checked;
    int16_t *rings;
    float lut_lat;
    int lut_r, lut_row, lut_publish, ring_lat_valid;
    uint16_t *texture[TEX_PAGES], *night[TEX_PAGES];
    uint8_t *ring_tex, *lighting;
    FILE *file;
    unsigned loaded;
    int load_stage, tex_ok, night_ok, ring_ok, load_ob;
    unsigned load_failed;
    float last_spin, last_tilt;
    int last_mode, last_free;
} C;
static int card_frame_work_active;
static unsigned card_frame_work_quanta;
static uint32_t card_frame_work_start;
/* All sprite owners share a monotonically increasing damage revision. Preview
 * readiness is not a reusable image identity across consecutive body commits. */
static unsigned card_image_revision;
static unsigned card_image_key(void){if(++card_image_revision==0)++card_image_revision;return card_image_revision;}
static struct { unsigned max_us,calls;int phase,cursor; } card_prepare_step_stat;

/* UI-thread row window; disabled for the independently retained legacy renderer.
 * All loops read this once per quantum; front pixels are never written here. */
enum { CW_SHADER=1, CW_OVERLAY=2 };
static struct { int active,pass,first,end,detail; } card_window;
static void card_render_cancel(void);
static void card_render_invalidate(void);
static void card_prepare_owned_cancel(void);
static int card_prepare_job_active(void);
static void card_prepare_owned_report(void);
void orrery_card_render_report(void);
static void card_proxy_report(void);
int orrery_card_progressive=1;
unsigned orrery_card_render_budget_us=3000;
unsigned orrery_card_render_quanta=128;
unsigned orrery_card_motion_step=2;

enum {
    CP_ALLOC,
    CP_LIGHT,
    CP_PREPARE,
    CP_EXTRA,
    CP_OPEN,
    CP_READ,
    CP_CLOSE,
    CP_LUT,
    CP_RINGS,
    CP_GLOBE,
    CP_PREVIEW,
    CP_COEFFICIENTS,
    CP_PROXY,
    CP_COUNT
};
typedef struct {
    uint64_t sum_us;
    uint32_t calls, max_us, max_frame, frame, frame_us, frame_max_us, frame_max_id;
    int max_ob, max_load_stage;
} card_profile_stat;
typedef struct {
    uint32_t start, frame;
    int ob, load_stage, enabled;
} card_profile_token;
static card_profile_stat card_profile[CP_COUNT];
static int card_profile_enabled;
static card_profile_token card_profile_begin(void) {
    card_profile_token token = {0};
    if (card_profile_enabled) {
        token.enabled = 1;
        token.frame = nos_frame;
        token.ob = C.ob;
        token.load_stage = C.load_stage;
        token.start = hal_micros();
    }
    return token;
}
static void card_profile_end(unsigned stage, card_profile_token token) {
    if (!token.enabled)
        return;
    uint32_t us = hal_micros() - token.start; /* exact unsigned timer wrap */
    card_profile_stat *stat = &card_profile[stage];
    stat->sum_us += us;
    stat->calls++;
    if (stat->calls == 1 || us > stat->max_us) {
        stat->max_us = us;
        stat->max_frame = token.frame;
        stat->max_ob = token.ob;
        stat->max_load_stage = token.load_stage;
    }
    if (stat->frame != token.frame || stat->calls == 1) {
        stat->frame = token.frame;
        stat->frame_us = 0;
    }
    stat->frame_us += us;
    if (stat->frame_us > stat->frame_max_us) {
        stat->frame_max_us = stat->frame_us;
        stat->frame_max_id = token.frame;
    }
}
/* UI thread only, between frames. No hot-path logging or allocation. */
static struct { uint64_t open_us,read_us,close_us; uint32_t calls,max_us; } asset_profile;
static unsigned asset_profile_epoch;
void orrery_card_profile_set(int enabled, int reset) {
    if (reset) { memset(&card_prepare_step_stat,0,sizeof card_prepare_step_stat);memset(card_profile,0,sizeof card_profile); memset(&asset_profile,0,sizeof asset_profile); ++asset_profile_epoch; }
    card_profile_enabled = enabled != 0;
}
int orrery_card_profile_enabled(void) { return card_profile_enabled; }
void orrery_card_profile_report(void) {
    static const char *const names[CP_COUNT] = {"allocate", "lighting", "prepare", "extra",
                                                "fopen",    "fread",    "close",   "lut",
                                                "rings",    "globe",    "preview", "coefficients", "proxy"};
    printf("CARDPROFILE enabled=%d frame=%u inclusive=1\n", card_profile_enabled,
           (unsigned)nos_frame);
    for (unsigned i = 0; i < CP_COUNT; i++) {
        const card_profile_stat *p = &card_profile[i];
        printf("CARDSTAGE name=%s calls=%u sum_us=%llu max_us=%u frame=%u ob=%d stage=%d "
               "frame_max_us=%u frame_max_id=%u\n",
               names[i], (unsigned)p->calls, (unsigned long long)p->sum_us, (unsigned)p->max_us,
               (unsigned)p->max_frame, p->max_ob, p->max_load_stage, (unsigned)p->frame_max_us,
               (unsigned)p->frame_max_id);
    }
    printf("CARDASSET asynchronous=1 calls=%u read_sum_us=%llu read_max_us=%u open_sum_us=%llu close_sum_us=%llu\n",
           (unsigned)asset_profile.calls,(unsigned long long)asset_profile.read_us,
           (unsigned)asset_profile.max_us,(unsigned long long)asset_profile.open_us,
           (unsigned long long)asset_profile.close_us);
}
static unsigned asset_generation=1,asset_pending_generation;
static int asset_pending_stage,asset_pending_ob,asset_pending_profile_enabled;
static unsigned asset_pending_profile_epoch;
static uint32_t asset_retry_ms;
static uint32_t asset_failed_retry_ms;
static int release_pending;
static struct {
    unsigned completed, failed, cancelled, stale, generation;
    int code, ob, stage, adopted;
} asset_status = {.code = -1, .ob = -1, .stage = -1};
void orrery_card_report_state(void) {
    printf("CARDSTATE active=%d ob=%d load_ob=%d stage=%d tex=%d night=%d ring=%d pending=%u generation=%u\n",
           nos_top() == &scr_orrery_card, C.ob, C.load_ob, C.load_stage,
           C.tex_ok, C.night_ok, C.ring_ok, asset_pending_generation, asset_generation);
    printf("CARDRESULT completed=%u failed=%u cancelled=%u stale=%u generation=%u code=%d ob=%d stage=%d adopted=%d\n",
           asset_status.completed, asset_status.failed, asset_status.cancelled,
           asset_status.stale, asset_status.generation, asset_status.code,
           asset_status.ob, asset_status.stage, asset_status.adopted);
    float raw=C.prepared&&C.b?C.b->olat+C.tilt:0;
    float clamped=raw< -88?-88:raw>88?88:raw;
    float target=roundf(clamped*.5f)*2;
    float active_lat=C.lut_r&&(C.lut_row<2*C.lut_r||C.lut_publish)?C.lut_lat:target;
    printf("CARDPOSE mode=%d free=%d spin=%.3f tilt=%.3f lat=%.3f target=%.3f lut_r=%d\n",
           C.mode,C.free,(double)C.spin,(double)C.tilt,(double)active_lat,(double)target,C.lut_r);
    orrery_card_render_report();
    card_proxy_report();
    card_prepare_owned_report();
}
/* Small internal pointer tables, PSRAM pages. No contiguous megabyte is required. */
static void texture_free(uint16_t **pages)
{
    for (unsigned i = 0; i < TEX_PAGES; i++) { hal_big_free(pages[i]); pages[i] = 0; }
}
static int texture_alloc(uint16_t **pages)
{
    if (pages[0]) return 1;
    for (unsigned i = 0; i < TEX_PAGES; i++) {
        pages[i] = hal_big_alloc(TEX_PAGE_BYTES);
        if (!pages[i]) { texture_free(pages); return 0; }
    }
    return 1;
}
static uint16_t texture_pixel(uint16_t *const *pages, unsigned index)
{
    return pages[index >> 15][index & 32767u];
}
static const char *const tex_name[OB_N] = {"sun",     "mercury", "venus",  "earth",   "moon", "mars",
                                           "jupiter", "saturn",  "uranus", "neptune", 0};
static const float radii[OB_N] = {696000, 2440, 6052, 6371, 1737, 3390, 69911, 58232, 25362, 24622, 1188};
static const uint16_t heat[8] = {GFX_RGB(52, 20, 120),  GFX_RGB(92, 24, 128),  GFX_RGB(140, 36, 112),
                                 GFX_RGB(188, 55, 84),  GFX_RGB(226, 88, 50),  GFX_RGB(246, 140, 20),
                                 GFX_RGB(250, 200, 45), GFX_RGB(252, 255, 164)};
static const float ranges[OB_N][2] = {{5000, 6000}, {90, 700},  {700, 745}, {220, 310}, {90, 400}, {145, 300},
                                      {160, 170},   {130, 138}, {72, 80},   {68, 76},   {33, 55}};
static float clamp(float x, float a, float b)
{
    return x < a ? a : x > b ? b : x;
}
static float norm(float a)
{
    while (a > 180)
        a -= 360;
    while (a < -180)
        a += 360;
    return a;
}
static float quad(float t)
{
    return t < .5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
}
static double card_jd(void)
{
    uint64_t now = hal_epoch_seconds();
    return C.open_jd + (now && C.open_epoch ? ((double)now - (double)C.open_epoch) / 86400.0 : 0);
}
static int pages(int ob)
{
    return ob == OB_SUN || ob == OB_MOON ? 3 : 4;
}
static int radius(int ob)
{
    return ob == OB_SATURN ? 88 : 106;
}
/* Fit the complete ring ellipse between the mode label and hero. Keep the
 * allocation fixed; only the rendered scale changes with latitude. */
static int globe_radius(int ob, float lat)
{
    return ob == OB_SATURN ? (int)fminf(88, 106 / fmaxf(1, 2.33f * fabsf(fk_sin(lat * RAD))))
                           : radius(ob);
}
static int ring_extent(int r, float lat)
{
    return (int)(ceilf(fmaxf((float)r, 2.33f * r * fabsf(fk_sin(lat * RAD))) / 2) * 2) + 2;
}
static double length(ep_vec a)
{
    return sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
static void upper(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}
static uint16_t hc(float t)
{
    int i = (int)(t * 8);
    return heat[i < 0 ? 0 : i > 7 ? 7 : i];
}
/* atan error < .012 degrees; no libm in pixel loops. */
static float atan_fast(float y, float x)
{
    float ax = fabsf(x), ay = fabsf(y), z = (ax > ay ? ay : ax) / (ax > ay ? ax + 1e-12f : ay + 1e-12f),
          q = z * z;
    float a = z * (.999866f + q * (-.3302995f + q * (.180141f + q * (-.085133f + q * .0208351f))));
    if (ay > ax) a = PI * .5f - a;
    if (x < 0) a = PI - a;
    return y < 0 ? -a : a;
}
static float root(float x)
{
    return x <= 0 ? 0 : gfx_fast_sqrt(x);
}
/* Bounds include glyph descent/AA; recorder must retain the full scene. */
static int card_rows(const gfx_fb *fb, int y0, int y1) {
    return fb->rec || (y1 + fb->oy - fb->y_base >= 0 && y0 + fb->oy - fb->y_base < fb->h);
}
static void card_wrap(gfx_fb *fb, const font_t *font, int x, int y, int width, int line_height,
                      int lines, const char *text, uint16_t color, int align) {
    if (card_rows(fb, y - font->ascent - 2,
                  y + (lines - 1) * line_height + font->line_height - font->ascent + 2))
        ui_text_wrap(fb, font, x, y, width, line_height, lines, text, color, align);
}
static void small(gfx_fb *fb, int x, int y, const char *s, uint16_t c, int align) {
    if (!card_rows(fb, y - UI_F_SMALL->ascent - 2,
                   y + UI_F_SMALL->line_height - UI_F_SMALL->ascent + 2))
        return;
    ui_text(fb, UI_F_SMALL, x, y, s, c, align, 1);
}
static void hero(gfx_fb *fb, int y, const char *s, float pitch) {
    if (!card_rows(fb, y - 2, y + (int)(9 * pitch) + 2))
        return;
    dot_style st = {DOT_FONT_5X7, pitch, pitch * .8f, C_PRI, 0, 1, 1};
    dot_text_a(fb, &st, 233, (float)y, s, UI_CENTER);
}
static void light_time(double au, char *out, size_t n) {
    double s = au * 499.005;
    if (s < 60)
        snprintf(out, n, "%.1f S", s);
    else if (s < 3600)
        snprintf(out, n, "%d MIN %d S", (int)(s / 60), (int)fmod(s, 60));
    else
        snprintf(out, n, "%d H %d MIN", (int)(s / 3600), (int)(fmod(s, 3600) / 60));
}
static float interp(const heat_profile *p, float v)
{
    if (v <= p->pts[0][0]) return p->pts[0][1];
    for (int i = 1; i < p->n; i++)
        if (v <= p->pts[i][0]) {
            float a = p->pts[i - 1][0], b = p->pts[i - 1][1];
            return b + (p->pts[i][1] - b) * (v - a) / (p->pts[i][0] - a);
        }
    return p->pts[p->n - 1][1];
}
static float graph_y(const heat_profile *p, float v)
{
    float f = p->ylog ? logf(v / p->y0) / logf(p->y1 / p->y0) : (v - p->y0) / (p->y1 - p->y0);
    return p->yinv ? 124 + 190 * f : 314 - 190 * f;
}
static void add_item(body_state *b, const char *name, ep_vec v, int pole, int craft, const char *info)
{
    if (b->n >= MAX_ITEMS) return;
    moon_item *m = &b->items[b->n++];
    if (pole)
        ep_pole_frame(ob_ephem(b->ob), v, &m->x, &m->y);
    else {
        m->x = (float)v.x;
        m->y = (float)v.y;
    }
    m->r = (float)length(v);
    m->craft = craft;
    snprintf(m->name, sizeof m->name, "%s", name);
    upper(m->name);
    snprintf(m->info, sizeof m->info, "%s", info);
    upper(m->info);
}
static void add_direction(body_state *b, float x, float y, const char *name)
{
    float len = root(x * x + y * y);
    if (len < 1e-6f) return;
    moon_item *m = &b->directions[b->direction_n++];
    /* Keep northward direction dots below the title and count, preserving
     * their bearing rather than clamping only the final Y coordinate. */
    float reach = y > 0 ? fminf(194, 126 * len / y) : 194;
    m->x = 233 + reach * x / len;
    m->y = 236 - reach * y / len;
    m->ly = clamp(m->y + (m->y > 236 ? -12 : 26), 116, 405);
    int left, right, half = ui_text_width(UI_F_SMALL, name, 1) / 2;
    ui_row_span(m->ly < 236 ? m->ly - UI_F_SMALL->ascent : m->ly + 6, 24, &left, &right);
    m->lx = clamp(m->x, (float)(left + half), (float)(right - half));
    snprintf(m->name, sizeof m->name, "%s", name);
}
static void card_count_text(body_state *b, int nm, int nc)
{
    if (b->moons_state)
        snprintf(b->count, sizeof b->count, "%s / %d ORBITER%s", b->moons_state == 2 ? "MOON DATA MISSING" : "MOONS LOADING", nc, nc == 1 ? "" : "S");
    else
        snprintf(b->count, sizeof b->count, "%d KNOWN / %d ORBITER%s", nm, nc, nc == 1 ? "" : "S");
}
static void prepare_profile_impl(body_state *b, int ob, double jd)
{
    memset(b, 0, sizeof *b);
    b->ob = ob;
    b->jd = jd;
    ep_vec earth = ep_helio(EP_EARTH, jd), h = {0, 0, 0};
    int ep = ob_ephem(ob);
    if (ob == OB_PLUTO)
        h = orrery_pluto(jd);
    else if (ob != OB_SUN && ob != OB_MOON)
        h = ep_helio(ep, jd);
    ep_geometry(ep, jd, ob == OB_PLUTO ? h : (ep_vec){0, 0, 0}, &b->olat, &b->olon, &b->slat, &b->slon);
    b->sun_dist = length(h);
    ep_body sky = ep_observe(h, earth);
    b->dist = sky.dist;
    b->elong = (float)sky.elong;
    b->lit = (float)sky.lit;
    if (ob == OB_EARTH) {
        b->dist = 0;
        snprintf(b->hero, sizeof b->hero, "%.3f", b->sun_dist);
        snprintf(b->unit, sizeof b->unit, "AU FROM THE SUN");
    } else {
        if (ob == OB_MOON) {
            double lo, la, km;
            int waxing;
            ep_moon(jd, &lo, &la, &km);
            b->dist = km / AU;
            b->sun_dist = length(earth);
            b->lit = (float)ep_moon_lit(jd, &waxing);
            snprintf(b->sub, sizeof b->sub, "%d%% LIT / %s", (int)(b->lit * 100 + .5f),
                     waxing ? "WAXING" : "WANING");
        }
        char lt[24];
        light_time(b->dist, lt, sizeof lt);
        if (b->dist < .01)
            snprintf(b->hero, sizeof b->hero, "%.0f", b->dist * AU);
        else
            snprintf(b->hero, sizeof b->hero, b->dist < 2 ? "%.3f" : "%.2f", b->dist);
        snprintf(b->unit, sizeof b->unit, "%s FROM YOU / %s", b->dist < .01 ? "KM" : "AU", lt);
        if (ob != OB_SUN && ob != OB_MOON) {
            const char *side = sky.elong < 15 ? "LOST IN THE SUN"
                               : sky.evening  ? "EVENING SKY"
                                              : "MORNING SKY";
            if (ob == OB_VENUS)
                snprintf(b->sub, sizeof b->sub, "%d%% LIT / %s", (int)(100 * b->lit + .5f), side);
            else if (ob == OB_SATURN)
                snprintf(b->sub, sizeof b->sub, "RINGS %.1f DEG / %s", fabsf(b->olat), side);
            else
                snprintf(b->sub, sizeof b->sub, "%s / %.0f DEG", side, sky.elong);
        }
    }
    const heat_profile *p = &profiles[ob];
    b->low = 1e20f;
    b->high = 0;
    for (int i = 0; i < p->n; i++) {
        if (p->pts[i][1] < b->low) b->low = p->pts[i][1];
        if (p->pts[i][1] > b->high) b->high = p->pts[i][1];
    }
    if (ob == OB_SUN)
        snprintf(b->range, sizeof b->range, "4K - 15.7M");
    else
        snprintf(b->range, sizeof b->range, "%.0f  %.0f", b->low - 273.15f, b->high - 273.15f);
    b->graph_n = p->vertical ? 33 : 49;
    for (int i = 0; i < b->graph_n; i++) {
        float f = (float)i / (b->graph_n - 1), v, t;
        point *q = &b->graph[i];
        if (p->vertical) {
            v = p->ylog ? p->y0 * powf(p->y1 / p->y0, f) : p->y0 + (p->y1 - p->y0) * f;
            t = interp(p, v);
            q->x = 100 + 280 * (t - p->x0) / (p->x1 - p->x0);
            q->y = graph_y(p, v);
        } else {
            v = p->x0 + (p->x1 - p->x0) * f;
            t = interp(p, v);
            q->x = 100 + 280 * f;
            q->y = graph_y(p, t);
        }
        q->d = 4.8f;
        q->c = hc((t - b->low) / (b->high - b->low));
    }
    if (pages(ob) == 4) {
        int planet = ob == OB_PLUTO ? 10 : ep, nm = od_moons_known(planet), nc = 0, nmo = 0;
        const od_moon *mo = NULL;
        if (ob != OB_EARTH && nm) {
            int st = od_moons_use(planet);
            if (st > 0) mo = od_moons_of(planet, &nmo);
            else b->moons_state = st < 0 ? 2 : 1;
        }
        ep_vec sun = {-h.x, -h.y, -h.z}, you = {earth.x - h.x, earth.y - h.y, earth.z - h.z};
        ep_pole_frame(ep, sun, &b->sx, &b->sy);
        ep_pole_frame(ep, you, &b->ex, &b->ey);
        float sn = root(b->sx * b->sx + b->sy * b->sy), en = root(b->ex * b->ex + b->ey * b->ey);
        if (ob != OB_EARTH && sn > 0 && en > 0 && (b->sx * b->ex + b->sy * b->ey) / (sn * en) > .98f)
            add_direction(b, b->ex, b->ey, "SUN/YOU");
        else {
            add_direction(b, b->sx, b->sy, "SUN");
            if (ob != OB_EARTH) add_direction(b, b->ex, b->ey, "YOU");
        }
        if (ob == OB_EARTH) {
            double lo, la, km;
            ep_moon(jd, &lo, &la, &km);
            ep_vec v = {km * cos(la * RAD) * cos(lo * RAD), km * cos(la * RAD) * sin(lo * RAD),
                        km * sin(la * RAD)};
            char info[80];
            snprintf(info, sizeof info, "MOON / %.0f KM / %.0f%% LIT", km, 100 * ep_moon_lit(jd, 0));
            add_item(b, "MOON", v, 1, 0, info);
        }
        for (int i = 0; i < nmo; i++) {
                const od_moon *m = &mo[i];
                ep_vec v = ep_sat_pos(&m->el, jd);
                if (m->fresh || nm < 6) {
                    char info[96];
                    snprintf(info, sizeof info, "%s / %.0f KM / %.2f D", m->name, m->el.a, m->el.P);
                    add_item(b, m->name, v, m->fresh, 0, info);
                } else if (b->minor_n < 512) {
                    float a = atan_fast((float)v.y, (float)v.x), r = 198 + (m->code % 7);
                    point q = {233 + r * fk_cos(a), 236 - r * fk_sin(a), 1.8f, C_DIS};
                    if (q.y >= 110) b->minor[b->minor_n++] = q;
                }
            }
        for (int i = 0; i < od_crafts_n; i++) {
            const od_craft *c = &od_crafts[i];
            if (c->planet == planet) {
                nc++;
                if (c->has_orbit) {
                    char info[96];
                    snprintf(info, sizeof info, "%s / %s", c->name, c->status);
                    add_item(b, c->name, ep_sat_pos(&c->orbit, jd), 1, 1, info);
                }
            }
            if (ob == OB_EARTH && !strcmp(c->name, "JWST")) {
                ep_vec e = ep_helio(EP_EARTH, od_pack_jd),
                       v = {(c->xyz[0] - e.x) * AU, (c->xyz[1] - e.y) * AU, (c->xyz[2] - e.z) * AU};
                char info[96];
                snprintf(info, sizeof info, "JWST / %.2f M KM / L2 / PACK %s", length(v) / 1e6, od_built);
                add_item(b, "JWST", v, 1, 1, info);
            }
            if (ob == OB_MERCURY && !strcmp(c->name, "BepiColombo")) {
                ep_vec m = ep_helio(EP_MERCURY, od_pack_jd),
                       v = {c->xyz[0] - m.x, c->xyz[1] - m.y, c->xyz[2] - m.z};
                snprintf(b->bepi, sizeof b->bepi, "BEPICOLOMBO %.1f M KM / %d D", length(v) * AU / 1e6,
                         (int)(2461365.5 - jd));
            }
        }
        card_count_text(b, nm, nc);
        float maxr = 4;
        for (int i = 0; i < b->n; i++) {
            float r = root(b->items[i].r / radii[ob]);
            if (r > maxr) maxr = r;
        }
        /* The full named orbit system fits below the title/count block. */
        float k = 128 / maxr;
        b->planet_r = clamp(k, 9, 18);
        for (int i = 0; i < b->n; i++) {
            moon_item *m = &b->items[i];
            float a = atan_fast(m->y, m->x);
            m->r = k * root(m->r / radii[ob]);
            m->x = 233 + m->r * fk_cos(a);
            m->y = 236 - m->r * fk_sin(a);
        }
        /* Position every marker before placing labels. Use complete font boxes,
         * not just centres, for the disc, marker and label exclusions. */
        int ascent = UI_F_SMALL->ascent, descent = UI_F_SMALL->line_height - ascent;
        /* Outer markers have less room beside the round edge and header. Place
         * them first so a close-in orbiter cannot displace their names far away. */
        int order[MAX_ITEMS];
        for (int i = 0; i < b->n; i++) {
            int j = i;
            while (j > 0 && b->items[order[j - 1]].r < b->items[i].r) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
        int item_width[MAX_ITEMS],direction_width[2],row_left[15],row_right[15];
        for(int i=0;i<b->n;i++) item_width[i]=ui_text_width(UI_F_SMALL,b->items[i].name,1);
        for(int i=0;i<b->direction_n;i++) direction_width[i]=ui_text_width(UI_F_SMALL,b->directions[i].name,1);
        for(int row=0;row<15;row++) {
            int yy=116+20*row;
            ui_row_span((float)(yy<236?yy-ascent:yy+descent),24,&row_left[row],&row_right[row]);
        }
        for (int index = 0; index < b->n; index++) {
            int i = order[index];
            moon_item *m = &b->items[i];
            m->lx = clamp(m->x, 85, 381);
            m->ly = clamp(m->y + (m->y > 236 ? 20 : -12), 110, 386);
            float wantx = m->lx, wanty = m->ly, best = 1e20f;
            int found = 0;
            int width = item_width[i];
            for (int yy = 116; yy <= 396; yy += 20)
                for (int xx = 65; xx <= 401; xx += 12) {
                    float cost = (xx - wantx) * (xx - wantx) + (yy - wanty) * (yy - wanty);
                    if(cost>=best) continue;
                    int left=row_left[(yy-116)/20],right=row_right[(yy-116)/20];
                    if (xx - width / 2 < left || xx + width / 2 > right) continue;
                    float dx = fmaxf(0, (float)abs(xx - 233) - width * .5f),
                          dy = fmaxf(0, fmaxf(yy - ascent - 236, 236 - yy - descent));
                    float disc = b->planet_r * (ob == OB_SATURN ? 1.6f : 1) + 8;
                    if (dx * dx + dy * dy < disc * disc) continue;
                    int collision = 0;
                    for (int j = 0; j < b->direction_n; j++) {
                        const moon_item *d = &b->directions[j];
                        int dw = direction_width[j];
                        if (fabsf(xx - d->lx) < (width + dw) * .5f + 10 &&
                            fabsf(yy - d->ly) < UI_F_SMALL->line_height + 6) collision = 1;
                    }
                    for (int j = 0; j < b->n; j++) {
                        const moon_item *other = &b->items[j];
                        if (fabsf(xx - other->x) < width * .5f + 9 &&
                            other->y > yy - ascent - 9 && other->y < yy + descent + 9) {
                            collision = 1;
                            break;
                        }
                    }
                    for (int j = 0; j < index; j++) {
                        moon_item *prev = &b->items[order[j]];
                        int pw = item_width[order[j]];
                        if (fabsf(xx - prev->lx) < (width + pw) * .5f + 10 &&
                            fabsf(yy - prev->ly) < UI_F_SMALL->line_height + 6) {
                            collision = 1;
                            break;
                        }
                    }
                    if (!collision && cost < best) {
                        best = cost;
                        m->lx = (float)xx;
                        m->ly = (float)yy;
                        found = 1;
                    }
                }
            if (!found) {
                m->lx = wantx;
                m->ly = wanty;
            }
        }
    }
    int ne;
    const ep_event *ev = orrery_events(&ne);
    for (int i = 0; i < ne; i++)
        if (ev[i].jd >= jd - 1 && ((ev[i].kind == EV_LINEUP && ep < EP_N && (ev[i].mask & (1 << ep))) ||
                                   (ev[i].kind != EV_LINEUP && (ev[i].a == ep || ev[i].b == ep)))) {
            char title[40];
            ep_event_title(&ev[i], title, sizeof title);
            snprintf(b->next, sizeof b->next, "%s / %d D", title, (int)(ev[i].jd - jd));
            break;
        }
    b->ready = 1;
}
static void prepare(body_state *b, int ob, double jd)
{
    if (!card_profile_enabled) { prepare_profile_impl(b, ob, jd); return; }
    card_profile_token token = card_profile_begin();
    prepare_profile_impl(b, ob, jd);
    card_profile_end(CP_PREPARE, token);
}

static void close_load_profile_impl(void)
{
    if (++asset_generation==0)++asset_generation;
    if(asset_pending_generation)hal_asset_cancel();
    C.file=NULL; /* all file handles now belong exclusively to backend worker */
}

static void close_load(void)
{
    if (!card_profile_enabled) { close_load_profile_impl(); return; }
    card_profile_token token = card_profile_begin();
    close_load_profile_impl();
    card_profile_end(CP_CLOSE, token);
}

static void load_begin(int ob)
{
    close_load();
    C.load_ob = ob;
    if (ob != OB_EARTH) texture_free(C.night);
    C.load_stage = 0;
    C.load_failed = 0;
    asset_retry_ms = asset_failed_retry_ms = 0;
    C.loaded = 0;
    C.tex_ok = C.night_ok = C.ring_ok = 0;
}
/* UI thread, including stable OFF/AOD and after explicit shutdown drain. */
void orrery_card_asset_poll(void) {
    od_sd_poll(); /* the sky pack's SD files share this job slot (and the shutdown drain) */
    hal_asset_poll();
    if (!asset_pending_generation)
        return;
    asset_request request;
    asset_result result;
    if (!hal_asset_take(&request, &result))
        return;
    int stage = asset_pending_stage;
    int current = request.tag == asset_generation && C.b && !release_pending &&
                  C.load_stage == stage && C.load_ob == asset_pending_ob;
    asset_status.completed++;
    asset_status.failed += result.code == ASSET_FAILED;
    asset_status.cancelled += result.code == ASSET_CANCELLED;
    asset_status.stale += !current;
    asset_status.generation = request.tag;
    asset_status.code = result.code;
    asset_status.ob = asset_pending_ob;
    asset_status.stage = stage;
    asset_status.adopted = current && result.code != ASSET_CANCELLED;
    asset_pending_generation = 0;
    if (card_profile_enabled && asset_pending_profile_enabled &&
        asset_pending_profile_epoch==asset_profile_epoch) {
        asset_profile.open_us += result.open_us;
        asset_profile.read_us += result.read_us;
        asset_profile.close_us += result.close_us;
        asset_profile.calls += result.read_calls;
        if (result.read_max_us > asset_profile.max_us)
            asset_profile.max_us = result.read_max_us;
    }
    if (current && result.code != ASSET_CANCELLED) {
        if (stage == 0)
            memcpy(C.texture, request.pages, sizeof C.texture);
        else if (stage == 1)
            memcpy(C.night, request.pages, sizeof C.night);
        else
            C.ring_tex = request.pages[0];
        if (stage == 0)
            C.tex_ok = result.code == ASSET_OK;
        else if (stage == 1)
            C.night_ok = result.code == ASSET_OK;
        else
            C.ring_ok = result.code == ASSET_OK;
        if (result.code == ASSET_OK) C.load_failed &= ~(1u << stage);
        else {
            C.load_failed |= 1u << stage;
            asset_failed_retry_ms = hal_millis() + 5000;
        }
        C.load_stage++;
        C.dirty = 1;
    } else {
        for (unsigned i = 0; i < request.page_count; i++)
            hal_big_free(request.pages[i]);
    }
}
static void load_tick(void) {
    orrery_card_asset_poll();
    od_sd_pump();
    if (asset_pending_generation || hal_asset_busy())
        return;
    uint32_t now = hal_millis();
    /* Revisit failed stages while this body remains open. Returned pages still
     * move to the worker exclusively; successful stages are never reloaded. */
    if (C.load_stage >= 3) {
        if (!C.load_failed || (int32_t)(now - asset_failed_retry_ms) < 0) return;
        for (int stage = 0; stage < 3; stage++) if (C.load_failed & (1u << stage)) { C.load_stage = stage; break; }
    }
    if ((int32_t)(now - asset_retry_ms) < 0)
        return;
    const char *name = NULL;
    uint16_t **texture = NULL;
    void *ring = NULL;
    unsigned size = TEX_BYTES;
    if (C.load_stage == 0) {
        if (C.tex_ok) { C.load_stage++; return; }
        name = tex_name[C.load_ob];
        texture = C.texture;
    } else if (C.load_stage == 1 && C.load_ob == OB_EARTH) {
        if (C.night_ok) { C.load_stage++; return; }
        name = "earth_night";
        texture = C.night;
    } else if (C.load_stage == 2 && C.load_ob == OB_SATURN) {
        if (C.ring_ok) { C.load_stage++; return; }
        name = "saturn_ring";
        size = 2048;
        ring = C.ring_tex;
    }
    if (!name) {
        C.load_stage++;
        return;
    }
    if (texture && !texture_alloc(texture)) {
        asset_retry_ms = now + 1000;
        return;
    }
    if (!texture && !ring) {
        C.ring_tex = hal_big_alloc(2048);
        ring = C.ring_tex;
        if (!ring) {
            asset_retry_ms = now + 1000;
            return;
        }
    }
    asset_request request = {0};
    snprintf(request.path, sizeof request.path, "%s/sky/%s.%s", hal_sd_root(), name,
             size == 2048 ? "rgba" : "565");
    request.bytes = size;
    request.page_bytes = texture ? TEX_PAGE_BYTES : 2048;
    request.page_count = texture ? TEX_PAGES : 1;
    request.tag = asset_generation;
    if (texture)
        memcpy(request.pages, texture, sizeof C.texture);
    else
        request.pages[0] = ring;
    int accepted = hal_asset_submit(&request);
    if (accepted == 1) {
        asset_pending_profile_enabled=card_profile_enabled;
        asset_pending_profile_epoch=asset_profile_epoch;
        asset_pending_generation = asset_generation;
        asset_pending_stage = C.load_stage;
        asset_pending_ob = C.load_ob;
        /* Destination pointers are moved, never borrowed: renderer/free cannot reach them. */
        if (texture)
            memset(texture, 0, sizeof C.texture);
        else
            C.ring_tex = NULL;
    } else if (accepted < 0)
        asset_retry_ms = now + 1000;
}

/* Fast LUT build: two bounded slices, then shade on a separate frame. This
 * keeps tilt latency at roughly 100 ms without combining both CPU peaks. */
static int build_lut_profile_impl(int r, float lat)
{
    int n = r * 2;
    const float *axis = card_coords_axis(C.coords, 0, r);
    if (C.lut_r != r || C.lut_lat != lat) {
        C.lut_r = r;
        C.lut_lat = lat;
        C.lut_row = 0;
        C.lut_publish = 0;
        C.ring_lat_valid = 0;
    }
    if (C.lut_row >= n) return 1;
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD);
    int end = C.lut_row + 128;
    if (end > n) end = n;
    for (int y = C.lut_row; y < end; y++)
        for (int x = 0; x < n; x++) {
            globe_lut *q = &C.lut[y * n + x];
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, rr = nx * nx + ny * ny;
            q->z = 65535;
            if (rr > 1) continue;
            float z = root(1 - rr), bx = -ny * sl + z * cl, bz = ny * cl + z * sl,
                  la = atan_fast(bz, root(1 - bz * bz)) / RAD;
            q->lon = (int16_t)(atan_fast(nx, bx) / PI * 32767);
            q->lat = (int16_t)(la * 100);
            q->v = (uint16_t)clamp((90 - la) * 512 / 180, 0, 511);
            q->z = (uint16_t)(z * 32767);
        }
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)((end - C.lut_row) * n) * 2u);
    C.lut_row = end;
    C.lut_publish = end == n;
    return 0;
}
static int build_lut(int r, float lat)
{
    if (!card_profile_enabled) { return build_lut_profile_impl(r, lat); }
    card_profile_token token = card_profile_begin();
    int result = build_lut_profile_impl(r, lat);
    card_profile_end(CP_LUT, token);
    return result;
}

static float surface(float l, float h, float ss, float night, float dusk, float tau)
{
    return l > 0 ? fmaxf(night, ss * root(root(l))) : night + (dusk - night) * gfx_fast_exp(-h / tau);
}
static float temperature(const body_state *b, float lat, float lon, float l)
{
    float h = norm(lon - b->slon - 90);
    if (h < 0) h += 360;
    float s = fk_sin(lat * RAD);
    switch (b->ob) {
    case OB_SUN:
        return 5772;
    case OB_MERCURY:
        return surface(l, h, 700 * root(.387f / (float)b->sun_dist), 100, 300, 25);
    case OB_VENUS:
        return 737 - 4 * s * s;
    case OB_EARTH:
        s = fk_sin((lat - .45f * b->slat) * RAD);
        return 300 - 70 * s * s + (l > 0 ? 8 * l : -4);
    case OB_MOON:
        return surface(l, h, 390, 95, 250, 30);
    case OB_MARS:
        return fabsf(lat) > 72 && lat * b->slat < 0
                   ? 150
                   : surface(l, h, 290 * root(1.52f / (float)b->sun_dist), 175, 230, 40);
    case OB_JUPITER:
        return 165 + 2 * fk_cos(lat * 9 * RAD);
    case OB_SATURN:
        return 134 + 2 * fk_cos(lat * 7 * RAD);
    case OB_URANUS:
        return 76 + 1.5f * fk_cos(lat * 4 * RAD);
    case OB_NEPTUNE:
        return 72 + 1.5f * fk_cos(lat * 4 * RAD);
    default:
        return 40 + 12 * fmaxf(0, l);
    }
}
static uint16_t shade(uint16_t c, float k, uint16_t night, float nk)
{
    if (!night && (c >> 11) == (c & 31) && abs((int)((c >> 5) & 63) / 2 - (int)(c >> 11)) <= 1) {
        int v = (int)((c >> 11) * k + .5f);
        return (uint16_t)(v << 11 | (v * 2 + (v >= 16)) << 5 | v);
    }
    int r = (int)((c >> 11) * k + (night >> 11) * nk * .9f),
        g = (int)(((c >> 5) & 63) * k + ((night >> 5) & 63) * nk * .8f),
        b = (int)((c & 31) * k + (night & 31) * nk * .6f);
    return (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
}
/* Integer path only for verified standard coefficients and zero night contribution. */
static inline __attribute__((always_inline)) uint16_t shade_q16(uint16_t c,uint32_t multiplier)
{
    unsigned r=c>>11,g=(c>>5)&63,b=c&31;
    if(r==b && abs((int)(g/2)-(int)r)<=1) {
        unsigned v=(r*multiplier+32768u)>>16;
        return (uint16_t)(v<<11|(v*2+(v>=16))<<5|v);
    }
    return (uint16_t)(((r*multiplier)>>16)<<11|((g*multiplier)>>16)<<5|((b*multiplier)>>16));
}
static void halftone(sprite *s)
{
    float mx = .05f;
    for (int i = 0; i < s->w * s->h; i += 7) {
        uint16_t c = s->px[i];
        float l = (c >> 11) * .3f / 31 + ((c >> 5) & 63) * .59f / 63 + (c & 31) * .11f / 31;
        if (l > mx) mx = l;
    }
    for (int y = 0; y < s->h; y += 4)
        for (int x = 0; x < s->w; x += 4) {
            float r = 0, g = 0, b = 0;
            int n = 0;
            for (int j = y; j < y + 4 && j < s->h; j++)
                for (int i = x; i < x + 4 && i < s->w; i++) {
                    uint16_t c = s->px[j * s->w + i];
                    if (c) {
                        r += (c >> 11) * 255.f / 31;
                        g += ((c >> 5) & 63) * 255.f / 63;
                        b += (c & 31) * 255.f / 31;
                        n++;
                    }
                    s->px[j * s->w + i] = 0;
                }
            if (!n) continue;
            r /= n;
            g /= n;
            b /= n;
            float l = clamp((r * .3f + g * .59f + b * .11f) / 255 / mx, 0, 1);
            if (l < .04f) continue;
            float m = fmaxf(r, fmaxf(g, b)), k = (.6f + .4f * l) * 255 / fmaxf(m, 1),
                  rr = 5.0176f * l * n / 16;
            uint16_t c =
                GFX_RGB((int)clamp(r * k, 0, 255), (int)clamp(g * k, 0, 255), (int)clamp(b * k, 0, 255));
            for (int j = 0; j < 4 && y + j < s->h; j++)
                for (int i = 0; i < 4 && x + i < s->w; i++) {
                    float d = (i - 1.5f) * (i - 1.5f) + (j - 1.5f) * (j - 1.5f),
                          a = clamp(rr - d + .5f, 0, 1);
                    s->px[(y + j) * s->w + x + i] = shade(c, a, 0, 0);
                }
        }
}
static void build_rings_profile_impl(float lat)
{
    float sl = fk_sin(lat * RAD), cl = fk_cos(lat * RAD);
    int r = C.lut_r, ext = (int)ceilf(2.33f * r) + 2, ey = ring_extent(r, lat), w = ext * 2;
    int band = (int)ceilf(2.33f * r * fabsf(sl));
    memset(C.rings, 0, 416u * 236u * 2);
    if (fabsf(sl) > .001f)
        for (int y = ey - band; y < ey + band; y++)
            for (int x = 0; x < w; x++) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl,
                      rho2 = qx * qx + px * px;
                if (rho2 <= 1.11f * 1.11f || rho2 >= 2.33f * 2.33f) continue;
                float rr = px * px + py * py, t = -py * cl / sl;
                int front = rr > 1 || t > root(1 - rr),
                    ri = (int)clamp((root(rho2) - 1.11f) / 1.22f * 511, 0, 511);
                C.rings[y * w + x] = (int16_t)((ri + 1) * (front ? 1 : -1));
            }
    C.ring_lat_valid = 1;
    GFX_COST_PX(GFX_COST_BLIT, (unsigned)(w * band * 2) * 4u);
}
static void build_rings(float lat)
{
    if (!card_profile_enabled) { build_rings_profile_impl(lat); return; }
    card_profile_token token = card_profile_begin();
    build_rings_profile_impl(lat);
    card_profile_end(CP_RINGS, token);
}

#include "orrery_card_qz.h"
static void render_globe_legacy(sprite *s, const body_state *b, float spin, float lat, int textured)
{
    int r = C.lut_r, n = r * 2, ext = b->ob == OB_SATURN ? (int)ceilf(2.33f * r) + 2 : r;
    int ey = b->ob == OB_SATURN ? ring_extent(r, lat) : r;
    const float *axis = card_coords_axis(C.coords, 0, r);
    s->w = ext * 2;
    s->h = ey * 2;
    if (!card_window.active) memset(s->px, 0, (size_t)s->w * s->h * 2);
    int step=card_window.active?card_window.detail:1;
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD), dl = (b->slon - b->olon - spin) * RAD,
          ss = fk_sin(b->slat * RAD), cs = fk_cos(b->slat * RAD);
    float bx = cs * fk_cos(dl), by = cs * fk_sin(dl), sx = by, sy = -bx * sl + ss * cl,
          sz = bx * cl + ss * sl;
    int offset = (int)((spin + b->olon + 180) * 1024 / 360);
    if (!card_window.active || card_window.pass==CW_SHADER)
    for (int y=card_window.active?card_window.first:0; y<n && (!card_window.active || y<card_window.end); y+=step)
        for (int x=0; x<n; x+=step) {
            const globe_lut *q = &C.lut[y * n + x];
            if (q->z == 65535) continue;
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, nz = q->z / 32767.f,
                  real = nx * sx + ny * sy + nz * sz,
                  l = C.free ? fmaxf(.35f, -.25f * nx + .3f * ny + .92f * nz) : real;
            int u = (offset + (int)q->lon * 512 / 32767) & 1023;
            unsigned idx = q->v * 1024u + (unsigned)u;
            uint16_t color = textured && C.tex_ok ? texture_pixel(C.texture, (unsigned)idx) : ob_tint[b->ob];
            if (C.mode == 1) {
                float t = temperature(b, q->lat * .01f, q->lon * (180.f / 32767) + spin + b->olon, real);
                color = hc((t - ranges[b->ob][0]) / (ranges[b->ob][1] - ranges[b->ob][0]));
            } else {
                float k = b->ob == OB_SUN
                              ? .55f + .45f * C.lighting[256 + (int)clamp(nz * 255, 0, 255)] / 255.f
                              : .05f + .95f * C.lighting[(int)clamp(l * 255, 0, 255)] / 255.f;
                uint16_t night = 0;
                float nk = 0;
                if (textured && C.night_ok && b->ob == OB_EARTH && !C.free && l < .12f) {
                    night = texture_pixel(C.night, (unsigned)idx);
                    nk = clamp((.12f - l) / .15f, 0, 1);
                }
                color = shade(color, k, night, nk);
            }
            int dx=x+ext-r,dy=y+ey-r;
            for(int yy=dy;yy<dy+step && yy<s->h;yy++)
                for(int xx=dx;xx<dx+step && xx<s->w;xx++)s->px[yy*s->w+xx]=color;
        }
    if ((!card_window.active || card_window.pass==CW_OVERLAY) && b->ob == OB_SATURN && fabsf(sl) > .001f) {
        int band = (int)ceilf(2.33f * r * fabsf(sl));
        int first=card_window.active?card_window.first:ey-band;
        if(first<ey-band)first=ey-band;
        /* Preserve the original ring sample lattice across arbitrary row quanta. */
        if(step>1)first-=((first-(ey-band))%step);
        for (int y=first; y<ey+band && (!card_window.active || y<card_window.end); y+=step)
            for (int x=0; x<s->w; x+=step) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl, qy = px;
                int j = y * s->w + x;
                int ri = C.rings[j] - 1;
                if (ri < 0) continue;
                float dot = qx * bx + qy * by, perp = qx * qx + qy * qy - dot * dot;
                float k = C.free ? .95f : (dot < 0 && perp < 1) ? .12f : ss * sl > 0 ? .95f : .4f;
                uint16_t col = ob_tint[OB_SATURN];
                float alpha = .7f;
                if (textured && C.ring_ok) {
                    const uint8_t *rt = C.ring_tex + ri * 4;
                    col = GFX_RGB(rt[0], rt[1], rt[2]);
                    alpha = rt[3] / 255.f;
                }
                uint16_t rc=shade(col,k,0,0);uint8_t ra=(uint8_t)(alpha*255);
                for(int yy=y;yy<y+step && yy<s->h && (!card_window.active || yy<card_window.end);yy++)
                    if(!card_window.active || yy>=card_window.first)
                    for(int xx=x;xx<x+step && xx<s->w;xx++) {
                        int at=yy*s->w+xx;s->px[at]=gfx_blend565(s->px[at],rc,ra);
                    }
            }
        C.ring_lat_valid = 1;
        GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * (card_window.active?card_window.end-card_window.first:band*2)) * 2u);
    }
    if (!card_window.active && C.mode == 2) halftone(s);
    /* Proxy estimates for tick math omitted by the stock raster model. These
     * are explicit estimates, NOT measured Xtensa timings. */
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)(card_window.active ? (card_window.end-card_window.first)*n/(step*step) : r*r*PI) * (C.mode == 1 ? 3u : 1u));

    if (!card_window.active && C.mode == 2) GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * s->h) * 3u);
}
static void render_globe_fast(sprite *s, const body_state *b, float spin, float lat, int textured)
{
    int r = C.lut_r, n = r * 2, ext = b->ob == OB_SATURN ? (int)ceilf(2.33f * r) + 2 : r;
    int ey = b->ob == OB_SATURN ? ring_extent(r, lat) : r;
    const float *axis = card_coords_axis(C.coords, 0, r);
    s->w = ext * 2;
    s->h = ey * 2;
    if (!card_window.active) memset(s->px, 0, (size_t)s->w * s->h * 2);
    int step=card_window.active?card_window.detail:1;
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD), dl = (b->slon - b->olon - spin) * RAD,
          ss = fk_sin(b->slat * RAD), cs = fk_cos(b->slat * RAD);
    float bx = cs * fk_cos(dl), by = cs * fk_sin(dl), sx = by, sy = -bx * sl + ss * cl,
          sz = bx * cl + ss * sl;
    int offset = (int)((spin + b->olon + 180) * 1024 / 360);
    if (!card_window.active || card_window.pass==CW_SHADER)
    for (int y=card_window.active?card_window.first:0; y<n && (!card_window.active || y<card_window.end); y+=step)
        for (int x=0; x<n; x+=step) {
            const globe_lut *q = &C.lut[y * n + x];
            if (q->z == 65535) continue;
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, nz = card_qz_exact(q->z),
                  real = nx * sx + ny * sy + nz * sz,
                  l = C.free ? fmaxf(.35f, -.25f * nx + .3f * ny + .92f * nz) : real;
            int u = (offset + (int)q->lon * 512 / 32767) & 1023;
            unsigned idx = q->v * 1024u + (unsigned)u;
            uint16_t color = textured && C.tex_ok ? texture_pixel(C.texture, (unsigned)idx) : ob_tint[b->ob];
            if (C.mode == 1) {
                float t = temperature(b, q->lat * .01f, q->lon * (180.f / 32767) + spin + b->olon, real);
                color = hc((t - ranges[b->ob][0]) / (ranges[b->ob][1] - ranges[b->ob][0]));
            } else {
                float k = b->ob == OB_SUN
                              ? .55f + .45f * C.lighting[256 + (int)clamp(nz * 255, 0, 255)] / 255.f
                              : .05f + .95f * C.lighting[(int)clamp(l * 255, 0, 255)] / 255.f;
                uint16_t night = 0;
                float nk = 0;
                if (textured && C.night_ok && b->ob == OB_EARTH && !C.free && l < .12f) {
                    night = texture_pixel(C.night, (unsigned)idx);
                    nk = clamp((.12f - l) / .15f, 0, 1);
                }
                color = shade(color, k, night, nk);
            }
            int dx=x+ext-r,dy=y+ey-r;
            for(int yy=dy;yy<dy+step && yy<s->h;yy++)
                for(int xx=dx;xx<dx+step && xx<s->w;xx++)s->px[yy*s->w+xx]=color;
        }
    if ((!card_window.active || card_window.pass==CW_OVERLAY) && b->ob == OB_SATURN && fabsf(sl) > .001f) {
        int band = (int)ceilf(2.33f * r * fabsf(sl));
        int first=card_window.active?card_window.first:ey-band;
        if(first<ey-band)first=ey-band;
        /* Preserve the original ring sample lattice across arbitrary row quanta. */
        if(step>1)first-=((first-(ey-band))%step);
        for (int y=first; y<ey+band && (!card_window.active || y<card_window.end); y+=step)
            for (int x=0; x<s->w; x+=step) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl, qy = px;
                int j = y * s->w + x;
                int ri = C.rings[j] - 1;
                if (ri < 0) continue;
                float dot = qx * bx + qy * by, perp = qx * qx + qy * qy - dot * dot;
                float k = C.free ? .95f : (dot < 0 && perp < 1) ? .12f : ss * sl > 0 ? .95f : .4f;
                uint16_t col = ob_tint[OB_SATURN];
                float alpha = .7f;
                if (textured && C.ring_ok) {
                    const uint8_t *rt = C.ring_tex + ri * 4;
                    col = GFX_RGB(rt[0], rt[1], rt[2]);
                    alpha = rt[3] / 255.f;
                }
                uint16_t rc=shade(col,k,0,0);uint8_t ra=(uint8_t)(alpha*255);
                for(int yy=y;yy<y+step && yy<s->h && (!card_window.active || yy<card_window.end);yy++)
                    if(!card_window.active || yy>=card_window.first)
                    for(int xx=x;xx<x+step && xx<s->w;xx++) {
                        int at=yy*s->w+xx;s->px[at]=gfx_blend565(s->px[at],rc,ra);
                    }
            }
        C.ring_lat_valid = 1;
        GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * (card_window.active?card_window.end-card_window.first:band*2)) * 2u);
    }
    if (!card_window.active && C.mode == 2) halftone(s);
    /* Proxy estimates for tick math omitted by the stock raster model. These
     * are explicit estimates, NOT measured Xtensa timings. */
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)(card_window.active ? (card_window.end-card_window.first)*n/(step*step) : r*r*PI) * (C.mode == 1 ? 3u : 1u));

    if (!card_window.active && C.mode == 2) GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * s->h) * 3u);
}
/* Diagnostic A/B switch: one selection per globe, never in the pixel loop. */
static void render_globe_legacy_cached(sprite *s, const body_state *b, float spin, float lat, int textured)
{
    int r = C.lut_r, n = r * 2, ext = b->ob == OB_SATURN ? (int)ceilf(2.33f * r) + 2 : r;
    int ey = b->ob == OB_SATURN ? ring_extent(r, lat) : r;
    const float *axis = card_coords_axis(C.coords, 0, r);
    s->w = ext * 2;
    s->h = ey * 2;
    if (!card_window.active) memset(s->px, 0, (size_t)s->w * s->h * 2);
    int step=card_window.active?card_window.detail:1;
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD), dl = (b->slon - b->olon - spin) * RAD,
          ss = fk_sin(b->slat * RAD), cs = fk_cos(b->slat * RAD);
    float bx = cs * fk_cos(dl), by = cs * fk_sin(dl), sx = by, sy = -bx * sl + ss * cl,
          sz = bx * cl + ss * sl;
    int offset = (int)((spin + b->olon + 180) * 1024 / 360);
    if (!card_window.active || card_window.pass==CW_SHADER)
    for (int y=card_window.active?card_window.first:0; y<n && (!card_window.active || y<card_window.end); y+=step)
        for (int x=0; x<n; x+=step) {
            const globe_lut *q = &C.lut[y * n + x];
            if (q->z == 65535) continue;
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, nz = q->z / 32767.f,
                  real = nx * sx + ny * sy + nz * sz,
                  l = C.free ? fmaxf(.35f, -.25f * nx + .3f * ny + .92f * nz) : real;
            int u = (offset + (int)q->lon * 512 / 32767) & 1023;
            unsigned idx = q->v * 1024u + (unsigned)u;
            uint16_t color = textured && C.tex_ok ? texture_pixel(C.texture, (unsigned)idx) : ob_tint[b->ob];
            if (C.mode == 1) {
                float t = temperature(b, q->lat * .01f, q->lon * (180.f / 32767) + spin + b->olon, real);
                color = hc((t - ranges[b->ob][0]) / (ranges[b->ob][1] - ranges[b->ob][0]));
            } else {
                float k = b->ob == OB_SUN
                              ? C.lighting_coeff->k[256 + (int)clamp(nz * 255, 0, 255)]
                              : C.lighting_coeff->k[(int)clamp(l * 255, 0, 255)];
                uint16_t night = 0;
                float nk = 0;
                if (textured && C.night_ok && b->ob == OB_EARTH && !C.free && l < .12f) {
                    night = texture_pixel(C.night, (unsigned)idx);
                    nk = clamp((.12f - l) / .15f, 0, 1);
                }
                color = shade(color, k, night, nk);
            }
            int dx=x+ext-r,dy=y+ey-r;
            for(int yy=dy;yy<dy+step && yy<s->h;yy++)
                for(int xx=dx;xx<dx+step && xx<s->w;xx++)s->px[yy*s->w+xx]=color;
        }
    if ((!card_window.active || card_window.pass==CW_OVERLAY) && b->ob == OB_SATURN && fabsf(sl) > .001f) {
        int band = (int)ceilf(2.33f * r * fabsf(sl));
        int first=card_window.active?card_window.first:ey-band;
        if(first<ey-band)first=ey-band;
        /* Preserve the original ring sample lattice across arbitrary row quanta. */
        if(step>1)first-=((first-(ey-band))%step);
        for (int y=first; y<ey+band && (!card_window.active || y<card_window.end); y+=step)
            for (int x=0; x<s->w; x+=step) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl, qy = px;
                int j = y * s->w + x;
                int ri = C.rings[j] - 1;
                if (ri < 0) continue;
                float dot = qx * bx + qy * by, perp = qx * qx + qy * qy - dot * dot;
                float k = C.free ? .95f : (dot < 0 && perp < 1) ? .12f : ss * sl > 0 ? .95f : .4f;
                uint16_t col = ob_tint[OB_SATURN];
                float alpha = .7f;
                if (textured && C.ring_ok) {
                    const uint8_t *rt = C.ring_tex + ri * 4;
                    col = GFX_RGB(rt[0], rt[1], rt[2]);
                    alpha = rt[3] / 255.f;
                }
                uint16_t rc=shade(col,k,0,0);uint8_t ra=(uint8_t)(alpha*255);
                for(int yy=y;yy<y+step && yy<s->h && (!card_window.active || yy<card_window.end);yy++)
                    if(!card_window.active || yy>=card_window.first)
                    for(int xx=x;xx<x+step && xx<s->w;xx++) {
                        int at=yy*s->w+xx;s->px[at]=gfx_blend565(s->px[at],rc,ra);
                    }
            }
        C.ring_lat_valid = 1;
        GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * (card_window.active?card_window.end-card_window.first:band*2)) * 2u);
    }
    if (!card_window.active && C.mode == 2) halftone(s);
    /* Proxy estimates for tick math omitted by the stock raster model. These
     * are explicit estimates, NOT measured Xtensa timings. */
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)(card_window.active ? (card_window.end-card_window.first)*n/(step*step) : r*r*PI) * (C.mode == 1 ? 3u : 1u));

    if (!card_window.active && C.mode == 2) GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * s->h) * 3u);
}

static void render_globe_fast_cached(sprite *s, const body_state *b, float spin, float lat, int textured)
{
    int r = C.lut_r, n = r * 2, ext = b->ob == OB_SATURN ? (int)ceilf(2.33f * r) + 2 : r;
    int ey = b->ob == OB_SATURN ? ring_extent(r, lat) : r;
    const float *axis = card_coords_axis(C.coords, 0, r);
    s->w = ext * 2;
    s->h = ey * 2;
    if (!card_window.active) memset(s->px, 0, (size_t)s->w * s->h * 2);
    int step=card_window.active?card_window.detail:1;
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD), dl = (b->slon - b->olon - spin) * RAD,
          ss = fk_sin(b->slat * RAD), cs = fk_cos(b->slat * RAD);
    float bx = cs * fk_cos(dl), by = cs * fk_sin(dl), sx = by, sy = -bx * sl + ss * cl,
          sz = bx * cl + ss * sl;
    int offset = (int)((spin + b->olon + 180) * 1024 / 360);
    if (!card_window.active || card_window.pass==CW_SHADER)
    for (int y=card_window.active?card_window.first:0; y<n && (!card_window.active || y<card_window.end); y+=step)
        for (int x=0; x<n; x+=step) {
            const globe_lut *q = &C.lut[y * n + x];
            if (q->z == 65535) continue;
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, nz = card_qz_exact(q->z),
                  real = nx * sx + ny * sy + nz * sz,
                  l = C.free ? fmaxf(.35f, -.25f * nx + .3f * ny + .92f * nz) : real;
            int u = (offset + (int)q->lon * 512 / 32767) & 1023;
            unsigned idx = q->v * 1024u + (unsigned)u;
            uint16_t color = textured && C.tex_ok ? texture_pixel(C.texture, (unsigned)idx) : ob_tint[b->ob];
            if (C.mode == 1) {
                float t = temperature(b, q->lat * .01f, q->lon * (180.f / 32767) + spin + b->olon, real);
                color = hc((t - ranges[b->ob][0]) / (ranges[b->ob][1] - ranges[b->ob][0]));
            } else {
                float k = b->ob == OB_SUN
                              ? C.lighting_coeff->k[256 + (int)clamp(nz * 255, 0, 255)]
                              : C.lighting_coeff->k[(int)clamp(l * 255, 0, 255)];
                uint16_t night = 0;
                float nk = 0;
                if (textured && C.night_ok && b->ob == OB_EARTH && !C.free && l < .12f) {
                    night = texture_pixel(C.night, (unsigned)idx);
                    nk = clamp((.12f - l) / .15f, 0, 1);
                }
                color = shade(color, k, night, nk);
            }
            int dx=x+ext-r,dy=y+ey-r;
            for(int yy=dy;yy<dy+step && yy<s->h;yy++)
                for(int xx=dx;xx<dx+step && xx<s->w;xx++)s->px[yy*s->w+xx]=color;
        }
    if ((!card_window.active || card_window.pass==CW_OVERLAY) && b->ob == OB_SATURN && fabsf(sl) > .001f) {
        int band = (int)ceilf(2.33f * r * fabsf(sl));
        int first=card_window.active?card_window.first:ey-band;
        if(first<ey-band)first=ey-band;
        /* Preserve the original ring sample lattice across arbitrary row quanta. */
        if(step>1)first-=((first-(ey-band))%step);
        for (int y=first; y<ey+band && (!card_window.active || y<card_window.end); y+=step)
            for (int x=0; x<s->w; x+=step) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl, qy = px;
                int j = y * s->w + x;
                int ri = C.rings[j] - 1;
                if (ri < 0) continue;
                float dot = qx * bx + qy * by, perp = qx * qx + qy * qy - dot * dot;
                float k = C.free ? .95f : (dot < 0 && perp < 1) ? .12f : ss * sl > 0 ? .95f : .4f;
                uint16_t col = ob_tint[OB_SATURN];
                float alpha = .7f;
                if (textured && C.ring_ok) {
                    const uint8_t *rt = C.ring_tex + ri * 4;
                    col = GFX_RGB(rt[0], rt[1], rt[2]);
                    alpha = rt[3] / 255.f;
                }
                uint16_t rc=shade(col,k,0,0);uint8_t ra=(uint8_t)(alpha*255);
                for(int yy=y;yy<y+step && yy<s->h && (!card_window.active || yy<card_window.end);yy++)
                    if(!card_window.active || yy>=card_window.first)
                    for(int xx=x;xx<x+step && xx<s->w;xx++) {
                        int at=yy*s->w+xx;s->px[at]=gfx_blend565(s->px[at],rc,ra);
                    }
            }
        C.ring_lat_valid = 1;
        GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * (card_window.active?card_window.end-card_window.first:band*2)) * 2u);
    }
    if (!card_window.active && C.mode == 2) halftone(s);
    /* Proxy estimates for tick math omitted by the stock raster model. These
     * are explicit estimates, NOT measured Xtensa timings. */
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)(card_window.active ? (card_window.end-card_window.first)*n/(step*step) : r*r*PI) * (C.mode == 1 ? 3u : 1u));

    if (!card_window.active && C.mode == 2) GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * s->h) * 3u);
}

static void render_globe_legacy_cached_q16(sprite *s, const body_state *b, float spin, float lat, int textured, const uint32_t *multipliers)
{
    int r = C.lut_r, n = r * 2, ext = b->ob == OB_SATURN ? (int)ceilf(2.33f * r) + 2 : r;
    int ey = b->ob == OB_SATURN ? ring_extent(r, lat) : r;
    const float *axis = card_coords_axis(C.coords, 0, r);
    s->w = ext * 2;
    s->h = ey * 2;
    if (!card_window.active) memset(s->px, 0, (size_t)s->w * s->h * 2);
    int step=card_window.active?card_window.detail:1;
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD), dl = (b->slon - b->olon - spin) * RAD,
          ss = fk_sin(b->slat * RAD), cs = fk_cos(b->slat * RAD);
    float bx = cs * fk_cos(dl), by = cs * fk_sin(dl), sx = by, sy = -bx * sl + ss * cl,
          sz = bx * cl + ss * sl;
    int offset = (int)((spin + b->olon + 180) * 1024 / 360);
    if (!card_window.active || card_window.pass==CW_SHADER)
    for (int y=card_window.active?card_window.first:0; y<n && (!card_window.active || y<card_window.end); y+=step)
        for (int x=0; x<n; x+=step) {
            const globe_lut *q = &C.lut[y * n + x];
            if (q->z == 65535) continue;
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, nz = q->z / 32767.f,
                  real = nx * sx + ny * sy + nz * sz,
                  l = C.free ? fmaxf(.35f, -.25f * nx + .3f * ny + .92f * nz) : real;
            int u = (offset + (int)q->lon * 512 / 32767) & 1023;
            unsigned idx = q->v * 1024u + (unsigned)u;
            uint16_t color = textured && C.tex_ok ? texture_pixel(C.texture, (unsigned)idx) : ob_tint[b->ob];
            if (C.mode == 1) {
                float t = temperature(b, q->lat * .01f, q->lon * (180.f / 32767) + spin + b->olon, real);
                color = hc((t - ranges[b->ob][0]) / (ranges[b->ob][1] - ranges[b->ob][0]));
            } else {
                int level = b->ob == OB_SUN ? 256 + (int)clamp(nz * 255, 0, 255)
                                           : (int)clamp(l * 255, 0, 255);
                uint16_t night = 0;
                float nk = 0;
                if (textured && C.night_ok && b->ob == OB_EARTH && !C.free && l < .12f) {
                    night = texture_pixel(C.night, (unsigned)idx);
                    nk = clamp((.12f - l) / .15f, 0, 1);
                }
                uint32_t m = multipliers[level];
                color = !night && m<=65536u ? shade_q16(color,m) : shade(color,C.lighting_coeff->k[level],night,nk);
            }
            int dx=x+ext-r,dy=y+ey-r;
            for(int yy=dy;yy<dy+step && yy<s->h;yy++)
                for(int xx=dx;xx<dx+step && xx<s->w;xx++)s->px[yy*s->w+xx]=color;
        }
    if ((!card_window.active || card_window.pass==CW_OVERLAY) && b->ob == OB_SATURN && fabsf(sl) > .001f) {
        int band = (int)ceilf(2.33f * r * fabsf(sl));
        int first=card_window.active?card_window.first:ey-band;
        if(first<ey-band)first=ey-band;
        /* Preserve the original ring sample lattice across arbitrary row quanta. */
        if(step>1)first-=((first-(ey-band))%step);
        for (int y=first; y<ey+band && (!card_window.active || y<card_window.end); y+=step)
            for (int x=0; x<s->w; x+=step) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl, qy = px;
                int j = y * s->w + x;
                int ri = C.rings[j] - 1;
                if (ri < 0) continue;
                float dot = qx * bx + qy * by, perp = qx * qx + qy * qy - dot * dot;
                float k = C.free ? .95f : (dot < 0 && perp < 1) ? .12f : ss * sl > 0 ? .95f : .4f;
                uint16_t col = ob_tint[OB_SATURN];
                float alpha = .7f;
                if (textured && C.ring_ok) {
                    const uint8_t *rt = C.ring_tex + ri * 4;
                    col = GFX_RGB(rt[0], rt[1], rt[2]);
                    alpha = rt[3] / 255.f;
                }
                uint16_t rc=shade(col,k,0,0);uint8_t ra=(uint8_t)(alpha*255);
                for(int yy=y;yy<y+step && yy<s->h && (!card_window.active || yy<card_window.end);yy++)
                    if(!card_window.active || yy>=card_window.first)
                    for(int xx=x;xx<x+step && xx<s->w;xx++) {
                        int at=yy*s->w+xx;s->px[at]=gfx_blend565(s->px[at],rc,ra);
                    }
            }
        C.ring_lat_valid = 1;
        GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * (card_window.active?card_window.end-card_window.first:band*2)) * 2u);
    }
    if (!card_window.active && C.mode == 2) halftone(s);
    /* Proxy estimates for tick math omitted by the stock raster model. These
     * are explicit estimates, NOT measured Xtensa timings. */
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)(card_window.active ? (card_window.end-card_window.first)*n/(step*step) : r*r*PI) * (C.mode == 1 ? 3u : 1u));

    if (!card_window.active && C.mode == 2) GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * s->h) * 3u);
}

static void render_globe_fast_cached_q16(sprite *s, const body_state *b, float spin, float lat, int textured, const uint32_t *multipliers)
{
    int r = C.lut_r, n = r * 2, ext = b->ob == OB_SATURN ? (int)ceilf(2.33f * r) + 2 : r;
    int ey = b->ob == OB_SATURN ? ring_extent(r, lat) : r;
    const float *axis = card_coords_axis(C.coords, 0, r);
    s->w = ext * 2;
    s->h = ey * 2;
    if (!card_window.active) memset(s->px, 0, (size_t)s->w * s->h * 2);
    int step=card_window.active?card_window.detail:1;
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD), dl = (b->slon - b->olon - spin) * RAD,
          ss = fk_sin(b->slat * RAD), cs = fk_cos(b->slat * RAD);
    float bx = cs * fk_cos(dl), by = cs * fk_sin(dl), sx = by, sy = -bx * sl + ss * cl,
          sz = bx * cl + ss * sl;
    int offset = (int)((spin + b->olon + 180) * 1024 / 360);
    if (!card_window.active || card_window.pass==CW_SHADER)
    for (int y=card_window.active?card_window.first:0; y<n && (!card_window.active || y<card_window.end); y+=step)
        for (int x=0; x<n; x+=step) {
            const globe_lut *q = &C.lut[y * n + x];
            if (q->z == 65535) continue;
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, nz = card_qz_exact(q->z),
                  real = nx * sx + ny * sy + nz * sz,
                  l = C.free ? fmaxf(.35f, -.25f * nx + .3f * ny + .92f * nz) : real;
            int u = (offset + (int)q->lon * 512 / 32767) & 1023;
            unsigned idx = q->v * 1024u + (unsigned)u;
            uint16_t color = textured && C.tex_ok ? texture_pixel(C.texture, (unsigned)idx) : ob_tint[b->ob];
            if (C.mode == 1) {
                float t = temperature(b, q->lat * .01f, q->lon * (180.f / 32767) + spin + b->olon, real);
                color = hc((t - ranges[b->ob][0]) / (ranges[b->ob][1] - ranges[b->ob][0]));
            } else {
                int level = b->ob == OB_SUN ? 256 + (int)clamp(nz * 255, 0, 255)
                                           : (int)clamp(l * 255, 0, 255);
                uint16_t night = 0;
                float nk = 0;
                if (textured && C.night_ok && b->ob == OB_EARTH && !C.free && l < .12f) {
                    night = texture_pixel(C.night, (unsigned)idx);
                    nk = clamp((.12f - l) / .15f, 0, 1);
                }
                uint32_t m = multipliers[level];
                color = !night && m<=65536u ? shade_q16(color,m) : shade(color,C.lighting_coeff->k[level],night,nk);
            }
            int dx=x+ext-r,dy=y+ey-r;
            for(int yy=dy;yy<dy+step && yy<s->h;yy++)
                for(int xx=dx;xx<dx+step && xx<s->w;xx++)s->px[yy*s->w+xx]=color;
        }
    if ((!card_window.active || card_window.pass==CW_OVERLAY) && b->ob == OB_SATURN && fabsf(sl) > .001f) {
        int band = (int)ceilf(2.33f * r * fabsf(sl));
        int first=card_window.active?card_window.first:ey-band;
        if(first<ey-band)first=ey-band;
        /* Preserve the original ring sample lattice across arbitrary row quanta. */
        if(step>1)first-=((first-(ey-band))%step);
        for (int y=first; y<ey+band && (!card_window.active || y<card_window.end); y+=step)
            for (int x=0; x<s->w; x+=step) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl, qy = px;
                int j = y * s->w + x;
                int ri = C.rings[j] - 1;
                if (ri < 0) continue;
                float dot = qx * bx + qy * by, perp = qx * qx + qy * qy - dot * dot;
                float k = C.free ? .95f : (dot < 0 && perp < 1) ? .12f : ss * sl > 0 ? .95f : .4f;
                uint16_t col = ob_tint[OB_SATURN];
                float alpha = .7f;
                if (textured && C.ring_ok) {
                    const uint8_t *rt = C.ring_tex + ri * 4;
                    col = GFX_RGB(rt[0], rt[1], rt[2]);
                    alpha = rt[3] / 255.f;
                }
                uint16_t rc=shade(col,k,0,0);uint8_t ra=(uint8_t)(alpha*255);
                for(int yy=y;yy<y+step && yy<s->h && (!card_window.active || yy<card_window.end);yy++)
                    if(!card_window.active || yy>=card_window.first)
                    for(int xx=x;xx<x+step && xx<s->w;xx++) {
                        int at=yy*s->w+xx;s->px[at]=gfx_blend565(s->px[at],rc,ra);
                    }
            }
        C.ring_lat_valid = 1;
        GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * (card_window.active?card_window.end-card_window.first:band*2)) * 2u);
    }
    if (!card_window.active && C.mode == 2) halftone(s);
    /* Proxy estimates for tick math omitted by the stock raster model. These
     * are explicit estimates, NOT measured Xtensa timings. */
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)(card_window.active ? (card_window.end-card_window.first)*n/(step*step) : r*r*PI) * (C.mode == 1 ? 3u : 1u));

    if (!card_window.active && C.mode == 2) GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * s->h) * 3u);
}

#define CARD_SUN_NAME render_sun_legacy_0_0
#define CARD_SUN_QZ card_qz_legacy
#define CARD_SUN_CACHED 0
#define CARD_SUN_Q16 0
#include "orrery_card_sun.inc"
#define CARD_SUN_NAME render_sun_exact_0_0
#define CARD_SUN_QZ card_qz_exact
#define CARD_SUN_CACHED 0
#define CARD_SUN_Q16 0
#include "orrery_card_sun.inc"
#define CARD_SUN_NAME render_sun_legacy_1_0
#define CARD_SUN_QZ card_qz_legacy
#define CARD_SUN_CACHED 1
#define CARD_SUN_Q16 0
#include "orrery_card_sun.inc"
#define CARD_SUN_NAME render_sun_exact_1_0
#define CARD_SUN_QZ card_qz_exact
#define CARD_SUN_CACHED 1
#define CARD_SUN_Q16 0
#include "orrery_card_sun.inc"
#define CARD_SUN_NAME render_sun_legacy_1_1
#define CARD_SUN_QZ card_qz_legacy
#define CARD_SUN_CACHED 1
#define CARD_SUN_Q16 1
#include "orrery_card_sun.inc"
#define CARD_SUN_NAME render_sun_exact_1_1
#define CARD_SUN_QZ card_qz_exact
#define CARD_SUN_CACHED 1
#define CARD_SUN_Q16 1
#include "orrery_card_sun.inc"

int orrery_card_qz_exact = 1;
int orrery_card_lighting_cache = 1;
int orrery_card_shade_q16 = 1;
int orrery_card_sun_fast = 1;
unsigned orrery_card_shade_q16_check(void)
{
    if(!C.lighting_coeff)return ~0u;
    const uint32_t *multipliers=orrery_shade_q16_get(C.lighting_coeff->k);
    if(!multipliers)return ~0u;
    unsigned mismatches=0;
    for(unsigned i=0;i<512;i++) {
        uint32_t m=multipliers[i];
        if(m>65536u) {mismatches++;continue;}
        float k=C.lighting_coeff->k[i];
        for(unsigned v=0;v<64;v++) {
            uint16_t c=(uint16_t)(v<<5|31); /* unequal R/B forces original truncation */
            mismatches+=shade(c,k,0,0)!=shade_q16(c,m);
        }
        for(unsigned v=0;v<32;v++) {
            uint16_t c=(uint16_t)(v<<11|((v+1)&31)); /* every red and blue value */
            mismatches+=shade(c,k,0,0)!=shade_q16(c,m);
            c=(uint16_t)(v<<11|(v*2)<<5|v); /* every original rounded-gray value */
            mismatches+=shade(c,k,0,0)!=shade_q16(c,m);
        }
    }
    return mismatches;
}
unsigned orrery_card_lighting_check(void)
{
    if(!C.lighting || !C.lighting_coeff)return ~0u;
    unsigned bad=card_lighting_mismatches(C.lighting_coeff,C.lighting);
    /* Diagnostic only: independently check any borrowed pre-bake against the
     * original target expressions. Never run these powf calls in a UI tick. */
    for(int i=0;i<256;i++) {
        bad+=C.lighting[i]!=(uint8_t)(255*powf(i/255.f,.85f)+.5f);
        bad+=C.lighting[256+i]!=(uint8_t)(255*powf(i/255.f,.6f)+.5f);
    }
    return bad;
}
unsigned orrery_card_qz_check(void)
{
    unsigned mismatches = 0;
    for (unsigned n = 0; n < 65536; n++) {
        union { float f; uint32_t u; } a = { .f = card_qz_legacy((uint16_t)n) },
                                      b = { .f = card_qz_exact((uint16_t)n) };
        if (a.u != b.u) mismatches++;
    }
    return mismatches;
}
static void render_globe_profile_impl(sprite *s,const body_state *b,float spin,float lat,int textured)
{
    /* One dispatch per globe. No mode test or function pointer in the pixel loop. */
    if(orrery_card_lighting_cache && C.lighting_coeff) {
        if(orrery_card_shade_q16 && !C.shade_q16_checked && orrery_shade_q16_ready()) {
            C.shade_q16=orrery_shade_q16_get(C.lighting_coeff->k);
            C.shade_q16_checked=1; /* immutable source and global table: compare only once per card */
        }
        if(orrery_card_sun_fast && b->ob==OB_SUN && C.mode==0) {
            if(orrery_card_shade_q16 && C.shade_q16) {
                if(orrery_card_qz_exact)render_sun_exact_1_1(s,b,spin,textured,C.shade_q16);
                else render_sun_legacy_1_1(s,b,spin,textured,C.shade_q16);
            } else {
                if(orrery_card_qz_exact)render_sun_exact_1_0(s,b,spin,textured,NULL);
                else render_sun_legacy_1_0(s,b,spin,textured,NULL);
            }
            return;
        }
        if(orrery_card_shade_q16 && C.shade_q16) {
            if(orrery_card_qz_exact)render_globe_fast_cached_q16(s,b,spin,lat,textured,C.shade_q16);
            else render_globe_legacy_cached_q16(s,b,spin,lat,textured,C.shade_q16);
        } else {
            if(orrery_card_qz_exact)render_globe_fast_cached(s,b,spin,lat,textured);
            else render_globe_legacy_cached(s,b,spin,lat,textured);
        }
    } else {
        if(orrery_card_sun_fast && b->ob==OB_SUN && C.mode==0) {
            if(orrery_card_qz_exact)render_sun_exact_0_0(s,b,spin,textured,NULL);
            else render_sun_legacy_0_0(s,b,spin,textured,NULL);
            return;
        }
        if(orrery_card_qz_exact)render_globe_fast(s,b,spin,lat,textured);
        else render_globe_legacy(s,b,spin,lat,textured);
    }
}
static void render_globe(sprite *s,const body_state *b,float spin,float lat,int textured)
{
    if (!card_profile_enabled) { render_globe_profile_impl(s, b, spin, lat, textured); return; }
    card_profile_token token = card_profile_begin();
    render_globe_profile_impl(s, b, spin, lat, textured);
    card_profile_end(CP_GLOBE, token);
}


static void point_at(int i, const void *ctx, float *x, float *y, float *d, uint16_t *c)
{
    const point *p = (const point *)ctx + i;
    *x = p->x;
    *y = p->y;
    *d = p->d;
    *c = p->c;
}
/* All plot coordinates, including logarithms, were prepared in tick. */
static void page_heat(gfx_fb *fb, const body_state *b)
{
    const heat_profile *p = &profiles[b->ob];
    ui_title(fb, "HEAT", 0);
    small(fb, 233, 94,
          b->ob == OB_MERCURY ? "EQUATOR / SOLAR DAY 176 D"
          : b->ob == OB_MOON  ? "EQUATOR / LUNAR DAY 29.5 D"
                              : p->title,
          C_SEC, UI_CENTER);
    gfx_fill_rect(fb, 100, 124, 1, 191, C_LINE2);
    gfx_fill_rect(fb, 100, 314, 281, 1, C_LINE2);
    fk_dots(fb, b->graph_n, 16, point_at, b->graph);
    for (int i = 0; i < p->nx; i++)
        small(fb, (int)(100 + 280 * (p->xt[i].v - p->x0) / (p->x1 - p->x0)), 338,
              b->ob == OB_SUN && i == 2 ? "SURF" : p->xt[i].s, C_SEC, UI_CENTER);
    /* y ticks stored alongside graph to keep draw free of libm. */
    for (int i = 0; i < p->ny; i++)
        small(fb, 92, (int)b->graph[49 + i].y + 4, p->yt[i].s, C_SEC, UI_RIGHT);
    if (p->mark_text) {
        float xy = b->mark;
        for (int i = 0; i < 32; i++) {
            int xx = p->vertical ? 100 + i * 9 : (int)xy, yy = p->vertical ? (int)xy : 124 + i * 6;
            if (xx <= 380 && yy <= 314) gfx_fill_rect(fb, xx, yy, 2, 2, C_LINE2);
        }
        if (b->ob == OB_VENUS) small(fb, 377, (int)xy - 31, "EARTH-LIKE", C_SEC, UI_RIGHT);
        small(fb, p->vertical ? 377 : (int)xy, p->vertical ? (int)xy - 11 : 112, p->mark_text, C_SEC,
              p->vertical ? UI_RIGHT : UI_CENTER);
    }
    hero(fb, 357, b->range, 4.0f);
    small(fb, 233, 401, b->ob == OB_SUN ? "K / LOW - HIGH" : "DEG C / LOW - HIGH", C_SEC, UI_CENTER);
    /* Two short lines fit the narrowest lower chord, including font descent. */
    card_wrap(fb, UI_F_SMALL, 233, 418, 230, 17, 2, b->ob == OB_EARTH ? "CLIMATE MEAN / MODEL" : p->note,
                 C_SEC, UI_CENTER);
}
#include "orrery_card_progressive.inc"
#include "orrery_card_geometry_check.inc"
#include "orrery_card_proxy.inc"

static void page_globe(gfx_fb *fb, const body_state *b, const sprite *s, unsigned key, int preview)
{
    if (s->px && key) {
        if(!preview && orrery_card_progressive && orrery_card_motion_proxy && V.active && R.front_detail)card_proxy_blit(fb,s,233-s->w/2,214-s->h/2);
        else sprite_blit(fb,s,233-s->w/2,214-s->h/2,key);
    }
    else
        gfx_fill_circle(fb, 233, 214, (float)radius(b->ob), ui_dim(ob_tint[b->ob], .4f));
    ui_title(fb, ob_name[b->ob], 0);
    small(fb, 233, 92, C.mode == 0 ? (C.load_failed & 1u ? "[ PHOTO UNAVAILABLE ]" : "[ PHOTO ]") : C.mode == 1 ? "[ HEAT ]" : "[ DOT ]", C_SEC, UI_CENTER);
    if (!preview && C.load_stage < 3 && !(C.load_failed & 1u)) small(fb, 233, 214, "[ LOADING ]", C_PRI, UI_CENTER);
    hero(fb, 346, b->hero, 4.0f);
    small(fb, 233, 387, b->unit, C_SEC, UI_CENTER);
    small(fb, 233, 407, b->sub, C_SEC, UI_CENTER);
    gfx_fill_pill(fb, 169, 430, 297, 18, C_SURF2);
    float x = C.free ? 275 : 191;
    gfx_fill_pill(fb, x - 25, 430, x + 25, 14, C.pressed ? C_SEC : C_PRI);
    small(fb, 191, 435, "NOW", C.free ? C_SEC : C_BG, UI_CENTER);
    small(fb, 275, 435, "FREE", C.free ? C_BG : C_SEC, UI_CENTER);
}
static void page_moons(gfx_fb *fb, const body_state *b)
{
    ui_title(fb, b->ob == OB_EARTH ? "MOON & CRAFT" : "MOONS & CRAFT", 0);
    small(fb, 233, 94, b->count, C_SEC, UI_CENTER);
    for (int i = 0; i < b->n; i++)
        if (!b->items[i].craft) gfx_ring(fb, 233, 236, b->items[i].r, 1, C_LINE);
    fk_dots(fb, b->minor_n, 32, point_at, b->minor);
    if (b->ob == OB_SATURN) gfx_ring(fb, 233, 236, b->planet_r * 1.4f, 3, C_LINE2);
    gfx_fill_circle(fb, 233, 236, b->planet_r, ob_tint[b->ob]);
    for (int i = 0; i < b->direction_n; i++) {
        const moon_item *d = &b->directions[i];
        dot_disc(fb, d->x, d->y, 5, C_SEC);
        small(fb, (int)d->lx, (int)d->ly, d->name, C_SEC, UI_CENTER);
    }
    for (int i = 0; i < b->n; i++) {
        const moon_item *m = &b->items[i];
        if (m->craft) {
            gfx_line(fb, m->x - 3, m->y - 3, m->x + 3, m->y + 3, 1, C_SEC);
            gfx_line(fb, m->x - 3, m->y + 3, m->x + 3, m->y - 3, 1, C_SEC);
        } else
            dot_disc(fb, m->x, m->y, 6, C_PRI);
        small(fb, (int)m->lx, (int)m->ly, m->name, C_SEC, UI_CENTER);
    }
    if (!b->n) {
        ui_label(fb, 233, 332, b->moons_state == 2 ? "MOON DATA MISSING" : b->moons_state ? "LOADING" : "NO MOONS", C_SEC, UI_CENTER);
        if (b->moons_state == 2) small(fb, 233, 361, "SD CARD / SKY/MOONS.BIN", C_DIS, UI_CENTER);
        if (b->ob == OB_MERCURY) {
            card_wrap(fb, UI_F_SMALL, 233, 361, 300, 18, 2, b->bepi, C_SEC, UI_CENTER);
            small(fb, 233, 410, "CRAFT POSITION / SKY PACK", C_DIS, UI_CENTER);
        } else if (b->ob == OB_VENUS) {
            small(fb, 233, 360, "NO ORBITER SINCE AKATSUKI", C_SEC, UI_CENTER);
            small(fb, 233, 381, "ENDED / SEP 2025", C_SEC, UI_CENTER);
        }
    }
}
static void fact_row(gfx_fb *fb, int y, const char *k, const char *v) {
    if (!card_rows(fb, y - UI_F_SMALL->ascent - 2, y + 34))
        return;
    int x0, x1;
    ui_row_span((float)y, 34, &x0, &x1);
    x0 += 12;
    x1 -= 12;
    small(fb, x0, y, k, C_SEC, UI_LEFT);
    /* Long facts take their own second line rather than colliding at the edge. */
    small(fb, x1, y + 20, v, C_PRI, UI_RIGHT);
    gfx_fill_rect(fb, x0, y + 27, x1 - x0, 1, C_LINE);
}
static void page_facts(gfx_fb *fb, const body_state *b) {
    ui_title(fb, "FACTS", 0);
    int row = 0;
    char v[40];
    if (b->ob != OB_SUN && b->ob != OB_EARTH && b->ob != OB_MOON) {
        if (card_rows(fb, 105 - UI_F_SMALL->ascent - 2, 139)) {
            snprintf(v, sizeof v, "%.2f AU", b->sun_dist);
            fact_row(fb, 105, "FROM SUN NOW", v);
        }
        row++;
    }
    for (int i = 0; i < 5; i++)
        if (facts[b->ob][i][0])
            fact_row(fb, 105 + row++ * 48, facts[b->ob][i][0], facts[b->ob][i][1]);
    if (b->next[0]) {
        small(fb, 233, 416, "NEXT", C_SEC, UI_CENTER);
        card_wrap(fb, UI_F_SMALL, 233, 434, 250, 16, 1, b->next, C_PRI, UI_CENTER);
    }
}
static void draw_body(gfx_fb *fb, const body_state *b, const sprite *s, unsigned key, float xpos,
                      int preview) {
    if (!b || !b->ready)
        return;
    float pos = fminf(C.pos, (float)(pages(b->ob) - 1));
    for (int p = 0; p < pages(b->ob); p++) {
        int y = (int)lroundf((p - pos) * 466);
        if (y <= -466 || y >= 466) continue;
        gfx_fb sub;
        if (!nos_subview(fb, &sub, 0, 0, 466, 466, (int)lroundf(xpos), y)) continue;
        if (p == 0)
            page_globe(&sub, b, s, key, preview);
        else if (p == 1)
            page_heat(&sub, b);
        else if (p == pages(b->ob) - 1)
            page_facts(&sub, b);
        else
            page_moons(&sub, b);
    }
}
static void draw(gfx_fb *fb)
{
    if (C.failed) {
        ui_title(fb, "ORRERY", 0);
        ui_state(fb, 233, 233, "MEMORY UNAVAILABLE", C_SEC);
        return;
    }
    if(!C.prepared){ui_title(fb,ob_name[C.ob],0);return;}
    if (C.peek && C.preview) {
        draw_body(fb, C.b, &C.globe, C.key, C.peek, 0);
        draw_body(fb, C.nb, &C.neighbour, C.preview_key, C.peek + (C.peek < 0 ? 466 : -466), 1);
    } else
        draw_body(fb, C.b, &C.globe, C.key, 0, 0);
    if (nos_t < C.info_until) {
        ui_rrect(fb, 73, 48, 320, 90, 36, C_PRI);
        ui_text_wrap(fb, UI_F_SMALL, 233, 76, 276, 18, 3, C.info, C_BG, UI_CENTER);
    }
    for (int i = 0; i < pages(C.ob); i++)
        dot_disc(fb, 446, 233 - (pages(C.ob) - 1) * 7 + i * 14, fabsf(i - C.pos) < .5f ? 6 : 5,
                 fabsf(i - C.pos) < .5f ? C_PRI : C_LINE2);
}
static void prepare_extra_profile_impl(body_state *b)
{
    const heat_profile *p = &profiles[b->ob];
    for (int i = 0; i < p->ny; i++)
        b->graph[49 + i].y = graph_y(p, p->yt[i].v);
    /* Separate mark cache, not the moon's centre radius. */
    if (p->mark_text)
        b->mark = p->vertical ? graph_y(p, p->mark) : 100 + 280 * (p->mark - p->x0) / (p->x1 - p->x0);
}
static void prepare_extra(body_state *b)
{
    if (!card_profile_enabled) { prepare_extra_profile_impl(b); return; }
    card_profile_token token = card_profile_begin();
    prepare_extra_profile_impl(b);
    card_profile_end(CP_EXTRA, token);
}

#include "orrery_card_prepare.inc"
enum { CO_NONE,CO_INITIAL,CO_PREVIEW,CO_REFRESH };
static int card_prepare_owner,card_refresh_pending;
static double card_refresh_jd;
static void card_prepare_owned_cancel(void) {
    card_prepare_job_cancel();card_prepare_owner=CO_NONE;
}
static void card_prepare_owned_report(void) {
    printf("CARDPREP owner=%d phase=%d cursor=%d pending=%d bytes=%u step_max_us=%u step_phase=%d step_cursor=%d steps=%u\n",card_prepare_owner,
        card_prepare_job_phase(),card_prepare_job_cursor(),card_refresh_pending,(unsigned)sizeof P,
        card_prepare_step_stat.max_us,card_prepare_step_stat.phase,card_prepare_step_stat.cursor,card_prepare_step_stat.calls);
}
static void card_prepare_owned_start(int owner,body_state*out,int ob,double jd) {
    card_render_cancel();card_prepare_owned_cancel();card_prepare_owner=owner;
    card_prepare_job_start(out,ob,jd);
}
static void card_prepare_owned_pump(void) {
    while(card_prepare_job_active() && !card_work_budget_hit()) {
        /* Candidate checks are tiny; don't let the deterministic quantum cap
         * turn17400 labels into seconds. Check time at EACH candidate. */
        int batch=card_prepare_job_cheap()?16:1;
        card_profile_token token=card_profile_begin();
        for(int i=0;i<batch;i++) {
            if(i && !card_prepare_job_cheap())break;
            unsigned before=0;int phase=0,cursor=0;
            if(card_profile_enabled){before=hal_micros();phase=card_prepare_job_phase();cursor=card_prepare_job_cursor();}
            int done=card_prepare_job_step();
            if(card_profile_enabled) {
                unsigned elapsed=hal_micros()-before;card_prepare_step_stat.calls++;
                if(elapsed>card_prepare_step_stat.max_us){card_prepare_step_stat.max_us=elapsed;card_prepare_step_stat.phase=phase;card_prepare_step_stat.cursor=cursor;}
            }
            if(done) {
                int owner=card_prepare_owner;card_prepare_owner=CO_NONE;
                if(owner==CO_INITIAL)C.prepared=1;
                else if(owner==CO_PREVIEW){C.pending=0;card_preview_start();}
                else if(owner==CO_REFRESH) {
                    body_state*old=C.b;C.b=C.nb;C.nb=old;
                    C.dirty=1;card_refresh_pending=0;
                }
                break;
            }
#ifdef ESP_PLATFORM
            if(card_work_budget_hit())break;
#endif
            if(!card_prepare_job_cheap())break;
        }
        card_profile_end(CP_PREPARE,token);card_frame_work_quanta++;
    }
}

static void release_buffers(void)
{
    card_prepare_owned_cancel();card_refresh_pending=0;od_moons_release();
    card_render_invalidate();
    close_load();
    hal_big_free(C.b); hal_big_free(C.nb); texture_free(C.texture); texture_free(C.night);
    hal_big_free(C.coords); C.coords = 0;
    hal_big_free(C.lighting_coeff); C.lighting_coeff = 0;
    C.shade_q16 = NULL; C.shade_q16_checked = 0;
    hal_big_free(C.lut); hal_big_free(C.rings); hal_big_free(C.ring_tex); hal_big_free(C.lighting);
    hal_big_free(C.globe.px); hal_big_free(C.neighbour.px);
    C.b = C.nb = 0; C.lut = 0; C.rings = 0;
    C.ring_tex = C.lighting = 0; C.globe.px = C.neighbour.px = 0;
    C.prepared = C.tex_ok = C.night_ok = C.ring_ok = C.ring_lat_valid = 0;
    C.lut_r = C.lut_row = C.lut_publish = 0;
}
void orrery_card_idle(void)
{
    orrery_card_asset_poll();
    /* leave also runs before replace/cover animations: wait until no outgoing frame
     * can reference sprites. Covering keeps the card alive for reveal. */
    if (release_pending && !nos_transitioning() && nos_top() != &scr_orrery_card) {
        release_buffers(); release_pending = 0;
    }
}
static int allocate_profile_impl(void)
{
    if (C.b) return !C.failed;
    nos_mem_card_prepare();
    /* Place the largest blocks in the released arenas before small texture pages. */
    C.lut = hal_big_alloc(236u * 236u * sizeof *C.lut);
    C.rings = hal_big_alloc(416u * 236u * 2u);
    int a = sprite_alloc(&C.globe, MAX_SIDE, 236), b = sprite_alloc(&C.neighbour, MAX_SIDE, 236);
    C.b = hal_big_alloc(sizeof *C.b);
    C.nb = hal_big_alloc(sizeof *C.nb);
    int texture_ok = hal_asset_busy() ? 1 : texture_alloc(C.texture);
    C.ring_tex = hal_big_alloc(2048);
    C.lighting = hal_big_alloc(512);
    card_profile_token lighting_token = card_profile_begin();
    if (C.lighting) {
        const uint8_t *prebaked=orrery_shade_lighting_get();
        if(prebaked)memcpy(C.lighting,prebaked,512);
        else for (int i = 0; i < 256; i++) {
            C.lighting[i] = (uint8_t)(255 * powf(i / 255.f, .85f) + .5f);
            C.lighting[256 + i] = (uint8_t)(255 * powf(i / 255.f, .6f) + .5f);
        }
    }
    card_profile_end(CP_LIGHT, lighting_token);
    C.failed = !C.b || !C.nb || !texture_ok || !C.lut || !C.rings || !C.ring_tex || !C.lighting || !a || !b;
    if (C.failed) release_buffers(); /* transactional: a later open can retry */
    else {
        C.coords = card_coords_new(); /* optional: scalar fallback stays exact */
        card_profile_token coefficient_token = card_profile_begin();
        C.lighting_coeff = card_lighting_new(C.lighting); /* optional, original expressions on denial */
        card_profile_end(CP_COEFFICIENTS, coefficient_token);
    }
    return !C.failed;
}
static int allocate(void)
{
    if (!card_profile_enabled) { return allocate_profile_impl(); }
    card_profile_token token = card_profile_begin();
    int result = allocate_profile_impl();
    card_profile_end(CP_ALLOC, token);
    return result;
}

static void body_reset(int ob)
{
    card_prepare_owned_cancel();card_refresh_pending=0;
    card_render_invalidate();V.active=0;V.since=0;
    C.ob = (ob % OB_N + OB_N) % OB_N;
    C.spin = C.tilt = C.vel = C.hold = 0;
    C.page = (int)clamp((float)C.page, 0, (float)(pages(C.ob) - 1));
    C.pos = (float)C.page;
    C.prepared = 0;
    if(C.b)C.b->ready=0;
    C.dirty = 1;
    C.key = 0;
    C.lut_r = 0;
    C.page_n = 0;
    load_begin(C.ob);
}
static void enter(void)
{
    release_pending = 0;
    if (!allocate()) return;
    if (nos_revealed()) return;
    C.open_jd = orrery_jd();
    C.open_epoch = hal_epoch_seconds();
    C.page = 0;
    C.pos = 0;
    C.mode = 0;
    C.free = 0;
    C.info_until = 0;
    C.dial_queue = 0;
    C.peek = 0;
    C.drag = 0;
    C.preview = 0;
    C.anim_n = 0;
    C.page_n = 0;
    body_reset(C.ob);
}
static void leave(void)
{
    if (!nos_leaving_covered()) { card_prepare_owned_cancel(); card_render_cancel(); close_load(); release_pending = 1; }
}
static void render_preview_profile_impl(void)
{
    body_state *b = C.nb;
    sprite *s = &C.neighbour;
    int r = radius(b->ob), n = 2 * r;
    const float *axis = card_coords_axis(C.coords, 1, r);
    s->w = s->h = n;
    float lat = b->olat * RAD, dl = (b->slon - b->olon) * RAD;
    float cl = fk_cos(lat), sl = fk_sin(lat), ss = fk_sin(b->slat * RAD), cs = fk_cos(b->slat * RAD);
    float bx = cs * fk_cos(dl), sx = cs * fk_sin(dl), sy = -bx * sl + ss * cl, sz = bx * cl + ss * sl;
    for (int y=card_window.active?card_window.first:0; y<n && (!card_window.active || y<card_window.end); y++)
        for (int x = 0; x < n; x++) {
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, rr = nx * nx + ny * ny;
            uint16_t col = 0;
            if (rr <= 1) {
                float z = root(1 - rr),
                      l = C.free ? fmaxf(.35f, -.25f * nx + .3f * ny + .92f * z) : nx * sx + ny * sy + z * sz;
                col = shade(ob_tint[b->ob], b->ob == OB_SUN ? .55f + .45f * z : .05f + .95f * fmaxf(0, l), 0,
                            0);
            }
            s->px[y * n + x] = col;
        }
    if(!card_window.active)C.preview_key=card_image_key();
    GFX_COST_PX(GFX_COST_BLIT, (unsigned)n * (card_window.active?card_window.end-card_window.first:n) * 2u);
}
static void render_preview(void)
{
    if (!card_profile_enabled) { render_preview_profile_impl(); return; }
    card_profile_token token = card_profile_begin();
    render_preview_profile_impl();
    card_profile_end(CP_PREVIEW, token);
}

static void card_preview_rows(int first,int end) {
    card_window.active=1;card_window.first=first;card_window.end=end;
    render_preview_profile_impl();card_window.active=0;
}
static void start_preview(int dir)
{
    int ob = (C.ob + dir + OB_N) % OB_N;
    if (C.preview && (C.pending == ob + 1 || (!C.pending && C.nb->ob == ob))) return;
    card_render_cancel();card_prepare_owned_cancel();
    C.pending = ob + 1;
    C.preview = 1;
    C.preview_key = 0;
    /* A tinted sphere is immediately available; texture memory belongs to the
     * current body until the swipe commits. */
}
static void start_slide(int dir, float from)
{
    start_preview(dir);
    C.anim_from = from;
    C.anim_to = -dir * 466.f;
    C.anim_t = 0;
    C.anim_n = fmaxf(6, ceilf(fabsf(C.anim_to - from) * 15 / 466));
    C.commit = 1;
    nos_feedback(FB_TICK);
}
static void card_tick_impl(float dt)
{
    if (C.failed || !C.b) return;
    load_tick();
    if(orrery_card_progressive) {
        if(!C.prepared) {
            if(card_prepare_owner!=CO_INITIAL)card_prepare_owned_start(CO_INITIAL,C.b,C.ob,card_jd());
            card_prepare_owned_pump();
            if(!C.prepared)return;
        }
        if(C.pending && card_prepare_owner!=CO_PREVIEW) {
            card_prepare_owned_start(CO_PREVIEW,C.nb,C.pending-1,C.b->jd);
        }
        if(card_prepare_owner==CO_PREVIEW)card_prepare_owned_pump();
    } else {
        if(card_prepare_job_active())card_prepare_owned_cancel();
        if(!C.prepared){prepare(C.b,C.ob,card_jd());prepare_extra(C.b);C.prepared=1;}
        if(C.pending){prepare(C.nb,C.pending-1,C.b->jd);prepare_extra(C.nb);C.pending=0;render_preview();}
    }
    if (C.page_n > 0 && C.drag != 2) {
        C.page_t += dt * 30;
        float t = clamp(C.page_t / C.page_n, 0, 1);
        C.pos = C.page_from + (C.page_to - C.page_from) * quad(t);
        if (t >= 1) {
            C.pos = C.page_to;
            C.page_n = 0;
        }
    }
    if (C.anim_n > 0) {
        C.anim_t += dt * 30;
        float t = clamp(C.anim_t / C.anim_n, 0, 1);
        C.peek = C.anim_from + (C.anim_to - C.anim_from) * quad(t);
        if (t >= 1 && C.preview_key) {
            int ob = C.nb->ob, commit = C.commit;
            C.peek = 0;
            C.anim_n = 0;
            C.preview = 0;
            if (commit) {
                body_reset(ob);
                sprite swap = C.globe;
                C.globe = C.neighbour;
                C.neighbour = swap;
                C.key = C.preview_key;
                if(orrery_card_progressive) {
                    /* Incoming preview is complete. Keep its coherent metadata
                     * while preparing the exact commit epoch into the old body. */
                    body_state*old=C.b;C.b=C.nb;C.nb=old;C.prepared=1;
                    card_refresh_pending=1;card_refresh_jd=card_jd();
                } else {prepare(C.b,ob,card_jd());prepare_extra(C.b);C.prepared=1;}

            }
        }
    }
    if (!C.anim_n && C.dial_queue) {
        int dir = C.dial_queue > 0 ? 1 : -1;
        C.dial_queue -= dir;
        start_slide(dir, 0);
    }
    if (C.drag != 1) {
        if (fabsf(C.vel) > .02f) {
            C.spin = norm(C.spin + C.vel * dt * 30);
            C.vel *= powf(.93f, dt * 30);
        } else
            C.vel = 0;
        if (!C.free) {
            if (fabsf(C.vel) < .3f)
                C.hold += dt * 30;
            else
                C.hold = 0;
            if (C.hold > 45) C.spin = norm(C.spin) * powf(.86f, dt * 30);
            C.tilt *= powf(.8f, dt * 30);
            if (fabsf(C.spin) < .05f) C.spin = 0;
            if (fabsf(C.tilt) < .05f) C.tilt = 0;
        }
    }
    if (!C.prepared) return;
    /* Snap the slow clock to minutes; this is a cached view between those steps. */
    double jd = card_jd();
    if ((C.b->moons_state == 1 ? od_moons_ready() || od_moons_failed() : C.b->moons_state && od_moons_ready()) && !card_refresh_pending) { card_refresh_pending=1; card_refresh_jd=jd; }
    int refresh_quiet=!orrery_card_progressive ||
        (R.stage==CR_IDLE && !C.preview && (unsigned)(hal_millis()-R.pose_ms)>=125 &&
         fabsf(norm(C.spin-R.prior_spin))<=.01f && fabsf(C.tilt-R.prior_tilt)<=.01f);
    if (fabs(jd-C.b->jd)>1.0/1440 && !card_refresh_pending) {
        card_refresh_pending=1;card_refresh_jd=jd;
    }
    if(card_refresh_pending && C.drag==0 && C.anim_n==0 && refresh_quiet && !C.pending) {
        if(orrery_card_progressive) {
            if(card_prepare_owner==CO_NONE)card_prepare_owned_start(CO_REFRESH,C.nb,C.ob,card_refresh_jd);
        } else {prepare(C.b,C.ob,card_refresh_jd);prepare_extra(C.b);C.dirty=1;card_refresh_pending=0;}
    }

    if(orrery_card_progressive) {
        unsigned now=hal_millis();
        int changed=fabsf(norm(C.spin-R.prior_spin))>.01f || fabsf(C.tilt-R.prior_tilt)>.01f;
        if(changed)R.pose_ms=now;
        R.prior_spin=C.spin;R.prior_tilt=C.tilt;
        int moving=(unsigned)(now-R.pose_ms)<125;
        if(moving){R.rest_started_ms=0;R.was_moving=1;}
        else if(R.was_moving){R.rest_started_ms=now;R.was_moving=0;R.restore_pending=1;}
        int detail=moving?(orrery_card_motion_step>=4?4:2):1;
        if(card_prepare_owner==CO_REFRESH && moving)card_prepare_owned_cancel();
        if(card_prepare_owner==CO_REFRESH)card_prepare_owned_pump();
        if(R.stage!=CR_IDLE && R.stage!=CR_PREVIEW &&
           (C.mode!=R.mode || C.free!=R.free || (changed && R.detail==1)))card_render_cancel();
        if(C.pos<1 && R.stage==CR_IDLE && !C.preview && card_prepare_owner==CO_NONE &&
           (C.dirty||C.spin!=C.last_spin||C.tilt!=C.last_tilt||C.mode!=C.last_mode||C.free!=C.last_free||R.front_detail!=detail)) {
            float target=roundf(clamp(C.b->olat+C.tilt,-88,88)*.5f)*2;
            card_render_start(target,detail);
        }
        card_render_pump();
        card_proxy_update();
        return;
    }
    V.active=0;
    if(R.stage==CR_PREVIEW)render_preview();
    if(R.stage!=CR_IDLE || R.lut_valid || R.ring_valid)card_render_invalidate();
    if (C.pos < 1 && (C.dirty || C.spin != C.last_spin || C.tilt != C.last_tilt || C.mode != C.last_mode ||
                      C.free != C.last_free)) {
        float target = roundf(clamp(C.b->olat + C.tilt, -88, 88) * .5f) * 2;
        /* Finish an in-flight LUT before taking the latest tilt; no build starvation. */
        float lat = C.lut_r && (C.lut_row < 2 * C.lut_r || C.lut_publish)
                        ? C.lut_lat : target;
        if (build_lut(globe_radius(C.ob, lat), lat)) {
            if (C.ob == OB_SATURN && !C.ring_lat_valid) {
                build_rings(lat);
                return;
            }
            render_globe(&C.globe, C.b, C.spin, lat, 1);
            C.lut_publish = 0;
            C.key=card_image_key();
            C.last_spin = C.spin;
            C.last_tilt = fabsf(lat - target) < .1f ? C.tilt : 999;
            C.last_mode = C.mode;
            C.last_free = C.free;
            C.dirty = 0;
        }
    }
}
static void tick(float dt) {
    card_frame_work_active=1;card_frame_work_quanta=0;card_frame_work_start=hal_micros();
    card_tick_impl(dt);
    card_frame_work_active=0;
}
static void settle_page(int page)
{
    C.page = (int)clamp((float)page, 0, (float)(pages(C.ob) - 1));
    C.page_from = C.pos;
    C.page_to = (float)C.page;
    C.page_t = 0;
    C.page_n = fmaxf(6, ceilf(fabsf(C.page_to - C.pos) * 15));
    nos_feedback(FB_TICK);
}
static int input(const nos_input *in)
{
    if (C.failed || !C.prepared) return 0;
    if(in->type==IN_CANCEL) {
        C.pressed=C.drag=C.commit=C.dial_queue=0;C.vel=C.acc=C.peek=0;
        C.anim_n=C.page_n=0;C.pos=(float)C.page;return 1;
    }
    if (in->type == IN_DOWN) {
        C.pressed = C.page == 0 && abs(in->y - 430) < 25 && abs(in->x - 233) < 94;
        C.spin0 = C.spin;
        C.tilt0 = C.tilt;
        C.start_pos = C.pos;
        C.drag = 0;
        C.vel = 0;
        C.acc = 0;
        C.angle = atan2f(in->y - 233, in->x - 233) / RAD;
        return 1;
    }
    if (in->type == IN_DRAG_START) {
        float x = (float)in->x0 - 233, y = (float)in->y0 - 233, r = root(x * x + y * y),
              gy = (float)in->y0 - 214;
        float radial = r > 1 ? (in->dx * x + in->dy * y) / r : 0,
              tangent = r > 1 ? (in->dx * (-y) + in->dy * x) / r : 0;
        C.pressed = 0;
        C.page_n = 0;
        C.drag = r >= 197 && fabsf(tangent) > fabsf(radial) ? 4
                 : C.page == 0 && C.pos == 0 && r < 177 &&
                         x * x + gy * gy < radius(C.ob) * radius(C.ob) * .62f * .62f
                     ? 1
                 : in->axis == AXIS_X ? 3
                                      : 2;
    }
    if (in->type == IN_DRAG_START || in->type == IN_DRAG) {
        if (C.drag == 1) {
            C.spin = norm(C.spin0 - in->dx / radius(C.ob) / RAD);
            C.tilt = clamp(C.tilt0 + in->dy / radius(C.ob) / RAD, C.free ? -88 : -70, C.free ? 88 : 70);
            C.hold = 0;
        } else if (C.drag == 2)
            C.pos = clamp(C.start_pos - in->dy / 466, -.25f, pages(C.ob) - .75f);
        else if (C.drag == 3) {
            C.anim_n = 0;
            C.peek = clamp(in->dx, -450, 450);
            start_preview(C.peek < 0 ? 1 : -1);
        } else if (C.drag == 4) {
            float a = atan2f(in->y - 233, in->x - 233) / RAD;
            C.acc += norm(a - C.angle);
            C.angle = a;
            while (fabsf(C.acc) >= 34) {
                int dir = C.acc > 0 ? 1 : -1;
                C.acc -= dir * 34;
                C.dial_queue += dir;
            }
        }
        return 1;
    }
    if (in->type == IN_DRAG_END) {
        if (C.drag == 1) {
            C.vel = clamp(-in->vx / 30 / radius(C.ob) / RAD, -18, 18);
            C.hold = 0;
        } else if (C.drag == 2) {
            float dp = C.pos - C.page;
            int step = dp > .12f || in->vy < -600 ? 1 : dp < -.12f || in->vy > 600 ? -1 : 0;
            settle_page(C.page + step);
        } else if (C.drag == 3) {
            int go = fabsf(C.peek) > 90 || (fabsf(in->vx) > 600 && in->vx * C.peek > 0);
            if (go)
                start_slide(C.peek < 0 ? 1 : -1, C.peek);
            else {
                C.anim_from = C.peek;
                C.anim_to = 0;
                C.anim_t = 0;
                C.anim_n = fmaxf(6, ceilf(fabsf(C.peek) * 15 / 466));
                C.commit = 0;
            }
        }
        C.drag = 0;
        return 1;
    }
    if (in->type == IN_UP) {
        C.pressed = 0;
        return 1;
    }
    if (in->type == IN_TAP) {
        if (C.page == 0 && abs(in->x - 233) < 94 && abs(in->y - 430) < 25) {
            C.free = in->x >= 233;
            if (!C.free) C.hold = 46;
            C.dirty = 1;
            nos_feedback(FB_TICK);
            return 1;
        }
        if (C.page == 0 && fk_in(in->x, in->y, 233, 214, (float)radius(C.ob))) {
            C.mode = (C.mode + 1) % 3;
            C.dirty = 1;
            nos_feedback(FB_TICK);
            return 1;
        }
        if (C.page == 2 && pages(C.ob) == 4) {
            int best = -1;
            float dd = 44 * 44;
            for (int i = 0; i < C.b->n; i++) {
                float x = in->x - C.b->items[i].x, y = in->y - C.b->items[i].y, d = x * x + y * y;
                if (d < dd) {
                    dd = d;
                    best = i;
                }
            }
            if (best >= 0) {
                snprintf(C.info, sizeof C.info, "%s", C.b->items[best].info);
                C.info_until = nos_t + 2.5f;
                nos_feedback(FB_TAP);
            }
        }
        return 1;
    }
    return 0;
}
const nos_screen scr_orrery_card = {"ORRERY CARD", enter, leave, tick, draw, input, SCR_HDRAG | SCR_NATIVE_CACHE};
void orrery_open_card(int ob, int x, int y)
{
    C.ob = (int)clamp((float)ob, 0, OB_N - 1);
    nos_open(&scr_orrery_card, x, y);
}
/* Script-only selector. No additions to the face's shared API. */
void orrery_card_debug_body(int ob)
{
    C.ob = (int)clamp((float)ob, 0, OB_N - 1);
    if (nos_top() == &scr_orrery_card) {
        C.page = 0;
        C.mode = 0;
        C.free = 0;
        C.peek = 0;
        C.pending = 0;
        C.preview = 0;
        C.dial_queue = 0;
        C.info_until = 0;
        C.anim_n = 0;
        C.drag = 0;
        body_reset(C.ob);
    }
}
