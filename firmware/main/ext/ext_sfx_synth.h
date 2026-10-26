/* ext_sfx_synth.h — procedural UI sound effects, shared verbatim by the device
 * mixer (firmware/main/ext/ext_audio.c) and the simulator (sim/nos_ext_sim.c),
 * so what you hear on the PC is what the watch plays. Portable C99, header-only.
 *
 * Sound design brief: Nothing-style. Short, precise, mechanical/digital. Thin
 * pulse waves (12.5/25 % duty) instead of fat squares, a sprinkle of
 * sample-and-hold ("bit-crushed") noise for a transient edge, exponential
 * decays of a few ms, and tiny downward pitch drops so blips feel physical
 * rather than beepy. No vibrato, no reverb tails, no major-chord jingles.
 * Every effect is under 0.6 s; most are under 60 ms.
 *
 * A recipe is a list of segments. Each segment is one oscillator:
 *   t0, dur   start offset and length (ms, from the effect's start)
 *   f0 -> f1  pitch glide in Hz (noise: f = sample-and-hold rate, 0 = white)
 *   wave      SFXW_SQ50 / SQ25 / SQ12 (pulse duty), SFXW_TRI, SFXW_NOISE
 *   vol       0-255
 *   atk       linear attack, ms
 *   tau       exponential decay time constant, ms (0 = hold level)
 *   steps     quantise the glide into N stairs (0 = smooth): "digital" sweeps
 * Every segment also gets a 1.5 ms linear release so nothing clicks.
 */
#ifndef NOS_EXT_SFX_SYNTH_H
#define NOS_EXT_SFX_SYNTH_H
#include <stdint.h>
#include <math.h>

#define SFX_FS        16000
#define SFX_MAX_SEG   10

enum { SFXW_SQ50 = 0, SFXW_SQ25, SFXW_SQ12, SFXW_TRI, SFXW_NOISE };

typedef struct {
    uint16_t t0, dur, f0, f1;
    uint8_t  wave, vol, atk, tau, steps;
} sfx_seg;
typedef struct { const sfx_seg *seg; uint8_t n; uint16_t loop_ms; } sfx_recipe;

/* SFX_TAP — "tk". One 9 ms 25 % pulse dropping 2.4 -> 1.7 kHz with a 3 ms
 * decay, plus a 3 ms white-noise edge for the contact transient. */
static const sfx_seg SFXR_TAP[] = {
    {  0,  9, 2400, 1700, SFXW_SQ25,  150, 0, 3, 0 },
    {  0,  3,    0,    0, SFXW_NOISE,  70, 0, 1, 0 },
};
/* SFX_TICK — list detent. Quieter and shorter than TAP: a 5 ms 12.5 % pulse at
 * 3.2 kHz with a 2 ms crushed-noise grain (S&H at 8 kHz). Repeats cleanly at
 * scroll rates up to ~30/s without smearing. */
static const sfx_seg SFXR_TICK[] = {
    {  0,  5, 3200, 2800, SFXW_SQ12,   90, 0, 2, 0 },
    {  0,  2, 8000, 8000, SFXW_NOISE,  50, 0, 1, 0 },
};
/* SFX_BACK — two descending blips, a minor third apart (1760 -> 1320 Hz),
 * each with a slight pitch sag. Reads as "out / down". */
static const sfx_seg SFXR_BACK[] = {
    {  0, 14, 1760, 1600, SFXW_SQ25, 140, 0, 6, 0 },
    { 38, 18, 1320, 1180, SFXW_SQ25, 130, 0, 7, 0 },
};
/* SFX_OPEN — mirror of BACK: two rising blips (1320 -> 1980 Hz, a fifth) and a
 * faint airy noise tail under the second, like a panel sliding open. */
static const sfx_seg SFXR_OPEN[] = {
    {  0, 12, 1320, 1400, SFXW_SQ25,  130, 0, 6,  0 },
    { 34, 22, 1980, 2000, SFXW_SQ25,  140, 0, 9,  0 },
    { 34, 30, 6000, 6000, SFXW_NOISE,  25, 1, 10, 0 },
};
/* SFX_TOGGLE_ON — rising fifth: thin 12.5 % pre-blip at 1 kHz, then a firmer
 * 25 % note at 1.5 kHz that lands with a 2 ms noise "latch" click. */
