#include "render_history.h"
#include "hal.h"
#include "render_config.h"
#include <string.h>

#define HISTORY_WORDS ((GFX_W + 31) / 32)
#define HISTORY_BITS ((uint32_t)GFX_H * HISTORY_WORDS * sizeof(uint32_t))
#define HISTORY_PIXELS ((uint32_t)GFX_W * GFX_H)
#define HISTORY_BYTES (HISTORY_PIXELS * sizeof(uint16_t))
#define HISTORY_STORAGE (HISTORY_BYTES + 2u * HISTORY_BITS)
static uint16_t *s_sharp;
static uint32_t *s_valid, *s_seen;
static nos_history_stats s_stats;
static int s_begun;
int nos_history_cadence=0; /* Keep75/25: cadence candidate failed target frame gates. */
static unsigned s_previous_q5=8;
static uint32_t s_interval_us;
/* Idle frames touch no PSRAM coverage bytes. Clear a row lazily only when a
 * stripe actually visits it in this generation. 1.8 KiB of internal tags. */
static uint32_t s_row_generation[GFX_H], s_generation;
static uint16_t s_valid_lo[GFX_H],s_valid_hi[GFX_H];
int nos_history_native_copy=1; /* exact native-span copy A/B; generic exposure unchanged */
int nos_history_uniform=1; /* runtime exact A/B, no blur eligibility change */
static int s_virtual_enabled; /* Exact experimental representation, off by default. */
static nos_history_virtual_stats s_virtual_stats;
static uint32_t s_uniform[GFX_H]; /* 2 bits/32px: unknown, zero, white */

int nos_history_init(void)
{
    if(NOS_RENDER_SHARP)return 1;
    if (s_sharp) return 1;
    /* RGB565 history size is 4-byte aligned for the two following bitmaps. */
    s_sharp = hal_big_alloc(HISTORY_STORAGE);
    if (!s_sharp) return 0;
    s_valid = (uint32_t *)((uint8_t *)s_sharp + HISTORY_BYTES);
    s_seen = (uint32_t *)((uint8_t *)s_valid + HISTORY_BITS);
    nos_history_reset();
    return 1;
}
void nos_history_reset(void)
{
    if (s_sharp) {
        memset(s_valid, 0, HISTORY_BITS);
    }
    memset(s_row_generation, 0, sizeof s_row_generation);
    s_generation = 1;
    /* Validity is retired too, so virtual tags need no physical materialization. */
    memset(s_uniform,0,sizeof s_uniform);
    memset(s_valid_lo,0,sizeof s_valid_lo);memset(s_valid_hi,0,sizeof s_valid_hi);
    memset(&s_stats, 0, sizeof s_stats);
    s_begun = 0;
    s_previous_q5 = 8; s_interval_us = 0;
}
static void history_begin(uint32_t frame,uint32_t elapsed_us)
{
    if (s_begun && s_stats.frame == frame) return;
    s_interval_us = elapsed_us;
    s_previous_q5 = 8;
    if (nos_history_cadence && elapsed_us) {
        /* Target mean age is1/120s. Quantize its previous-sharp weight once
         * per generation, using the same elapsed interval as scene motion.
         * Wide arithmetic also bounds unusual UINT32_MAX host intervals. */
        uint64_t denominator = 120ull * elapsed_us;
        uint64_t weight = (32000000ull + denominator / 2) / denominator;
        s_previous_q5 = weight > 32 ? 32 : (unsigned)weight;
    }
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.frame = frame;
    if (++s_generation == 0) {
        memset(s_row_generation, 0, sizeof s_row_generation);
        s_generation = 1;
    }
    s_begun = 1;
}
void nos_history_begin(uint32_t frame) { history_begin(frame,0); }
void nos_history_begin_interval(uint32_t frame,uint32_t elapsed_us) { history_begin(frame,elapsed_us); }
unsigned nos_history_previous_weight_q5(void) { return s_previous_q5; }
uint32_t nos_history_interval_us(void) { return s_interval_us; }
static unsigned bit_count(uint32_t v)
{
    v -= (v >> 1) & 0x55555555u;
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0f0f0f0fu) * 0x01010101u) >> 24;
}
static uint16_t expose(uint16_t current, uint16_t previous)
{
    /* Separate R/B/G lanes, add half the denominator in EACH channel.
     * No division, cross-channel carry, or feedback of a blurred output. */
    const uint32_t mask = 0x07e0f81fu;
    uint32_t c = (current | ((uint32_t)current << 16)) & mask;
    uint32_t p = (previous | ((uint32_t)previous << 16)) & mask;
    uint32_t v = ((3u * c + p + 0x00401002u) >> 2) & mask;
    return (uint16_t)(v | (v >> 16));
}
static uint16_t expose_q5(uint16_t current,uint16_t previous,unsigned weight)
{
    const uint32_t mask = 0x07e0f81fu;
    uint32_t c = (current | ((uint32_t)current << 16)) & mask;
    uint32_t p = (previous | ((uint32_t)previous << 16)) & mask;
    /* R/B sums fit their guard bits; G<=63*32+16 fits its11-bit lane.
     * Modular difference multiplication equals (32-weight)*c+weight*p. */
    uint32_t v = (((c << 5) + (p - c) * weight + 0x02008010u) >> 5) & mask;
    return (uint16_t)(v | (v >> 16));
}
static uint16_t expose_frame(uint16_t current,uint16_t previous)
{
    return s_previous_q5 == 8 ? expose(current,previous) : expose_q5(current,previous,s_previous_q5);
}
/* For every channel C=4*qC+rC and P=4*qP+rP. The rounded result is
 * 3*qC+qP+floor((3*rC+rP+2)/4). HIGH selects quotient bits, LOW the
 * two remainder bits, repeated in both RGB565 halfwords. The remainder sum
 * is at most14 and cannot cross a channel boundary; the final result fits
 * each channel. This exactly matches two scalar expose calls. */
