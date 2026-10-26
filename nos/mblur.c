#include "mblur.h"
#include <string.h>

#define MASK 0x07E0F81Fu
static inline uint32_t expand(uint16_t c) { return ((uint32_t)c | (uint32_t)c << 16) & MASK; }
static inline uint16_t pack(uint32_t c) { c &= MASK; return (uint16_t)(c | c >> 16); }

/* Keep edge/pooling bookkeeping out of this hot loop's register set. On Xtensa
 * inlining it into mb_h4 spills those long-lived values on every four pixels. */
static __attribute__((noinline)) void h4_span(uint16_t *dst, int groups, int k, const uint32_t *s)
{
    int len = 1 << k;
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) sum += s[i];
    uint32_t a = (sum >> k) & MASK;
    const uint32_t *out = s, *in = s + len;
    for (int i = 0; i < groups; i++, dst += 4) {
        sum -= *out++; sum += *in++;
        uint32_t b = (sum >> k) & MASK;
        dst[0] = pack(a);
        dst[1] = pack((3 * a + b) >> 2);
        dst[2] = pack((a + b) >> 1);
        dst[3] = pack((a + 3 * b) >> 2);
        a = b;
    }
}

/* Quarter-width point sampling, box filtering, then linear reconstruction.
 * Deliberately no full-width prefilter: it would retain most of the original
 * expansion cost. This fallback trades thin-stroke fidelity only during motion.
 * Keep the complete four-pixel groups branch-free; handle the final 1-3 separately. */
void mb_h4(uint16_t *d, int width, int log2_len, uint32_t *s)
{
    int groups = width / 4, tail = width & 3;
    int n = groups + (tail != 0), k = log2_len - 2;
    int len = 1 << k, half = len / 2;
    for (int i = 0; i < half; i++) s[i] = 0;
    for (int i = 0; i < groups; i++) s[half + i] = expand(d[i * 4]);
    if (tail) s[half + groups] = expand(d[groups * 4]);
    for (int i = 0; i <= len; i++) s[half + n + i] = 0;
    h4_span(d, groups, k, s);
    if (tail) {
        uint16_t last[4];
        h4_span(last, 1, k, s + groups);
        for (int i = 0; i < tail; i++) d[groups * 4 + i] = last[i];
    }
}

/* Share one horizontally filtered row per pair. The extra cross-axis reduction
 * pays for linear reconstruction within the live budget. Carry an unmatched row
 * over a stripe boundary, so 1-row stripes and 16-row stripes give identical pixels. */
void mb_h4_rows(gfx_fb *fb, int c0, int c1, int k, uint32_t *scratch, mb_h4_state *s)
{
    size_t bytes = (size_t)(c1 - c0) * sizeof(uint16_t);
    for (int r = 0; r < fb->h;) {
        int y = fb->y_base + r;
        uint16_t *d = gfx_row(fb, r) + c0;
        if (y != s->next) s->pending = 0;
        if (s->pending) {
            memcpy(d, s->row, bytes); s->pending = 0; r++; s->next = y + 1;
        } else {
            mb_h4(d, c1 - c0, k, scratch);
            if (r + 1 < fb->h) {
                memcpy(gfx_row(fb, r + 1) + c0, d, bytes); r += 2; s->next = y + 2;
            } else {
                memcpy(s->row, d, bytes); s->pending = 1; r++; s->next = y + 1;
            }
        }
    }
}

/* Causal quarter-height smear, shared by four columns (mean, so narrow vertical
 * strokes still contribute). Sample
 * every fourth row, update its exponential state with tau/4, and interpolate
 * from the previous filtered sample to this one over four output rows. This
 * adds at most three rows of spatial lag, but needs no future stripe. State
 * survives arbitrary stripe boundaries; a discontinuity starts a new segment. */
