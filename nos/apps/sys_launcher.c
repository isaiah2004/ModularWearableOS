/* sys_launcher.c — the app list: a rotary dial on the round glass.
 *
 * The row nearest the centre is the focus: full white name, the largest icon disc.
 * Rows further away dim (64 quantised levels) and slide right along one bezel curve
 * that only ever moves outward, so the column follows the glass like a dial. A name
 * too long for its chord fades out at the glass (3 ink steps over NAME_FADE px)
 * instead of pulling its row back in, which used to bend outer rows back left and
 * leave rows with long names out of line. Scrolling is iOS momentum (0.998/ms
 * decay) whose resting point is moved to the nearest row at release, so a fling lands
 * exactly on a row; below 3.5 px a frame the glide steps whole pixels (3, then 2 px a
 * frame) onto the row and stops there, instead of creeping its last ~40 px at 1 px a
 * frame (MOTION30 R5, R6). A slow release that is a placement rather than a fling goes
 * to a critically damped pixel spring (1 px snap). Every row crossing the centre gives a
 * haptic tick. A tap opens the app through the iris from its icon. The scroll position
 * survives between openings.
 *
 * Cost (docs/FLAT60_DESIGN.md rule 1): each app's icon disc (three sizes) and name are
 * baked once into 8-bit PSRAM sprites. Every pixel in them is a neutral grey, so the
 * index IS the grey level and a 256-entry table turns it into RGB565: dimming a row,
 * or showing it pressed, is a different table, never a re-raster. Motion is only
 * opaque blits (~33 ns/px): ~6 rows = ~90k px = ~3 ms on the watch. Home bakes the
 * sprites in bounded resumable slices after boot and while the launcher is open
 * (sys_launcher_prewarm); visible and predicted rows take priority.
 *
 * List blur: eight directional variants (four widths per screen direction) are
 * precomputed. Separate up/down pixels retain sharp artwork orientation.
 * Row-pitch clipping bounds ownership; ROM assets require no pixel PSRAM. */
#include <stdlib.h>
#include "nos.h"
#include "apps/sys_launcher_page.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "native_menu_assets.h"

void sys_home_open_after(const struct nos_screen *s);   /* sys_home.c (asked for in nos_apps.h) */
#include "sprite.h"
#include "gfx_cost.h"
#include "apps/native_tile.h"
#include "apps/scroll_diag.h"
#include <math.h>
#include <string.h>

#define ROW_P     92.0f     /* row pitch */
#define ICON_X    96        /* focused icon centre x */
#define NAME_DX   58        /* name starts this far right of the icon centre */
#define CURVE_K   0.85f     /* how closely rows follow the bezel (1 = hug it) */
#define NAME_MARGIN 20      /* a name stays this far inside the glass... */
#define NAME_FADE 36        /* ...fading out over its last this-many px when it is too long */
#define NBLUR     8         /* pre-blurred fling variants */
#define BAND      92        /* Blur stays within one row pitch. */
#define NVAR      3         /* icon disc sizes: far / near / focus */
#define ISPR      76        /* icon sprite side */
#define NLEV      64        /* finer ink steps; LUT remains tracked in L */
#define MAXAPP    32
#define TAU_S     0.4995f   /* decay time constant of 0.998/ms */
#define CATCH_V   250.0f    /* px/s: a touch on a list moving faster only stops it */

static const float k_var_r[NVAR] = { 29.0f, 32.0f, 35.0f };
static const int k_blur_len[NBLUR] = { 15, 30, 60, 90, 15, 30, 60, 90 };

typedef struct { sprite8 s; int y0; } blur8;   /* y0: sprite top relative to the row centre */

typedef struct {
    sprite8 icon[NVAR];
    sprite8 name;
    int name_w, name_h, name_base;
    int ready;
    blur8 bicon[NBLUR], bname[NBLUR];       /* fling blur: icon (middle size) and name */
    int blurred;
    int rom;                 /* all pixel views borrowed from immutable flash */
    int stage, row, failed;     /* resumable job; published ready flags stay atomic */
} row_spr;

static struct {
    row_spr rows[MAXAPP];
    uint16_t lut[NLEV + 1][256];      /* grey index -> RGB565 at level/NLEV brightness */
    uint16_t lut_press[256];          /* pressed: the disc lifts to C_LINE2 */
    uint16_t sums[320];         /* one active blur level, resumable between frames */
    uint16_t scratch[320 * 64];       /* RGB565 canvas sprites are drawn on */
    int16_t  curve[GFX_H + 2 * (int)ROW_P];   /* x shift by |row centre - CY| */
} *L;
static int s_baked, s_failed, s_tables_ready, s_table_stage, s_memory_wait;
static int s_rom_mode=-1;
static const font_t *s_source_body,*s_source_label,*s_source_title;
#define LAUNCHER_RESERVE (512u * 1024u)
/* Adjustable caps: elapsed time is checked after each <=8-row work slice.
 * A slice/allocation itself is indivisible; device worst-case timing still matters. */
unsigned sys_launcher_prewarm_budget_us = 750;
unsigned sys_launcher_prewarm_max_steps = 16;
#define BAKE_ROWS 8
static struct { int item, stage, phase, pos, first, last, copy; native_trail_kernel trail; } J = { .item = -1 };

/* scroll state */
static struct {
    float x, v, target;
    int mode;                 /* 0 idle, 1 drag, 2 decay to target, 3 spring */
    mspring sp;
    float grab_x, grab_dy;
    int was_moving;
    int focus;
    float prev_xr;            /* last frame's drawn scroll, for the fling blur */
    int blur;                 /* 0 sharp, else 1 + index into k_blur_len */
    nos_time_t t_enter;
    int inited;
} S;
enum { M_IDLE = 0, M_DRAG, M_DECAY, M_SPRING };

/* §8 pressed state: hidden for the first 100 ms (a scroll's slop passes first and cancels
 * it silently), then on until release; a quick tap still shows it for 3 frames. */
static struct { int id, down, shown; nos_time_t t0, until; } P = { -1, 0, 0, 0, 0 };
static void press_down(int id) { P.id = id; P.down = id >= 0; P.shown = 0; P.t0 = nos_t; P.until = 0; }
static void press_cancel(void) { P.id = -1; P.down = 0; P.until = 0; }
static void press_up(void)
{
    if (!P.down) return;
    P.down = 0;
    nos_time_t vis = P.t0 + 0.09f;                    /* when it became visible */
    P.until = nos_t < vis ? nos_t + 0.09f : vis + 0.10f;
}
static int press_on(int id)
{
    if (id < 0 || P.id != id) return 0;
    if (P.down) return P.shown && nos_touch_travel() <= 8;
    return nos_t < P.until;
}
/* a held press lights once 90 ms have passed with the finger still (<= 3 px), then
 * stays lit until release unless the finger passes 8 px (a drag starting) */
static void press_tick(void)
{
    if (P.down && !P.shown && nos_t - P.t0 >= 0.09f && nos_touch_travel() <= 3) P.shown = 1;
}