static const sfx_seg SFXR_TOG_ON[] = {
    {  0, 10, 1000, 1000, SFXW_SQ12,  120, 0, 5, 0 },
    { 30, 24, 1500, 1500, SFXW_SQ25,  150, 0, 8, 0 },
    { 30,  2,    0,    0, SFXW_NOISE,  60, 0, 1, 0 },
};
/* SFX_TOGGLE_OFF — falling fifth, softer, the second note sags 6 % like a
 * switch relaxing. No latch click: "off" should feel lighter than "on". */
static const sfx_seg SFXR_TOG_OFF[] = {
    {  0, 10, 1500, 1500, SFXW_SQ12, 110, 0, 5, 0 },
    { 30, 26, 1000,  940, SFXW_SQ25, 120, 0, 9, 0 },
};
/* SFX_NOTIFY — glyph motif: two quick dots (G6, C7) then a ringing triangle E7
 * with a 45 ms decay and a noise tick on its onset. ~260 ms total. */
static const sfx_seg SFXR_NOTIFY[] = {
    {   0,  22, 1568, 1568, SFXW_SQ25, 120, 1, 10, 0 },
    {  60,  22, 2093, 2093, SFXW_SQ25, 120, 1, 10, 0 },
    { 120, 140, 2637, 2600, SFXW_TRI,  210, 2, 45, 0 },
    { 120,   3,    0,    0, SFXW_NOISE, 60, 0,  1, 0 },
};
/* SFX_ALARM — loops every 900 ms until hal_sfx_stop(): four flat 2.2 kHz
 * square bursts (60 ms on / 40 ms off) and an octave-down 25 % "tock" after
 * the train. Insistent but dry. */
static const sfx_seg SFXR_ALARM[] = {
    {   0, 60, 2200, 2200, SFXW_SQ50, 130, 1, 0,  0 },
    { 100, 60, 2200, 2200, SFXW_SQ50, 130, 1, 0,  0 },
    { 200, 60, 2200, 2200, SFXW_SQ50, 130, 1, 0,  0 },
    { 300, 60, 2200, 2200, SFXW_SQ50, 130, 1, 0,  0 },
    { 420, 16, 1100,  900, SFXW_SQ25, 150, 0, 6,  0 },
};
/* SFX_TIMER_DONE — descending stepped arpeggio (E7 B6 G6 E6, 28 ms notes on a
 * 45 ms grid), played twice; the second pass ends in a 150 ms triangle ring. */
static const sfx_seg SFXR_TIMER[] = {
    {   0, 28, 2637, 2637, SFXW_SQ25, 130, 0, 12, 0 },
    {  45, 28, 1976, 1976, SFXW_SQ25, 130, 0, 12, 0 },
    {  90, 28, 1568, 1568, SFXW_SQ25, 130, 0, 12, 0 },
    { 135, 28, 1319, 1319, SFXW_SQ25, 130, 0, 12, 0 },
    { 300, 28, 2637, 2637, SFXW_SQ25, 130, 0, 12, 0 },
    { 345, 28, 1976, 1976, SFXW_SQ25, 130, 0, 12, 0 },
    { 390, 28, 1568, 1568, SFXW_SQ25, 130, 0, 12, 0 },
    { 435,150, 1319, 1310, SFXW_TRI,  200, 1, 60, 0 },
};
/* SFX_SUCCESS — triangle fifth up (E6 -> B6) with a tiny 12.5 % sparkle an
 * octave above the second note. Warm, but still short (230 ms). */
static const sfx_seg SFXR_SUCCESS[] = {
    {  0,  40, 1319, 1319, SFXW_TRI,  180, 1, 20, 0 },
    { 50, 180, 1976, 1976, SFXW_TRI,  200, 1, 60, 0 },
    { 50,  12, 3952, 3952, SFXW_SQ12,  50, 0,  4, 0 },
};
/* SFX_ERROR — two low, dropping square buzzes (330 -> 290, 300 -> 250 Hz) each
 * doubled by sample-and-hold noise at ~1 kHz, which gives the "crushed" grit.
 * The tiny speaker barely reproduces 300 Hz; the harmonics and grit carry it. */
