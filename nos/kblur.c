/* kblur.c — blurred copies of items that keep their look (kblur.h). */
#include "kblur.h"
#include "nos.h"
#include "sprite.h"
#include "gfx_cost.h"
#include <string.h>

#define KB_STRIP 16
#define KB_MAXS  32
static const int k_len[KB_NLEV] = { 8, 16, 32, 48 };

typedef struct {
    unsigned key, sig;
    int used, ok, h;
    uint8_t lev_ok;
    uint32_t stamp;              /* last use, for eviction */
    uint8_t *blur[KB_NLEV];
} kb_slot;

struct kblur {
    int W, MH, C, nslots;
    uint16_t (*lut)[256];
    uint16_t *canvas, *sums;
    uint8_t *grey, *line;
    kb_slot s[KB_MAXS];
    /* the bake in progress */
    kb_slot *job;
    int stage, strip, blur_y;
    uint32_t tick;
};

static uint16_t grey565(int v)
{
    if (v >= 255) return C_WHITE;
    int r = (v * 31 + 127) / 255;                /* neutral: G = 2R, B = R */
    return (uint16_t)(r << 11 | (r * 2) << 5 | r);
}

kblur *kblur_new(int w, int max_h, int ctx, unsigned bytes)
{
    /* Reject undersized arenas before laying out scratch or returning zero slots. */
    if (w <= 0 || max_h <= 0 || ctx < 0) return 0;
    size_t scratch = ((sizeof(kblur) + 7) & ~7u) + 17 * 256 * 2 +
        (size_t)w * KB_STRIP * 2 + (size_t)w * 3 + 16 + (size_t)w * (max_h + 2 * ctx);
    size_t slot = (size_t)w * ((max_h + 1) / 2) * KB_NLEV;
    if (scratch + slot > bytes) return 0;
    uint8_t *p = hal_big_alloc(bytes);
    if (!p) return 0;
    uint8_t *end = p + bytes;
    kblur *c = (kblur *)p; p += (sizeof *c + 7) & ~7u;
    memset(c, 0, sizeof *c);
    c->W = w; c->MH = max_h; c->C = ctx;
    c->lut = (uint16_t (*)[256])p; p += 17 * 256 * 2;
    for (int l = 0; l <= 16; l++)
        for (int i = 0; i < 256; i++) c->lut[l][i] = grey565((i * l + 8) / 16);
    c->canvas = (uint16_t *)p; p += (size_t)w * KB_STRIP * 2;
    c->sums = (uint16_t *)p; p += (size_t)w * 2 + 8;
    c->line = p; p += (size_t)w + 8;
    c->grey = p; p += (size_t)w * (max_h + 2 * ctx);
    size_t lev = (size_t)w * (size_t)((max_h + 1) / 2);
    while (c->nslots < KB_MAXS && p + lev * KB_NLEV <= end) {
        kb_slot *s = &c->s[c->nslots++];
        for (int k = 0; k < KB_NLEV; k++) { s->blur[k] = p; p += lev; }
    }
    return c;
}

static kb_slot *find(const kblur *c, unsigned key)
{
    for (int i = 0; i < c->nslots; i++) if (c->s[i].used && c->s[i].key == key) return (kb_slot *)&c->s[i];
    return 0;
}

/* vertical box blur of len px over the band's own rows (tails cropped at its edges),
 * each pair of rows kept as their mean */
static int blur_level(kblur *c, kb_slot *s, int lv)
{
    int W = c->W, H = s->h, HC = H + 2 * c->C, len = k_len[lv], h0 = len / 2 - c->C;
    const uint8_t *src = c->grey;
    uint8_t *dst = s->blur[lv], *ln = c->line;
    uint16_t *sum = c->sums;
    if (!c->blur_y) {
        memset(sum, 0, (size_t)W * 2);
        for (int r = -h0 < 0 ? 0 : -h0; r < len - h0 && r < HC; r++)
            for (int x = 0; x < W; x++) sum[x] += src[r * W + x];
    }
    int end = c->blur_y + KB_STRIP;
    if (end > H) end = H;
    GFX_COST_PX(GFX_COST_DIM, (unsigned)(end - c->blur_y) * (unsigned)W * 12u);
    for (int y = c->blur_y; y < end; y++) {
        uint8_t *o = dst + (size_t)(y >> 1) * W;
        if (!(y & 1)) {
            for (int x = 0; x < W; x++) ln[x] = (uint8_t)((sum[x] + len / 2) / len);
            if (y == H - 1) memcpy(o, ln, (size_t)W);
        } else
            for (int x = 0; x < W; x++) o[x] = (uint8_t)((ln[x] + (sum[x] + len / 2) / len + 1) >> 1);
        int ra = y - h0 + len, rs = y - h0;
        if (ra >= 0 && ra < HC) for (int x = 0; x < W; x++) sum[x] += src[ra * W + x];
        if (rs >= 0 && rs < HC) for (int x = 0; x < W; x++) sum[x] -= src[rs * W + x];
    }
    c->blur_y = end;
    if (end < H) return 0;
    c->blur_y = 0;
    s->lev_ok |= (uint8_t)(1u << lv);
    return 1;
}

