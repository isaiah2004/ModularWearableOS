/* The resting locked face as one full-resolution RGB565 picture (PSRAM, 434 KB).
 * A pager drag below the blur threshold used to re-rasterise the whole sky every frame
 * (every stripe is dirty while the page slides); the snapshot turns that into a row copy.
 * The clock, hold ring and unread dot stay live on top. Key = S.scene_ink: the sky only
 * changes at the 0.05-day ephemeris cadence (~72 min), so it rebakes about that often.
 * Baked in 16-row slices on resting ticks only; a stale or partial picture is never shown
 * (draw falls back to the retained replay, the old path). */
#include "sprite.h"
extern void sys_home_nav_snapshot(int *page, float *offset, int *dragging);
static uint16_t *orr_snap;
static unsigned orr_snap_key, orr_snap_bake_key;
static int orr_snap_valid, orr_snap_row = -1;
#define ORR_SNAP_BUDGET_US 6000u

static int orr_snap_ready(void)
{
    return orr_snap && orr_snap_valid && orr_snap_key == S.scene_ink && S.clock_overlay;
}
static void orr_snap_free(void)
{
    hal_big_free(orr_snap); orr_snap = 0; orr_snap_valid = 0; orr_snap_row = -1;
}
/* Replay the retained scene over C_BG into the picture, 16 rows at a time, until the
 * budget runs out; publishes when the last row lands. Same writer as the live path. */
static void orr_snap_bake(uint32_t budget_us)
{
    uint32_t start = hal_micros();
    while (orr_snap_row >= 0 && orr_snap_row < GFX_H) {
        int rows = GFX_H - orr_snap_row < 16 ? GFX_H - orr_snap_row : 16;
        uint16_t *px = orr_snap + orr_snap_row * GFX_W;
        for (int i = 0; i < rows * GFX_W; i++) px[i] = C_BG;
        gfx_fb f = {0};
        f.px = px; f.h = rows; f.y_base = orr_snap_row; f.cx1 = GFX_W;
        orr_baked_background = 1; orr_replay(&f); orr_baked_background = 0;
        orr_snap_row += rows;
        if (hal_micros() - start >= budget_us) break;
    }
    if (orr_snap_row == GFX_H) { orr_snap_key = orr_snap_bake_key; orr_snap_valid = 1; orr_snap_row = -1; }
}
static void orr_snap_tick(const face_ctx *c)
{
    if (orr_snap_ready()) return;
    int page = 0, dragging = 0; float offset = 0;
    sys_home_nav_snapshot(&page, &offset, &dragging);
    /* Rest only: the face on the glass, no finger, sky sweeps finished. A swipe aborts. */
    int rest = !c->aod && S.clock_overlay && S.scene_valid && S.list && !S.list->overflow &&
               !S.hold && !S.belt_left && !S.small_left && offset == 0 && !dragging &&
               !nos_transitioning() && nos_top() == &scr_home;
    if (!rest) { orr_snap_row = -1; return; }
    if (!orr_snap && !(orr_snap = hal_big_alloc(GFX_W * GFX_H * 2))) return;
    if (orr_snap_row < 0 || orr_snap_bake_key != S.scene_ink) {
        orr_snap_valid = 0; orr_snap_row = 0; orr_snap_bake_key = S.scene_ink;
    }
    orr_snap_bake(ORR_SNAP_BUDGET_US);
}
/* Raster path only (record mode keeps the scene's damage commands). A dimmed backdrop
 * replays instead: dimming per primitive and dimming the picture can round apart. */
static int orr_snap_draw(gfx_fb *fb)
{
    if (fb->dim || !orr_snap_ready()) return 0;
    sprite sp = { orr_snap, GFX_W, GFX_H };
    sprite_blit(fb, &sp, 0, 0, orr_snap_key);
    return 1;
}
