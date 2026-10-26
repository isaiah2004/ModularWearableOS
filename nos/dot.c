#include "cost_profile.h"
/* dot.c — dot-matrix type, icons and glyph rings (see dot.h).
 *
 * Rendering model
 *  - Every dot is a stamp of a cached 8-bit coverage mask: one mask per quantised
 *    diameter (0.25 px steps, up to 40 px) and per sub-pixel phase (1/4 px below
 *    16 px, 1/2 px above), baked lazily into PSRAM (hal_big_alloc) the first time a
 *    size is used. Each mask row carries its ink span and its solid (coverage 255)
 *    run, so a stamp is: blend the rim pixels, plain-store the interior. No per-pixel
 *    distance maths at draw time.
 *  - Glyph / icon bitmaps are hand-drawn ASCII art in dot_glyphs.c, parsed once into
 *    row bitmasks (also PSRAM).
 *  - draw() runs once per 16-row stripe, so every call first rejects against the
 *    stripe's logical rows and the column clip, and text only walks the glyph rows
 *    that overlap the stripe.
 *  - Record mode (fb->rec): one damage command per call (bbox + hash of everything
 *    that changes pixels) — except dot_ring, which records one command per arc chunk
 *    (up to 16) so a moving progress tip repaints a small box, not the whole ring.
 *    Animation progress is quantised (1/1024) and the quantised value is what is
 *    drawn, so equal hashes always mean equal pixels.
 *
 * Conventions (where dot.h leaves room)
 *  - dot_style.gap == 0 means the default 1 cell; DOT_GAP_NONE (0xFF) means 0.
 *    dot_style.dot <= 0 means pitch * 0.78.
 *  - mono: digits, letters and symbols advance the font's nominal width with the
 *    glyph centred; the narrow punctuation . , : ; ! ' | keep their own width (still
 *    fixed, so a clock like 12:45 stays compact and never jitters).
 *  - Text width = cells * pitch, no trailing gap. Lowercase descenders (g j p q y ,)
 *    hang up to 2 rows below dot_text_height().
 *  - dot_bar: (x, y) is the centre of the first dot. dot_columns: (x, y) is the
 *    top-left of the cell area. dot_icon / dot_ring / dot_disc: centres.
 *  - Partially lit dots (ring/bar/columns ends) grow by area (d * sqrt(f)) so the
 *    brightness of a moving end changes linearly.
 *  - Strings are ASCII; the degree sign may be given as UTF-8 "\xC2\xB0" or Latin-1
 *    0xB0. Glyphs missing from a font fall back to uppercase, then to a blank cell. */
#include "dot.h"
#include "gfx_cost.h"
#include "hal.h"
#include <string.h>
#include <stdlib.h>

#define DOT_GAP_NONE 0xFF
#define MAXCH        64          /* glyphs per string handled */
#define NSLOT        96          /* ASCII 32..126 + degree */
#define SLOT_DEG     95
#define MASK_QMAX    160         /* 40 px in 0.25 px steps */
#define PQ           1024.0f     /* animation progress quantum */

#ifdef DOT_STATS                 /* host profiling (tools/dot_preview.c) */
unsigned long dot_stat_solid, dot_stat_blend, dot_stat_stamps;
#define STAT(x) (x)
#else
#define STAT(x) ((void)0)
#endif

/* ---- glyph art (dot_glyphs.c) -------------------------------------------- */
typedef struct { uint8_t w; const char *art; } dot_art;
extern const dot_art dot_art_5x7[NSLOT], dot_art_tall[NSLOT], dot_art_3x5[NSLOT];
extern const char *const dot_art_icons[DOT_ICON_COUNT];

typedef struct { uint8_t w, rows; uint16_t bits[11]; } dglyph;   /* bit c = column c */

static const uint8_t k_fw[DOT_FONT_COUNT] = { 5, 5, 3 };   /* nominal cell width */
static const uint8_t k_fh[DOT_FONT_COUNT] = { 7, 9, 5 };   /* cap height in cells */

static dglyph *g_tab;            /* [DOT_FONT_COUNT * NSLOT + DOT_ICON_COUNT] */

static void parse_art(dglyph *g, int w, const char *art)
{
    memset(g, 0, sizeof *g);
    if (!art || w <= 0 || w > 16) return;
    size_t n = strlen(art);
    if (n == 0 || n % (size_t)w) return;
    int rows = (int)(n / (size_t)w);
    if (rows > 11) rows = 11;
    g->w = (uint8_t)w;
    g->rows = (uint8_t)rows;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < w; c++)
            if (art[r * w + c] == '#') g->bits[r] |= (uint16_t)(1u << c);
}

static int tables(void)
{
    if (g_tab) return 1;
    int n = DOT_FONT_COUNT * NSLOT + DOT_ICON_COUNT;
    dglyph *t = (dglyph *)hal_big_alloc((uint32_t)(sizeof(dglyph) * (unsigned)n));
    if (!t) return 0;
    const dot_art *fonts[DOT_FONT_COUNT] = { dot_art_5x7, dot_art_tall, dot_art_3x5 };
    for (int f = 0; f < DOT_FONT_COUNT; f++)
        for (int i = 0; i < NSLOT; i++)
            parse_art(&t[f * NSLOT + i], fonts[f][i].w, fonts[f][i].art);
    for (int i = 0; i < DOT_ICON_COUNT; i++)
        parse_art(&t[DOT_FONT_COUNT * NSLOT + i], 11, dot_art_icons[i]);
    g_tab = t;
    return 1;
}

