/* nos_motion.c — the simulator's motion audit (docs/NOS_SIM.md "Motion audit").
 *
 * Screens and the engine report the on-screen position of every animated thing from
 * tick() with nos_motion_probe(name, class, px) (nos/nos.h). Each frame the sim takes the
 * step since the previous frame, groups consecutive moving frames into moves, and checks
 * every move against the docs/MOTION30.html §14 budget of its class:
 *   - per-frame step budget (OVER), a deliberate single-frame cut is recognised (CUT)
 *   - programmatic curves: peak step <= ~1.6x the average (PEAK), frame 1 >= ~15 % of
 *     the move (FIRST), the last moving frame >= 1 px (SLIVER)
 *   - crisp endings: at most 2 frames moving under 1.5 px (CREEP)
 *   - warnings: length outside the class range (LEN), stalls inside a move (HOLD),
 *     uneven whole-pixel cadence of tracked motion (UNEVEN), component steps > 32 (SOFT)
 * Output: DIR/motion.csv (frame, section, name, class, px, step) and DIR/motion.txt. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nos.h"
#include "nos_sim.h"
#undef nos_motion_probe   /* -NoCostModel builds: nos.h makes it a no-op macro */

static const char *CLS[MC_COUNT] = { "finger", "momentum", "transition", "component", "micro",
                                     "tracked", "discrete", "decorative", "fade", "fill" };
/* per-frame step budget (px, or % for fades); 0 = none */
static const float BUDGET[MC_COUNT] = { 0, 64, 64, 64, 16, 16, 1, 16, 0, 64 };
/* whole-frame length range (0 = any) */
static const int LEN_LO[MC_COUNT] = { 0, 0, 7, 7, 3, 0, 4, 0, 3, 12 };
static const int LEN_HI[MC_COUNT] = { 0, 0, 12, 9, 6, 0, 10, 0, 9, 45 };
#define STILL_EPS 0.05f        /* smaller steps count as still */
#define CREEP_PX 1.5f
#define CREEP_FADE 3.0f        /* % : half a 16-level ink step */
#define PEAK_WARN 1.6f
#define PEAK_FAIL 1.8f         /* whole-pixel rounding of short moves adds ~0.1 */
#define FIRST_FAIL 0.12f       /* ~15 %; a 12-frame EASE_STEADY move shows 13 % */
#define FIRST_FAIL_FILL 0.03f  /* a 30-frame sweep's first frame is ~4 % of it */
#define CUT_REST 20.0f         /* a cut: one frame over budget, every other <= 20 px */

typedef struct {
    char name[48];
    /* this frame */
    int rep, nrep, cls;
    float first, cur;
    /* history */
    float last;
    uint32_t last_frame;
    /* open move */
    int open, n, cap, still;
    uint32_t f0;
    float origin;
    float *st;                 /* steps (0 = a hold inside the move) */
    unsigned char *sc;         /* class per step */
    char section[32];
} probe;

typedef struct {
    char name[48], section[32];
    uint32_t f0;
    int frames, cls, finger, creep, holds, uneven, cut, bad;
    float travel, path, peak, avg, ratio, first, last;
    uint32_t peak_f;
    char flags[200];
} move;

static int s_on, s_inited;
static char s_dir[260] = ".";
static FILE *s_csv;
static probe *s_pr; static int s_np, s_cap;
static move *s_mv; static int s_nm, s_mcap;
static const char *s_section = "-";
static uint32_t s_frame;

void sim_motion_dir(const char *dir) { snprintf(s_dir, sizeof s_dir, "%s", dir); }
int  sim_motion_enabled(void) { return s_on; }

static void open_csv(void)
{
    if (s_csv) return;
    char p[300];
    snprintf(p, sizeof p, "%s/motion.csv", s_dir);
    s_csv = fopen(p, "w");
    if (s_csv) fprintf(s_csv, "frame,section,name,class,px,step\n");
}

void sim_motion_on(int on)
{
    s_on = on;
    s_inited = 1;
    if (on) open_csv();
}

static probe *find(const char *name)
{
    for (int i = 0; i < s_np; i++) if (!strcmp(s_pr[i].name, name)) return &s_pr[i];
    if (s_np == s_cap) {
        s_cap = s_cap ? s_cap * 2 : 64;
        s_pr = realloc(s_pr, sizeof *s_pr * s_cap);
    }
    probe *p = &s_pr[s_np++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    return p;
}

void nos_motion_probe(const char *name, int cls, float px)
{
    if (!s_on || !name) return;
    if (cls < 0 || cls >= MC_COUNT) cls = MC_COMPONENT;
    probe *p = find(name);
    if (!p->rep) { p->rep = 1; p->nrep = 0; p->first = px; }
    p->nrep++;
    p->cur = px;
    p->cls = cls;
}

static void push_step(probe *p, float step, int cls)
{
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 32;
        p->st = realloc(p->st, sizeof *p->st * p->cap);
        p->sc = realloc(p->sc, p->cap);
    }
    p->st[p->n] = step;
    p->sc[p->n] = (unsigned char)cls;
    p->n++;
}