/* the last app opened from anywhere (widget, complication, OPEN, REC chip, here) */
const nos_screen *sys_launcher_last(void) { return nos_last_app(); }

static int napps(void) { return nos_app_count < MAXAPP ? nos_app_count : MAXAPP; }
static float native_previous;
static int native_pose,native_exposure,native_direction;
static float max_x(void) { return (napps() - 1) * ROW_P; }
static int launcher_rom_active(void) {
    return nos_native_menu_rom_enabled && native_menu_assets_compatible();
}
void sys_launcher_cache_reset(void);
static void launcher_rom_sync(void) {
    int mode=launcher_rom_active();
    int fonts=s_source_body!=UI_F_BODY || s_source_label!=UI_F_LABEL || s_source_title!=UI_F_TITLE;
    /* Two custom fonts can both reject ROM while requiring different mutable
     * names. Identity belongs to the source revision, independently of mode. */
    if(s_rom_mode>=0 && (mode!=s_rom_mode || fonts))sys_launcher_cache_reset();
    s_source_body=UI_F_BODY;s_source_label=UI_F_LABEL;s_source_title=UI_F_TITLE;
    s_rom_mode=mode;
}
static const uint16_t *launcher_palette(int level,int pressed) {
    if(launcher_rom_active())return native_menu_launcher_palette(level,pressed);
    return pressed?L->lut_press:L->lut[level];
}
/* Install a complete immutable row in one publication. No baker or free path
 * may treat these borrowed pixels as writable allocations. Registry mismatch
 * retains the existing live/baked fallback for that row. */
static int launcher_rom_row(int i,row_spr *out) {
    if(!launcher_rom_active() || !native_menu_launcher_matches((unsigned)i,nos_app_list[i].name,nos_app_list[i].icon))return 0;
    row_spr r={0};
    for(int k=0;k<NVAR;k++) {
        const native_asset_desc *d=native_menu_asset(1,(unsigned)i,(unsigned)k,0);
        if(!d || d->w!=ISPR || d->h!=ISPR || d->x!=-ISPR/2 || d->y!=-ISPR/2)return 0;
        r.icon[k]=native_menu_sprite(d);if(!r.icon[k].px)return 0;
    }
    const native_asset_desc *name=native_menu_asset(2,(unsigned)i,0,0);
    if(!name || name->x!=NAME_DX)return 0;
    r.name=native_menu_sprite(name);if(!r.name.px)return 0;
    r.name_w=r.name.w;r.name_h=r.name.h;
    r.name_base=UI_F_TITLE->ascent/2-1-name->y;
    /* SHARP draws never exposure-blur, so level-0-only art (the SD sharp pack) is complete. */
#if !NOS_RENDER_SHARP
    for(int k=0;k<NBLUR;k++) {
        const native_asset_desc *ic=native_menu_asset(1,(unsigned)i,1,(unsigned)k+1);
        const native_asset_desc *nm=native_menu_asset(2,(unsigned)i,0,(unsigned)k+1);
        if(!ic || !nm || ic->x!=-ISPR/2 || nm->x!=NAME_DX || ic->w!=ISPR || nm->w!=r.name.w)return 0;
        r.bicon[k].s=native_menu_sprite(ic);r.bicon[k].y0=ic->y;
        r.bname[k].s=native_menu_sprite(nm);r.bname[k].y0=nm->y;
        if(!r.bicon[k].s.px || !r.bname[k].s.px)return 0;
    }
#endif
    r.rom=r.ready=r.blurred=1;r.stage=NVAR+1+2*NBLUR;
    *out=r;return 1;
}
static int launcher_adopt_rom(int i) {
    row_spr *r=&L->rows[i];
    /* Never overwrite a partially allocated dynamic bake or its scratch job. */
    if(r->ready || r->stage || r->row || r->icon[0].px || J.item==i)return 0;
    if(!launcher_rom_row(i,r))return 0;
    s_baked++;return 1;
}
static void trim_rows(int sharp_too)
{
    if (!L) { s_failed = 0; return; }
    /* No offscreen item can appear in the outgoing stationary viewport. Card
     * allocation may reclaim its sharp variants too; all near rows stay intact. */
    for (int i = 0; i < napps(); i++) {
        if (fabsf(i * ROW_P - S.x) <= GFX_H / 2 + ISPR + ROW_P) continue;
        row_spr *r = &L->rows[i];
        if (J.item == i) J.item = -1;
        if(r->rom){memset(r,0,sizeof *r);continue;}
        for (int k = 0; k < NBLUR; k++) {
            hal_big_free(r->bicon[k].s.px);
            hal_big_free(r->bname[k].s.px);
            memset(&r->bicon[k], 0, sizeof r->bicon[k]);
            memset(&r->bname[k], 0, sizeof r->bname[k]);
        }
        if (sharp_too) {
            for (int k = 0; k < NVAR; k++) hal_big_free(r->icon[k].px);
            hal_big_free(r->name.px);
            memset(r, 0, sizeof *r);
        } else {
            r->blurred = 0;
            if (r->ready) { r->stage = NVAR + 1; r->row = 0; }
            r->failed = 0;
        }
    }
    s_baked = 0;
    for (int i = 0; i < napps(); i++) s_baked += L->rows[i].blurred != 0;
}
void sys_launcher_trim(void) { trim_rows(0); }
void sys_launcher_card_prepare(void) { trim_rows(1); }
/* UI-thread diagnostic safe point: discard pixels and preparation state while
 * preserving the user's scroll, input, focus and press state. */
void sys_launcher_cache_reset(void)
{
    if(L){
        for(int i=0;i<MAXAPP;i++){
            row_spr *r=&L->rows[i];
            if(r->rom)continue;
            for(int k=0;k<NVAR;k++)hal_big_free(r->icon[k].px);
            hal_big_free(r->name.px);
            for(int k=0;k<NBLUR;k++){
                hal_big_free(r->bicon[k].s.px);hal_big_free(r->bname[k].s.px);
            }
        }
        hal_big_free(L);L=NULL;
    }
    memset(&J,0,sizeof J);J.item=-1;
    s_baked=s_failed=s_tables_ready=s_table_stage=s_memory_wait=0;
}
/* Optional prewarm defers BEFORE touching the allocator. Essential app state and
 * first-use geometry need headroom even when every cache would individually fit. */
static int launcher_room(uint32_t bytes)
{
#ifdef NOS_PSRAM_TRACKING
    uint32_t room = nos_mem_available();
    if (room < LAUNCHER_RESERVE || bytes > room - LAUNCHER_RESERVE) {
        s_memory_wait = 1; return 0;
    }
#else
    (void)bytes;
#endif
    return 1;
}
static int launcher_sprite(sprite8 *dst, int w, int h)
{
    if (!launcher_room((uint32_t)w * (uint32_t)h)) return -1;
    return sprite8_alloc(dst, w, h);
}

/* ---- sprite baking ---------------------------------------------------------------- */
static uint16_t grey565(int v)
{
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (v == 255) return C_WHITE;
    int r = (v * 31 + 127) / 255;              /* neutral: G = 2R, B = R (no casts) */
    return (uint16_t)(r << 11 | (r * 2) << 5 | r);
}