static const dglyph *glyph_of(int font, int slot)
{
    const dglyph *g = &g_tab[font * NSLOT + slot];
    if (g->w) return g;
    int ch = slot + 32;
    if (ch >= 'a' && ch <= 'z') {
        g = &g_tab[font * NSLOT + (ch - 'a' + 'A' - 32)];
        if (g->w) return g;
    }
    return NULL;
}

static int narrow_punct(int slot)
{
    int ch = slot + 32;
    return ch == '.' || ch == ',' || ch == ':' || ch == ';' || ch == '!' || ch == '\'' || ch == '|';
}

/* Decode the next character to a table slot; -1 = end of string. */
static int next_slot(const unsigned char **ps)
{
    for (;;) {
        unsigned c = **ps;
        if (!c) return -1;
        (*ps)++;
        if (c >= 32 && c < 127) return (int)c - 32;
        if (c == 0xC2 && **ps == 0xB0) { (*ps)++; return SLOT_DEG; }
        if (c == 0xB0) return SLOT_DEG;
        if (c >= 0x80) {                       /* other UTF-8: skip continuation bytes */
            while ((**ps & 0xC0) == 0x80) (*ps)++;
            return '?' - 32;
        }
        /* control characters: ignored */
    }
}

/* ---- small maths (libm is soft-float on the target) ---------------------- */
static inline int ifloor(float x) { int i = (int)x; return i - (x < (float)i); }
static inline int iceil(float x)  { int i = (int)x; return i + (x > (float)i); }
static inline float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static inline float qprog(float p) { return (float)(int)(clamp01(p) * PQ + 0.5f) / PQ; }
static inline float ease_out(float g) { float u = 1.0f - g; return 1.0f - u * u * u; }
static inline float fsqrt(float x) { return gfx_fast_sqrt(x); }

static unsigned hash32(unsigned a, unsigned b)
{
    unsigned h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return h;
}
static inline float hash01(unsigned a, unsigned b) { return (float)(hash32(a, b) >> 8) * (1.0f / 16777216.0f); }

/* ---- coverage masks ------------------------------------------------------- */
typedef struct {
    uint8_t  S;        /* mask is S x S */
    uint8_t  o;        /* disc centre sits at (o + phase, o + phase) */
    uint8_t  nph;      /* sub-pixel phases per axis */
    uint8_t  pad;
    uint32_t blk;      /* bytes per phase block: S*S coverage + S*4 row spans */
    uint8_t *data;
} dmask;

static dmask **g_masks;          /* [MASK_QMAX + 1], allocated on first use */
int dot_prebake_begin(void);

static dmask *bake_mask(int q)
{
    float d = (float)q * 0.25f, r = d * 0.5f;
    int o = (int)r + 1, S = 2 * o + 1;
    int nph = d <= 16.0f ? 4 : 2;
    uint32_t blk = (uint32_t)(S * S + S * 4);
    uint8_t *mem = (uint8_t *)hal_big_alloc((uint32_t)sizeof(dmask) + blk * (uint32_t)(nph * nph));
    if (!mem) return NULL;
    dmask *m = (dmask *)mem;
    m->S = (uint8_t)S; m->o = (uint8_t)o; m->nph = (uint8_t)nph; m->pad = 0;
    m->blk = blk;
    m->data = mem + sizeof(dmask);
    int ss = r < 2.0f ? 8 : r < 6.0f ? 4 : 0;          /* supersampling for small dots */
    float r2 = r * r;
    for (int py = 0; py < nph; py++)
        for (int px = 0; px < nph; px++) {
            uint8_t *cov = m->data + (uint32_t)(py * nph + px) * blk;
            uint8_t *sp = cov + S * S;
            float ccx = (float)o + (float)px / (float)nph;
            float ccy = (float)o + (float)py / (float)nph;
            for (int j = 0; j < S; j++) {
                int lo = S, hi = 0, slo = -1, shi = -1;
                for (int i = 0; i < S; i++) {
                    int a;
                    if (ss) {
                        int hit = 0;
                        for (int sy = 0; sy < ss; sy++) {
                            float dy = (float)j + ((float)sy + 0.5f) / (float)ss - ccy;
                            for (int sx = 0; sx < ss; sx++) {
                                float dx = (float)i + ((float)sx + 0.5f) / (float)ss - ccx;
                                hit += dx * dx + dy * dy <= r2;
                            }
                        }
                        a = (hit * 255 + ss * ss / 2) / (ss * ss);
                    } else {
                        float dx = (float)i + 0.5f - ccx, dy = (float)j + 0.5f - ccy;
                        float c = r - fsqrt(dx * dx + dy * dy) + 0.5f;
                        a = c <= 0.0f ? 0 : c >= 1.0f ? 255 : (int)(c * 255.0f + 0.5f);
                    }
                    cov[j * S + i] = (uint8_t)a;
                    if (a) { if (i < lo) lo = i; hi = i + 1; }
                    if (a == 255) { if (slo < 0) slo = i; shi = i + 1; }
                }
                if (lo >= hi) { lo = hi = 0; }
                if (slo < 0) { slo = shi = lo; }
                sp[j * 4 + 0] = (uint8_t)lo; sp[j * 4 + 1] = (uint8_t)hi;
                sp[j * 4 + 2] = (uint8_t)slo; sp[j * 4 + 3] = (uint8_t)shi;
            }
        }
    return m;
}