/* one step of the job: a 16-row strip of the item into grey, or one blur level */
static int step(kblur *c, ui_blur_row_fn fn, int i, void *ctx)
{
    kb_slot *s = c->job;
    int W = c->W, HC = s->h + 2 * c->C;
    if (c->stage == 0) {
        int r0 = c->strip * KB_STRIP, rows = HC - r0 < KB_STRIP ? HC - r0 : KB_STRIP;
        memset(c->canvas, 0, (size_t)W * rows * 2);
        gfx_fb f;
        memset(&f, 0, sizeof f);
        /* the item drawn with its top at y = C in a W-wide view whose column 0 is x0 = 0 */
        f.px = c->canvas; f.h = rows; f.y_base = r0; f.stride = W; f.x_base = 0; f.cx0 = 0; f.cx1 = W;
        fn(&f, i, 0, c->C, W, s->h, ctx);
        uint8_t *g = c->grey + (size_t)r0 * W;
        for (int k = 0; k < W * rows; k++) {
            int v = (c->canvas[k] >> 5) & 63;    /* green carries the most bits */
            g[k] = (uint8_t)((v * 255 + 31) / 63);
        }
        if (++c->strip * KB_STRIP >= HC) c->stage = 1;
        return 1;
    }
    if (!blur_level(c, s, c->stage - 1)) return 1;
    if (++c->stage > KB_NLEV) { s->ok = 1; c->job = 0; return 0; }
    return 1;
}

int kblur_bake(kblur *c, unsigned key, unsigned sig, int h, ui_blur_row_fn fn, int i, void *ctx, unsigned budget_us)
{
    if (!c || !c->nslots || h <= 0 || h > c->MH) return 0;
    if (nos_transitioning()) return 1;  /* no allocation/raster/filter jobs in transitions */
    c->tick++;
    kb_slot *s = find(c, key);
    if (s && s->ok && s->sig == sig && s->h == h) { s->stamp = c->tick; return 0; }
    if (c->job && (c->job->key != key || c->job->sig != sig || c->job->h != h)) {
        c->job->used = 0; c->job->ok = 0;
        c->job = 0;
    }
    if (!c->job) {
        if (!s) {                                 /* a free slot, else the least recently used */
            for (int k = 0; k < c->nslots && !s; k++) if (!c->s[k].used) s = &c->s[k];
            if (!s) for (int k = 0; k < c->nslots; k++)
                if (!s || c->s[k].stamp < s->stamp) s = &c->s[k];
            if (!s) return 0;
        }
        s->used = 1; s->key = key; s->sig = sig; s->h = h; s->ok = 0; s->lev_ok = 0; s->stamp = c->tick;
        c->job = s; c->stage = 0; c->strip = 0; c->blur_y = 0;
    }
#ifdef GFX_COST_MODEL
    (void)budget_us;
    for (int n = 0; n < 6; n++) if (!step(c, fn, i, ctx)) return 0;
    return 1;
#else
    uint32_t t0 = hal_micros();
    while (step(c, fn, i, ctx)) if (hal_micros() - t0 > budget_us) return 1;
    return 0;
#endif
}

int kblur_ready(const kblur *c, unsigned key, unsigned sig, int lv)
{
    if (!c || lv < 1 || lv > KB_NLEV) return 0;
    const kb_slot *s = find(c, key);
    return s && s->ok && s->sig == sig && (s->lev_ok & (1u << (lv - 1)));
}

int kblur_draw(gfx_fb *fb, const kblur *c, unsigned key, int lv, int x, int y, float fade)
{
    if (!c || lv < 1 || lv > KB_NLEV) return 0;
    kb_slot *s = find(c, key);
    if (!s || !s->ok || !(s->lev_ok & (1u << (lv - 1)))) return 0;
    s->stamp = ((kblur *)c)->tick;
    int l = (int)(fade * 16.0f + 0.5f);
    l = l < 0 ? 0 : l > 16 ? 16 : l;
    sprite8 sp = { s->blur[lv - 1], c->W, (s->h + 1) / 2 };
    sprite8_blit_y2(fb, &sp, x, y, s->h, c->lut[l],
                    gfx_hash_u(gfx_hash_u(gfx_hash_u(s->sig ^ 0x4B1Bu, key), (unsigned)lv), (unsigned)l));
    return 1;
}

int kblur_slots(const kblur *c) { return c ? c->nslots : 0; }

void kblur_keep_only(kblur *c, const unsigned *keys, int n)
{
    if (!c) return;
    for (int k = 0; k < c->nslots; k++) {
        kb_slot *s = &c->s[k];
        if (!s->used) continue;
        int keep = 0;
        for (int j = 0; j < n && !keep; j++) keep = keys[j] == s->key;
        if (!keep) { if (c->job == s) c->job = 0; s->used = 0; s->ok = 0; }
    }
}
