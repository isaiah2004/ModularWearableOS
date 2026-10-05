/* app_launcher.c — "the hive": grouped radial app menu (LEFT overlay).
 *
 * GROUPS view — 24 apps in 5 GROUPS on a ring. Drag to spin; it SNAPS the nearest group
 *               to 12 o'clock and comes to rest. The focused (top) group shows its
 *               app-count DOTS + an outline + its name as true CIRCULAR text on the rim.
 * APPS view   — tap a group: others despawn, the disc flies to centre, then its dots
 *               travel out and grow into app icons (staggered, <=200ms/item). Spin here too;
 *               tap an app to open it, tap the centre disc to go back.
 *
 * Rendering: EXPLICIT dirty rects (wos_dirty). Every frame we declare one tight box per
 * element that actually MOVED (its old->new sweep) — never a full-screen repaint, and never
 * the fat auto-damage boxes. A settled view declares nothing -> 0 dirty -> free/60fps. */
#include "apps.h"
#include "../icons.h"
#include "../anim.h"
#include "../pathtext.h"
#include "../hal.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265f

/* ---- groups: indices into the app registry (main.c order) ---- */
static const int G_TIME[]    = { 0, 1, 2, 3, 4, 5, 22 };
static const int G_HEALTH[]  = { 7, 8, 16 };
static const int G_MEDIA[]   = { 9, 10, 11 };
static const int G_CONNECT[] = { 12, 13, 14, 15 };
static const int G_TOOLS[]   = { 6, 17, 18, 19, 20, 21, 23 };
typedef struct { const char *name; int icon; uint16_t accent; const int *apps; int n; } group_t;
static const group_t GROUPS[] = {
    { "Time",    ICON_CLOCK,    GFX_ACCENT, G_TIME,    7 },
    { "Health",  ICON_ACTIVITY, GFX_GREEN,  G_HEALTH,  3 },
    { "Media",   ICON_MUSIC,    GFX_PURPLE, G_MEDIA,   3 },
    { "Connect", ICON_SIGNAL,   GFX_YELLOW, G_CONNECT, 4 },
    { "Tools",   ICON_SETTINGS, GFX_ORANGE, G_TOOLS,   7 },
};
#define NGROUP 5

/* ---- layout ---- */
#define RING_R   GFX_S(68)
#define GDISC_R  GFX_S(20)     /* group disc ~78px (was ~100) */
#define DOT_R    GFX_S(2.5f)
#define DOT_OFF  GFX_S(13)
#define OUTLINE  GFX_S(5)
#define APP_RING GFX_S(76)
#define APP_R    GFX_S(18)     /* ~70px app icon (was ~95) */
#define TEXT_R   (GFX_CX - GFX_S(13))
#define TEXT_MID (-0.92f)

/* ---- drill-in timeline (ms) ---- */
#define FADE_MS  150.0f
#define MOVE_MS  230.0f
#define APP_STAG 70.0f
#define APP_ITEM 200.0f
#define ENTER_STAGGER 60.0f
#define ENTER_DUR     260.0f
#define ROT_STEP  0.1625f      /* max inertial rotation per tick after release */
#define DRAG_DEADZONE GFX_S(12) /* angle is unstable too close to the dial centre */

/* ---- state ---- */
static int      mode, sel;
static float    rot, rot_v, arot, arot_v;
static int      dragging, drag_angle_valid;
static float    last_drag_angle;
static uint32_t last_drag_ms;
static uint32_t t0;
/* icon-flush state: the (clock, rotation) at which the ICON boxes were last emitted. The
 * top-left label gets its OWN frame (icons frozen), so the icons' next old->new sweep must
 * start from here, not simply last frame. */
static float    iflush_e, iflush_rot, iflush_arot;
static int      iflush_foc = -1, iflush_af = -1;   /* focus at last icon flush -> erase its
                                                    * old outline/dots next frame (no orphan ring) */
static int      prev_foc = -1, prev_af = -1, prev_mode = -1;
static int      s_af = -1;      /* focused app this frame (set in draw, used by dirty) */