int nos_history_packed = 1; /* Debug A/B selected once per span, outside pixels. */
static uint32_t expose_pair(uint32_t c, uint32_t p) {
    const uint32_t high = 0xe79ce79cu, low = 0x18631863u, bias = 0x10421042u;
    return 3u * ((c & high) >> 2) + ((p & high) >> 2) +
           (((3u * (c & low) + (p & low) + bias) >> 2) & low);
}
/* Interleave the two pixels' channels into two independent groups:
 * A has B0/R0/G1 at bits0/11/21; B has G0/B1/R1 at bits0/11/22 after >>5.
 * With0<=weight<=32, each rounded 5-bit sum is<=31*32+16=1008 (10 bits),
 * and each6-bit sum is<=63*32+16=2032 (11 bits). These fit their lanes without
 * carries or uint32_t overflow. Unsigned modular subtraction gives the same
 * (32-weight)*current+weight*previous sum; +16 then >>5 rounds each channel
 * exactly as the scalar kernel. The final masks restore RGB565 channel bits. */
static uint32_t expose_pair_q5(uint32_t c,uint32_t p,unsigned weight)
{
    const uint32_t ma = 0x07e0f81fu, mb = 0x07c0f83fu;
    uint32_t cc = c & ma, pp = p & ma;
    uint32_t a = (((cc << 5) + (pp - cc) * weight + 0x02008010u) >> 5) & ma;
#if defined(__GNUC__)
    /* GNU/Clang register fence finishes A before expanding B: no instructions,
     * and fewer live intermediates in the Xtensa changed-pair loop. */
    __asm__ __volatile__("" : "+r"(c), "+r"(p), "+r"(a));
#endif
    cc = (c >> 5) & mb; pp = (p >> 5) & mb;
    uint32_t b = (((cc << 5) + (pp - cc) * weight + 0x04008010u) >> 5) & mb;
    return a | (b << 5);
}
/* Normal rendering visits disjoint windows with fully initialized history.
 * Check coverage once per bitmap word, then keep its pixel loop free of mask
 * tests and global statistics stores. Unaligned-to-four RGB565 is valid here. */