/* Masks are immutable after publication. A foreground first use can race the
 * background prewarmer: the CAS loser owns only its unpublished allocation. */
static dmask *publish_mask(dmask **slot,dmask *candidate)
{
    if(!candidate) return __atomic_load_n(slot,__ATOMIC_ACQUIRE);
    dmask *winner=NULL;
    if(__atomic_compare_exchange_n(slot,&winner,candidate,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return candidate;
    /* Preserve allocation accounting/header ownership when tracking is enabled.
     * Never free the published winner. */
    hal_big_free(candidate);
    return winner;
}

/* Mask for diameter d (quantised to 0.25 px); NULL when d is ~0, too large, or
 * memory ran out (callers fall back to gfx_fill_circle for the latter two). */
static const dmask *mask_get(float d, int *q_out)
{
    int q = (int)(d * 4.0f + 0.5f);
    *q_out = q;
    if (q <= 0 || q > MASK_QMAX) return NULL;
    dmask **masks=__atomic_load_n(&g_masks,__ATOMIC_ACQUIRE);
    if(!masks) {
        if(!dot_prebake_begin()) return NULL;
        masks=__atomic_load_n(&g_masks,__ATOMIC_ACQUIRE);
    }
    dmask *m=__atomic_load_n(&masks[q],__ATOMIC_ACQUIRE);
    if(!m) m=publish_mask(&masks[q],bake_mask(q));
    return m;
}

/* Bake every mask size ahead of use (a host task on the idle core at boot): a growing
 * dot otherwise bakes a new size per 0.25 px on its first run, 0.5-2.5 ms each on the
 * watch, which turned first wakes and picker builds into 50-470 ms frames. ~550 KB. */
int dot_prebake_begin(void)
{
    if(__atomic_load_n(&g_masks,__ATOMIC_ACQUIRE)) return 1;
    size_t bytes=sizeof(dmask *)*(MASK_QMAX+1);
    dmask **candidate=(dmask **)hal_big_alloc((uint32_t)bytes);
    if(!candidate) return __atomic_load_n(&g_masks,__ATOMIC_ACQUIRE)!=NULL;
    memset(candidate,0,bytes);
    dmask **winner=NULL;
    if(!__atomic_compare_exchange_n(&g_masks,&winner,candidate,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) hal_big_free(candidate);
    return 1;
}
void dot_prebake_all(void)
{
    if (!dot_prebake_begin()) return;
    dmask **masks=__atomic_load_n(&g_masks,__ATOMIC_ACQUIRE);
    for (int q = 1; q <= MASK_QMAX; q++) {
        if(__atomic_load_n(&masks[q],__ATOMIC_ACQUIRE)) continue;
        publish_mask(&masks[q],bake_mask(q));
    }
}

/* ---- stamping ------------------------------------------------------------- */
typedef struct {
    gfx_fb *fb;
    int c0, c1;        /* column clip, LOGICAL x [c0, c1) */
    int ylo, yhi;      /* logical rows this buffer holds [ylo, yhi) */
} dclip;

static void clip_init(dclip *k, gfx_fb *fb)
{
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    k->fb = fb;
    k->c0 = c0 - fb->ox; k->c1 = c1 - fb->ox;
    k->ylo = fb->y_base - fb->oy; k->yhi = k->ylo + fb->h;
    GFX_COST(GFX_COST_MASK, 0);   /* sim cost model: every raster entry calls this once */
}

/* Does the logical box [x0,x1) x [y0,y1) touch this buffer at all? */
static inline int clip_hit(const dclip *k, float x0, float y0, float x1, float y1)
{
    return x1 > (float)k->c0 && x0 < (float)k->c1 && y1 > (float)k->ylo && y0 < (float)k->yhi;
}

static void stamp_mask(const dclip *k, const dmask *m, float cx, float cy, uint16_t col)
{
    col = gfx_ink(k->fb, col);
    int n = m->nph, S = m->S;
    int ix = ifloor(cx), iy = ifloor(cy);
    int qx = (int)((cx - (float)ix) * (float)n + 0.5f), qy = (int)((cy - (float)iy) * (float)n + 0.5f);
    if (qx >= n) { qx -= n; ix++; }
    if (qy >= n) { qy -= n; iy++; }
    int X0 = ix - m->o, Y0 = iy - m->o;
    int j0 = k->ylo - Y0, j1 = k->yhi - Y0;
    if (j0 < 0) j0 = 0;
    if (j1 > S) j1 = S;
    if (j0 >= j1) return;
    int i0 = k->c0 - X0, i1 = k->c1 - X0;
    if (i0 < 0) i0 = 0;
    if (i1 > S) i1 = S;
    if (i0 >= i1) return;
    const uint8_t *cov = m->data + (uint32_t)(qy * n + qx) * m->blk;
    const uint8_t *sp = cov + S * S;
    gfx_fb *fb = k->fb;
    int bx = X0 + fb->ox;
    uint32_t s = ((uint32_t)col | ((uint32_t)col << 16)) & 0x07E0F81Fu;
    STAT(dot_stat_stamps++);
    for (int j = j0; j < j1; j++) {
        const uint8_t *r4 = sp + j * 4;
        int lo = r4[0], hi = r4[1], slo = r4[2], shi = r4[3];
        if (lo < i0) lo = i0;
        if (hi > i1) hi = i1;
        if (lo >= hi) continue;
        if (slo < lo) slo = lo;
        if (slo > hi) slo = hi;
        if (shi > hi) shi = hi;
        if (shi < slo) shi = slo;
        GFX_COST_PX(GFX_COST_MASK, hi - lo);   /* sim cost model only */
        uint16_t *d = gfx_row(fb, Y0 + j - k->ylo) + bx;
        const uint8_t *cv = cov + j * S;
        int i = lo;
        for (; i < hi; i++) {
            if (i == slo) {                       /* solid interior: plain stores */
                STAT(dot_stat_solid += (unsigned long)(shi - slo));
                for (; i < shi; i++) d[i] = col;
                if (i >= hi) break;
            }
            uint32_t a = cv[i];
            if (!a) continue;
            STAT(dot_stat_blend++);
            a = (a + 7) >> 3;
            uint32_t t = ((uint32_t)d[i] | ((uint32_t)d[i] << 16)) & 0x07E0F81Fu;
            t = ((s * a + t * (32 - a) + 0x02008010u) >> 5) & 0x07E0F81Fu;   /* rounded: greys stay grey (gfx_blend565) */
            d[i] = (uint16_t)(t | (t >> 16));
        }
    }
}

/* One dot of diameter d centred at (cx, cy), raster mode only. */
static void stamp(const dclip *k, float cx, float cy, float d, uint16_t col)
{
    float e = d * 0.5f + 1.0f;
    if (!clip_hit(k, cx - e, cy - e, cx + e, cy + e)) return;
    int q;
    const dmask *m = mask_get(d, &q);
    if (m) { stamp_mask(k, m, cx, cy, col); return; }
    if (q > 0) gfx_fill_circle(k->fb, cx, cy, d * 0.5f, col);   /* > 40 px or out of memory */
}

/* ---- style helpers -------------------------------------------------------- */
static inline int st_gap(const dot_style *st) { return st->gap == DOT_GAP_NONE ? 0 : st->gap ? st->gap : 1; }
static inline float st_dot(const dot_style *st) { return st->dot > 0.0f ? st->dot : st->pitch * 0.78f; }
static inline int st_font(const dot_style *st) { return st->font < DOT_FONT_COUNT ? st->font : 0; }

static unsigned hash_style(unsigned h, const dot_style *st)
{
    h = gfx_hash_u(h, st->font);
    h = gfx_hash_f(h, st->pitch);
    h = gfx_hash_f(h, st_dot(st));
    h = gfx_hash_u(h, st->color);
    h = gfx_hash_u(h, st->ghost);
    h = gfx_hash_u(h, (unsigned)st_gap(st));
    return gfx_hash_u(h, st->mono);
}

/* ---- text layout ---------------------------------------------------------- */
typedef struct {
    int16_t col0;      /* cell column of glyph column 0, from the string's left */
    int16_t box0;      /* first cell column of the glyph's box (ghost area) */
    uint8_t boxw;      /* box width in cells */
    uint8_t slot;      /* table slot; SLOT_BLANK = missing glyph (blank cell) */
} dslot;
#define SLOT_BLANK 0xFF

typedef struct {
    dslot s[MAXCH];
    int n;
    int cells;         /* total width in cells */
    int rows;          /* rows incl. descenders */
} dlayout;

/* Lay a string out in cells. store = 0 only measures (n, cells, rows); the cheap
 * path for a stripe that the text doesn't touch. */
static void layout(dlayout *L, const dot_style *st, const char *str, int store)
{
    int font = st_font(st), W = k_fw[font], gap = st_gap(st), cum = 0;
    L->n = 0; L->rows = k_fh[font]; L->cells = 0;
    if (!str) return;
    const unsigned char *p = (const unsigned char *)str;
    int slot;
    while (L->n < MAXCH && (slot = next_slot(&p)) >= 0) {
        const dglyph *g = glyph_of(font, slot);
        int gw = g ? g->w : W;
        int adv = gw, off = 0;
        if (st->mono && !narrow_punct(slot) && gw < W) {
            adv = W;
            off = (W - gw) / 2;
            /* the flag-1's stem is its last column: centre the stem, not the box */
            if (slot == '1' - 32 && (W - 1) / 2 - (gw - 1) >= 0) off = (W - 1) / 2 - (gw - 1);
        }
        if (store) {
            dslot *d = &L->s[L->n];
            d->slot = g ? (uint8_t)slot : SLOT_BLANK;
            d->box0 = (int16_t)cum;
            d->boxw = (uint8_t)adv;
            d->col0 = (int16_t)(cum + off);
        }
        L->n++;
        if (g && g->rows > L->rows) L->rows = g->rows;
        cum += adv + gap;
    }
    L->cells = L->n ? cum - gap : 0;
}

static float align_left(float x, float wpx, int align)
{
    return align == 1 ? x - wpx * 0.5f : align == 2 ? x - wpx : x;
}

int dot_text_height(const dot_style *st)
{
    return (int)((float)k_fh[st_font(st)] * st->pitch + 0.5f);
}

int dot_text_width(const dot_style *st, const char *s)
{
    if (!tables()) return 0;
    dlayout L;
    layout(&L, st, s, 0);
    return (int)((float)L.cells * st->pitch + 0.5f);
}

/* Per-dot reveal moment t in [0,1] for cell (gc, r) of a gcols x grows block. */
static float reveal_t(int mode, int gc, int r, int gcols, int grows, unsigned seed, unsigned idx)
{
    switch (mode) {
    case DOT_REVEAL_SCAN:   return gcols > 1 ? (float)gc / (float)(gcols - 1) : 0.0f;
    case DOT_REVEAL_RAIN:   return grows > 1 ? (float)r / (float)(grows - 1) : 0.0f;
    case DOT_REVEAL_RANDOM: return hash01(seed * 2654435761u + 0x51ED27u, idx);
    case DOT_REVEAL_CENTER: {
        float dx = (float)gc - (float)(gcols - 1) * 0.5f, dy = (float)r - (float)(grows - 1) * 0.5f;
        float mx = (float)(gcols - 1) * 0.5f, my = (float)(grows - 1) * 0.5f;
        float md = fsqrt(mx * mx + my * my);
        return md > 0.0f ? clamp01(fsqrt(dx * dx + dy * dy) / md) : 0.0f;
    }
    default: return 0.0f;
    }
}

/* Growth (0..1, eased) of a dot whose moment is t at progress p. */
static float reveal_g(int mode, float t, float p)
{
    float span = mode == DOT_REVEAL_NONE ? 1.0f : mode == DOT_REVEAL_RANDOM ? 0.3f : 0.45f;
    return ease_out(clamp01((p - t * (1.0f - span)) / span));
}

/* Draw one block of cells (a glyph or an icon) at top-left cell origin (ox, oy).
 * gc0 = global column of the glyph's column 0 (for SCAN/CENTER ordering). */
typedef struct {
    const dclip *k;
    float pitch, d;
    uint16_t color, ghost;
    float p;           /* quantised progress; >= 1 = static */
    int mode;
    unsigned seed;
    int gcols, grows;  /* size of the whole animated block, in cells */
    float ext;         /* half extent of a dot incl. AA */
    int r0, r1;        /* common stripe limits before glyph-specific row clamp */
} drawctx;

static void draw_row_bounds(drawctx *c, float oy)
{
    /* Every glyph in a text run shares this origin, pitch and stripe. Preserve
     * the original expression/order; compute its two software divides once.
     * Icons pass their own top-left origin, rather than the text-run origin. */
    c->r0 = ifloor(((float)c->k->ylo - c->ext - oy) / c->pitch - 0.5f);
    c->r1 = iceil(((float)c->k->yhi + c->ext - oy) / c->pitch + 0.5f);
    if (c->r0 < 0) c->r0 = 0;
}

static void draw_cells(const drawctx *c, const dglyph *g, float ox, float oy, int col0,
                       int box0, int boxw, int boxrows, unsigned gidx)
{
    const dclip *k = c->k;
    float pitch = c->pitch;
    int rows = g ? g->rows : 0;
    if (boxrows > rows && c->ghost) rows = boxrows;
    /* glyph rows overlapping this stripe only */
    int r0 = c->r0, r1 = c->r1;
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

static int text_impl(gfx_fb *fb, const dot_style *st, float x, float y, const char *s,
                     int align, float p, int mode, unsigned seed)
{
    NOS_COST_SCOPE(NOS_COST_TEXT,!fb->rec);
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
    draw_row_bounds(&c, y);
    int font = st_font(st), boxrows = k_fh[font];
    for (int i = 0; i < L.n; i++) {
        const dslot *sl = &L.s[i];
        const dglyph *g = sl->slot == SLOT_BLANK ? NULL : glyph_of(font, sl->slot);
        int cmin = sl->box0 < sl->col0 ? sl->box0 : sl->col0;
        int cmax = sl->box0 + sl->boxw;
        if (g && sl->col0 + g->w > cmax) cmax = sl->col0 + g->w;
        if (xl + (float)cmax * pitch + pad <= (float)k.c0 || xl + (float)cmin * pitch - pad >= (float)k.c1)
            continue;                                   /* glyph outside the column clip */
        draw_cells(&c, g, xl, y, sl->col0, sl->box0, sl->boxw, boxrows, (unsigned)i);
    }
    return ret;
}

int dot_text(gfx_fb *fb, const dot_style *st, float x, float y, const char *s)
{
    return text_impl(fb, st, x, y, s, 0, 1.0f, DOT_REVEAL_NONE, 0);
}

int dot_text_a(gfx_fb *fb, const dot_style *st, float x, float y, const char *s, int align)
{
    return text_impl(fb, st, x, y, s, align, 1.0f, DOT_REVEAL_NONE, 0);
}

int dot_text_anim(gfx_fb *fb, const dot_style *st, float x, float y, const char *s,
                  int align, float p, int mode, unsigned seed)
{
    return text_impl(fb, st, x, y, s, align, p, mode, seed);
}

/* ---- morph ---------------------------------------------------------------- */
int dot_text_morph(gfx_fb *fb, const dot_style *st, float x, float y,
                   const char *from, const char *to, int align, float p)
{
    if (!st || st->pitch <= 0.0f || !tables()) return 0;
    p = qprog(p);
    if (p <= 0.0f) return text_impl(fb, st, x, y, from, align, 1.0f, DOT_REVEAL_NONE, 0);
    if (p >= 1.0f) return text_impl(fb, st, x, y, to, align, 1.0f, DOT_REVEAL_NONE, 0);
    dlayout A, B;
    layout(&A, st, from, 1);
    layout(&B, st, to, 1);
    float pitch = st->pitch, d = st_dot(st);
    float wa = (float)A.cells * pitch, wb = (float)B.cells * pitch;
    float xa = align_left(x, wa, align), xb = align_left(x, wb, align);
    float e = ease_out(p);
    float wpx = wa + (wb - wa) * e;
    int ret = (int)(wpx + 0.5f);
    int rows = A.rows > B.rows ? A.rows : B.rows;
    float pad = (d > pitch ? (d - pitch) * 0.5f : 0.0f) + 1.0f;
    float bx0 = xa < xb ? xa : xb, bx1 = xa + wa > xb + wb ? xa + wa : xb + wb;
    if (fb->rec) {
        unsigned h = hash_style(0x30F7u, st);
        h = gfx_hash_f(gfx_hash_f(gfx_hash_f(h, x), y), p);
        h = gfx_hash_u(h, (unsigned)align);
        for (const unsigned char *c = (const unsigned char *)from; c && *c; c++) h = gfx_hash_u(h, *c);
        h = gfx_hash_u(h, 0xFFu);
        for (const unsigned char *c = (const unsigned char *)to; c && *c; c++) h = gfx_hash_u(h, *c);
        gfx_rec_cmd(fb, ifloor(bx0 - pad), ifloor(y - pad), iceil(bx1 + pad),
                    iceil(y + (float)rows * pitch + pad), h);
        return ret;
    }
    dclip k;
    clip_init(&k, fb);
    if (!clip_hit(&k, bx0 - pad, y - pad, bx1 + pad, y + (float)rows * pitch + pad)) return ret;
    int boxrows = k_fh[st_font(st)];
    float ext = d * 0.5f + 1.0f;
    int n = A.n > B.n ? A.n : B.n;
    for (int i = 0; i < n; i++) {
        const dslot *sa = i < A.n ? &A.s[i] : NULL, *sb = i < B.n ? &B.s[i] : NULL;
        /* slot origin glides from its `from` position to its `to` position */
        float pa = sa ? xa + (float)sa->box0 * pitch : xb + (float)sb->box0 * pitch;
        float pb = sb ? xb + (float)sb->box0 * pitch : pa;
        float sx = pa + (pb - pa) * e;
        int font = st_font(st);
        const dglyph *ga = (sa && sa->slot != SLOT_BLANK) ? glyph_of(font, sa->slot) : NULL;
        const dglyph *gb = (sb && sb->slot != SLOT_BLANK) ? glyph_of(font, sb->slot) : NULL;
        int oa = sa ? sa->col0 - sa->box0 : 0, ob = sb ? sb->col0 - sb->box0 : 0;
        int bwa = sa ? sa->boxw : 0, bwb = sb ? sb->boxw : 0;
        int bw = bwa > bwb ? bwa : bwb;
        int ra = ga ? ga->rows : 0, rb = gb ? gb->rows : 0;
        int nr = ra > rb ? ra : rb;
        if (st->ghost && nr < boxrows) nr = boxrows;
        for (int r = 0; r < nr; r++) {
            float cy = y + ((float)r + 0.5f) * pitch;
            if (cy + ext <= (float)k.ylo || cy - ext >= (float)k.yhi) continue;
            unsigned ba = (ga && r < ga->rows) ? (unsigned)ga->bits[r] << oa : 0u;
            unsigned bb = (gb && r < gb->rows) ? (unsigned)gb->bits[r] << ob : 0u;
            float rt = rows > 1 ? (float)r / (float)(rows - 1) * 0.25f : 0.0f;
            float gA = 1.0f - ease_out(clamp01((p - rt) / 0.5f));
            float gB = ease_out(clamp01((p - 0.25f - rt) / 0.5f));
            for (int cc = 0; cc < bw; cc++) {
                int la = (ba >> cc) & 1u, lb = (bb >> cc) & 1u;
                int ghost = st->ghost && r < boxrows;
                if (!la && !lb && !ghost) continue;
                float cx = sx + ((float)cc + 0.5f) * pitch;
                if (cx + ext <= (float)k.c0 || cx - ext >= (float)k.c1) continue;
                if (la && lb) { stamp(&k, cx, cy, d, st->color); continue; }
                float g = la ? gA : lb ? gB : 0.0f;
                if (ghost && g < 1.0f) stamp(&k, cx, cy, d, st->ghost);
                if (g > 0.0f) stamp(&k, cx, cy, d * g, st->color);
            }
        }
    }
    return ret;
}

/* ---- single dot ----------------------------------------------------------- */
void dot_disc(gfx_fb *fb, float cx, float cy, float d, uint16_t color)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
    if (d <= 0.0f) return;
    float e = d * 0.5f + 1.0f;
    if (fb->rec) {
        unsigned h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(0xD15Cu, color), cx), cy), d);
        gfx_rec_cmd(fb, ifloor(cx - e), ifloor(cy - e), iceil(cx + e), iceil(cy + e), h);
        return;
    }
    dclip k;
    clip_init(&k, fb);
    stamp(&k, cx, cy, d, color);
}