#define LABEL_SIZE GFX_S(7.0f)
#define LABEL_MAX_W  GFX_S(125)
#define LABEL_MAX_H  GFX_S(46)
typedef struct {
    const char *text;
    uint16_t col;
    int x0, y0, w, h;
    uint8_t *alpha;       /* compact retained label, allocated in PSRAM */
    uint16_t *stage;      /* short GFX_W-stride target required by gfx_fb */
} label_cache;
static label_cache s_label;
int launcher_label_dbg_renders;

typedef struct { float x, y, r, a; } disc_geom;
typedef struct {
    uint32_t fid;
    float e, dot_a;
    int mode, sel, foc, af, napps;
    disc_geom group[NGROUP], apps[7], selected;
} launcher_frame;
static launcher_frame s_frame = { .fid = 0xFFFFFFFFu };

/* ---- helpers ---- */
static uint16_t dim565(uint16_t c, float f)
{
    if (f <= 0.0f) return 0;
    if (f > 1.0f) f = 1.0f;
    int r = (int)(((c >> 11) & 0x1F) * f), g = (int)(((c >> 5) & 0x3F) * f), b = (int)((c & 0x1F) * f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float ease_out(float t) { t = clamp01(t); float x = 1.0f - t; return 1.0f - x * x * x; }
static float ease_back(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

/* Ultra's supersampled vector arc renderer is expensive, so rasterize only
 * when focus changes. The retained alpha patch is compact; stage is merely a
 * bounded-height GFX_W-stride framebuffer used while pathtext is composing. */
static void prepare_label(const char *s, float R, float a_mid, uint16_t col)
{
    if (s_label.text == s && s_label.col == col) return;
    int x0, y0, x1, y1;
    if (!pathtext_arc_bounds(PATHTEXT_CONDENSED, s, R, a_mid, LABEL_SIZE,
                             &x0, &y0, &x1, &y1)) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GFX_W) x1 = GFX_W;
    if (y1 > GFX_H) y1 = GFX_H;
    int w = x1 - x0, h = y1 - y0;
    if (w > (int)LABEL_MAX_W || h > (int)LABEL_MAX_H) return;
    if (!s_label.alpha) s_label.alpha = hal_big_alloc((uint32_t)LABEL_MAX_W * (uint32_t)LABEL_MAX_H);
    if (!s_label.stage) s_label.stage = hal_big_alloc((uint32_t)GFX_W * (uint32_t)LABEL_MAX_H * 2u);
    if (!s_label.alpha || !s_label.stage) return;
    memset(s_label.stage, 0, (size_t)GFX_W * h * 2u);
    gfx_fb stage = { s_label.stage, 0, 0, y0, h, x0, x1, 0 };
    launcher_label_dbg_renders++;
    pathtext_arc(&stage, PATHTEXT_CONDENSED, s, R, a_mid, LABEL_SIZE, GFX_WHITE);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint16_t p = s_label.stage[y * GFX_W + x0 + x];
        s_label.alpha[y * (int)LABEL_MAX_W + x] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
    }
    s_label.text = s; s_label.col = col; s_label.x0 = x0; s_label.y0 = y0;
    s_label.w = w; s_label.h = h;
}
/* text bounding box (used both to draw and to declare its dirty rect) */
static void circ_text_bbox(const char *s, float R, float a_mid, uint16_t col, int *bx0, int *by0, int *bx1, int *by1)
{
    (void)col;
    if (!pathtext_arc_bounds(PATHTEXT_CONDENSED, s, R, a_mid, LABEL_SIZE,
                             bx0, by0, bx1, by1))
        *bx0 = *by0 = *bx1 = *by1 = 0;
}
static void circ_text(gfx_fb *fb, const char *s, float R, float a_mid, uint16_t col)
{
    if (fb->rec) return;   /* explicit-dirty: its box is declared in the dirty pass */
    prepare_label(s, R, a_mid, col);
    if (s_label.text != s || s_label.col != col) {
        /* Allocation/size failure is unusual, but must not make the focus
         * label disappear. This slow path remains stripe-correct. */
        pathtext_arc(fb, PATHTEXT_CONDENSED, s, R, a_mid, LABEL_SIZE, col);
        return;
    }
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    int x0 = c0 - fb->ox - s_label.x0, x1 = c1 - fb->ox - s_label.x0;
    int y0 = fb->y_base - fb->oy - s_label.y0;
    int y1 = fb->y_base + fb->h - fb->oy - s_label.y0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_label.w) x1 = s_label.w;
    if (y1 > s_label.h) y1 = s_label.h;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        uint8_t a = s_label.alpha[y * (int)LABEL_MAX_W + x];
        if (a) gfx_px_blend(fb, s_label.x0 + x, s_label.y0 + y, col, a);
    }
}

