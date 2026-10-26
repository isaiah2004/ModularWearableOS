/* Independent callbacks before row culling. Do not regenerate from guarded code. */
static void small_reference(gfx_fb *fb, int x, int y, const char *s, uint16_t c, int align)
{
    ui_text(fb, UI_F_SMALL, x, y, s, c, align, 1);
}
static void hero_reference(gfx_fb *fb, int y, const char *s, float pitch)
{
    dot_style st = {DOT_FONT_5X7, pitch, pitch * .8f, C_PRI, 0, 1, 1};
    dot_text_a(fb, &st, 233, (float)y, s, UI_CENTER);
}
static void page_heat_reference(gfx_fb *fb, const body_state *b)
{
    const heat_profile *p = &profiles[b->ob];
    ui_title(fb, "HEAT", 0);
    small_reference(fb, 233, 94,
          b->ob == OB_MERCURY ? "EQUATOR / SOLAR DAY 176 D"
          : b->ob == OB_MOON  ? "EQUATOR / LUNAR DAY 29.5 D"
                              : p->title,
          C_SEC, UI_CENTER);
    gfx_fill_rect(fb, 100, 124, 1, 191, C_LINE2);
    gfx_fill_rect(fb, 100, 314, 281, 1, C_LINE2);
    fk_dots(fb, b->graph_n, 16, point_at, b->graph);
    for (int i = 0; i < p->nx; i++)
        small_reference(fb, (int)(100 + 280 * (p->xt[i].v - p->x0) / (p->x1 - p->x0)), 338,
              b->ob == OB_SUN && i == 2 ? "SURF" : p->xt[i].s, C_SEC, UI_CENTER);
    /* y ticks stored alongside graph to keep draw free of libm. */
    for (int i = 0; i < p->ny; i++)
        small_reference(fb, 92, (int)b->graph[49 + i].y + 4, p->yt[i].s, C_SEC, UI_RIGHT);
    if (p->mark_text) {
        float xy = b->mark;
        for (int i = 0; i < 32; i++) {
            int xx = p->vertical ? 100 + i * 9 : (int)xy, yy = p->vertical ? (int)xy : 124 + i * 6;
            if (xx <= 380 && yy <= 314) gfx_fill_rect(fb, xx, yy, 2, 2, C_LINE2);
        }
        if (b->ob == OB_VENUS) small_reference(fb, 377, (int)xy - 31, "EARTH-LIKE", C_SEC, UI_RIGHT);
        small_reference(fb, p->vertical ? 377 : (int)xy, p->vertical ? (int)xy - 11 : 112, p->mark_text, C_SEC,
              p->vertical ? UI_RIGHT : UI_CENTER);
    }
    hero_reference(fb, 357, b->range, 4.0f);
    small_reference(fb, 233, 401, b->ob == OB_SUN ? "K / LOW - HIGH" : "DEG C / LOW - HIGH", C_SEC, UI_CENTER);
    /* Two short lines fit the narrowest lower chord, including font descent. */
    ui_text_wrap(fb, UI_F_SMALL, 233, 418, 230, 17, 2, b->ob == OB_EARTH ? "CLIMATE MEAN / MODEL" : p->note,
                 C_SEC, UI_CENTER);
}
static void page_globe_reference(gfx_fb *fb, const body_state *b, const sprite *s, unsigned key, int preview)
{
    if (s->px && key)
        sprite_blit(fb, s, 233 - s->w / 2, 214 - s->h / 2, key);
    else
        gfx_fill_circle(fb, 233, 214, (float)radius(b->ob), ui_dim(ob_tint[b->ob], .4f));
    ui_title(fb, ob_name[b->ob], 0);
    small_reference(fb, 233, 92, C.mode == 0 ? "[ PHOTO ]" : C.mode == 1 ? "[ HEAT ]" : "[ DOT ]", C_SEC, UI_CENTER);
    if (!preview && C.load_stage < 3) small_reference(fb, 233, 214, "[ LOADING ]", C_PRI, UI_CENTER);
    hero_reference(fb, 346, b->hero, 4.0f);
    small_reference(fb, 233, 387, b->unit, C_SEC, UI_CENTER);
    small_reference(fb, 233, 407, b->sub, C_SEC, UI_CENTER);
    gfx_fill_pill(fb, 169, 430, 297, 18, C_SURF2);
    float x = C.free ? 275 : 191;
    gfx_fill_pill(fb, x - 25, 430, x + 25, 14, C.pressed ? C_SEC : C_PRI);
    small_reference(fb, 191, 435, "NOW", C.free ? C_SEC : C_BG, UI_CENTER);
    small_reference(fb, 275, 435, "FREE", C.free ? C_BG : C_SEC, UI_CENTER);
}
static void page_moons_reference(gfx_fb *fb, const body_state *b)
{
    ui_title(fb, b->ob == OB_EARTH ? "MOON & CRAFT" : "MOONS & CRAFT", 0);
    small_reference(fb, 233, 94, b->count, C_SEC, UI_CENTER);
    for (int i = 0; i < b->n; i++)
        if (!b->items[i].craft) gfx_ring(fb, 233, 236, b->items[i].r, 1, C_LINE);
    fk_dots(fb, b->minor_n, 32, point_at, b->minor);
    if (b->ob == OB_SATURN) gfx_ring(fb, 233, 236, b->planet_r * 1.4f, 3, C_LINE2);
    gfx_fill_circle(fb, 233, 236, b->planet_r, ob_tint[b->ob]);
    for (int i = 0; i < b->direction_n; i++) {
        const moon_item *d = &b->directions[i];
        dot_disc(fb, d->x, d->y, 5, C_SEC);
        small_reference(fb, (int)d->lx, (int)d->ly, d->name, C_SEC, UI_CENTER);
    }
    for (int i = 0; i < b->n; i++) {
        const moon_item *m = &b->items[i];
        if (m->craft) {
            gfx_line(fb, m->x - 3, m->y - 3, m->x + 3, m->y + 3, 1, C_SEC);
            gfx_line(fb, m->x - 3, m->y + 3, m->x + 3, m->y - 3, 1, C_SEC);
        } else
            dot_disc(fb, m->x, m->y, 6, C_PRI);
        small_reference(fb, (int)m->lx, (int)m->ly, m->name, C_SEC, UI_CENTER);
    }
    if (!b->n) {
        ui_label(fb, 233, 332, "NO MOONS", C_SEC, UI_CENTER);
        if (b->ob == OB_MERCURY) {
            ui_text_wrap(fb, UI_F_SMALL, 233, 361, 300, 18, 2, b->bepi, C_SEC, UI_CENTER);
            small_reference(fb, 233, 410, "CRAFT POSITION / SKY PACK", C_DIS, UI_CENTER);
        } else if (b->ob == OB_VENUS) {
            small_reference(fb, 233, 360, "NO ORBITER SINCE AKATSUKI", C_SEC, UI_CENTER);
            small_reference(fb, 233, 381, "ENDED / SEP 2025", C_SEC, UI_CENTER);
        }
    }
}
static void fact_row_reference(gfx_fb *fb, int y, const char *k, const char *v)
{
    int x0, x1;
    ui_row_span((float)y, 34, &x0, &x1);
    x0 += 12;
    x1 -= 12;
    small_reference(fb, x0, y, k, C_SEC, UI_LEFT);
    /* Long facts take their own second line rather than colliding at the edge. */
    small_reference(fb, x1, y + 20, v, C_PRI, UI_RIGHT);
    gfx_fill_rect(fb, x0, y + 27, x1 - x0, 1, C_LINE);
}
static void page_facts_reference(gfx_fb *fb, const body_state *b)
{
    ui_title(fb, "FACTS", 0);
    int row = 0;
    char v[40];
    if (b->ob != OB_SUN && b->ob != OB_EARTH && b->ob != OB_MOON) {
        snprintf(v, sizeof v, "%.2f AU", b->sun_dist);
        fact_row_reference(fb, 105, "FROM SUN NOW", v);
        row++;
    }
    for (int i = 0; i < 5; i++)
        if (facts[b->ob][i][0]) fact_row_reference(fb, 105 + row++ * 48, facts[b->ob][i][0], facts[b->ob][i][1]);
    if (b->next[0]) {
        small_reference(fb, 233, 416, "NEXT", C_SEC, UI_CENTER);
        ui_text_wrap(fb, UI_F_SMALL, 233, 434, 250, 16, 1, b->next, C_PRI, UI_CENTER);
    }
}