static const sfx_seg SFXR_ERROR[] = {
    {   0, 70,  330,  290, SFXW_SQ50,  150, 1, 40, 0 },
    {   0, 70, 1200, 1200, SFXW_NOISE,  45, 1, 30, 0 },
    { 110, 90,  300,  250, SFXW_SQ50,  150, 1, 50, 0 },
    { 110, 90, 1000, 1000, SFXW_NOISE,  45, 1, 40, 0 },
};
/* SFX_CHARGE — power connected: an 8-stair rising triangle staircase
 * 500 -> 1500 Hz over 200 ms (each stair a hard step, like a level meter
 * filling), capped by a 25 % tick at 3 kHz. */
static const sfx_seg SFXR_CHARGE[] = {
    {   0, 200,  500, 1500, SFXW_TRI,  160, 2, 0, 8 },
    { 205,   8, 3000, 2600, SFXW_SQ25, 120, 0, 3, 0 },
};
/* SFX_BOOT — a 60 ms noise swell (40 ms attack), three 25 % stairs C5 G5 C6,
 * then a bright G6 triangle pair detuned by 4 Hz ringing out over ~0.3 s. */
static const sfx_seg SFXR_BOOT[] = {
    {   0,  60,    0,    0, SFXW_NOISE,  50, 40,  15, 0 },
    {  60,  30,  523,  523, SFXW_SQ25,  110,  0,  12, 0 },
    { 100,  30,  784,  784, SFXW_SQ25,  110,  0,  12, 0 },
    { 140,  30, 1047, 1047, SFXW_SQ25,  120,  0,  12, 0 },
    { 180, 320, 1568, 1560, SFXW_TRI,   200,  2, 110, 0 },
    { 180, 320, 1572, 1566, SFXW_TRI,    90,  2, 110, 0 },
};
/* SFX_SHUTTER — mechanical double curtain: white-noise snap + a dropping square
 * click, then 45 ms later a crisper crushed-noise snap and a lower click. */
static const sfx_seg SFXR_SHUTTER[] = {
    {  0, 18,    0,    0, SFXW_NOISE, 200, 0, 4, 0 },
    {  0,  3, 1800, 1200, SFXW_SQ50,  120, 0, 1, 0 },
    { 45, 24, 4000, 4000, SFXW_NOISE, 160, 0, 6, 0 },
    { 45,  3, 1400,  900, SFXW_SQ50,  110, 0, 1, 0 },
};

#define SFXR_(a, loop) { a, (uint8_t)(sizeof a / sizeof a[0]), loop }
/* Indexed by the SFX_* enum of nos/hal_ext.h. */
static const sfx_recipe SFX_RECIPES[] = {
    SFXR_(SFXR_TAP, 0), SFXR_(SFXR_TICK, 0), SFXR_(SFXR_BACK, 0), SFXR_(SFXR_OPEN, 0),
    SFXR_(SFXR_TOG_ON, 0), SFXR_(SFXR_TOG_OFF, 0), SFXR_(SFXR_NOTIFY, 0),
    SFXR_(SFXR_ALARM, 900), SFXR_(SFXR_TIMER, 0), SFXR_(SFXR_SUCCESS, 0),
    SFXR_(SFXR_ERROR, 0), SFXR_(SFXR_CHARGE, 0), SFXR_(SFXR_BOOT, 0), SFXR_(SFXR_SHUTTER, 0),
};
#define SFX_NRECIPES ((int)(sizeof SFX_RECIPES / sizeof SFX_RECIPES[0]))

typedef struct {
    int      id;                   /* -1 = idle */
    uint32_t t;                    /* samples since start (of this loop pass) */
    uint32_t len;                  /* samples until the last segment ends */
    uint32_t lfsr;
    uint32_t phase[SFX_MAX_SEG];
    float    env[SFX_MAX_SEG], k[SFX_MAX_SEG], hold[SFX_MAX_SEG];
} sfx_voice;