/* ---- prerendered disc sprites (experiment; toggle with dbg "sprites 0|1") ----
 * A steady-state disc is a fixed image that only translates, so it is rendered once
 * into PSRAM and copied row-by-row. Positions round to whole pixels. Fades and
 * scale-ins keep the vector path. */
int launcher_sprites = 1;
#define SPR_MAX 40
typedef struct { uint16_t accent; int icon, size; uint16_t *px; } disc_sprite;
static disc_sprite s_spr[SPR_MAX];
static int s_nspr;
static uint16_t *s_spr_stage;
static int s_spr_stage_rows;

static const disc_sprite *sprite_for(uint16_t accent, int icon, float r)
{
    if (fabsf(r - GDISC_R) > 0.01f && fabsf(r - APP_R) > 0.01f) return NULL;
    int size = ((int)ceilf(2.0f * r) + 7) & ~1;
    for (int i = 0; i < s_nspr; i++)
        if (s_spr[i].accent == accent && s_spr[i].icon == icon && s_spr[i].size == size) return &s_spr[i];
    if (s_nspr >= SPR_MAX) return NULL;
    if (size > s_spr_stage_rows) {
        uint16_t *st = hal_big_alloc((uint32_t)GFX_W * size * 2u);
        if (!st) return NULL;
        s_spr_stage = st; s_spr_stage_rows = size;     /* old stage leaks once; sizes are fixed */
    }
    uint16_t *px = hal_big_alloc((uint32_t)size * size * 2u);
    if (!px) return NULL;
    gfx_fb st = { s_spr_stage, 0, 0, 0, size, 0, size, 0 };
    gfx_clear(&st, GFX_BLACK);
    float c = size * 0.5f;
    gfx_fill_circle(&st, c, c, r, accent);
    icon_glyph(&st, icon, c, c, r * 0.6f, GFX_WHITE);
    for (int y = 0; y < size; y++) memcpy(&px[y * size], &s_spr_stage[y * GFX_W], (size_t)size * 2u);
    s_spr[s_nspr] = (disc_sprite){ accent, icon, size, px };
    return &s_spr[s_nspr++];
}

static void blit_sprite(gfx_fb *fb, const disc_sprite *s, float cx, float cy)
{
    int S = s->size, x0 = (int)lroundf(cx) - S / 2, y0 = (int)lroundf(cy) - S / 2;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int bx0 = x0 + fb->ox, bx1 = bx0 + S, sx0 = 0;
    if (bx0 < c0) { sx0 = c0 - bx0; bx0 = c0; }
    if (bx1 > c1) bx1 = c1;
    if (bx1 <= bx0) return;
    int by0 = y0 + fb->oy - fb->y_base, sy0 = 0, sy1 = S;
    if (by0 < 0) { sy0 = -by0; }
    if (by0 + sy1 > fb->h) sy1 = fb->h - by0;
    for (int sy = sy0; sy < sy1; sy++)
        memcpy(&fb->px[(by0 + sy) * GFX_W + bx0], &s->px[sy * S + sx0], (size_t)(bx1 - bx0) * 2u);
}