/* Original16-level RGB565 inks remain exact anchors. Intermediate channel
 * interpolation uses green's extra bit instead of changing a whole dark disc
 * by one coarse neutral-grey code at once; deviation is at most one green bit. */
static uint16_t level_ink(int index,int level) {
    int anchor=level>>2,fraction=level&3;
    uint16_t a=grey565((index*anchor+8)/16);
    if(!fraction)return a;
    uint16_t b=grey565((index*(anchor+1)+8)/16);
    unsigned red=(((a>>11)*(4-fraction)+(b>>11)*fraction+2)>>2);
    unsigned ga=(a>>5)&63,gb=(b>>5)&63;
    unsigned green=(ga*(4-fraction)+gb*fraction+2)>>2;
    return (uint16_t)(red<<11|green<<5|red);
}

/* One lookup/curve slice at a time, including initialization on a cold boot.
 * Scratch and sums are intentionally left uninitialized until their jobs use them. */
static int ensure_tables(void)
{
    if (s_tables_ready) return 1;
    if (s_failed) return 0;
    if (!L) {
        if (!launcher_room(sizeof *L)) return 0;
        L = hal_big_alloc(sizeof *L);
        if (!L) { s_failed = 1; return 0; }
        memset(L->rows, 0, sizeof L->rows);
        if(!launcher_rom_active())return 0;
    }
    if(launcher_rom_active()){s_tables_ready=1;return 1;}
    if (s_table_stage <= NLEV) {
        /* Four bounded palette rows preserve the old cold-start frame count. */
        for(int row=0;row<4 && s_table_stage<=NLEV;row++) {
            int l=s_table_stage++;
            for(int i=0;i<256;i++)L->lut[l][i]=level_ink(i,l);
        }
        return 0;
    }
    if (s_table_stage == NLEV + 1) {
        for (int i = 0; i < 256; i++)
            L->lut_press[i] = grey565(i <= 26 ? i * 51 / 26 : 51 + (i - 26) * (255 - 51) / (255 - 26));
        s_table_stage++; return 0;
    }
    int y0 = (s_table_stage - NLEV - 2) * 32;
    int n = (int)(sizeof L->curve / sizeof L->curve[0]);
    int y1 = y0 + 32 < n ? y0 + 32 : n;
    for (int y = y0; y < y1; y++) {
        float d = (float)y, r = (float)UI_R;
        L->curve[y] = (int16_t)(CURVE_K * (d < r ? r - sqrtf(r * r - d * d) : r) + 0.5f);
    }
    s_table_stage++;
    if (y1 == n) s_tables_ready = 1;
    return s_tables_ready;
}
static void to_index(uint8_t *dst, const uint16_t *src, int n)
{
    for (int i = 0; i < n; i++) {
        int g = (src[i] >> 5) & 63;
        dst[i] = (uint8_t)((g * 255 + 31) / 63);
    }
}

/* Publish a sharp row only after all three icon sizes and its full name exist.
 * An allocation failure parks this row for the session, never retrying per frame.
 * Already published sharp assets remain usable after a later blur failure. */
static void bake_sharp_step(int i)
{
    row_spr *R = &L->rows[i];
    const nos_app *a = &nos_app_list[i];
    int icon = R->stage < NVAR;
    sprite8 *dst = icon ? &R->icon[R->stage] : &R->name;
    int w = ISPR, h = ISPR;
    const font_t *f = UI_F_TITLE;
    if (!icon) {
        w = ui_text_width(f, a->name, 0) + 6; if (w > 320) w = 320;
        h = f->line_height + 6; if (h > 64) h = 64;
        R->name_base = f->ascent + 3;
    }
    if (!dst->px) {
        int allocated = launcher_sprite(dst, w, h);
        if (allocated < 0) return;
        if (!allocated) { R->failed = 1; return; }
    }
    int rows = h - R->row < BAKE_ROWS ? h - R->row : BAKE_ROWS;
    memset(L->scratch, 0, (size_t)w * rows * 2);
    gfx_fb fb = {0}; fb.px = L->scratch; fb.stride = w;
    fb.h = rows; fb.y_base = R->row; fb.cx1 = w;
    if (icon) {
        float c = ISPR * 0.5f, r = k_var_r[R->stage], pitch = r * 0.118f;
        gfx_fill_circle(&fb, c, c, r, C_SURF2);
        dot_icon(&fb, a->icon, c, c, pitch, pitch * 0.8f, C_WHITE);
    } else ui_text(&fb, f, 2, R->name_base, a->name, C_WHITE, UI_LEFT, 0);
    to_index(dst->px + (size_t)R->row * w, L->scratch, w * rows);
    R->row += rows;
    if (R->row == h) {
        R->row = 0; R->stage++;
        if (!icon) { R->name_w = w; R->name_h = h; R->ready = 1; }
    }
}
static void blur_finish(row_spr *R)
{
    R->stage++;
    if (R->stage == NVAR + 1 + 2 * NBLUR) { R->blurred = 1; s_baked++; }
    J.item = -1;
}
/* The whole active level owns scratch until its final cropped sprite is copied.
 * Causal normalized weights bake independent up/down tails without flipping art. */
static void bake_blur_step(int i)
{
    row_spr *R = &L->rows[i];
    int k = R->stage - NVAR - 1, lv = k / 2, name = k & 1;
    const sprite8 *src = name ? &R->name : &R->icon[1];
    blur8 *dst = name ? &R->bname[lv] : &R->bicon[lv];
    int w = src->w, len = k_blur_len[lv];
    int top = name ? BAND / 2 - R->name_base + UI_F_TITLE->ascent / 2 - 1 : BAND / 2 - ISPR / 2;
    if (J.item < 0) {
        J.item = i; J.stage = R->stage; J.phase = 1; J.pos = 0;
        J.first = BAND; J.last = -1; J.copy = 0;
        native_trail_kernel_make(&J.trail,len);
        memset(L->sums, 0, (size_t)w * sizeof L->sums[0]);
        return;
    }
    uint8_t *tmp = (uint8_t *)L->scratch;
    if (J.phase == 1) {
        int end = J.pos + BAKE_ROWS < BAND ? J.pos + BAKE_ROWS : BAND;
        for (int y = J.pos; y < end; y++) {
            int any = 0;
            for (int x = 0; x < w; x++) {
                unsigned v=native_trail_sample(&J.trail,src->px,w,src->h,x,y-top,lv<4?1:-1);
                tmp[y*w+x]=(uint8_t)v;any|=v;
            }
            if (any) { if (J.first == BAND) J.first = y; J.last = y; }
        }
        J.pos = end; if (end == BAND) J.phase = 2;
        return;
    }
    if (J.phase == 2) {
        if (J.last < J.first) { dst->s.px = NULL; blur_finish(R); return; }
        int allocated = launcher_sprite(&dst->s, w, J.last - J.first + 1);
        if (allocated < 0) return;
        if (!allocated) { R->failed = 1; J.item = -1; return; }
        dst->y0 = J.first - BAND / 2; J.phase = 3; return;
    }
    int h = J.last - J.first + 1;
    int rows = h - J.copy < BAKE_ROWS ? h - J.copy : BAKE_ROWS;
    memcpy(dst->s.px + (size_t)J.copy * w, tmp + (size_t)(J.first + J.copy) * w, (size_t)rows * w);
    J.copy += rows; if (J.copy == h) blur_finish(R);
}
static int bake_next(void)
{
    if (J.item >= 0) return J.item; /* scratch belongs to this in-flight level */
    int best = -1, score = 0;
    float current = S.x / ROW_P, predicted = (S.x + S.v * 0.15f) / ROW_P;
    for (int i = 0; i < napps(); i++) {
        const row_spr *R = &L->rows[i];
        if (R->failed || R->blurred) continue;
        float d = fabsf(i - current), p = fabsf(i - predicted);
        int visible = d <= 3.0f || p <= 3.0f;
        int rank = (visible ? 0 : 8000) + (R->ready ? 4000 : 0) + (int)(fminf(d, p + 1.0f) * 100.0f);
        if (best < 0 || rank < score) { best = i; score = rank; }
    }
    return best;
}
/* Desktop pixel references replay wall-clock cache work, including distinct
 * invocations in the same frame. Device budget decisions remain unchanged. */