void mb_v4(gfx_fb *fb, int c0, int c1, int y0, int y1, int k, mb_v4_state *s)
{
    int shift = k > 2 ? k - 2 : 0;
    uint32_t lo = (1u << shift) - 1u;
    uint32_t hi = ~(lo | lo << 11 | lo << 21);
    for (int r = 0; r < fb->h;) {
        int y = fb->y_base + r - fb->oy;
        if (y < y0 || y >= y1) { r++; continue; }
        int fresh = y != s->next;
        if (fresh) s->origin = y;
        int phase = (y - s->origin) & 3;
        int rows = 4 - phase;
        if (rows > fb->h - r) rows = fb->h - r;
        if (rows > y1 - y) rows = y1 - y;
        uint16_t *dst[4];
        for (int j = 0; j < rows; j++) dst[j] = gfx_row(fb, r + j);
        for (int x = c0, i = 0; x < c1; x += 4, i++) {
            int cols = c1 - x < 4 ? c1 - x : 4;
            if (!phase) {
                uint32_t v = expand(dst[0][x]);
                v += expand(dst[0][x + (cols > 1 ? 1 : 0)]);
                v += expand(dst[0][x + (cols > 2 ? 2 : cols - 1)]);
                v += expand(dst[0][x + cols - 1]);
                v = (v >> 2) & MASK;
                uint32_t old = s->sum[i];
                if (fresh) old = v << 5;
                s->prev[i] = (old >> 5) & MASK;
                s->sum[i] = old - ((old & hi) >> shift) + (v << (5 - shift));
            }
            uint32_t a = s->prev[i], b = (s->sum[i] >> 5) & MASK;
            uint16_t out[4] = { pack((3 * a + b) >> 2), pack((a + b) >> 1),
                                pack((a + 3 * b) >> 2), pack(b) };
            if (rows == 4 && cols == 4) {
                for (int j = 0; j < 4; j++) {
                    dst[j][x] = out[j]; dst[j][x + 1] = out[j];
                    dst[j][x + 2] = out[j]; dst[j][x + 3] = out[j];
                }
                continue;
            }
            for (int j = 0; j < rows; j++) {
                for (int dx = 0; dx < cols; dx++) dst[j][x + dx] = out[phase + j];
            }
        }
        s->next = y + rows;
        r += rows;
    }
}

void mb_v_columns_reset(mb_v_columns_state *s)
{
    memset(s->next,0xff,sizeof s->next);
}
/* Only the first processed row needs per-column continuity checks. Every
 * following row within this call has the same columns and contiguous y, so its
 * span needs no next-tag loads/stores. Publish each column's final next row once
 * at the end; separate calls still retain independent tags for clipped damage. */
static __attribute__((noinline)) void v_first_1(uint16_t *row,uint32_t *sums,const uint16_t *next,int n,uint16_t y)
{
    const uint32_t low=(1u<<1)-1u,high=~(low | low<<11 | low<<21);
    for(int x=0;x<n;x++) {
        uint32_t value=expand(row[x]);
        uint32_t sum=next[x]!=y?value<<5:sums[x]-((sums[x]&high)>>1)+(value<<(5-1));
        sums[x]=sum;row[x]=pack((sum>>5)&MASK);
    }
}
static __attribute__((noinline)) void v_continue_1(uint16_t *row,uint32_t *sums,int n)
{
    const uint32_t low=(1u<<1)-1u,high=~(low | low<<11 | low<<21);
    for(int x=0;x<n;x++) {
        uint32_t value=expand(row[x]);
        uint32_t sum=sums[x]-((sums[x]&high)>>1)+(value<<(5-1));
        sums[x]=sum;row[x]=pack((sum>>5)&MASK);
    }
}