static inline void sfx_voice_start(sfx_voice *v, int id)
{
    v->id = -1;
    if (id < 0 || id >= SFX_NRECIPES) return;
    const sfx_recipe *r = &SFX_RECIPES[id];
    uint32_t end = 0;
    for (int i = 0; i < r->n && i < SFX_MAX_SEG; i++) {
        uint32_t e = (uint32_t)(r->seg[i].t0 + r->seg[i].dur);
        if (e > end) end = e;
    }
    v->len = end * SFX_FS / 1000;
    v->t = 0;
    if (!v->lfsr) v->lfsr = 0x1234567u;
    v->id = id;
}

static inline float sfx_noise(sfx_voice *v)
{
    uint32_t x = v->lfsr;            /* xorshift32 */
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    v->lfsr = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* Adds n samples (about -1..+1 per full-volume segment) into acc.
 * Returns 0 once the voice has finished (and marks it idle). */
static inline int sfx_voice_render(sfx_voice *v, float *acc, int n)
{
    if (v->id < 0) return 0;
    const sfx_recipe *r = &SFX_RECIPES[v->id];
    const uint32_t rel = SFX_FS * 3 / 2000;              /* 1.5 ms release */
    for (int i = 0; i < n; i++) {
        if (v->t >= v->len) {
            if (!r->loop_ms) { v->id = -1; return 0; }
            if (v->t >= (uint32_t)r->loop_ms * SFX_FS / 1000) v->t = 0;
            else { v->t++; continue; }                    /* loop gap: silence */
        }
        float s = 0;
        for (int j = 0; j < r->n && j < SFX_MAX_SEG; j++) {
            const sfx_seg *g = &r->seg[j];
            uint32_t s0 = (uint32_t)g->t0 * SFX_FS / 1000, sd = (uint32_t)g->dur * SFX_FS / 1000;
            if (v->t < s0 || v->t >= s0 + sd) continue;
            uint32_t u = v->t - s0, atk = (uint32_t)g->atk * SFX_FS / 1000;
            if (u == 0) {
                v->phase[j] = 0; v->hold[j] = 0;
                v->env[j] = 1.0f;
                v->k[j] = g->tau ? expf(-1000.0f / ((float)g->tau * SFX_FS)) : 1.0f;
            }
            float prog = (float)u / (float)sd;
            if (g->steps) prog = floorf(prog * g->steps) / (float)(g->steps > 1 ? g->steps - 1 : 1);
            if (prog > 1) prog = 1;
            float f = (float)g->f0 + ((float)g->f1 - (float)g->f0) * prog;
            uint32_t inc = (uint32_t)(f * (4294967296.0f / SFX_FS));
            uint32_t ph = v->phase[j], nph = ph + inc;
            v->phase[j] = nph;
            float o;
            switch (g->wave) {
            case SFXW_SQ50: o = ph < 0x80000000u ? 1.0f : -1.0f; break;
            case SFXW_SQ25: o = ph < 0x40000000u ? 1.0f : -0.33f; break;   /* DC-balanced pulse */
            case SFXW_SQ12: o = ph < 0x20000000u ? 1.0f : -0.14f; break;
            case SFXW_TRI: {
                float p = (float)ph * (1.0f / 4294967296.0f);
                o = p < 0.5f ? 4 * p - 1 : 3 - 4 * p;
                break;
            }
            default:  /* noise: white, or sample-and-hold at f */
                if (!g->f0 || nph < ph || u == 0) v->hold[j] = sfx_noise(v);
                o = v->hold[j];
                break;
            }
            float e;
            if (u < atk) e = (float)u / (float)atk;
            else { e = v->env[j]; v->env[j] *= v->k[j]; }
            uint32_t left = sd - u;
            if (left < rel) e *= (float)left / (float)rel;
            s += o * e * (float)g->vol * (1.0f / 255.0f);
        }
        acc[i] += s;
        v->t++;
    }
    return 1;
}
#endif