/* ---- icons ---------------------------------------------------------------- */
void dot_icon_anim(gfx_fb *fb, int icon, float cx, float cy, float pitch, float dot,
                   uint16_t color, float p, int mode)
{
    NOS_COST_SCOPE(NOS_COST_SHAPE,!fb->rec);
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
    draw_row_bounds(&c, cy - half);
    draw_cells(&c, g, cx - half, cy - half, 0, 0, 0, 0, (unsigned)icon);
}

void dot_icon(gfx_fb *fb, int icon, float cx, float cy, float pitch, float dot, uint16_t color)
{
    dot_icon_anim(fb, icon, cx, cy, pitch, dot, color, 1.0f, DOT_REVEAL_NONE);
}

/* ---- ring / bar / columns ------------------------------------------------- */
static inline float lit_frac(float i, float lit0, float lit1)
{
    float a = lit1 - i, b = i + 1.0f - lit0;
    return clamp01(a < b ? a : b);
}

/* A dot that is `f` lit: off dot underneath, on dot grown by area on top. */
static void lit_dot(const dclip *k, float cx, float cy, float d, float f, uint16_t on, uint16_t off)
{
    if (f >= 1.0f) { stamp(k, cx, cy, d, on); return; }
    if (off) stamp(k, cx, cy, d, off);
    if (f > 0.0f) stamp(k, cx, cy, d * fsqrt(f), on);
}

