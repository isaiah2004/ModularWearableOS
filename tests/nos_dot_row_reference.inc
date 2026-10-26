/* Independent pre-hoist cell/text/icon raster entry points. Helpers share the
 * unchanged glyph art and coverage masks, never the candidate row bounds. */
static void row_reference_draw_cells(const drawctx *c, const dglyph *g, float ox, float oy, int col0,
                       int box0, int boxw, int boxrows, unsigned gidx)
{
    const dclip *k = c->k;
    float pitch = c->pitch;
    int rows = g ? g->rows : 0;
    if (boxrows > rows && c->ghost) rows = boxrows;
    /* glyph rows overlapping this stripe only */
    int r0 = ifloor(((float)k->ylo - c->ext - oy) / pitch - 0.5f);
    int r1 = iceil(((float)k->yhi + c->ext - oy) / pitch + 0.5f);
    if (r0 < 0) r0 = 0;
    if (r1 > rows) r1 = rows;
    int cmin = box0 < col0 ? box0 : col0;
    int cmax = col0 + (g ? g->w : 0);
    if (box0 + boxw > cmax) cmax = box0 + boxw;
    for (int r = r0; r < r1; r++) {
        float cy = oy + ((float)r + 0.5f) * pitch;
        unsigned bits = (g && r < g->rows) ? g->bits[r] : 0u;
        int ghost_row = c->ghost && r < boxrows;
        if (!bits && !ghost_row) continue;
        for (int cc = cmin; cc < cmax; cc++) {
            int gcbit = cc - col0;
            int lit = gcbit >= 0 && gcbit < 16 && ((bits >> gcbit) & 1u);
            int inbox = ghost_row && cc >= box0 && cc < box0 + boxw;
            if (!lit && !inbox) continue;
            float cx = ox + ((float)cc + 0.5f) * pitch;
            if (cx + c->ext <= (float)k->c0 || cx - c->ext >= (float)k->c1) continue;
            if (!lit) { stamp(k, cx, cy, c->d, c->ghost); continue; }
            if (c->p >= 1.0f) { stamp(k, cx, cy, c->d, c->color); continue; }
            float t = reveal_t(c->mode, cc, r, c->gcols, c->grows, c->seed, gidx * 256u + (unsigned)(r * 16 + cc));
            float gr = reveal_g(c->mode, t, c->p);
            if (gr < 1.0f && c->ghost) stamp(k, cx, cy, c->d, c->ghost);
            if (gr > 0.0f) stamp(k, cx, cy, c->d * gr, c->color);
        }
    }
}

static int row_reference_text_impl(gfx_fb *fb, const dot_style *st, float x, float y, const char *s,
                     int align, float p, int mode, unsigned seed)
{
    if (!st || st->pitch <= 0.0f || !tables()) return 0;
    dlayout L;
    float pitch = st->pitch, d = st_dot(st);
    if (!fb->rec) {
        /* stripe rejection before any per-glyph work: rows incl. descenders */
        float e = (d > pitch ? (d - pitch) * 0.5f : 0.0f) + 1.0f;
        float lo = (float)(fb->y_base - fb->oy);
        if (y + (float)(k_fh[st_font(st)] + 2) * pitch + e <= lo || y - e >= lo + (float)fb->h) {
            layout(&L, st, s, 0);
            return (int)((float)L.cells * pitch + 0.5f);
        }
    }
    layout(&L, st, s, 1);
    float wpx = (float)L.cells * pitch;
    int ret = (int)(wpx + 0.5f);
    if (!L.n) return 0;
    p = qprog(p);
    if (mode == DOT_REVEAL_NONE && p >= 1.0f) p = 1.0f;
    float xl = align_left(x, wpx, align);
    float pad = (d > pitch ? (d - pitch) * 0.5f : 0.0f) + 1.0f;
    int vrows = L.rows;
    if (fb->rec) {
        unsigned h = hash_style(0xD07u, st);
        h = gfx_hash_f(gfx_hash_f(h, xl), y);
        h = gfx_hash_f(gfx_hash_u(gfx_hash_u(h, (unsigned)mode), seed), p);
        for (const unsigned char *c = (const unsigned char *)s; *c; c++) h = gfx_hash_u(h, *c);
        gfx_rec_cmd(fb, ifloor(xl - pad), ifloor(y - pad), iceil(xl + wpx + pad),
                    iceil(y + (float)vrows * pitch + pad), h);
        return ret;
    }
    dclip k;
    clip_init(&k, fb);
    if (!clip_hit(&k, xl - pad, y - pad, xl + wpx + pad, y + (float)vrows * pitch + pad)) return ret;
    if (p <= 0.0f && !st->ghost) return ret;
    drawctx c = { &k, pitch, d, st->color, st->ghost, p, mode, seed, L.cells, vrows, d * 0.5f + 1.0f, 0, 0 };
    int font = st_font(st), boxrows = k_fh[font];
    for (int i = 0; i < L.n; i++) {
        const dslot *sl = &L.s[i];
        const dglyph *g = sl->slot == SLOT_BLANK ? NULL : glyph_of(font, sl->slot);
        int cmin = sl->box0 < sl->col0 ? sl->box0 : sl->col0;
        int cmax = sl->box0 + sl->boxw;
        if (g && sl->col0 + g->w > cmax) cmax = sl->col0 + g->w;
        if (xl + (float)cmax * pitch + pad <= (float)k.c0 || xl + (float)cmin * pitch - pad >= (float)k.c1)
            continue;                                   /* glyph outside the column clip */
        row_reference_draw_cells(&c, g, xl, y, sl->col0, sl->box0, sl->boxw, boxrows, (unsigned)i);
    }
    return ret;
}

void row_reference_dot_icon_anim(gfx_fb *fb, int icon, float cx, float cy, float pitch, float dot,
                   uint16_t color, float p, int mode)
{
    if (icon <= DOT_ICON_NONE || icon >= DOT_ICON_COUNT || pitch <= 0.0f || !tables()) return;
    const dglyph *g = &g_tab[DOT_FONT_COUNT * NSLOT + icon];
    if (!g->w) return;
    if (dot <= 0.0f) dot = pitch * 0.78f;
    p = qprog(p);
    float half = 5.5f * pitch;
    float pad = (dot > pitch ? (dot - pitch) * 0.5f : 0.0f) + 1.0f;
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(0x1C0u, (unsigned)icon), color);
        h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(h, cx), cy), pitch), dot);
        h = gfx_hash_f(gfx_hash_u(h, (unsigned)mode), p);
        gfx_rec_cmd(fb, ifloor(cx - half - pad), ifloor(cy - half - pad),
                    iceil(cx + half + pad), iceil(cy + half + pad), h);
        return;
    }
    if (p <= 0.0f) return;
    dclip k;
    clip_init(&k, fb);
    if (!clip_hit(&k, cx - half - pad, cy - half - pad, cx + half + pad, cy + half + pad)) return;
    drawctx c = { &k, pitch, dot, color, 0, p, mode, (unsigned)icon, 11, 11, dot * 0.5f + 1.0f, 0, 0 };
    row_reference_draw_cells(&c, g, cx - half, cy - half, 0, 0, 0, 0, (unsigned)icon);
}