static void draw_disc(gfx_fb *fb, float cx, float cy, float r, uint16_t accent, int icon, float alpha)
{
    if (alpha <= 0.003f) return;
    if (r < 0.4f) r = 0.4f;
    if (!fb->rec) {
        float ext = r + 2.0f;
        int c0 = fb->cx0, c1 = fb->cx1;
        if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
        if (cx + ext + fb->ox < c0 || cx - ext + fb->ox >= c1 ||
            cy + ext + fb->oy < fb->y_base || cy - ext + fb->oy >= fb->y_base + fb->h)
            return;
    }
    if (launcher_sprites && !fb->rec && alpha >= 0.999f && icon != ICON_NONE) {
        const disc_sprite *s = sprite_for(accent, icon, r);
        if (s) { blit_sprite(fb, s, cx, cy); return; }
    }
    gfx_fill_circle(fb, cx, cy, r, dim565(accent, alpha));
    if (icon != ICON_NONE && r > DOT_R + 1.0f)
        icon_glyph(fb, icon, cx, cy, r * 0.6f, dim565(GFX_WHITE, alpha));
}

/* ---- geometry (position/size at a given clock/rotation) ---- */
static void grp_geom(int k, float e, float rr, float *cx, float *cy, float *r, float *al)
{
    float a = rr + (2.0f * PI * k) / NGROUP;
    *cx = GFX_CX + RING_R * gfx_fast_sin(a);
    *cy = GFX_CY - RING_R * gfx_fast_cos(a);
    float u = e - k * ENTER_STAGGER;
    float t = u <= 0 ? 0 : clamp01(u / ENTER_DUR);
    *al = t; *r = GDISC_R * ease_back(t);
}
static int focus_group(void)
{
    int best = 0; float bc = -2.0f;
    for (int k = 0; k < NGROUP; k++) { float c = gfx_fast_cos(rot + 2.0f * PI * k / NGROUP); if (c > bc) { bc = c; best = k; } }
    return best;
}
/* App-count dot placement: cluster at the four 45-degree CORNERS (TR,BR,BL,TL), filled
 * round-robin CLOCKWISE from top-right. Diagonal corners have a smaller axis-aligned extent
 * than up/down/left/right, so the necklace's bounding box (its dirty rect) is far smaller.
 * With a max of 7 that lands as 2/2/2/1 (TR/BR/BL/TL). */
static void dot_pos(int d, int n, float cx, float cy, float discr, float *dx, float *dy)
{
    static const float CORNER[4] = { -PI * 0.25f, PI * 0.25f, PI * 0.75f, PI * 1.25f };
    int corner = d & 3, within = d >> 2;
    int cnt = (n - corner + 3) / 4;                 /* dots that land in this corner */
    float ang = CORNER[corner] + (within - (cnt - 1) * 0.5f) * 0.26f;
    float dr = discr + DOT_OFF;
    *dx = cx + dr * gfx_fast_cos(ang);
    *dy = cy + dr * gfx_fast_sin(ang);
}
static void seldisc_geom(float e, float *gx, float *gy, float *gr)
{
    float sa = rot + (2.0f * PI * sel) / NGROUP;
    float mv = ease_out(clamp01((e - FADE_MS) / MOVE_MS));
    *gx = lerp(GFX_CX + RING_R * gfx_fast_sin(sa), GFX_CX, mv);
    *gy = lerp(GFX_CY - RING_R * gfx_fast_cos(sa), GFX_CY, mv);
    *gr = GDISC_R * (1.0f - 0.18f * mv);
}
static void app_geom(int i, int n, float e, float ar, float *cx, float *cy, float *r, float *al)
{
    float appsE = e - (FADE_MS + MOVE_MS);
    if (appsE <= 0.0f) {                            /* dots clustered on the flying disc */
        float gx, gy, gr; seldisc_geom(e, &gx, &gy, &gr);
        dot_pos(i, n, gx, gy, gr, cx, cy);
        *r = DOT_R; *al = 1.0f;
        return;
    }
    float ai = clamp01((appsE - i * APP_STAG) / APP_ITEM);
    float posT = ease_out(clamp01(ai / 0.5f));
    float scaleT = ease_back(clamp01((ai - 0.5f) / 0.5f));
    float sx, sy; dot_pos(i, n, GFX_CX, GFX_CY, GDISC_R * 0.82f, &sx, &sy);  /* corner start */
    float a = ar + (2.0f * PI * i) / n;
    float ex = GFX_CX + APP_RING * gfx_fast_sin(a);
    float ey = GFX_CY - APP_RING * gfx_fast_cos(a);
    *cx = lerp(sx, ex, posT); *cy = lerp(sy, ey, posT);
    *r = lerp(DOT_R, APP_R, scaleT); *al = clamp01(ai * 3.0f);
}