#define RING_CHUNKS 16

/* Four immutable unit circles, shared by all radii and colours. PSRAM holds at
 * most 32 KiB; allocation failure retains the direct path. The angles use the
 * original multiplication order so recording and raster pixels stay identical. */
#define RING_CACHE_SLOTS 4
typedef struct { int n; unsigned age; float *xy; } ring_circle;
static ring_circle s_ring_circle[RING_CACHE_SLOTS];
static unsigned s_ring_age;
static const float *ring_units(int n)
{
    ring_circle *slot = &s_ring_circle[0];
    for (int j = 0; j < RING_CACHE_SLOTS; j++) {
        ring_circle *c = &s_ring_circle[j];
        if (c->n == n && c->xy) { c->age = ++s_ring_age; return c->xy; }
        if (c->age < slot->age) slot = c;
    }
    if (!slot->xy) slot->xy = hal_big_alloc(1024u * 2u * sizeof(float));
    if (!slot->xy) return NULL;
    float step = 6.2831853f / (float)n;
    for (int i = 0; i < n; i++) {
        float a = step * (float)i;
        slot->xy[2 * i] = gfx_fast_sin(a);
        slot->xy[2 * i + 1] = gfx_fast_cos(a);
    }
    slot->n = n; slot->age = ++s_ring_age;
    return slot->xy;
}