#ifdef GFX_COST_MODEL
static unsigned bake_tape_frame=~0u,bake_tape_call;
#endif
void sys_launcher_prewarm(void)
{
    launcher_rom_sync();
    if (s_failed || s_baked >= napps() || !sys_launcher_prewarm_max_steps || !sys_launcher_prewarm_budget_us) return;
    s_memory_wait = 0;
    uint32_t start = hal_micros();
#ifdef GFX_COST_MODEL
    if (bake_tape_frame != nos_frame) { bake_tape_frame=nos_frame; bake_tape_call=0; }
    unsigned tape_call=++bake_tape_call;
#endif
    for (unsigned step = 0; step < sys_launcher_prewarm_max_steps; step++) {
        if (!s_tables_ready) ensure_tables();
        else {
            int i = bake_next(); if (i < 0) return;
            if(launcher_adopt_rom(i)){}
            else if (L->rows[i].ready) {
                /* SHARP never draws the 16 blur sprites (about 3 MB): finish the row without them. */
                if (NOS_RENDER_SHARP) { L->rows[i].blurred = 1; L->rows[i].stage = NVAR + 1 + 2 * NBLUR; s_baked++; }
                else bake_blur_step(i);
            } else bake_sharp_step(i);
        }
        if (s_failed || s_memory_wait) return;
        int stop=hal_micros() - start >= sys_launcher_prewarm_budget_us;
#ifdef GFX_COST_MODEL
        if (nos_quality_decision) {
            /* Both ordinals fit the explicit16-bit key fields in valid runs. */
            if (tape_call>65535u || step>65535u) abort();
            stop=nos_quality_decision(nos_frame,4,(tape_call<<16)|step,stop);
        }
#endif
        if (stop) return;
    }
}

/* ---- scrolling ---------------------------------------------------------------------- */
static float snap_row(float x) { return m_clamp(floorf(x / ROW_P + 0.5f) * ROW_P, 0, max_x()); }

static void spring_to(float target, float v)
{
    msp_init_px(&S.sp, S.x, SPRING_SNAPPY);
    float dist = target - S.x, w0 = 6.2831853f / S.sp.response;
    if (v * dist > 0 && fabsf(v) > w0 * fabsf(dist)) v = (dist > 0 ? 1.0f : -1.0f) * w0 * fabsf(dist);
    S.sp.v = v;
    msp_to(&S.sp, target);
    S.target = target;
    S.mode = M_SPRING;
}

static void release(float v)
{
    float mx = max_x();
    if (S.x < 0 || S.x > mx) {                /* back from the rubber band */
        msp_init_px(&S.sp, S.x, 0.40f, 1.0f);
        S.sp.v = v;
        msp_to(&S.sp, S.x < 0 ? 0 : mx);
        S.target = S.sp.target;
        S.mode = M_SPRING;
        return;
    }
    /* where the fling would rest, moved to the nearest row: the decay is re-aimed so
     * it lands exactly there (velocity changes by at most half a row / tau) */
    float tgt = snap_row(decay_project(S.x, v));
    S.target = tgt;
    S.v = (tgt - S.x) / TAU_S;
    S.mode = fabsf(tgt - S.x) < 0.5f ? M_IDLE : M_DECAY;
    if (S.mode == M_IDLE) S.x = tgt;
    else if (fabsf(tgt - S.x) < 3.0f || fabsf(v) < 250.0f) spring_to(tgt, v);   /* a placement, not a fling */
}

static void scroll_tick(float dt)
{
    if (S.mode == M_DECAY) {
        float rem = S.target - S.x, fr = dt * M_FPS;    /* fr: frames in this step */
        float d = rem * (1.0f - expf(-dt / TAU_S));
        if (fabsf(d) < 3.5f * fr) {
            /* the slow tail in whole pixels on a whole-pixel position, 3 then 2 px a frame,
             * straight onto the row (a whole number of px): no sub-pixel shimmer, no creep */
            S.x = floorf(S.x + 0.5f);
            rem = S.target - S.x;
            float st = floorf(fabsf(d) / fr + 0.5f);
            st = (st < 2.0f ? 2.0f : st > 3.0f ? 3.0f : st) * fr;
            if (st >= fabsf(rem)) { S.x = S.target; S.v = 0; S.mode = M_IDLE; }
            else { S.x += rem > 0 ? st : -st; S.v = (rem > 0 ? st : -st) / dt; }
        } else {
            S.x += d;
            S.v = (S.target - S.x) / TAU_S;
        }
    }
    if (S.mode == M_SPRING) {
        int m = msp_step(&S.sp, dt);
        S.x = S.sp.x; S.v = S.sp.v;
        if (!m) { S.x = S.target; S.v = 0; S.mode = M_IDLE; }
    }
    int f = (int)floorf(S.x / ROW_P + 0.5f);
    if (f < 0) f = 0;
    if (f > napps() - 1) f = napps() - 1;
    if (f != S.focus) { S.focus = f; nos_feedback(FB_TICK); }
}

/* ---- screen ------------------------------------------------------------------------- */
static void l_enter(void)
{
    if (!S.inited) { memset(&S, 0, sizeof S); S.inited = 1; }
    S.x = snap_row(S.x);                       /* remembered between openings */
    S.mode = M_IDLE; S.v = 0;
    S.focus = (int)(S.x / ROW_P + 0.5f);
    /* back from an app the list slid in fully drawn under it: no rebuild (§8) */
    S.t_enter = nos_revealed() ? nos_t - 2.0f : nos_enter_t();
    S.prev_xr = floorf(S.x + 0.5f);
    S.blur = 0;
    native_previous=S.x;native_pose=native_exposure=0;
    press_cancel();
}

