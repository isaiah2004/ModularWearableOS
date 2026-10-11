/* nos_img.c — image output for the NOS simulator: PNG (own deflate: LZ77 + fixed
 * Huffman, small files for mostly-black UI frames), streaming animated GIF (LZW,
 * fixed NOS palette, each frame only re-encodes the box that changed), RGB canvas
 * helpers and a 5x7 label font for contact sheets. No external libraries. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "nos_sim.h"

/* ---- round panel mask ------------------------------------------------------------ */
static uint8_t s_mask[GFX_W * GFX_H];
static int s_mask_ok;
uint8_t sim_mask(int x, int y)
{
    if (!s_mask_ok) {
        float rad = GFX_W / 2.0f;
        for (int yy = 0; yy < GFX_H; yy++)
            for (int xx = 0; xx < GFX_W; xx++) {
                float dx = xx + 0.5f - rad, dy = yy + 0.5f - rad;
                float d = rad - sqrtf(dx * dx + dy * dy) + 0.5f;
                s_mask[yy * GFX_W + xx] = d <= 0 ? 0 : d >= 1 ? 255 : (uint8_t)(d * 255);
            }
        s_mask_ok = 1;
    }
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return 0;
    return s_mask[y * GFX_W + x];
}

/* ---- canvas ---------------------------------------------------------------------- */
#define BEZEL_R 18
#define BEZEL_G 18
#define BEZEL_B 20

sim_img img_new(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    sim_img im = { w, h, (uint8_t *)malloc((size_t)w * h * 3) };
    if (im.rgb)
        for (int i = 0; i < w * h; i++) { im.rgb[i * 3] = r; im.rgb[i * 3 + 1] = g; im.rgb[i * 3 + 2] = b; }
    return im;
}
void img_free(sim_img *im) { free(im->rgb); im->rgb = 0; }

static void c565(uint16_t p, int *r, int *g, int *b)
{
    *r = ((p >> 11) & 31) * 255 / 31;
    *g = ((p >> 5) & 63) * 255 / 63;
    *b = (p & 31) * 255 / 31;
}
static void put(sim_img *im, int x, int y, int r, int g, int b)
{
    if ((unsigned)x >= (unsigned)im->w || (unsigned)y >= (unsigned)im->h) return;
    uint8_t *p = &im->rgb[((size_t)y * im->w + x) * 3];
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
}
void img_from_565(sim_img *im, int dx, int dy, const uint16_t *fb, int masked)
{
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) {
            int r, g, b;
            c565(fb[y * GFX_W + x], &r, &g, &b);
            if (masked) {
                int m = sim_mask(x, y);
                r = (r * m + BEZEL_R * (255 - m)) / 255;
                g = (g * m + BEZEL_G * (255 - m)) / 255;
                b = (b * m + BEZEL_B * (255 - m)) / 255;
            }
            put(im, dx + x, dy + y, r, g, b);
        }
}
void img_from_565_scaled(sim_img *im, int dx, int dy, const uint16_t *fb, int masked, int div)
{
    if (div <= 1) { img_from_565(im, dx, dy, fb, masked); return; }
    for (int y = 0; y < GFX_H / div; y++)
        for (int x = 0; x < GFX_W / div; x++) {
            int sr = 0, sg = 0, sb = 0;
            for (int j = 0; j < div; j++)
                for (int i = 0; i < div; i++) {
                    int px = x * div + i, py = y * div + j, r, g, b;
                    c565(fb[py * GFX_W + px], &r, &g, &b);
                    if (masked) {
                        int m = sim_mask(px, py);
                        r = (r * m + BEZEL_R * (255 - m)) / 255;
                        g = (g * m + BEZEL_G * (255 - m)) / 255;
                        b = (b * m + BEZEL_B * (255 - m)) / 255;
                    }
                    sr += r; sg += g; sb += b;
                }
            int n = div * div;
            put(im, dx + x, dy + y, sr / n, sg / n, sb / n);
        }
}
void img_rect(sim_img *im, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) put(im, xx, yy, r, g, b);
}
void img_frame(sim_img *im, int x, int y, int w, int h, int t, uint8_t r, uint8_t g, uint8_t b)
{
    img_rect(im, x, y, w, t, r, g, b);
    img_rect(im, x, y + h - t, w, t, r, g, b);
    img_rect(im, x, y, t, h, r, g, b);
    img_rect(im, x + w - t, y, t, h, r, g, b);
}