void dot_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d,
              float lit0, float lit1, uint16_t on, uint16_t off)
{
    if (n <= 0 || d <= 0.0f) return;
    if (n > 1024) n = 1024;
    lit0 = (float)ifloor(lit0 * 64.0f + 0.5f) / 64.0f;
    lit1 = (float)ifloor(lit1 * 64.0f + 0.5f) / 64.0f;
    if (lit1 < lit0) lit1 = lit0;
    float span = lit1 - lit0;
    if (span > (float)n) { lit0 = 0.0f; lit1 = (float)n; }
    else if (lit0 < 0.0f || lit0 >= (float)n) {          /* normalise start into [0, n) */
        float sh = (float)ifloor(lit0 / (float)n) * (float)n;
        lit0 -= sh; lit1 -= sh;
    }
    float step = 6.2831853f / (float)n, e = d * 0.5f + 1.0f;
    const float *unit = ring_units(n);
    if (fb->rec) {
        int K = n < RING_CHUNKS ? n : RING_CHUNKS;
        for (int ch = 0; ch < K; ch++) {
            int i0 = ch * n / K, i1 = (ch + 1) * n / K;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(0x6A11u, on), off), (unsigned)(n * 64 + ch));
            h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(h, cx), cy), r), d);
            for (int i = i0; i < i1; i++) {
                float a = step * (float)i;
                float px = cx + r * (unit ? unit[2 * i] : gfx_fast_sin(a));
                float py = cy - r * (unit ? unit[2 * i + 1] : gfx_fast_cos(a));
                if (px < x0) x0 = px;
                if (px > x1) x1 = px;
                if (py < y0) y0 = py;
                if (py > y1) y1 = py;
                float f = lit_frac((float)i, lit0, lit1);
                float f2 = lit_frac((float)(i + n), lit0, lit1);
                h = gfx_hash_f(h, f > f2 ? f : f2);
            }
            gfx_rec_cmd(fb, ifloor(x0 - e), ifloor(y0 - e), iceil(x1 + e), iceil(y1 + e), h);
        }
        return;
    }
    dclip k;
    clip_init(&k, fb);
    float re = r + e;
    if (!clip_hit(&k, cx - re, cy - re, cx + re, cy + re)) return;
    for (int i = 0; i < n; i++) {
        float a = step * (float)i;
        float py = cy - r * (unit ? unit[2 * i + 1] : gfx_fast_cos(a));
        if (py + e <= (float)k.ylo || py - e >= (float)k.yhi) continue;
        float px = cx + r * (unit ? unit[2 * i] : gfx_fast_sin(a));
        float f = lit_frac((float)i, lit0, lit1), f2 = lit_frac((float)(i + n), lit0, lit1);
        if (f2 > f) f = f2;
        if (f <= 0.0f && !off) continue;
        lit_dot(&k, px, py, d, f, on, off);
    }
}