static void prepare_frame(void)
{
    if (s_frame.fid == wos_frame_id) return;
    s_frame.fid = wos_frame_id;
    s_frame.e = (float)(hal_millis() - t0);
    s_frame.mode = mode; s_frame.sel = sel;
    s_frame.foc = mode == 0 ? focus_group() : 0;
    s_frame.dot_a = mode == 0
        ? clamp01((gfx_fast_cos(rot + 2.0f * PI * s_frame.foc / NGROUP) - 0.55f) / 0.45f)
        : 0.0f;
    for (int k = 0; k < NGROUP; k++) {
        disc_geom *d = &s_frame.group[k];
        if (mode == 0) grp_geom(k, s_frame.e, rot, &d->x, &d->y, &d->r, &d->a);
        else {
            float angle = rot + 2.0f * PI * k / NGROUP;
            d->x = GFX_CX + RING_R * gfx_fast_sin(angle);
            d->y = GFX_CY - RING_R * gfx_fast_cos(angle);
            d->r = GDISC_R; d->a = 1.0f - clamp01(s_frame.e / FADE_MS);
        }
    }
    s_frame.af = -1; s_frame.napps = 0;
    if (mode != 0) {
        const group_t *g = &GROUPS[sel];
        seldisc_geom(s_frame.e, &s_frame.selected.x, &s_frame.selected.y, &s_frame.selected.r);
        s_frame.selected.a = 1.0f; s_frame.napps = g->n;
        float bc = -2.0f;
        for (int i = 0; i < g->n; i++) {
            disc_geom *d = &s_frame.apps[i];
            app_geom(i, g->n, s_frame.e, arot, &d->x, &d->y, &d->r, &d->a);
            float c = gfx_fast_cos(arot + 2.0f * PI * i / g->n);
            if (d->a > 0.95f && d->r > APP_R * 0.9f && c > bc) { bc = c; s_frame.af = i; }
        }
    }
}

/* ---- dirty-rect helper: box over a circle's old + new positions ---- */
static void dcirc(float x0, float y0, float r0, float x1, float y1, float r1)
{
    float m = GFX_S(2);
    float a = fminf(x0 - r0, x1 - r1) - m, b = fmaxf(x0 + r0, x1 + r1) + m;
    float c = fminf(y0 - r0, y1 - r1) - m, d = fmaxf(y0 + r0, y1 + r1) + m;
    if (b > a && d > c) wos_dirty((int)a, (int)c, (int)b, (int)d);
}

static void on_start(void)
{
    mode = 0; sel = 0;
    rot = rot_v = arot = arot_v = 0.0f;
    dragging = drag_angle_valid = 0; t0 = hal_millis(); last_drag_ms = t0;
    iflush_e = iflush_rot = iflush_arot = 0.0f;
    iflush_foc = iflush_af = prev_foc = prev_af = prev_mode = -1;
    s_frame.fid = 0xFFFFFFFFu; s_label.text = 0; s_label.w = s_label.h = 0;
    if (launcher_sprites)                         /* prerender once; later opens hit the cache */
        for (int k = 0; k < NGROUP; k++) {
            sprite_for(GROUPS[k].accent, GROUPS[k].icon, GDISC_R);
            for (int i = 0; i < GROUPS[k].n; i++)
                sprite_for(wos_app_accent(GROUPS[k].apps[i]), wos_app_icon(GROUPS[k].apps[i]), APP_R);
        }
}