static void addf(move *m, const char *fmt, ...)
{
    size_t l = strlen(m->flags);
    if (l >= sizeof m->flags - 2) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->flags + l, sizeof m->flags - l, fmt, ap);
    va_end(ap);
}

/* Judge a finished move. */
static void close_move(probe *p)
{
    if (!p->open) return;
    p->open = 0;
    int n = p->n;
    p->n = 0;
    if (n <= 0) return;
    if (s_nm == s_mcap) { s_mcap = s_mcap ? s_mcap * 2 : 128; s_mv = realloc(s_mv, sizeof *s_mv * s_mcap); }
    move *m = &s_mv[s_nm++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", p->name);
    snprintf(m->section, sizeof m->section, "%s", p->section);
    m->f0 = p->f0;
    m->frames = n;
    /* the judged part: what follows the last finger frame (a release hands over) */
    int a = 0;
    for (int i = 0; i < n; i++) if (p->sc[i] == MC_FINGER) { a = i + 1; m->finger = 1; }
    m->cls = a < n ? p->sc[a] : MC_FINGER;
    float pos = p->origin, sum = 0;
    for (int i = 0; i < n; i++) pos += p->st[i];
    m->travel = fabsf(pos - p->origin);
    /* steps of the judged part */
    int jn = n - a, moving = 0, over = 0, others_small = 1;
    float peak = 0, firsts = 0, lasts = 0, prev = -1;
    for (int i = a; i < n; i++) {
        float s = fabsf(p->st[i]);
        int c = p->sc[i];
        sum += s;
        if (s < STILL_EPS) { m->holds++; continue; }
        if (!moving) firsts = s;
        moving++;
        lasts = s;
        if (s > peak) { peak = s; m->peak_f = p->f0 + (uint32_t)i; }
        float creep = c == MC_FADE ? CREEP_FADE : CREEP_PX;
        if (s < creep) m->creep++;
        if (c == MC_TRACKED && prev >= 0 && fabsf(s - prev) >= 1.0f) m->uneven++;
        prev = s;
    }
    m->path = sum;
    m->peak = peak;
    m->avg = moving ? sum / moving : 0;
    m->ratio = m->avg > 0 ? peak / m->avg : 0;
    m->first = sum > 0 ? firsts / sum : 0;
    m->last = lasts;
    if (jn <= 0 || !moving) return;            /* all finger: nothing to judge */
    int cls = m->cls;
    /* per-frame budgets; momentum may keep the release's speed */
    float mom_cap = 0;
    for (int i = a; i < n; i++) {
        int c = p->sc[i];
        float s = fabsf(p->st[i]), b = BUDGET[c];
        if (c == MC_MOMENTUM) { if (mom_cap == 0) mom_cap = s > b ? s + 0.5f : b; b = mom_cap; }
        if (b > 0 && s > b) over++;
        else if (s > CUT_REST) others_small = 0;
    }
    int prog = cls == MC_TRANSITION || cls == MC_COMPONENT || cls == MC_MICRO || cls == MC_FILL;
    if (prog && cls != MC_FILL && over == 1 && others_small) { m->cut = 1; addf(m, "CUT "); over = 0; }
    if (over) {
        m->bad = 1;
        addf(m, "OVER(");
        int shown = 0;
        for (int i = a; i < n && shown < 4; i++) {
            int c = p->sc[i];
            float s = fabsf(p->st[i]), b = c == MC_MOMENTUM ? mom_cap : BUDGET[c];
            if (b > 0 && s > b) { addf(m, "%sf%u %.0f>%.0f", shown ? " " : "", (unsigned)(p->f0 + i), s, b); shown++; }
        }
        addf(m, over > shown ? " +%d) " : ") ", over - shown);
    }
    if (cls == MC_COMPONENT && peak > 32 && !m->cut && !over) addf(m, "SOFT(%.0f>32) ", peak);
    if (prog && !m->cut && moving >= 3) {
        if (m->ratio > PEAK_FAIL) { m->bad = 1; addf(m, "PEAK(x%.2f) ", m->ratio); }
        else if (m->ratio > PEAK_WARN) addf(m, "peak(x%.2f) ", m->ratio);
        if (m->first < (cls == MC_FILL ? FIRST_FAIL_FILL : FIRST_FAIL)) { m->bad = 1; addf(m, "FIRST(%.0f%%) ", m->first * 100); }
        if (lasts < 1.0f && m->path >= 20 && cls != MC_FADE) { m->bad = 1; addf(m, "SLIVER(%.2f) ", lasts); }
    }
    int judged_creep = (prog && !m->cut) || cls == MC_MOMENTUM || cls == MC_FADE;   /* a cut's 4-2-1 settle is the click */
    if (judged_creep && m->creep > 2 && !(m->path < 8 && cls != MC_FADE)) { m->bad = 1; addf(m, "CREEP(%d) ", m->creep); }
    if (m->holds > 0 && cls != MC_DECORATIVE && cls != MC_DISCRETE) addf(m, "HOLD(%d) ", m->holds);
    if (m->uneven > 2) addf(m, "UNEVEN(%d) ", m->uneven);
    int len = moving + m->holds;
    if (LEN_LO[cls] && !m->cut && (len < LEN_LO[cls] || len > LEN_HI[cls])) addf(m, "LEN(%d) ", len);
}

void sim_motion_frame(uint32_t frame, const char *section)
{
    s_frame = frame;
    s_section = section ? section : "-";
    if (!s_on) {
        for (int i = 0; i < s_np; i++) s_pr[i].rep = 0;
        return;
    }
    for (int i = 0; i < s_np; i++) {
        probe *p = &s_pr[i];
        if (!p->rep) {
            if (p->open) close_move(p);     /* it left the glass */
            continue;
        }
        /* two reports in one frame: the first is the origin of a new move */
        int fresh = p->nrep > 1;
        if (fresh && p->open) close_move(p);
        float prev = !fresh && p->last_frame && p->last_frame + 1 == frame ? p->last : p->first;
        float step = p->cur - prev;
        if (s_csv) fprintf(s_csv, "%u,%s,%s,%s,%.2f,%.2f\n", (unsigned)frame, s_section, p->name, CLS[p->cls], p->cur, step);
        if (fabsf(step) >= STILL_EPS) {
            if (!p->open) {
                p->open = 1; p->n = 0; p->still = 0; p->f0 = frame; p->origin = prev;
                snprintf(p->section, sizeof p->section, "%s", s_section);
            }
            for (; p->still > 0; p->still--) push_step(p, 0, p->cls);   /* a hold inside the move */
            push_step(p, step, p->cls);
        } else if (p->open && ++p->still >= 2) close_move(p);
        p->last = p->cur;
        p->last_frame = frame;
        p->rep = 0;
    }
}

static void write_move(FILE *o, const move *m)
{
    fprintf(o, "f%-5u %-24s %-10s %3df %7.1f px  peak %6.1f (f%-5u) avg %5.1f  x%-4.2f first %3.0f%%  last %5.1f  creep %d  %s%s\n",
            (unsigned)m->f0, m->name, CLS[m->cls], m->frames, m->travel, m->peak, (unsigned)m->peak_f, m->avg,
            m->ratio, m->first * 100, m->last, m->creep, m->bad ? "FAIL " : "ok   ", m->flags);
}

/* 1 if any move that began in `section` failed (expect motionok). */
int sim_motion_bad(const char *section, char *why, int n)
{
    int bad = 0;
    if (why && n) why[0] = 0;
    for (int i = 0; i < s_nm; i++)
        if (s_mv[i].bad && (!section || !strcmp(s_mv[i].section, section))) {
            if (!bad && why) snprintf(why, (size_t)n, "%s f%u %s", s_mv[i].name, (unsigned)s_mv[i].f0, s_mv[i].flags);
            bad++;
        }
    return bad;
}

void sim_motion_finish(FILE *summary)
{
    if (!s_inited) return;
    for (int i = 0; i < s_np; i++) close_move(&s_pr[i]);
    if (s_csv) { fclose(s_csv); s_csv = 0; }
    char path[300];
    snprintf(path, sizeof path, "%s/motion.txt", s_dir);
    FILE *o = fopen(path, "w");
    int bad = 0, judged = 0;
    for (int i = 0; i < s_nm; i++) { bad += s_mv[i].bad; judged += s_mv[i].cls != MC_FINGER; }
    if (o) {
        fprintf(o, "== motion audit: docs/MOTION30.html section 14 budgets ==\n");
        fprintf(o, "%d moves (%d judged), %d FAIL. Budgets px/frame: momentum 64 (or the release speed), transition 64,\n"
                   "component 64 (prefer 32), micro 16, tracked 16, decorative 16, discrete 0; curves peak <= 1.6x avg\n"
                   "(FAIL > %.1f), frame 1 >= 15%% (FAIL < %.0f%%), last moving frame >= 1 px, <= 2 creep frames (< 1.5 px).\n\n",
                s_nm, judged, bad, PEAK_FAIL, FIRST_FAIL * 100);
        if (bad) {
            fprintf(o, "FAILURES\n");
            for (int i = 0; i < s_nm; i++) if (s_mv[i].bad) write_move(o, &s_mv[i]);
            fprintf(o, "\n");
        }
        char last_sec[32] = "";
        for (int i = 0; i < s_nm; i++) {
            if (strcmp(last_sec, s_mv[i].section)) {
                snprintf(last_sec, sizeof last_sec, "%s", s_mv[i].section);
                fprintf(o, "-- %s\n", last_sec);
            }
            write_move(o, &s_mv[i]);
        }
        fclose(o);
    }
    char line[200];
    snprintf(line, sizeof line, "motion audit: %d moves (%d judged), %d FAIL -> %s\n", s_nm, judged, bad, path);
    printf("%s", line);
    if (summary) fputs(line, summary);
}