static void l_leave(void) { if (P.down) press_up(); if (S.mode == M_DRAG) release(0); }

/* build-in progress of row i: the 6-frame table from a whole-frame start 2 frames per row
 * out from the focus (at most 8 starts) */
static float row_in(int i, float xr)
{
    int d = (int)floorf(fabsf((float)i - xr / ROW_P) + 0.5f);
    return mf_at(&MF_MICRO6, m_stagger_k(m_frame_k(S.t_enter), d, 2));
}

typedef struct { int cy, ix, lev, var, other, mix, cut; } row_geo;
static void geometry_prepare(float xr);
static int launcher_trail_x(int cy,int level);
static void l_tick(float dt)
{
    press_tick();
    scroll_tick(dt);
    float xr = floorf(S.x + 0.5f);
    native_pose=NOS_RENDER_SHARP?0:(int)lroundf((xr-S.x)*256.0f);
    float travel=fabsf(S.x-native_previous)/(dt*M_FPS>.5f?dt*M_FPS:1.0f);
    native_exposure=nos_mblur && !NOS_HELD_SHARP(S.mode==M_DRAG) ?
        native_smear_exposure(travel) : 0;
    native_direction=S.x>native_previous?-1:S.x<native_previous?1:native_direction;
    native_previous=S.x;
    /* Normalize actual rounded movement to one NOS_FPS frame, then use the
     * shared cache ladder and the user's held-motion policy. */
    float fr = dt * M_FPS, st = fabsf(xr - S.prev_xr) / (fr > 0.5f ? fr : 1.0f);
    S.prev_xr = xr;
    S.blur = NOS_HELD_SHARP(S.mode == M_DRAG) ? 0 : ui_fblur_level(st);   /* the lists' ladder (ui.h) */
    /* motion audit (sim only): the list as drawn and the build-in of the focus row and
     * the two below it */
    nos_motion_probe("launcher.list", S.mode == M_DRAG ? MC_FINGER : MC_MOMENTUM, -xr);
    static const char *const k_b[3] = { "launcher.build.row0", "launcher.build.row1", "launcher.build.row2" };
    int c = (int)floorf(xr / ROW_P + 0.5f);
    for (int j = 0; j < 3; j++) {
        float e = row_in(c + j, xr);
        nos_motion_probe(k_b[j], MC_MICRO, (float)(int)lroundf((1.0f - e) * 40.0f));
        if (j == 0) nos_motion_probe("launcher.build.ink", MC_FADE, (int)(m_ink(e) * NLEV + 0.5f) * 100.0f / NLEV);
    }
    sys_launcher_prewarm();
    geometry_prepare(xr);
}

/* Layout of row i at the current scroll: centre y, icon x, dim level, icon variant.
 * Returns 0 if the row is off the glass. */
static int geo(int i, float xr, row_geo *g)
{
    float cy = UI_CY + i * ROW_P - xr;
    if (cy < -ISPR || cy > GFX_H + ISPR) return 0;
    float d = fabsf(cy - UI_CY);
    int di = (int)d;
    if (di >= (int)(GFX_H + 2 * ROW_P)) di = (int)(GFX_H + 2 * ROW_P) - 1;
    g->cy = (int)lroundf(cy);
    float r = UI_R, dy = (float)di;
    const int16_t *rom_curve=launcher_rom_active()?native_menu_launcher_curve():NULL;
    int curve = rom_curve?rom_curve[di]:s_tables_ready ? L->curve[di] : (int)(CURVE_K * (dy < r ? r - sqrtf(r * r - dy * dy) : r) + 0.5f);
    g->ix = ICON_X + curve;
    float u = d / ROW_P;
    g->var = u < 0.25f ? 2 : u < 0.5f ? 1 : 0;
    g->other=g->var;g->mix=0;
    /* Keep the authored endpoint sizes, easing across a nine-pixel band instead
     * of replacing an entire icon at the old23/46px distance thresholds. */
    if(u>0.20f && u<0.30f) {
        g->var=2;g->other=1;g->mix=(int)(m_smoothstep(0.20f,0.30f,u)*64.0f+0.5f);
    } else if(u>0.45f && u<0.55f) {
        g->var=1;g->other=0;g->mix=(int)(m_smoothstep(0.45f,0.55f,u)*64.0f+0.5f);
    } else if(u>=0.30f && u<=0.45f)g->var=g->other=1;
    else if(u>=0.55f)g->var=g->other=0;
    /* 1.0 at the focus, ~0.6 one row out, ~0.36 two rows out */
    /* Preserve old rounded resting ink:16/16,10/16,5/16 at0/1/2 rows. */
    float f = 1.0f - 0.375f * m_smoothstep(0.0f, 1.0f, u) - 0.3125f * m_smoothstep(1.0f, 2.0f, u);
    /* build-in: rows arrive in waves outward from the focus, 2 whole frames per row,
     * sliding in 40 px from the right on the 6-frame table (11 10 8 6 3 2 px) */
    float e = row_in(i, xr);
    /* every row rides the same outward curve; a name too long for the glass here fades
     * out at name_cut instead of moving its row (which bent outer rows back left) */
    float ey = cy < UI_CY ? cy - 16 : cy + 16;               /* the name's outer edge */
    g->cut = UI_CX + ui_half_width(ey) - NAME_MARGIN;
    g->ix += (int)lroundf((1.0f - e) * 40.0f);
    f *= m_ink(e);                             /* ink up in perceptual steps */
    g->lev = (int)(f * NLEV + 0.5f);
    if (g->lev > NLEV) g->lev = NLEV;
    return g->lev > 0;
}

/* Recording and each raster stripe share this frame's seven visible row poses.
 * Keep only geometry: sources and presses retain their existing live ownership.
 * Draw-only callers with a changed clock, scroll or table use the pure fallback. */
static struct {
    nos_time_t time,enter;
    float xr;
    const void *tables;
    int first,ready,rom,valid;
    int exposure,direction,low,high;
    unsigned visible;
    row_geo rows[7];
    int trail[7][2];
} geometry;
static void geometry_prepare(float xr)
{
    geometry.valid=0;geometry.time=nos_t;geometry.enter=S.t_enter;geometry.xr=xr;
    geometry.tables=L;geometry.ready=s_tables_ready;geometry.rom=launcher_rom_active();
    geometry.exposure=native_exposure;geometry.direction=native_direction;
    unsigned mix;
    native_prepared_levels(native_exposure,&geometry.low,&geometry.high,&mix);
    native_direction_levels(native_direction,&geometry.low,&geometry.high);
    geometry.first=(int)floorf(xr/ROW_P+.5f)-3;geometry.visible=0;
    for(int k=0;k<7;k++) {
        int i=geometry.first+k;
        if(i>=0 && i<napps() && geo(i,xr,&geometry.rows[k])) {
            geometry.visible|=1u<<k;
            geometry.trail[k][0]=launcher_trail_x(geometry.rows[k].cy,geometry.low);
            geometry.trail[k][1]=geometry.high==geometry.low?geometry.trail[k][0]:
                launcher_trail_x(geometry.rows[k].cy,geometry.high);
        }
    }
    geometry.valid=1;
}
static int geometry_current(float xr)
{
    return geometry.valid && geometry.time==nos_t && geometry.enter==S.t_enter && geometry.xr==xr &&
        geometry.tables==L && geometry.ready==s_tables_ready && geometry.rom==launcher_rom_active();
}
static int geometry_get(int i,float xr,row_geo *g)
{
    int k=i-geometry.first;
    if(geometry_current(xr) && (unsigned)k<7u) {
        if(!(geometry.visible&(1u<<k)))return 0;
        *g=geometry.rows[k];return 1;
    }
    return geo(i,xr,g);
}

