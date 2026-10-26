/* Independent pre-preparation derivation and ordered drawing oracle. */
static void reference_fmt_steps(char *b, int n, int v)
{
    if (v < 0) v = 0;
    if (v >= 10000) snprintf(b, (size_t)n, "%d,%03d", v / 1000, v % 1000);
    else snprintf(b, (size_t)n, "%d", v);
}

static uint8_t reference_col_value(float f, int rows, float g, int nonzero)
{
    f = floorf(m_clamp(f, 0, 1) * (float)rows + 0.5f) / (float)rows;
    if (nonzero && f < 1.0f / (float)rows) f = 1.0f / (float)rows;
    if (g >= 1.0f) return (uint8_t)floorf(f * 255.0f);
    return (uint8_t)lroundf(m_clamp(f * g, 0, 1) * 255.0f);
}

static void reference_value_block(gfx_fb *fb, float cx, float oy, int icon, const char *v, int zero, const char *unit, float p)
{
    float k = m_ink(p * 1.4f);
    dot_icon_anim(fb, icon, cx, 124 + oy, 2.6f, 2.1f, C_SEC, p, DOT_REVEAL_CENTER);
    dot_style st = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, zero ? C_DIS : C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, cx, 148 + oy, v, 1, p, DOT_REVEAL_RANDOM, 3);
    ui_label(fb, (int)cx, (int)(214 + oy), unit, ui_dim(C_SEC, k), UI_CENTER);
}

static void draw_today_reference(gfx_fb *fb, float oy)
{
    ui_label(fb, UI_CX, (int)(UI_TITLE_Y + oy), "TODAY", C_SEC, UI_CENTER);
    char b[16];
    int metric = nos_prefs.units_metric != 0;
    float dist = metric ? s_act.km : s_act.km * 0.621371f;
    if (dist < 10) snprintf(b, sizeof b, "%.1f", (double)dist);
    else snprintf(b, sizeof b, "%d", (int)(dist + 0.5f));
    reference_value_block(fb, UI_CX - 124, oy, DOT_ICON_ROUTE, b, dist < 0.05f, metric ? "KM" : "MI", A.blk_p[0]);
    snprintf(b, sizeof b, "%d", (int)(s_act.kcal + 0.5f));
    reference_value_block(fb, UI_CX, oy, DOT_ICON_FLAME, b, s_act.kcal < 0.5f, "ACT KCAL", A.blk_p[1]);
    snprintf(b, sizeof b, "%d", s_act.active_min);
    reference_value_block(fb, UI_CX + 124, oy, DOT_ICON_TIMER, b, s_act.active_min == 0, "ACT MIN", A.blk_p[2]);

    /* hourly chart: 24 dot columns; past = grey, this hour = white, future = grid only */
    const int rows = 7;
    const float pitch = 13.0f, d = 9.0f;
    float x0 = UI_CX - 12 * pitch, y0 = 252 + oy;
    int maxv = 400;
    for (int i = 0; i < 24; i++) if (s_act.hourly[i] > maxv) maxv = s_act.hourly[i];
    uint8_t past[24], now[1] = { 0 };
    for (int i = 0; i < 24; i++) {
        float g = A.col_g[i];
        uint8_t q = reference_col_value((float)s_act.hourly[i] / (float)maxv, rows, g, s_act.hourly[i] > 0);
        past[i] = i < s_hour ? q : 0;
        if (i == s_hour) now[0] = q;
    }
    dot_columns(fb, x0, y0, 24, rows, pitch, d, past, C_SEC, C_GHOST);
    if (s_hour >= 0 && s_hour < 24)
        dot_columns(fb, x0 + s_hour * pitch, y0, 1, rows, pitch, d, now, C_WHITE, C_GHOST);
    /* now marker (the screen's one red element unless the goal ring overflows) */
    float nx = x0 + (s_hour + 0.5f) * pitch, ny = y0 + rows * pitch + 10;
    if (A.now_on) dot_disc(fb, nx, ny, 6.0f, A.hero.over ? C_WHITE : C_RED);
    static const char *k_ax[4] = { "00", "06", "12", "18" };
    for (int i = 0; i < 4; i++) {
        float ax = x0 + (i * 6 + 0.5f) * pitch;
        if (fabsf(ax - nx) < 46) continue;
        ui_label(fb, (int)ax, (int)(y0 + rows * pitch + 40), k_ax[i], C_DIS, UI_CENTER);
    }
    ui_label(fb, (int)nx, (int)(y0 + rows * pitch + 40), "NOW", C_SEC, UI_CENTER);
    char total[36];
    snprintf(total, sizeof total, "TOTAL %s%d W/ REST", s_act.kcal_assumed ? "~" : "", (int)(s_act.kcal_total + 0.5f));
    ui_label(fb, UI_CX, (int)(y0 + rows * pitch + 72), total, ui_dim(C_DIS, m_ink(A.blk_p[1] * 1.4f)), UI_CENTER);
}