/* ---- 5x7 font: 5 column bytes per glyph, bit 0 = top row, ASCII 32..126 --------- */
static const uint8_t FONT5X7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08},
};
const uint8_t *img_glyph(int c)
{
    if (c < 32 || c > 126) c = '?';
    return FONT5X7[c - 32];
}
int img_text_width(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }
int img_text(sim_img *im, int x, int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    int x0 = x;
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *gl = img_glyph((unsigned char)*s);
        for (int c = 0; c < 5; c++)
            for (int row = 0; row < 7; row++)
                if (gl[c] >> row & 1) img_rect(im, x + c * scale, y + row * scale, scale, scale, r, g, b);
    }
    return x - x0;
}

/* ---- deflate (zlib stream, one fixed-Huffman block, greedy LZ77) ------------------ */
typedef struct { uint8_t *p; size_t n, cap; uint32_t bits; int nb; } bitw;
static void bw_byte(bitw *w, uint8_t v)
{
    if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 65536; w->p = (uint8_t *)realloc(w->p, w->cap); }
    w->p[w->n++] = v;
}
static void bw_bits(bitw *w, uint32_t v, int n)        /* LSB first */
{
    w->bits |= v << w->nb;
    w->nb += n;
    while (w->nb >= 8) { bw_byte(w, (uint8_t)w->bits); w->bits >>= 8; w->nb -= 8; }
}
static void bw_huff(bitw *w, uint32_t code, int n)     /* Huffman codes go MSB first */
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
    bw_bits(w, r, n);
}
static void put_lit(bitw *w, int sym)
{
    if (sym < 144) bw_huff(w, 0x30 + sym, 8);
    else if (sym < 256) bw_huff(w, 0x190 + sym - 144, 9);
    else if (sym < 280) bw_huff(w, sym - 256, 7);
    else bw_huff(w, 0xC0 + sym - 280, 8);
}
static const uint16_t LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXT[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                    4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXT[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static void put_match(bitw *w, int len, int dist)
{
    int l = 28;
    while (LBASE[l] > len) l--;
    put_lit(w, 257 + l);
    if (LEXT[l]) bw_bits(w, (uint32_t)(len - LBASE[l]), LEXT[l]);
    int d = 29;
    while (DBASE[d] > dist) d--;
    bw_huff(w, (uint32_t)d, 5);
    if (DEXT[d]) bw_bits(w, (uint32_t)(dist - DBASE[d]), DEXT[d]);
}

#define WIN 32768
#define HBITS 15
static uint8_t *zlib_compress(const uint8_t *src, size_t n, size_t *out_n)
{
    bitw w = { 0 };
    bw_byte(&w, 0x78); bw_byte(&w, 0x01);
    bw_bits(&w, 1, 1);              /* BFINAL */
    bw_bits(&w, 1, 2);              /* fixed Huffman */
    int32_t *head = (int32_t *)malloc(sizeof(int32_t) << HBITS);
    int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * WIN);
    for (int i = 0; i < 1 << HBITS; i++) head[i] = -1;
    size_t i = 0;
#define HASH(k) ((((uint32_t)src[k] << 16) ^ ((uint32_t)src[(k) + 1] << 8) ^ src[(k) + 2]) * 2654435761u >> (32 - HBITS))
    while (i < n) {
        int best = 0, bdist = 0;
        if (i + 3 <= n) {
            uint32_t h = HASH(i);
            int32_t c = head[h];
            int chain = 24;
            size_t maxl = n - i < 258 ? n - i : 258;
            while (c >= 0 && (size_t)c < i && i - (size_t)c <= WIN - 1 && chain-- > 0) {
                const uint8_t *a = src + c, *b = src + i;
                if (a[best] == b[best]) {
                    size_t l = 0;
                    while (l < maxl && a[l] == b[l]) l++;
                    if ((int)l > best) { best = (int)l; bdist = (int)(i - (size_t)c); if (l == maxl) break; }
                }
                c = prev[c & (WIN - 1)];
            }
        }
        size_t adv = best >= 3 ? (size_t)best : 1;
        if (best >= 3) put_match(&w, best, bdist);
        else put_lit(&w, src[i]);
        for (size_t k = 0; k < adv; k++, i++)
            if (i + 3 <= n) { uint32_t h = HASH(i); prev[i & (WIN - 1)] = head[h]; head[h] = (int32_t)i; }
    }
#undef HASH
    put_lit(&w, 256);
    if (w.nb) bw_bits(&w, 0, 8 - w.nb);
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < n; k++) { a = (a + src[k]) % 65521; b = (b + a) % 65521; }
    uint32_t ad = (b << 16) | a;
    bw_byte(&w, (uint8_t)(ad >> 24)); bw_byte(&w, (uint8_t)(ad >> 16));
    bw_byte(&w, (uint8_t)(ad >> 8)); bw_byte(&w, (uint8_t)ad);
    free(head); free(prev);
    *out_n = w.n;
    return w.p;
}