/* The image revision includes every rendered parameter; arithmetic row/level
 * packing aliases once ink precision exceeds the old16-level layout. */
static unsigned row_image_key(int row,const row_geo *g,int pressed,int blur,int name) {
    unsigned h=gfx_hash_u(0x1A5B71u,(unsigned)row);
    h=gfx_hash_u(h,(unsigned)g->lev);h=gfx_hash_u(h,(unsigned)pressed);
    h=gfx_hash_u(h,(unsigned)blur);h=gfx_hash_u(h,(unsigned)name);
    if(!name&&!blur){h=gfx_hash_u(h,(unsigned)g->var);h=gfx_hash_u(h,(unsigned)g->other);h=gfx_hash_u(h,(unsigned)g->mix);}
    return h;
}
static void icon_mix_blit(gfx_fb *fb,const sprite8 *a,const sprite8 *b,
                          int x,int y,const uint16_t *lut,unsigned key,unsigned mix) {
    if(NOS_RENDER_SHARP){sprite8_blit(fb,mix>=32?b:a,x,y,lut,key);return;}
    if(!mix||a==b){sprite8_blit(fb,a,x,y,lut,key);return;}
    if(mix>=64){sprite8_blit(fb,b,x,y,lut,key);return;}
    if(gfx_rec_cmd(fb,x,y,x+a->w,y+a->h,gfx_hash_u(gfx_hash_u(key,(unsigned)x),(unsigned)y)))return;
    if(!a->px||!b->px||a->w!=b->w||a->h!=b->h)return;
    uint16_t dimmed[256];
    if(fb->dim){for(int i=0;i<256;i++)dimmed[i]=gfx_ink(fb,lut[i]);lut=dimmed;}
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0){c0=0;c1=GFX_W;}
    if(c0<fb->x_base)c0=fb->x_base;
    int right=fb->x_base+(fb->stride?fb->stride:GFX_W);
    if(c1>right)c1=right;
    if(c0<0)c0=0;
    if(c1>GFX_W)c1=GFX_W;
    int bx=x+fb->ox,k0=c0>bx?c0-bx:0,k1=c1<bx+a->w?c1-bx:a->w;
    int first=fb->y_base-y-fb->oy,last=first+fb->h;
    if(first<0)first=0;
    if(last>a->h)last=a->h;
    if(k0>=k1||first>=last)return;
#ifdef GFX_COST_MODEL
    gfx_cost_target(fb->cost_psram);
#endif
    unsigned pixels=(unsigned)(last-first)*(unsigned)(k1-k0);
    (void)pixels;
    GFX_COST(GFX_COST_BLIT,pixels);
    /* Extra source/index interpolation is estimated, not target-calibrated. */
    GFX_COST_PX(GFX_COST_BLEND,pixels);
    for(int row=first;row<last;row++) {
        uint16_t *dst=gfx_row(fb,y+row+fb->oy-fb->y_base)+bx+k0;
        const uint8_t *pa=a->px+row*a->w+k0,*pb=b->px+row*b->w+k0;
        for(int col=k0;col<k1;col++) {
            unsigned index=(*pa++*(64-mix)+*pb++*mix+32)>>6;
            *dst++=lut[index];
        }
    }
}

/* Blit only logical columns [c0, c1) of a sprite, inside whatever clip the engine set.
 * In record mode the whole sprite is recorded (keys tell the segments apart). */
static const sprite8 *native_name_other;
/* Place the normalized trail's mean on the authored curve. This remains a
 * bounded vertical tile approximation, not a rerender of every curved pose.
 * Means are from the same quarter-pixel density used by native_trail_kernel. */
static int launcher_trail_x(int cy,int level) {
    static const float mean[4]={1.583601286f,4.300000000f,9.407710280f,14.519169329f};
    if(!level)return 0;
    float age=mean[(level-1)%4]*(level>4?1.0f:-1.0f);
    float d0=fabsf((float)cy-UI_CY),d1=fabsf((float)cy+age-UI_CY),r=UI_R;
    float x0=CURVE_K*(d0<r?r-sqrtf(r*r-d0*d0):r);
    float x1=CURVE_K*(d1<r?r-sqrtf(r*r-d1*d1):r);
    return (int)lroundf(x1-x0);
}
/* The tick publishes exact offsets with the row poses. Recording and every
 * stripe reuse them; draw-only/debug changes retain the original pure path.
 * Never rewrite this publication from a draw within the frame. */
static void geometry_trail_get(int row,float xr,int cy,int low,int high,int *xa,int *xb)
{
    int k=row-geometry.first;
    if(geometry_current(xr) && geometry.exposure==native_exposure && geometry.direction==native_direction &&
       geometry.low==low && geometry.high==high && (unsigned)k<7u &&
       (geometry.visible&(1u<<k)) && geometry.rows[k].cy==cy) {
        *xa=geometry.trail[k][0];*xb=geometry.trail[k][1];return;
    }
    *xa=launcher_trail_x(cy,low);
    *xb=high==low?*xa:launcher_trail_x(cy,high);
}
static int native_name_dx;
static int native_name_delta,native_name_top,native_name_bottom,native_name_blurred;
static unsigned native_name_mix;
static void native_name_blit(gfx_fb *fb,const sprite8 *s,int x,int y,const uint16_t *lut,unsigned key)
{
    native_prepared_blit(fb,s,x,y,native_name_other,x+native_name_dx,y+native_name_delta,native_name_mix,
                         lut,key,native_pose,native_name_bottom?native_name_top:y-25,
                         native_name_bottom?native_name_bottom:y+s->h+25,native_name_blurred,NULL,0);
}
static void blit_cols(gfx_fb *fb, const sprite8 *s, int x, int y, const uint16_t *lut, unsigned key, int c0, int c1)
{
    int left=x,right=x+s->w;
    if(native_name_other && native_name_mix){
        if(x+native_name_dx<left)left=x+native_name_dx;
        if(x+native_name_dx+native_name_other->w>right)right=x+native_name_dx+native_name_other->w;
    }
    if(c0<left)c0=left;
    if(c1>right)c1=right;
    if (c1 <= c0) return;
    if (fb->rec) { native_name_blit(fb,s,x,y,lut,key); return; }
    int o0 = fb->cx0, o1 = fb->cx1, a0 = o0, a1 = o1;
    if (a1 <= a0) { a0 = 0; a1 = GFX_W; }     /* empty window = no clip */
    int n0 = c0 + fb->ox > a0 ? c0 + fb->ox : a0, n1 = c1 + fb->ox < a1 ? c1 + fb->ox : a1;
    if (n1 <= n0) return;
    fb->cx0 = n0; fb->cx1 = n1;
    native_name_blit(fb,s,x,y,lut,key);
    fb->cx0 = o0; fb->cx1 = o1;
}