static __attribute__((noinline)) void v_first_2(uint16_t *row,uint32_t *sums,const uint16_t *next,int n,uint16_t y)
{
    const uint32_t low=(1u<<2)-1u,high=~(low | low<<11 | low<<21);
    for(int x=0;x<n;x++) {
        uint32_t value=expand(row[x]);
        uint32_t sum=next[x]!=y?value<<5:sums[x]-((sums[x]&high)>>2)+(value<<(5-2));
        sums[x]=sum;row[x]=pack((sum>>5)&MASK);
    }
}
static __attribute__((noinline)) void v_continue_2(uint16_t *row,uint32_t *sums,int n)
{
    const uint32_t low=(1u<<2)-1u,high=~(low | low<<11 | low<<21);
    for(int x=0;x<n;x++) {
        uint32_t value=expand(row[x]);
        uint32_t sum=sums[x]-((sums[x]&high)>>2)+(value<<(5-2));
        sums[x]=sum;row[x]=pack((sum>>5)&MASK);
    }
}

static __attribute__((noinline)) void v_first_3(uint16_t *row,uint32_t *sums,const uint16_t *next,int n,uint16_t y)
{
    const uint32_t low=(1u<<3)-1u,high=~(low | low<<11 | low<<21);
    for(int x=0;x<n;x++) {
        uint32_t value=expand(row[x]);
        uint32_t sum=next[x]!=y?value<<5:sums[x]-((sums[x]&high)>>3)+(value<<(5-3));
        sums[x]=sum;row[x]=pack((sum>>5)&MASK);
    }
}
static __attribute__((noinline)) void v_continue_3(uint16_t *row,uint32_t *sums,int n)
{
    const uint32_t low=(1u<<3)-1u,high=~(low | low<<11 | low<<21);
    for(int x=0;x<n;x++) {
        uint32_t value=expand(row[x]);
        uint32_t sum=sums[x]-((sums[x]&high)>>3)+(value<<(5-3));
        sums[x]=sum;row[x]=pack((sum>>5)&MASK);
    }
}

static __attribute__((noinline)) void v_first_4(uint16_t *row,uint32_t *sums,const uint16_t *next,int n,uint16_t y)
{
    const uint32_t low=(1u<<4)-1u,high=~(low | low<<11 | low<<21);
    for(int x=0;x<n;x++) {
        uint32_t value=expand(row[x]);
        uint32_t sum=next[x]!=y?value<<5:sums[x]-((sums[x]&high)>>4)+(value<<(5-4));
        sums[x]=sum;row[x]=pack((sum>>5)&MASK);
    }
}
static __attribute__((noinline)) void v_continue_4(uint16_t *row,uint32_t *sums,int n)
{
    const uint32_t low=(1u<<4)-1u,high=~(low | low<<11 | low<<21);
    for(int x=0;x<n;x++) {
        uint32_t value=expand(row[x]);
        uint32_t sum=sums[x]-((sums[x]&high)>>4)+(value<<(5-4));
        sums[x]=sum;row[x]=pack((sum>>5)&MASK);
    }
}
void mb_v_columns(gfx_fb *fb,int c0,int c1,int y0,int y1,int k,mb_v_columns_state *s)
{
    int first=y0+fb->oy-fb->y_base,last=y1+fb->oy-fb->y_base;
    if(first<0) first=0;
    if(last>fb->h) last=fb->h;
    if(last<=first || c1<=c0) return;
    int y=fb->y_base+first-fb->oy,n=c1-c0;
    uint16_t *row=gfx_row(fb,first)+c0;
    if(k==1) v_first_1(row,s->sum+c0,s->next+c0,n,(uint16_t)y);
    else if(k==2) v_first_2(row,s->sum+c0,s->next+c0,n,(uint16_t)y);
    else if(k==3) v_first_3(row,s->sum+c0,s->next+c0,n,(uint16_t)y);
    else v_first_4(row,s->sum+c0,s->next+c0,n,(uint16_t)y);
    for(int r=first+1;r<last;r++) {
        row=gfx_row(fb,r)+c0;
        if(k==1) v_continue_1(row,s->sum+c0,n);
        else if(k==2) v_continue_2(row,s->sum+c0,n);
        else if(k==3) v_continue_3(row,s->sum+c0,n);
        else v_continue_4(row,s->sum+c0,n);
    }
    uint16_t following=(uint16_t)(fb->y_base+last-fb->oy);
    for(int x=c0;x<c1;x++) s->next[x]=following;
}