static uint32_t s_crc[256];
static uint32_t crc32_buf(uint32_t c, const uint8_t *p, size_t n)
{
    if (!s_crc[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            s_crc[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = s_crc[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t hdr[8];
    be32(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32_buf(0, (const uint8_t *)type, 4);
    c = crc32_buf(c, data, n);
    uint8_t cb[4];
    be32(cb, c);
    fwrite(cb, 1, 4, f);
}
static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
int png_write(const char *path, const sim_img *im)
{
    int w = im->w, h = im->h, stride = w * 3;
    size_t rawn = (size_t)(stride + 1) * h;
    uint8_t *raw = (uint8_t *)malloc(rawn), *cand = (uint8_t *)malloc((size_t)stride * 5);
    if (!raw || !cand) { free(raw); free(cand); return 0; }
    for (int y = 0; y < h; y++) {
        const uint8_t *row = im->rgb + (size_t)y * stride, *up = y ? row - stride : 0;
        long best = -1; int bf = 0;
        for (int f = 0; f < 5; f++) {
            uint8_t *o = cand + (size_t)f * stride;
            long sum = 0;
            for (int i = 0; i < stride; i++) {
                int a = i >= 3 ? row[i - 3] : 0, b = up ? up[i] : 0, c = (up && i >= 3) ? up[i - 3] : 0;
                int pr = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : paeth(a, b, c);
                o[i] = (uint8_t)(row[i] - pr);
                sum += o[i] < 128 ? o[i] : 256 - o[i];
            }
            if (best < 0 || sum < best) { best = sum; bf = f; }
        }
        raw[(size_t)y * (stride + 1)] = (uint8_t)bf;
        memcpy(raw + (size_t)y * (stride + 1) + 1, cand + (size_t)bf * stride, (size_t)stride);
    }
    size_t zn;
    uint8_t *z = zlib_compress(raw, rawn, &zn);
    free(raw); free(cand);
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return 0; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, zn);
    png_chunk(f, "IEND", 0, 0);
    free(z);
    return fclose(f) == 0;
}

/* ---- GIF ----------------------------------------------------------------------------
 * Palette: 0-63 greys, 64-95 black->Nothing red (#D71921), 96-111 red->white,
 * 112-119 black->green, 120-127 black->amber, 128-252 a 5x5x5 cube, 253-255 spare
 * accents. RGB565 -> index through a 64K nearest-colour table built once. */
static uint8_t s_pal[256][3];
static uint8_t *s_lut565;
static void pal_set(int i, int r, int g, int b) { s_pal[i][0] = (uint8_t)r; s_pal[i][1] = (uint8_t)g; s_pal[i][2] = (uint8_t)b; }
static void pal_build(void)
{
    if (s_lut565) return;
    for (int i = 0; i < 64; i++) pal_set(i, i * 255 / 63, i * 255 / 63, i * 255 / 63);
    const int R[3] = { 0xD7, 0x19, 0x21 }, G[3] = { 0x4A, 0x9E, 0x5C }, A[3] = { 0xD4, 0xA8, 0x43 };
    for (int i = 0; i < 32; i++) { float t = (i + 1) / 32.0f; pal_set(64 + i, (int)(R[0] * t), (int)(R[1] * t), (int)(R[2] * t)); }
    for (int i = 0; i < 16; i++) {
        float t = (i + 1) / 17.0f;
        pal_set(96 + i, (int)(R[0] + (255 - R[0]) * t), (int)(R[1] + (255 - R[1]) * t), (int)(R[2] + (255 - R[2]) * t));
    }
    for (int i = 0; i < 8; i++) {
        float t = (i + 1) / 8.0f;
        pal_set(112 + i, (int)(G[0] * t), (int)(G[1] * t), (int)(G[2] * t));
        pal_set(120 + i, (int)(A[0] * t), (int)(A[1] * t), (int)(A[2] * t));
    }
    for (int i = 0; i < 125; i++) pal_set(128 + i, (i / 25) * 255 / 4, (i / 5 % 5) * 255 / 4, (i % 5) * 255 / 4);
    pal_set(253, 90, 200, 250); pal_set(254, 18, 18, 20); pal_set(255, 255, 150, 40);
    s_lut565 = (uint8_t *)malloc(65536);
    for (int c = 0; c < 65536; c++) {
        int r, g, b;
        c565((uint16_t)c, &r, &g, &b);
        int best = 0; long bd = -1;
        for (int i = 0; i < 256; i++) {
            long dr = r - s_pal[i][0], dg = g - s_pal[i][1], db = b - s_pal[i][2];
            long d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (bd < 0 || d < bd) { bd = d; best = i; if (!d) break; }
        }
        s_lut565[c] = (uint8_t)best;
    }
}
static uint8_t pal_index(const uint8_t *rgb)
{
    return s_lut565[((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3)];
}

struct gif_w {
    FILE *f;
    int w, h;
    uint8_t *prev, *cur;          /* indexed frames: last emitted, incoming */
    int have_prev;
    /* pending frame: written when the next differing frame arrives (delays merge) */
    int pend, px0, py0, px1, py1, pdelay;
    uint8_t *pbuf;
};

int gif_delay_30fps(int k) { return k % 3 == 2 ? 4 : 3; }

typedef struct { FILE *f; uint8_t blk[256]; int nblk; uint32_t acc; int nacc; } lzw_out;
static void lo_byte(lzw_out *o, uint8_t v)
{
    o->blk[1 + o->nblk++] = v;
    if (o->nblk == 255) { o->blk[0] = 255; fwrite(o->blk, 1, 256, o->f); o->nblk = 0; }
}
static void lo_code(lzw_out *o, int code, int bits)
{
    o->acc |= (uint32_t)code << o->nacc;
    o->nacc += bits;
    while (o->nacc >= 8) { lo_byte(o, (uint8_t)o->acc); o->acc >>= 8; o->nacc -= 8; }
}
static void lzw_encode(FILE *f, const uint8_t *px, int n)
{
    enum { CLEAR = 256, EOI = 257, HSIZE = 8192 };
    static int32_t hkey[HSIZE];
    static int16_t hval[HSIZE];
    lzw_out o = { f, {0}, 0, 0, 0 };
    fputc(8, f);
    int next = 258, bits = 9;
    for (int i = 0; i < HSIZE; i++) hkey[i] = -1;
    lo_code(&o, CLEAR, bits);
    int prefix = px[0];
    for (int i = 1; i < n; i++) {
        int c = px[i];
        int32_t key = (prefix << 8) | c;
        uint32_t h = ((uint32_t)key * 2654435761u) >> 19;
        while (hkey[h] != -1 && hkey[h] != key) h = (h + 1) & (HSIZE - 1);
        if (hkey[h] == key) { prefix = hval[h]; continue; }
        lo_code(&o, prefix, bits);
        if (next < 4096) {
            hkey[h] = key; hval[h] = (int16_t)next;
            if (next == (1 << bits) && bits < 12) bits++;
            next++;
        } else {
            lo_code(&o, CLEAR, bits);
            for (int k = 0; k < HSIZE; k++) hkey[k] = -1;
            next = 258; bits = 9;
        }
        prefix = c;
    }
    lo_code(&o, prefix, bits);
    lo_code(&o, EOI, bits);
    if (o.nacc) lo_byte(&o, (uint8_t)o.acc);
    if (o.nblk) { o.blk[0] = (uint8_t)o.nblk; fwrite(o.blk, 1, (size_t)o.nblk + 1, f); }
    fputc(0, f);
}
static void le16(FILE *f, int v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

gif_w *gif_open(const char *path, int w, int h)
{
    pal_build();
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    gif_w *g = (gif_w *)calloc(1, sizeof *g);
    g->f = f; g->w = w; g->h = h;
    g->prev = (uint8_t *)malloc((size_t)w * h);
    g->cur = (uint8_t *)malloc((size_t)w * h);
    g->pbuf = (uint8_t *)malloc((size_t)w * h);
    fwrite("GIF89a", 1, 6, f);
    le16(f, w); le16(f, h);
    fputc(0xF7, f); fputc(0, f); fputc(0, f);          /* global table, 256 entries */
    fwrite(s_pal, 1, 768, f);
    static const uint8_t loop[19] = { 0x21, 0xFF, 0x0B, 'N','E','T','S','C','A','P','E','2','.','0', 3, 1, 0, 0, 0 };
    fwrite(loop, 1, 19, f);
    return g;
}
static void gif_emit(gif_w *g)
{
    FILE *f = g->f;
    int d = g->pdelay > 65535 ? 65535 : g->pdelay;
    fputc(0x21, f); fputc(0xF9, f); fputc(4, f);
    fputc(1 << 2, f);                                   /* disposal: leave in place */
    le16(f, d); fputc(0, f); fputc(0, f);
    fputc(0x2C, f);
    le16(f, g->px0); le16(f, g->py0); le16(f, g->px1 - g->px0); le16(f, g->py1 - g->py0);
    fputc(0, f);
    lzw_encode(f, g->pbuf, (g->px1 - g->px0) * (g->py1 - g->py0));
    g->pend = 0;
}
void gif_add(gif_w *g, const sim_img *im, int delay_cs)
{
    if (!g) return;
    int w = g->w, h = g->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            g->cur[y * w + x] = (x < im->w && y < im->h) ? pal_index(&im->rgb[((size_t)y * im->w + x) * 3]) : 0;
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (g->have_prev) {
        x0 = w; y0 = h; x1 = 0; y1 = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (g->cur[y * w + x] != g->prev[y * w + x]) {
                    if (x < x0) x0 = x;
                    if (x >= x1) x1 = x + 1;
                    if (y < y0) y0 = y;
                    if (y >= y1) y1 = y + 1;
                }
        if (x1 <= x0) { g->pdelay += delay_cs; return; }   /* unchanged: extend the pending frame */
    }
    if (g->pend) gif_emit(g);
    g->px0 = x0; g->py0 = y0; g->px1 = x1; g->py1 = y1; g->pdelay = delay_cs; g->pend = 1;
    for (int y = y0; y < y1; y++) memcpy(g->pbuf + (size_t)(y - y0) * (x1 - x0), g->cur + (size_t)y * w + x0, (size_t)(x1 - x0));
    uint8_t *t = g->prev; g->prev = g->cur; g->cur = t;
    g->have_prev = 1;
}
void gif_close(gif_w *g)
{
    if (!g) return;
    if (g->pend) gif_emit(g);
    fputc(0x3B, g->f);
    fclose(g->f);
    free(g->prev); free(g->cur); free(g->pbuf);
    free(g);
}