/* A name at dim level lev that must be gone by x = cut: whole if it fits, else drawn
 * to NAME_FADE px before the cut and faded out over the last NAME_FADE px in three
 * ink steps (3/4, 1/2, 1/4). */
static void draw_name(gfx_fb *fb, const sprite8 *s, int x, int y, int lev, int cut, unsigned key)
{
    int left=x,right=x+s->w;
    if(native_name_other && native_name_mix){
        if(x+native_name_dx<left)left=x+native_name_dx;
        if(x+native_name_dx+native_name_other->w>right)right=x+native_name_dx+native_name_other->w;
    }
    if (right <= cut) { native_name_blit(fb,s,x,y,launcher_palette(lev,0),key); return; }
    unsigned k = gfx_hash_u(key,(unsigned)cut);   /* the cut moves: so does the key */
    int f0 = cut - NAME_FADE, seg = NAME_FADE / 3;
    blit_cols(fb, s, x, y, launcher_palette(lev,0), k, left, f0);
    for (int j = 0; j < 3; j++) {
        int l = lev * (3 - j) / 4;
        if (l > 0) blit_cols(fb, s, x, y, launcher_palette(l,0), gfx_hash_u(k,(unsigned)j), f0 + j * seg, f0 + (j + 1) * seg);
    }
}

/* Cold-cache names obey the same chord cut and three fade segments as sprites.
 * The vector fallback never shares scratch with an in-flight blur bake. */
static void live_text_cols(gfx_fb *fb, int x, int y, const char *text, int lev, int c0, int c1)
{
    if (c1 <= c0 || lev <= 0) return;
    gfx_fb sub = *fb;
    int a0 = fb->cx0, a1 = fb->cx1;
    if (a1 <= a0) { a0 = 0; a1 = GFX_W; }
    sub.cx0 = c0 + fb->ox > a0 ? c0 + fb->ox : a0;
    sub.cx1 = c1 + fb->ox < a1 ? c1 + fb->ox : a1;
    if (!fb->rec && sub.cx1 <= sub.cx0) return;
    ui_text(&sub, UI_F_TITLE, x + 2, y, text, grey565(255 * lev / NLEV), UI_LEFT, 0);
}
static void draw_live_name(gfx_fb *fb, int x, int y, const char *text, int lev, int cut)
{
    int w = ui_text_width(UI_F_TITLE, text, 0) + 6; if (w > 320) w = 320;
    if (fb->rec) {
        gfx_rec_cmd(fb, x, y - UI_F_TITLE->ascent - 3, x + w, y + 16,
                    gfx_hash_u(0x1a4ec017u, (unsigned)cut));
    }
    if (x + w <= cut) { live_text_cols(fb, x, y, text, lev, x, x + w); return; }
    int f0 = cut - NAME_FADE, seg = NAME_FADE / 3;
    live_text_cols(fb, x, y, text, lev, x, f0 < x + w ? f0 : x + w);
    for (int j = 0; j < 3; j++) {
        int a = f0 + j * seg, b = a + seg;
        if (a < x) a = x;
        if (b > x + w) b = x + w;
        live_text_cols(fb, x, y, text, lev * (3 - j) / 4, a, b);
    }
}

static void l_draw(gfx_fb *fb)
{
    launcher_rom_sync();
    float xr = floorf(S.x + 0.5f);
    int n = napps(), c = (int)floorf(xr / ROW_P + 0.5f);
    /* the standard title, lifting away as the list scrolls (§8); it arrives with the build */
    if (nos_t >= S.t_enter) ui_title(fb, "APPS", xr < 0 ? 0 : xr);
    for (int i = c - 3; i <= c + 3; i++) {
        if (i < 0 || i >= n) continue;
        row_geo g;
        if (!geometry_get(i, xr, &g)) continue;
        row_spr fallback_rom;
        row_spr *R = L ? &L->rows[i] : NULL;
        if((!R || !R->ready) && launcher_rom_active()){
            if(R && launcher_adopt_rom(i)){}
            else if(launcher_rom_row(i,&fallback_rom))R=&fallback_rom;
        }
        if (!R || !R->ready) {                        /* sprites unavailable: draw live */
            float r = k_var_r[g.var]+(k_var_r[g.other]-k_var_r[g.var])*g.mix/64.0f;
            int pressed = press_on(i);
            ui_circle(fb, (float)g.ix, (float)g.cy, r, grey565(pressed ? 51 : 26 * g.lev / NLEV));
            dot_icon(fb, nos_app_list[i].icon, (float)g.ix, (float)g.cy, r * 0.118f, r * 0.094f,
                     grey565(pressed ? 255 : 255 * g.lev / NLEV));
            draw_live_name(fb, g.ix + NAME_DX, g.cy + UI_F_TITLE->ascent / 2 - 1,
                           nos_app_list[i].name, g.lev, g.cut);
            continue;
        }
        int pressed = press_on(i);
        /* Published native rows own exactly this authored vertical band.
         * Keep record traversal and all cold adoption/live fallback above.
         * Preserve the backing call's first-use/frame-reset side effects even
         * when this stripe cannot receive a pixel from the row. */
        if(!fb->rec && (g.cy+BAND/2+fb->oy<=fb->y_base ||
                        g.cy-BAND/2+fb->oy>=fb->y_base+fb->h)) {
            if(native_exposure && R->blurred && row_in(i,xr)>=1.0f)
                ui_fblur_coverage_claim(fb,ICON_X-ISPR/2,g.cy-ROW_P/2,
                                       GFX_W-NAME_MARGIN,g.cy+ROW_P/2);
            continue;
        }
        const uint16_t *lut = launcher_palette(g.lev,pressed);
        unsigned key = row_image_key(i,&g,pressed,0,0);
        int low=0,high=0;unsigned mix=0;
        if(R->blurred){native_prepared_levels(native_exposure,&low,&high,&mix);native_direction_levels(native_direction,&low,&high);}
        key=gfx_hash_u(gfx_hash_u(key,(unsigned)low),(unsigned)high);
        const sprite8 *ia=low?&R->bicon[low-1].s:&R->icon[g.var];
        const sprite8 *ib=high?&R->bicon[high-1].s:&R->icon[g.var];
        int iya=low?R->bicon[low-1].y0:-ISPR/2;
        int iyb=high?R->bicon[high-1].y0:-ISPR/2;
        int ixa,ixb;geometry_trail_get(i,xr,g.cy,low,high,&ixa,&ixb);
        if(native_pose || native_exposure) {
            native_prepared_blit(fb,ia,g.ix-ISPR/2+ixa,g.cy+iya,ib,g.ix-ISPR/2+ixb,g.cy+iyb,mix,
                                 lut,key,native_pose,g.cy-BAND/2,g.cy+BAND/2,native_exposure!=0 && R->blurred,
                                 low?NULL:&R->icon[g.other],low?0:(unsigned)g.mix);
        } else icon_mix_blit(fb,&R->icon[g.var],&R->icon[g.other],g.ix-ISPR/2,g.cy-ISPR/2,lut,key,(unsigned)g.mix);
        const sprite8 *name=low?&R->bname[low-1].s:&R->name;
        int ny=low?g.cy+R->bname[low-1].y0:g.cy-R->name_base+UI_F_TITLE->ascent/2-1;
        native_name_other=high?&R->bname[high-1].s:&R->name;
        int by=high?g.cy+R->bname[high-1].y0:g.cy-R->name_base+UI_F_TITLE->ascent/2-1;
        native_name_delta=by-ny;native_name_dx=ixb-ixa;native_name_mix=mix;
        native_name_top=g.cy-BAND/2;native_name_bottom=g.cy+BAND/2;
        native_name_blurred=native_exposure!=0 && R->blurred;
        unsigned name_key=gfx_hash_u(gfx_hash_u(row_image_key(i,&g,pressed,0,1),(unsigned)low),(unsigned)high);
        draw_name(fb,name,g.ix+NAME_DX+ixa,ny,g.lev,g.cut,name_key);
        native_name_other=NULL;native_name_delta=0;native_name_mix=0;native_name_bottom=0;native_name_blurred=0;
        /* This row's icon and name are both native; its remaining opaque list
         * column is black backing, not a place to resurrect previous-pose ink. */
        if(native_exposure && R->blurred && row_in(i,xr)>=1.0f)
            ui_fblur_coverage_claim(fb,ICON_X-ISPR/2,g.cy-ROW_P/2,
                                   GFX_W-NAME_MARGIN,g.cy+ROW_P/2);
    }
    if(native_exposure){
        int first=(int)(UI_CY-xr-ROW_P/2),last=(int)(UI_CY+(n-1)*ROW_P-xr+ROW_P/2);
        if(first>0)ui_fblur_coverage_claim(fb,ICON_X-ISPR/2,0,GFX_W-NAME_MARGIN,first);
        if(last<GFX_H)ui_fblur_coverage_claim(fb,ICON_X-ISPR/2,last,GFX_W-NAME_MARGIN,GFX_H);
    }
}