static void draw_week_reference(gfx_fb *fb, float oy)
{
    uint32_t sum = 0, maxv = (uint32_t)s_act.goal;
    int ndays = 0;
    for (int i = 0; i < 7; i++) {
        uint32_t v = i == 6 ? (uint32_t)s_act.steps : s_act.week[i];
        if (i == 6 && s_act.week[6] > v) v = s_act.week[6];
        /* DAILY AVG over the six completed days: today's partial count would pull it
         * down every morning; today stays visible as its own (white) column */
        if (s_act.week_valid & (1u << i)) { sum += v; ndays++; }
        if (v > maxv) maxv = v;
    }
    ui_label(fb, UI_CX, (int)(UI_TITLE_Y + oy), "LAST 7 DAYS", C_SEC, UI_CENTER);
    char b[16];
    reference_fmt_steps(b, sizeof b, ndays ? (int)(sum / (uint32_t)ndays) : 0);
    dot_style st = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, ndays ? C_WHITE : C_DIS, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 112 + oy, b, 1, A.wk_p, DOT_REVEAL_RANDOM, 5);
    ui_label(fb, UI_CX, (int)(180 + oy), "AVG / INCLUDES TODAY", C_DIS, UI_CENTER);

    const int rows = 9;
    const float pitch = 10.0f, d = 7.6f, slot = 42.0f;
    float y0 = 222 + oy, chart_h = rows * pitch;
    float scale = (float)maxv * 1.08f;
    /* goal line first (bars paint over it), on a gap between dot rows */
    float gy = y0 + chart_h - chart_h * ((float)s_act.goal / scale);
    gy = y0 + floorf((gy - y0) / pitch + 0.5f) * pitch;
    if (A.goal_on && gy > y0 && gy < y0 + chart_h) {
        float gx0 = UI_CX - 3 * slot - pitch, gx1 = UI_CX + 3 * slot + pitch;
        dot_bar(fb, gx0, gy, 41, (gx1 - gx0) / 40.0f, 2.2f, 41, C_DIS, 0);
        ui_label(fb, (int)(gx1 + 12), (int)(gy + 6), "GOAL", C_DIS, UI_LEFT);
    }
    for (int i = 0; i < 7; i++) {
        uint32_t v = i == 6 ? (uint32_t)s_act.steps : s_act.week[i];
        if (i == 6 && s_act.week[6] > v) v = s_act.week[6];
        float g = A.bar_g[i];
        uint8_t q = reference_col_value(scale > 0 ? (float)v / scale : 0, rows, g, v > 0);
        uint8_t vals[2] = { q, q };
        float cx = UI_CX + (i - 3) * slot;
        dot_columns(fb, cx - pitch, y0, 2, rows, pitch, d, vals, i == 6 ? C_WHITE : C_SEC, C_GHOST);
        static const char *k_d = "SMTWTFS";
        char l[2] = { k_d[((s_wday - (6 - i)) % 7 + 7) % 7], 0 };
        ui_label(fb, (int)cx, (int)(y0 + chart_h + 32), l, i == 6 ? C_WHITE : C_DIS, UI_CENTER);
    }
}