static void on_draw(gfx_fb *fb)
{
    prepare_frame();
    float e = s_frame.e;
    gfx_clear(fb, GFX_BLACK);

    int foc = s_frame.foc;
    /* focused-group dot fade: strong at top, out as it leaves (only the top group shows dots) */
    float dotA = s_frame.dot_a;

    if (mode == 0) {
        for (int k = 0; k < NGROUP; k++) {
            disc_geom *d = &s_frame.group[k];
            draw_disc(fb, d->x, d->y, d->r, GROUPS[k].accent, GROUPS[k].icon, d->a);
        }
        /* dots + outline + label on the focused group only */
        disc_geom *fd = &s_frame.group[foc];
        float cx = fd->x, cy = fd->y, r = fd->r, al = fd->a;
        for (int d = 0; d < GROUPS[foc].n; d++) {
            float dx, dy; dot_pos(d, GROUPS[foc].n, cx, cy, r, &dx, &dy);
            gfx_fill_circle(fb, dx, dy, DOT_R, dim565(GFX_WHITE, dotA));
        }
        gfx_ring(fb, cx, cy, r + OUTLINE, GFX_S(2.4f), dim565(GFX_WHITE, al));
        circ_text(fb, GROUPS[foc].name, TEXT_R, TEXT_MID, dim565(GROUPS[foc].accent, 1.0f));
    } else {
        const group_t *g = &GROUPS[sel];
        float fade = clamp01(e / FADE_MS);
        for (int k = 0; k < NGROUP; k++) {
            if (k == sel) continue;
            disc_geom *d = &s_frame.group[k];
            draw_disc(fb, d->x, d->y, d->r, GROUPS[k].accent, GROUPS[k].icon, 1.0f - fade);
        }
        float gx = s_frame.selected.x, gy = s_frame.selected.y, gr = s_frame.selected.r;
        draw_disc(fb, gx, gy, gr, g->accent, g->icon, 1.0f);
        int af = s_frame.af;
        for (int i = 0; i < g->n; i++) {
            disc_geom *d = &s_frame.apps[i];
            draw_disc(fb, d->x, d->y, d->r, wos_app_accent(g->apps[i]), wos_app_icon(g->apps[i]), d->a);
        }
        s_af = af;
        if (af >= 0) {
            float a = arot + 2.0f * PI * af / g->n;
            gfx_ring(fb, GFX_CX + APP_RING * gfx_fast_sin(a), GFX_CY - APP_RING * gfx_fast_cos(a),
                     APP_R + OUTLINE, GFX_S(2.4f), GFX_WHITE);
            circ_text(fb, wos_app_name(g->apps[af]), TEXT_R, TEXT_MID, dim565(g->accent, 1.0f));
        }
    }

    /* ---------------- EXPLICIT DIRTY ----------------
     * One tight box per element that MOVED (its old->new sweep, from the last ICON flush).
     * The top-left LABEL gets its OWN frame: on a focus change we declare only the label box
     * (icons frozen that frame) so the rotated-glyph blit never shares a frame with the icon
     * boxes -> every frame stays under budget. */
    if (fb->rec) {
        int switched = (mode != prev_mode);
        int tx0, ty0, tx1, ty1;
        if (switched) {                          /* first frame of a view: one clean redraw */
            wos_dirty(0, 0, GFX_W, GFX_H);
            iflush_e = e; iflush_rot = rot; iflush_arot = arot;
            iflush_foc = foc; iflush_af = s_af; prev_foc = foc; prev_af = s_af;
        } else if (mode == 0) {
            int focus_changed = (foc != prev_foc);
            /* keep flushing icons until the LAST focus (iflush_foc) matches -> its old outline
             * gets one more full box and is cleared (no orphan ring), even after a snap. */
            int moving = (rot != iflush_rot) || (foc != iflush_foc) || (e < NGROUP * ENTER_STAGGER + ENTER_DUR + 40.0f);
            if (focus_changed) {                 /* LABEL frame: only the text box */
                int ox0 = s_label.x0, oy0 = s_label.y0;
                int ox1 = ox0 + s_label.w, oy1 = oy0 + s_label.h;
                uint16_t lc = dim565(GROUPS[foc].accent, 1.0f);
                circ_text_bbox(GROUPS[foc].name, TEXT_R, TEXT_MID, lc, &tx0, &ty0, &tx1, &ty1);
                if (s_label.w) {
                    if (ox0 < tx0) tx0 = ox0;
                    if (oy0 < ty0) ty0 = oy0;
                    if (ox1 > tx1) tx1 = ox1;
                    if (oy1 > ty1) ty1 = oy1;
                }
                wos_dirty(tx0, ty0, tx1, ty1);
            } else if (moving) {                 /* ICON frame: per-disc sweep, no text */
                /* diagonal-clustered dots reach only ~0.8*dr on each axis -> tighter box */
                float focext = (GDISC_R + DOT_OFF) * 0.80f + DOT_R - GDISC_R;
                if (focext < OUTLINE) focext = OUTLINE;
                for (int k = 0; k < NGROUP; k++) {
                    float nx, ny, ox, oy, orr, oa;
                    nx = s_frame.group[k].x; ny = s_frame.group[k].y;
                    grp_geom(k, iflush_e, iflush_rot, &ox, &oy, &orr, &oa);
                    float ext = (k == foc || k == iflush_foc) ? focext : GFX_S(1);  /* +erase old */
                    dcirc(ox, oy, GDISC_R + ext, nx, ny, GDISC_R + ext);
                }
                iflush_e = e; iflush_rot = rot; iflush_foc = foc;
            }
            prev_foc = foc;
        } else {
            const group_t *g = &GROUPS[sel];
            int focus_changed = (s_af != prev_af);
            int moving = (arot != iflush_arot) || (s_af != iflush_af) || (e < FADE_MS + MOVE_MS + g->n * APP_STAG + APP_ITEM + 40.0f);
            if (focus_changed && s_af >= 0) {    /* LABEL frame */
                int ox0 = s_label.x0, oy0 = s_label.y0;
                int ox1 = ox0 + s_label.w, oy1 = oy0 + s_label.h;
                uint16_t lc = dim565(g->accent, 1.0f);
                circ_text_bbox(wos_app_name(g->apps[s_af]), TEXT_R, TEXT_MID, lc, &tx0, &ty0, &tx1, &ty1);
                if (s_label.w) {
                    if (ox0 < tx0) tx0 = ox0;
                    if (oy0 < ty0) ty0 = oy0;
                    if (ox1 > tx1) tx1 = ox1;
                    if (oy1 > ty1) ty1 = oy1;
                }
                wos_dirty(tx0, ty0, tx1, ty1);
            } else if (moving) {                 /* ICON frame */
                /* group despawn / disc fly-in: ONLY during the transition (not while spinning
                 * the apps) — otherwise these ring->centre boxes redraw every frame for nothing. */
                if (e < FADE_MS + 60.0f) {
                    for (int k = 0; k < NGROUP; k++) {
                        if (k == sel) continue;
                        float a = rot + 2.0f * PI * k / NGROUP;
                        float cx = GFX_CX + RING_R * gfx_fast_sin(a), cy = GFX_CY - RING_R * gfx_fast_cos(a);
                        dcirc(cx, cy, GDISC_R + GFX_S(2), cx, cy, GDISC_R + GFX_S(2));
                    }
                }
                if (e < FADE_MS + MOVE_MS + 60.0f) {
                    float ngx, ngy, ngr, ogx, ogy, ogr;
                    ngx = s_frame.selected.x; ngy = s_frame.selected.y; ngr = s_frame.selected.r;
                    seldisc_geom(iflush_e, &ogx, &ogy, &ogr);
                    dcirc(ogx, ogy, ogr + GFX_S(2), ngx, ngy, ngr + GFX_S(2));
                }
                for (int i = 0; i < g->n; i++) {
                    float nx, ny, ox, oy, orr, oa;
                    nx = s_frame.apps[i].x; ny = s_frame.apps[i].y;
                    app_geom(i, g->n, iflush_e, iflush_arot, &ox, &oy, &orr, &oa);
                    float ext = (i == s_af || i == iflush_af) ? OUTLINE : GFX_S(1);  /* +erase old */
                    dcirc(ox, oy, APP_R + ext, nx, ny, APP_R + ext);
                }
                iflush_e = e; iflush_arot = arot; iflush_af = s_af;
            }
            prev_af = s_af;
        }
        prev_mode = mode;
    }
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TOUCH_DOWN: {
        float dx = ev->x - GFX_CX, dy = ev->y - GFX_CY;
        dragging = 1; rot_v = arot_v = 0.0f;
        drag_angle_valid = dx * dx + dy * dy >= DRAG_DEADZONE * DRAG_DEADZONE;
        if (drag_angle_valid) last_drag_angle = atan2f(dx, -dy);
        last_drag_ms = hal_millis();
        return 1;
    }
    case WOS_EV_TOUCH_MOVE:
        if (dragging) {
            float dx = ev->x - GFX_CX, dy = ev->y - GFX_CY;
            if (dx * dx + dy * dy < DRAG_DEADZONE * DRAG_DEADZONE) {
                drag_angle_valid = 0;
                rot_v = arot_v = 0.0f;
            } else {
                float angle = atan2f(dx, -dy);
                uint32_t now = hal_millis();
                if (drag_angle_valid) {
                    float d = angle - last_drag_angle;
                    if (d > PI) d -= 2.0f * PI;
                    else if (d < -PI) d += 2.0f * PI;
                    /* Track the finger exactly. Only the stored release velocity is capped so
                     * a sparse MOVE stream cannot launch an excessive inertial sweep. */
                    uint32_t dt = now - last_drag_ms;
                    float v = dt ? d * (16.67f / (float)dt) : d;
                    if (v > ROT_STEP) v = ROT_STEP;
                    else if (v < -ROT_STEP) v = -ROT_STEP;
                    if (mode == 0) { rot += d; rot_v = v; }
                    else { arot += d; arot_v = v; }
                }
                last_drag_angle = angle;
                last_drag_ms = now;
                drag_angle_valid = 1;
            }
        }
        return 1;
    case WOS_EV_TOUCH_UP:
        if (hal_millis() - last_drag_ms > 100u) rot_v = arot_v = 0.0f;
        dragging = 0; drag_angle_valid = 0;
        return 1;
    case WOS_EV_TAP: {
        float tx = ev->x, ty = ev->y;
        if (mode == 0) {
            for (int k = 0; k < NGROUP; k++) {
                float a = rot + 2.0f * PI * k / NGROUP;
                float cx = GFX_CX + RING_R * gfx_fast_sin(a), cy = GFX_CY - RING_R * gfx_fast_cos(a);
                float dx = tx - cx, dy = ty - cy, hit = GDISC_R + GFX_S(6);
                if (dx * dx + dy * dy <= hit * hit) { sel = k; mode = 1; arot = arot_v = 0.0f; t0 = hal_millis(); return 1; }
            }
            return 0;
        }
        float dcx = tx - GFX_CX, dcy = ty - GFX_CY;
        if (dcx * dcx + dcy * dcy <= GDISC_R * GDISC_R) { mode = 0; t0 = hal_millis(); return 1; }
        const group_t *g = &GROUPS[sel];
        for (int i = 0; i < g->n; i++) {
            float a = arot + 2.0f * PI * i / g->n;
            float cx = GFX_CX + APP_RING * gfx_fast_sin(a), cy = GFX_CY - APP_RING * gfx_fast_cos(a);
            float dx = tx - cx, dy = ty - cy, hit = APP_R + GFX_S(5);
            if (dx * dx + dy * dy <= hit * hit) { wos_open_app(g->apps[i]); return 1; }
        }
        return 1;
    }
    case WOS_EV_TICK:
        if (!dragging) {
            if (mode == 0) {
                rot += rot_v; rot_v *= 0.90f;
                if (rot_v < 0.012f && rot_v > -0.012f) {
                    rot_v = 0.0f; float step = 2.0f * PI / NGROUP, target = roundf(rot / step) * step;
                    rot += (target - rot) * 0.22f;
                    if (fabsf(target - rot) < 0.0015f) rot = target;
                }
            } else {
                int nn = GROUPS[sel].n;
                arot += arot_v; arot_v *= 0.90f;
                if (arot_v < 0.012f && arot_v > -0.012f) {
                    arot_v = 0.0f; float step = 2.0f * PI / nn, target = roundf(arot / step) * step;
                    arot += (target - arot) * 0.22f;
                    if (fabsf(target - arot) < 0.0015f) arot = target;
                }
            }
        }
        return 1;
    default:
        return 0;
    }
}

const wos_app app_launcher = { "launcher", on_start, on_event, on_draw, GFX_ACCENT, ICON_NONE };