static int row_at(int y)
{
    float xr = floorf(S.x + 0.5f);
    int i = (int)floorf((y - UI_CY + xr) / ROW_P + 0.5f);
    if (i < 0 || i >= napps()) return -1;
    float cy = UI_CY + i * ROW_P - xr;
    return fabsf(y - cy) <= ROW_P * 0.5f ? i : -1;
}

static int l_input(const nos_input *in)
{
    switch (in->type) {
    case IN_CANCEL: press_cancel();if(S.mode==M_DRAG)release(0);return 1;
    case IN_DOWN:
        /* a touch catches a gliding list; in the glide's last row or so (slower than
         * CATCH_V; a long fling takes ~3 s to rest) it is a tap on the row under it */
        S.was_moving = S.mode != M_IDLE && fabsf(S.v) > CATCH_V;
        S.mode = M_DRAG;                       /* the finger holds the list */
        S.grab_x = S.x;
        S.grab_dy = 0;
        press_down(S.was_moving ? -1 : row_at(in->y));
        return 1;
    case IN_DRAG_START:
        press_cancel();
        if (in->axis != AXIS_Y) { release(0); return 0; }
        S.grab_x = S.x;
        S.grab_dy = in->dy;                    /* absorb the slop: no jump when it locks */
        return 1;
    case IN_DRAG: {
        if (S.mode != M_DRAG) return 0;
        float raw = S.grab_x - (in->dy - S.grab_dy), mx = max_x();
        if (raw < 0) raw = rubber_band(raw, (float)GFX_H);
        else if (raw > mx) raw = mx + rubber_band(raw - mx, (float)GFX_H);
        S.x = raw;
        return 1;
    }
    case IN_DRAG_END:
        if (S.mode != M_DRAG) return 0;
        release(-in->vy);
        return 1;
    case IN_TAP: {
        int was = S.was_moving, i = row_at(in->y);
        press_up();
        if (S.mode == M_DRAG) release(0);
        if (was || i < 0) return 1;
        row_geo g;
        float xr = floorf(S.x + 0.5f);
        int ix = L && geo(i, xr, &g) ? g.ix : ICON_X, iy = (int)lroundf(UI_CY + i * ROW_P - xr);
        if (L && geo(i, xr, &g))
            nos_open_icon(nos_app_list[i].scr, ix, iy, nos_app_list[i].icon, k_var_r[g.var]+(k_var_r[g.other]-k_var_r[g.var])*g.mix/64.0f);
        else nos_open(nos_app_list[i].scr, ix, iy);
        return 1;
    }
    case IN_UP:
        press_up();
        if (S.mode == M_DRAG) release(0);
        return 1;
    case IN_BTN_DOUBLE: {                  /* button twice from the face: the last app */
        const nos_screen *last = sys_launcher_last();
        if (!last) return 0;
        /* the first press began opening this list: its iris keeps growing with the app
         * inside instead (no list it never meant to see, no reversal) */
        if (nos_retarget(last)) nos_feedback(FB_OPEN);
        else {
            nos_iris_x = GFX_W; nos_iris_y = UI_CY;
            nos_feedback(FB_OPEN);
            nos_replace(last, TX_IRIS);
        }
        return 1;
    }
    default:
        return 0;
    }
}

const nos_screen scr_launcher = { "LAUNCHER", l_enter, l_leave, l_tick, l_draw, l_input, SCR_NATIVE_CACHE };

/* Embedded HOME page. Its parent supplies translation/clipping and lifecycle;
 * there is no additional screen-stack entry or whole-page image cache. */
void sys_launcher_page_enter(void)
{
    l_enter();
    S.t_enter = nos_t - 2.0f; /* pager entry reveals existing content */
}
void sys_launcher_page_cancel(void)
{
    press_cancel();
    if (S.mode == M_DRAG) release(0);
}
void sys_launcher_page_leave(void) { l_leave(); }
void sys_launcher_page_tick(float dt) { l_tick(dt); }
void sys_launcher_page_draw(gfx_fb *fb) { l_draw(fb); }
int sys_launcher_page_input(const nos_input *in)
{
    if (in->type == IN_BTN || in->type == IN_BTN_DOUBLE || in->type == IN_BTN_LONG || in->type == IN_AIR)
        return 0;
    return l_input(in);
}
float sys_launcher_page_scroll(void) { return S.x; }

void sys_launcher_scroll_snapshot(nos_scroll_snapshot *s)
{
    *s=(nos_scroll_snapshot){7,2,0,1,S.mode==M_DRAG,S.x,max_x()};
}