void dot_bar(gfx_fb *fb, float x, float y, int n, float pitch, float d, float lit,
             uint16_t on, uint16_t off)
{
    if (n <= 0 || d <= 0.0f) return;
    lit = (float)ifloor(lit * 64.0f + 0.5f) / 64.0f;
    float e = d * 0.5f + 1.0f, xe = x + pitch * (float)(n - 1);
    float xa = x < xe ? x : xe, xb = x < xe ? xe : x;
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(0xBA2u, on), off), (unsigned)n);
        h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(h, x), y), pitch), d), lit);
        gfx_rec_cmd(fb, ifloor(xa - e), ifloor(y - e), iceil(xb + e), iceil(y + e), h);
        return;
    }
    dclip k;
    clip_init(&k, fb);
    if (!clip_hit(&k, xa - e, y - e, xb + e, y + e)) return;
    for (int i = 0; i < n; i++) {
        float f = clamp01(lit - (float)i);
        if (f <= 0.0f && !off) continue;
        lit_dot(&k, x + pitch * (float)i, y, d, f, on, off);
    }
}

void dot_columns(gfx_fb *fb, float x, float y, int cols, int rows, float pitch, float d,
                 const uint8_t *values, uint16_t on, uint16_t off)
{
    if (cols <= 0 || rows <= 0 || d <= 0.0f || !values) return;
    float pad = (d > pitch ? (d - pitch) * 0.5f : 0.0f) + 1.0f;
    float w = (float)cols * pitch, h = (float)rows * pitch;
    if (fb->rec) {
        unsigned hh = gfx_hash_u(gfx_hash_u(gfx_hash_u(0xC015u, on), off), (unsigned)(cols * 256 + rows));
        hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(hh, x), y), pitch), d);
        for (int c = 0; c < cols; c++) hh = gfx_hash_u(hh, values[c]);
        gfx_rec_cmd(fb, ifloor(x - pad), ifloor(y - pad), iceil(x + w + pad), iceil(y + h + pad), hh);
        return;
    }
    dclip k;
    clip_init(&k, fb);
    if (!clip_hit(&k, x - pad, y - pad, x + w + pad, y + h + pad)) return;
    float e = d * 0.5f + 1.0f;
    int r0 = ifloor(((float)k.ylo - e - y) / pitch - 0.5f), r1 = iceil(((float)k.yhi + e - y) / pitch + 0.5f);
    if (r0 < 0) r0 = 0;
    if (r1 > rows) r1 = rows;
    for (int c = 0; c < cols; c++) {
        float cx = x + ((float)c + 0.5f) * pitch;
        if (cx + e <= (float)k.c0 || cx - e >= (float)k.c1) continue;
        float hv = (float)values[c] * (float)rows / 255.0f;
        for (int r = r0; r < r1; r++) {
            float f = clamp01(hv - (float)(rows - 1 - r));     /* r counts from the top */
            if (f <= 0.0f && !off) continue;
            lit_dot(&k, cx, y + ((float)r + 0.5f) * pitch, d, f, on, off);
        }
    }
}
