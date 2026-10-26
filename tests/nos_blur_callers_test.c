/* Execute the actual app tick functions against a controlled cache selection.
 * The cache's pixel/velocity policy has its own RGB565 reference tests. These
 * checks catch callers applying the specialized image ladder a second time. */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#if APP_CASE == 1
#include "../nos/apps/out_weather.c"
#elif APP_CASE == 2 || APP_CASE == 3
#include "../nos/apps/set_modules.c"
#elif APP_CASE == 4
#include "../nos/apps/set_about.c"
#elif APP_CASE == 5
#include "../nos/apps/set_diag.c"
#elif APP_CASE == 6
#include "../nos/apps/time_alarm.c"
#elif APP_CASE == 7
#include "../nos/apps/time_stopwatch.c"
#else
#error APP_CASE required
#endif

double nos_t = 10.0f;
int nos_mblur = 2;
static int selected, ticked, queried;
static float expected_step;
static const void *last_owner;

float ui_list_offset(const ui_list *list) { (void)list; return 100.0f; }
void ui_row_span(float y, float pad, int *x0, int *x1)
{
    (void)y; (void)pad; *x0 = 20; *x1 = 446;
}
int ui_fblur_level(float step)
{
    float speed = fabsf(step) * M_FPS;
    return speed >= 1680 ? 3 : speed >= 840 ? 2 : speed >= 360 ? 1 : 0;
}
void ui_fblur_tick(const void *id, int held, float step, int i0, int i1,
                   int n, int row_h, int x0, int x1, int bands,
                   ui_blur_row_fn fn, void *ctx)
{
    (void)held; (void)i0; (void)i1; (void)n; (void)row_h;
    (void)x0; (void)x1; (void)bands; (void)fn; (void)ctx;
    assert(fabsf(step - expected_step) < 0.0001f);
    ticked++; last_owner = id;
}
int ui_fblur_active(const void *id)
{
    assert(ticked == 1 && id == last_owner);
    queried++;
    return selected;
}

static void setup(float step, int held)
{
    ticked = queried = 0;
    last_owner = 0;
    expected_step = step;
    float previous = 100.0f - step;
#if APP_CASE == 1
    memset(&W, 0, sizeof W);
    s_have_view = 1; s_cur.ws = WS_OK; s_swap_t = 0;
    W.sc.x = 100; W.band_prev = previous; W.dragging = held;
#elif APP_CASE == 2
    memset(&ML, 0, sizeof ML);
    ML.nunits = 20; ML.fb_prev = previous; ML.L.dragging = held;
#elif APP_CASE == 3
    memset(&D, 0, sizeof D);
    D.m = M_HAPTIC; D.L.n = 5; D.fb_prev = previous; D.L.dragging = held;
#elif APP_CASE == 4
    memset(&A, 0, sizeof A);
    A.nunits = 20; s_band_prev = previous; A.L.dragging = held;
#elif APP_CASE == 5
    memset(&DL, 0, sizeof DL); memset(&G, 0, sizeof G);
    G.view = VIEW_LIST; G.pview = VIEW_NONE;
    DL.nunits = 20; s_band_prev = previous; DL.L.dragging = held;
#elif APP_CASE == 6
    memset(&A, 0, sizeof A);
    A.n = 3; A.sc.x = 100; A.fb_prev = previous;
    A.build = 1; A.pressed = -1; A.dragging = held;
#elif APP_CASE == 7
    memset(&S, 0, sizeof S);
    S.sw.nlaps = 8; S.sc.x = 100; S.fb_prev = previous;
    S.rows_k = S.build = 1; S.dragging = held;
#endif
}

static int run_tick(void)
{
#if APP_CASE == 1
    band_tick(1.0f / M_FPS); return W.band_lv;
#elif APP_CASE == 2
    mband_tick(1.0f / M_FPS); return ML.fb_lv;
#elif APP_CASE == 3
    pband_tick(1.0f / M_FPS); return D.fb_lv;
#elif APP_CASE == 4 || APP_CASE == 5
    band_tick(1.0f / M_FPS); return s_band_lv;
#elif APP_CASE == 6
    fblur_tick(1.0f / M_FPS); return A.fb_lv;
#elif APP_CASE == 7
    fblur_tick(1.0f / M_FPS); return S.fb_lv;
#endif
}

int main(void)
{
    const float speeds[] = {0, 15, 150, 290, 900, 1800};
    for (unsigned i = 0; i < sizeof speeds / sizeof speeds[0]; i++)
    for (int sign = -1; sign <= 1; sign += 2)
    for (int held = 0; held <= 1; held++)
    for (int active = 0; active <= 1; active++) {
        selected = active;
        setup(sign * speeds[i] / M_FPS, held);
        assert(run_tick() == selected);
        assert(ticked == 1 && queried == 1);
    }
    /* Cache preparation must never replace an authored in-progress state. */
    selected = 1;
#if APP_CASE == 1
    setup(290.0f / M_FPS, 1); W.t0 = nos_t;
    assert(!run_tick() && ticked == 1);
    setup(290.0f / M_FPS, 1); s_swap_t = nos_t;
    assert(!run_tick() && ticked == 1);
    setup(290.0f / M_FPS, 1); s_have_view = 0;
    assert(!run_tick() && !ticked);
#elif APP_CASE == 5
    setup(290.0f / M_FPS, 1); G.covered = 1;
    assert(!run_tick() && !ticked);
    setup(290.0f / M_FPS, 1); G.pend = PEND_RELAYOUT;
    assert(!run_tick() && !ticked);
#elif APP_CASE == 6
    setup(290.0f / M_FPS, 1); A.build = 0.5f;
    assert(!run_tick() && ticked == 1);
    setup(290.0f / M_FPS, 1); A.pressed = 0;
    assert(!run_tick() && ticked == 1);
    setup(290.0f / M_FPS, 1); A.n = 0;
    assert(!run_tick() && !ticked);
#elif APP_CASE == 7
    setup(290.0f / M_FPS, 1); S.rows_k = 0.5f;
    assert(!run_tick() && ticked == 1);
    setup(290.0f / M_FPS, 1); S.build = 0.5f;
    assert(!run_tick() && ticked == 1);
    setup(290.0f / M_FPS, 1); S.ins_x = 1; S.fb_prev -= 1;
    assert(!run_tick() && ticked == 1);
    setup(290.0f / M_FPS, 1); S.sw.nlaps = 0;
    assert(!run_tick() && !ticked);
#endif
    printf("PASS app %d uses selected cache shutter and preserves live states\n", APP_CASE);
    return 0;
}