#if defined(__GNUC__)
__attribute__((noinline)) /* one call per bitmap word, keeping loop registers local */
#endif
static void ready_span_aligned(uint16_t *restrict out, uint16_t *restrict old, int count,
                               int enabled) {
#if defined(__GNUC__)
    typedef uint32_t history_word __attribute__((__may_alias__));
#else
    typedef uint32_t history_word;
#endif
    const int aligned = 1;

    unsigned changed = 0, unequal_words = 0;
    int i = 0;
    if (enabled) {
        for (; i + 1 < count; i += 2) {
            uint32_t c, p;
            if (aligned) {
                c = *(const history_word *)(out + i);
                p = *(const history_word *)(old + i);
            } else {
                c = (uint32_t)out[i] | ((uint32_t)out[i + 1] << 16);
                p = (uint32_t)old[i] | ((uint32_t)old[i + 1] << 16);
            }
            if (c == p)
                continue;
            unequal_words++;
#if defined(__GNUC__)
            /* Keep RGB565 arithmetic behind equality on Xtensa. GCC otherwise
             * speculates the entire exposure calculation for unchanged pairs. */
            __asm__ __volatile__("" : "+r"(c), "+r"(p));
#endif
            uint32_t difference = c ^ p;
            changed += ((difference & 0xffffu) != 0) + (difference >> 16 != 0);
            *(history_word *)(old + i) = c;
            *(history_word *)(out + i) = expose_pair(c, p);
        }
    } else {
        /* Already directionally blurred spans only update sharp history.
         * A separate loop prevents speculative exposure arithmetic on them. */
        for (; i + 1 < count; i += 2) {
            uint32_t c, p;
            if (aligned) {
                c = *(const history_word *)(out + i);
                p = *(const history_word *)(old + i);
            } else {
                c = (uint32_t)out[i] | ((uint32_t)out[i + 1] << 16);
                p = (uint32_t)old[i] | ((uint32_t)old[i + 1] << 16);
            }
            if (c == p)
                continue;
            unequal_words++;
#if defined(__GNUC__)
            /* Keep RGB565 arithmetic behind equality on Xtensa. GCC otherwise
             * speculates the entire exposure calculation for unchanged pairs. */
            __asm__ __volatile__("" : "+r"(c), "+r"(p));
#endif
            uint32_t difference = c ^ p;
            changed += ((difference & 0xffffu) != 0) + (difference >> 16 != 0);
            *(history_word *)(old + i) = c;
        }
    }
    if (i < count) {
        uint16_t c = out[i], p = old[i];
        if (c != p) {
            changed++;
            old[i] = c;
            if (enabled)
                out[i] = expose(c, p);
        }
    }
    s_stats.pixels += (unsigned)count;
    s_stats.changed += changed;
    s_stats.unchanged += (unsigned)count - changed;
    s_stats.blended += enabled ? changed : 0;
    s_stats.equal_words += (unsigned)(count / 2) - unequal_words;
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void ready_span_aligned_legacy(uint16_t *restrict out, uint16_t *restrict old, int count,
                                      int enabled) {
#if defined(__GNUC__)
    typedef uint32_t history_word __attribute__((__may_alias__));
#else
    typedef uint32_t history_word;
#endif
    const int aligned = 1;

    unsigned changed = 0, unequal_words = 0;
    int i = 0;
    if (enabled) {
        for (; i + 1 < count; i += 2) {
            uint32_t c, p;
            if (aligned) {
                c = *(const history_word *)(out + i);
                p = *(const history_word *)(old + i);
            } else {
                c = (uint32_t)out[i] | ((uint32_t)out[i + 1] << 16);
                p = (uint32_t)old[i] | ((uint32_t)old[i + 1] << 16);
            }
            if (c == p)
                continue;
            unequal_words++;
#if defined(__GNUC__)
            /* Keep RGB565 arithmetic behind equality on Xtensa. GCC otherwise
             * speculates the entire exposure calculation for unchanged pairs. */
            __asm__ __volatile__("" : "+r"(c), "+r"(p));
#endif
            uint16_t c0 = (uint16_t)c, c1 = (uint16_t)(c >> 16);
            uint16_t p0 = (uint16_t)p, p1 = (uint16_t)(p >> 16);
            changed += (c0 != p0) + (c1 != p1);
            old[i] = c0;
            old[i + 1] = c1;
            out[i] = expose(c0, p0);
            out[i + 1] = expose(c1, p1);
        }
    } else {
        /* Already directionally blurred spans only update sharp history.
         * A separate loop prevents speculative exposure arithmetic on them. */
        for (; i + 1 < count; i += 2) {
            uint32_t c, p;
            if (aligned) {
                c = *(const history_word *)(out + i);
                p = *(const history_word *)(old + i);
            } else {
                c = (uint32_t)out[i] | ((uint32_t)out[i + 1] << 16);
                p = (uint32_t)old[i] | ((uint32_t)old[i + 1] << 16);
            }
            if (c == p)
                continue;
            unequal_words++;
#if defined(__GNUC__)
            /* Keep RGB565 arithmetic behind equality on Xtensa. GCC otherwise
             * speculates the entire exposure calculation for unchanged pairs. */
            __asm__ __volatile__("" : "+r"(c), "+r"(p));
#endif
            uint16_t c0 = (uint16_t)c, c1 = (uint16_t)(c >> 16);
            uint16_t p0 = (uint16_t)p, p1 = (uint16_t)(p >> 16);
            changed += (c0 != p0) + (c1 != p1);
            old[i] = c0;
            old[i + 1] = c1;
        }
    }
    if (i < count) {
        uint16_t c = out[i], p = old[i];
        if (c != p) {
            changed++;
            old[i] = c;
            if (enabled)
                out[i] = expose(c, p);
        }
    }
    s_stats.pixels += (unsigned)count;
    s_stats.changed += changed;
    s_stats.unchanged += (unsigned)count - changed;
    s_stats.blended += enabled ? changed : 0;
    s_stats.equal_words += (unsigned)(count / 2) - unequal_words;
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void ready_span_unaligned(uint16_t *restrict out, uint16_t *restrict old, int count,
                                 int enabled) {
#if defined(__GNUC__)
    typedef uint32_t history_word __attribute__((__may_alias__));
#else
    typedef uint32_t history_word;
#endif
    const int aligned=0;

    unsigned changed = 0, unequal_words = 0;
    int i = 0;
    if (enabled) {
        for (; i + 1 < count; i += 2) {
            uint32_t c,p;
            if(aligned) {
                c=*(const history_word *)(out+i);p=*(const history_word *)(old+i);
            } else {
                c=(uint32_t)out[i]|((uint32_t)out[i+1]<<16);
                p=(uint32_t)old[i]|((uint32_t)old[i+1]<<16);
            }
            if(c==p) continue;
            unequal_words++;
#if defined(__GNUC__)
            /* Keep RGB565 arithmetic behind equality on Xtensa. GCC otherwise
             * speculates the entire exposure calculation for unchanged pairs. */
            __asm__ __volatile__("" : "+r"(c), "+r"(p));
#endif
            uint16_t c0=(uint16_t)c,c1=(uint16_t)(c>>16);
            uint16_t p0=(uint16_t)p,p1=(uint16_t)(p>>16);
            changed += (c0 != p0) + (c1 != p1);
            old[i] = c0; old[i + 1] = c1;
            out[i] = expose(c0, p0); out[i + 1] = expose(c1, p1);
        }
    } else {
        /* Already directionally blurred spans only update sharp history.
         * A separate loop prevents speculative exposure arithmetic on them. */
        for (; i + 1 < count; i += 2) {
            uint32_t c,p;
            if(aligned) {
                c=*(const history_word *)(out+i);p=*(const history_word *)(old+i);
            } else {
                c=(uint32_t)out[i]|((uint32_t)out[i+1]<<16);
                p=(uint32_t)old[i]|((uint32_t)old[i+1]<<16);
            }
            if(c==p) continue;
            unequal_words++;
#if defined(__GNUC__)
            /* Keep RGB565 arithmetic behind equality on Xtensa. GCC otherwise
             * speculates the entire exposure calculation for unchanged pairs. */
            __asm__ __volatile__("" : "+r"(c), "+r"(p));
#endif
            uint16_t c0=(uint16_t)c,c1=(uint16_t)(c>>16);
            uint16_t p0=(uint16_t)p,p1=(uint16_t)(p>>16);
            changed += (c0 != p0) + (c1 != p1);
            old[i] = c0; old[i + 1] = c1;
        }
    }
    if (i < count) {
        uint16_t c = out[i], p = old[i];
        if (c != p) {
            changed++; old[i] = c;
            if (enabled) out[i] = expose(c, p);
        }
    }
    s_stats.pixels += (unsigned)count;
    s_stats.changed += changed;
    s_stats.unchanged += (unsigned)count - changed;
    s_stats.blended += enabled ? changed : 0;
    s_stats.equal_words += (unsigned)(count/2)-unequal_words;
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void ready_span_cadence(uint16_t *restrict out,uint16_t *restrict old,int count,
                               unsigned weight,int packed)
{
#if defined(__GNUC__)
    typedef uint32_t history_word __attribute__((__may_alias__));
#else
    typedef uint32_t history_word;
#endif
    unsigned changed = 0, unequal_words = 0;
    int i = 0;
    if (packed) {
        for (; i + 1 < count; i += 2) {
            uint32_t c = *(const history_word *)(out + i);
            uint32_t p = *(const history_word *)(old + i);
            if (c == p) continue;
            unequal_words++;
#if defined(__GNUC__)
            __asm__ __volatile__("" : "+r"(c), "+r"(p));
#endif
            uint32_t difference = c ^ p;
            changed += ((difference & 0xffffu) != 0) + (difference >> 16 != 0);
            *(history_word *)(old + i) = c;
#if defined(__GNUC__)
            /* Store sharp history before the heavier exposure math; keeping its
             * source live across that math would spill the hot loop's state. */
            __asm__ __volatile__("" : "+r"(c), "+r"(p) : : "memory");
#endif
            *(history_word *)(out + i) = expose_pair_q5(c,p,weight);
        }
    } else {
        for (; i + 1 < count; i += 2) {
            uint16_t c0 = out[i], c1 = out[i + 1], p0 = old[i], p1 = old[i + 1];
            if (c0 == p0 && c1 == p1) continue;
            unequal_words++;
            changed += (c0 != p0) + (c1 != p1);
            old[i] = c0; old[i + 1] = c1;
            out[i] = expose_q5(c0,p0,weight);
            out[i + 1] = expose_q5(c1,p1,weight);
        }
    }
    if (i < count) {
        uint16_t c = out[i], p = old[i];
        if (c != p) {
            changed++; old[i] = c;
            out[i] = expose_q5(c,p,weight);
        }
    }
    s_stats.pixels += (unsigned)count;
    s_stats.changed += changed;
    s_stats.unchanged += (unsigned)count - changed;
    s_stats.blended += changed;
    s_stats.equal_words += (unsigned)(count / 2) - unequal_words;
}
static void ready_span(uint16_t *restrict out, uint16_t *restrict old, int count, int enabled) {
    if (enabled && s_previous_q5 != 8) {
        int packed = nos_history_packed && (((uintptr_t)out | (uintptr_t)old) & 3u) == 0;
        ready_span_cadence(out,old,count,s_previous_q5,packed);
        return;
    }
    if ((((uintptr_t)out | (uintptr_t)old) & 3u) == 0) {
        if (nos_history_packed)
            ready_span_aligned(out, old, count, enabled);
        else
            ready_span_aligned_legacy(out, old, count, enabled);
    } else
        ready_span_unaligned(out, old, count, enabled);
}

static unsigned uniform_current(const uint16_t *p,int count)
{
    typedef uint32_t word __attribute__((__may_alias__));
    if(!((uintptr_t)p&3u) && !(count&1)) {
        const word *w=(const word *)p;
        uint32_t c=w[0];if(c && c!=0xffffffffu) return 0;
        if(count==32) {
            /* GCC/Clang register fences keep second-group loads behind the
             * first check, avoiding Xtensa spills. They emit no instructions;
             * other compilers retain the same exact reductions without them. */
            if(c) {
                if((w[1]&w[2]&w[3]&w[4]&w[5]&w[6]&w[7])!=c)return 0;
#if defined(__GNUC__)
                __asm__ __volatile__("" : "+r"(w));
#endif
                if((w[8]&w[9]&w[10]&w[11]&w[12]&w[13]&w[14]&w[15])!=c)return 0;
                return 2u;
            } else {
                if((w[1]|w[2]|w[3]|w[4]|w[5]|w[6]|w[7])!=c)return 0;
#if defined(__GNUC__)
                __asm__ __volatile__("" : "+r"(w));
#endif
                if((w[8]|w[9]|w[10]|w[11]|w[12]|w[13]|w[14]|w[15])!=c)return 0;
                return 1u;
            }
        }
        int i=1, words=count/2;
        if(c) {
            for(;i+7<words;i+=8) {
                uint32_t v=w[i]&w[i+1]&w[i+2]&w[i+3]&w[i+4]&w[i+5]&w[i+6]&w[i+7];
                if(v!=c)return 0;
            }
        } else {
            for(;i+7<words;i+=8) {
                uint32_t v=w[i]|w[i+1]|w[i+2]|w[i+3]|w[i+4]|w[i+5]|w[i+6]|w[i+7];
                if(v!=c)return 0;
            }
        }
        for(;i<words;i++)if(w[i]!=c)return 0;
        return c?2u:1u;
    }
    uint16_t c=p[0];if(c && c!=0xffffu) return 0;
    for(int i=1;i<count;i++) if(p[i]!=c) return 0;
    return c?2u:1u;
}
/* Tag3 is logical black whose physical PSRAM contents may be stale. */
static void virtual_materialize(int y,int x0,int x1) {
    uint32_t tags=s_uniform[y];
    if(!(tags & (tags>>1) & 0x55555555u))return;
    unsigned bytes=0;
    for(int block=x0/32;block<=(x1-1)/32;block++) {
        unsigned shift=(unsigned)(2*block);
        if(((s_uniform[y]>>shift)&3u)!=3u)continue;
        int start=block*32,n=GFX_W-start;if(n>32)n=32;
        memset(s_sharp+(size_t)y*GFX_W+start,0,(size_t)n*sizeof(uint16_t));
        bytes+=(unsigned)n*2u;
        s_uniform[y]=(s_uniform[y]&~(3u<<shift))|(1u<<shift);
    }
    s_virtual_stats.materialize_bytes+=bytes;
}
/* Raw debug/test consumers must materialize before accessing RGB565 storage. */
void nos_history_materialize(void) {
    if(!s_sharp)return;
    for(int y=0;y<GFX_H;y++)virtual_materialize(y,0,GFX_W);
}
int nos_history_virtual_get(void) { return s_virtual_enabled; }
const nos_history_virtual_stats *nos_history_virtual_get_stats(void) { return &s_virtual_stats; }
void nos_history_virtual_set(int enabled) {
    if(!enabled)nos_history_materialize();
    s_virtual_enabled=!!enabled;
}
static void known_run(uint16_t *,uint16_t *,int,int,int);
static void virtual_native(uint16_t *row,uint16_t *old,int x0,int x1,int y) {
    int run=x0;
    unsigned made_virtual=0,classified=0;
    for(int x=x0;x<x1;) {
        int block=x/32,end=(block+1)*32;if(end>GFX_W)end=GFX_W;if(end>x1)end=x1;
        unsigned shift=(unsigned)(2*block);
        int complete=x==block*32 && (end==(block+1)*32 || end==GFX_W);
        int black=0;
        if(complete && !row[x-x0]){classified+=(unsigned)(end-x);black=uniform_current(row+x-x0,end-x)==1;}
        if(black) {
            if(x>run)known_run(row+run-x0,old+run-x0,run,x-run,0);
            s_uniform[y]|=3u<<shift;
            made_virtual+=(unsigned)(end-x);run=end;
        } else {
            /* Full overwrite replaces all stale bytes. Partial overwrite must
             * preserve untouched logical black pixels elsewhere in the block. */
            if(!complete)virtual_materialize(y,x,end);
            s_uniform[y]&=~(3u<<shift);
        }
        x=end;
    }
    known_run(row+run-x0,old+run-x0,run,x1-run,0);
    s_stats.pixels+=made_virtual;
    /* copied remains logical native updates, preserving the existing invariant.
     * Physical bytes and virtual savings are separate diagnostics. */
    s_stats.copied+=made_virtual;
    s_virtual_stats.virtual_pixels+=made_virtual;
    s_virtual_stats.avoided_bytes+=(uint64_t)made_virtual*2u;
    s_virtual_stats.classify_pixels+=classified;
}
static void known_run(uint16_t *row,uint16_t *old,int x,int n,int enabled)
{
    if(!n)return;
    if(!enabled) {
        memcpy(old,row,(size_t)n*sizeof *old);
        s_stats.pixels+=(unsigned)n;s_stats.copied+=(unsigned)n;return;
    }
    int prefix=(x&1)?32-(x&31):0;if(prefix>n)prefix=n;
    if(prefix) {ready_span(row,old,prefix,1);row+=prefix;old+=prefix;n-=prefix;}
    if(n)ready_span(row,old,n,1);
}
int nos_history_apply(gfx_fb *fb, int x0, int y0, int x1, int y1, int enabled)
{
    if (!s_sharp) return 0;
    if (!fb || !fb->px || fb->rec || fb->h <= 0) return 1;
    int stride = fb->stride ? fb->stride : GFX_W;
    if (stride <= 0) return 1;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (x0 < 0) x0 = 0;
    if (x0 < c0) x0 = c0;
    if (x0 < fb->x_base) x0 = fb->x_base;
    if (x1 > GFX_W) x1 = GFX_W;
    if (x1 > c1) x1 = c1;
    int64_t storage_x1 = (int64_t)fb->x_base + stride;
    if ((int64_t)x1 > storage_x1) x1 = (int)storage_x1;
    if (y0 < 0) y0 = 0;
    if (y0 < fb->y_base) y0 = fb->y_base;
    if (y1 > GFX_H) y1 = GFX_H;
    int64_t storage_y1 = (int64_t)fb->y_base + fb->h;
    if ((int64_t)y1 > storage_y1) y1 = (int)storage_y1;
    if (x1 <= x0 || y1 <= y0) return 1;
    for (int y = y0; y < y1; y++) {
        /* Physical readers must consume logical black before dropping tags. */
        virtual_materialize(y,x0,x1);
        int first=x0/32,last=(x1-1)/32;
        s_uniform[y]&=~(((1u<<(2*(last-first+1)))-1u)<<(2*first));
        if (s_row_generation[y] != s_generation) {
            memset(s_seen + (size_t)y * HISTORY_WORDS, 0, HISTORY_WORDS * sizeof(uint32_t));
            s_row_generation[y] = s_generation;
            s_stats.rows_cleared++;
        }
        uint16_t *row = fb->px + (size_t)(y - fb->y_base) * stride;
        uint16_t *old = s_sharp + (size_t)y * GFX_W;
        for (int x = x0; x < x1;) {
            int word = x / 32, end = (word + 1) * 32;
            if (end > x1) end = x1;
            int lo = x & 31, hi = (end - 1) & 31;
            uint32_t mask = (0xffffffffu << lo) & (0xffffffffu >> (31 - hi));
            size_t k = (size_t)y * HISTORY_WORDS + word;
            uint32_t seen = s_seen[k], valid = s_valid[k];
            s_stats.duplicates += bit_count(mask & seen);
            uint32_t todo = mask & ~seen;
            s_seen[k] = seen | mask;
            if((valid&mask)!=mask) s_valid[k] = valid | mask;
            if (todo == mask && !(valid & mask)) {
                unsigned count = (unsigned)(end - x);
                /* One bounded bulk copy seeds newly visible pixels; no blend
                 * or per-pixel validity work is needed on their first frame. */
                memcpy(old + x, row + (x - fb->x_base), count * sizeof(uint16_t));
                s_stats.pixels += count; s_stats.seeded += count;
                x = end; continue;
            }
            if (todo == mask && (valid & mask) == mask) {
                ready_span(row + (x - fb->x_base), old + x, end - x, enabled);
                x = end; continue;
            }
            while (x < end) {
                uint32_t bit = 1u << (x & 31);
                if (!(todo & bit)) { x++; continue; }
                uint16_t *out = row + (x - fb->x_base);
                /* The IDF build disables memcpy builtins: even a four-byte copy
                 * becomes a library call. Halfword loads are naturally aligned
                 * for RGB565, including odd packed strides and window origins. */
                if (x + 1 < end && (todo & (bit << 1)) &&
                    (valid & bit) && (valid & (bit << 1))) {
                    uint16_t c0 = out[0], c1 = out[1];
                    uint16_t p0 = old[x], p1 = old[x + 1];
                    s_stats.pixels += 2;
                    if (c0 == p0 && c1 == p1) {
                        s_stats.unchanged += 2; s_stats.equal_words++;
                    } else {
                        /* Consume the unequal pair now, rather than loading an
                         * overlapping word again on the following pixel. */
                        unsigned changed = (c0 != p0) + (c1 != p1);
                        s_stats.changed += changed; s_stats.unchanged += 2 - changed;
                        old[x] = c0; old[x + 1] = c1; /* sharp, never exposure */
                        if (enabled) {
                            out[0] = expose_frame(c0, p0); out[1] = expose_frame(c1, p1);
                            s_stats.blended += changed;
                        }
                    }
                    x += 2; continue;
                }
                uint16_t current = *out;
                s_stats.pixels++;
                if (!(valid & bit)) { old[x] = current; s_stats.seeded++; }
                else if (old[x] == current) s_stats.unchanged++;
                else {
                    uint16_t previous = old[x];
                    old[x] = current; s_stats.changed++;
                    if (enabled) { *out = expose_frame(current, previous); s_stats.blended++; }
                }
                x++;
            }
        }
        /* Only retain a known contiguous initialized interval, never a hole. */
        if(s_valid_hi[y]<=s_valid_lo[y] || (x0<=s_valid_hi[y] && x1>=s_valid_lo[y])) {
            if(s_valid_hi[y]<=s_valid_lo[y] || x0<s_valid_lo[y]) s_valid_lo[y]=(uint16_t)x0;
            if(x1>s_valid_hi[y]) s_valid_hi[y]=(uint16_t)x1;
        } else if(x1-x0>s_valid_hi[y]-s_valid_lo[y]) {
            s_valid_lo[y]=(uint16_t)x0;s_valid_hi[y]=(uint16_t)x1;
        }
    }
    return 1;
}
/* Caller guarantees disjoint screen pixels for all history calls in this frame.
 * Cold/uninitialized holes retain the generic initialization path. */
int nos_history_apply_disjoint(gfx_fb *fb,int x0,int y0,int x1,int y1,int enabled)
{
    if(!s_sharp) return 0;
    if(!fb || !fb->px || fb->rec || fb->h<=0) return 1;
    int stride=fb->stride?fb->stride:GFX_W;
    if(stride<=0) return 1;
    int c0=fb->cx0,c1=fb->cx1;if(c1<=c0) {c0=0;c1=GFX_W;}
    if(x0<0) x0=0;
    if(x0<c0) x0=c0;
    if(x0<fb->x_base) x0=fb->x_base;
    if(x1>GFX_W) x1=GFX_W;
    if(x1>c1) x1=c1;
    int64_t sx1=(int64_t)fb->x_base+stride;if(x1>sx1) x1=(int)sx1;
    if(y0<0) y0=0;
    if(y0<fb->y_base) y0=fb->y_base;
    if(y1>GFX_H) y1=GFX_H;
    int64_t sy1=(int64_t)fb->y_base+fb->h;if(y1>sy1) y1=(int)sy1;
    if(x1<=x0 || y1<=y0) return 1;
    for(int y=y0;y<y1;y++) {
        if(x0<s_valid_lo[y] || x1>s_valid_hi[y]) {
            nos_history_apply(fb,x0,y,x1,y+1,enabled);continue;
        }
        uint16_t *row=fb->px+(size_t)(y-fb->y_base)*stride+(x0-fb->x_base);
        uint16_t *old=s_sharp+(size_t)y*GFX_W+x0;
        if(!enabled && s_virtual_enabled && nos_history_uniform) {
            virtual_native(row,old,x0,x1,y);
            continue;
        }
        virtual_materialize(y,x0,x1);
        if(!enabled && nos_history_native_copy) {
            int first=x0/32,last=(x1-1)/32;
            uint32_t touched=((1u<<(2*(last-first+1)))-1u)<<(2*first);
            s_uniform[y]&=~touched;
            known_run(row,old,x0,x1-x0,0);continue;
        }
        if(!nos_history_uniform) {
            /* This legacy path clears the whole row, including untouched tags. */
            virtual_materialize(y,0,GFX_W);
            s_uniform[y]=0;known_run(row,old,x0,x1-x0,enabled);continue;
        }
        uint32_t states=s_uniform[y];int run=x0;
        int first=x0/32,last=(x1-1)/32;
        unsigned mask=((1u<<(2*(last-first+1)))-1u)<<(2*first);
        unsigned black=0x55555555u&mask,white=0xaaaaaaaau&mask;
        unsigned prior_span=(states&mask)==black?1u:(states&mask)==white?2u:0u;
        if(prior_span && row[0]==(prior_span==1?0:0xffffu) &&
           uniform_current(row,x1-x0)==prior_span) {
            int n=x1-x0,prefix=(x0&1)?32-(x0&31):0;if(prefix>n)prefix=n;
            s_stats.pixels+=(unsigned)n;s_stats.unchanged+=(unsigned)n;
            s_stats.equal_words+=(unsigned)(prefix/2+(n-prefix)/2);
            s_stats.uniform_pixels+=(unsigned)n;
            if(enabled)s_stats.uniform_read_bytes+=2u*(unsigned)n;
            else s_stats.uniform_write_bytes+=2u*(unsigned)n;
            continue;
        }
        unsigned skipped=0;
        for(int x=x0;x<x1;) {
            int block=x/32,end=(block+1)*32;
            if(end>GFX_W)end=GFX_W;
            int n=end-x;if(end>x1) {end=x1;n=end-x;}
            unsigned shift=(unsigned)(block*2),prior=(states>>shift)&3u,current=0;
            int complete=x==block*32 && (end==(block+1)*32 || end==GFX_W);
            if(complete)current=uniform_current(row+(x-x0),n);
            states=(states&~(3u<<shift))|(current<<shift);
            if(complete && current && prior==current) {
                if(x!=run)known_run(row+(run-x0),old+(run-x0),run,x-run,enabled);
                skipped+=(unsigned)n;run=end;
            }
            x=end;
        }
        known_run(row+(run-x0),old+(run-x0),run,x1-run,enabled);
        s_uniform[y]=states;
        s_stats.pixels+=skipped;s_stats.unchanged+=skipped;
        s_stats.equal_words+=skipped/2;s_stats.uniform_pixels+=skipped;
        if(enabled)s_stats.uniform_read_bytes+=2u*skipped;
        else s_stats.uniform_write_bytes+=2u*skipped;
    }
    return 1;
}
const nos_history_stats *nos_history_get_stats(void) { return &s_stats; }
uint32_t nos_history_storage_bytes(void) { return s_sharp ? HISTORY_STORAGE : 0; }
