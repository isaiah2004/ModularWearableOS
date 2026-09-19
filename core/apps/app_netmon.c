/* app_netmon.c — Wi-Fi Channel Congestion monitor (passive / white-hat).
 *
 * STRICTLY PASSIVE: the only radio call is hal_wifi_scan(), a read-only scan.
 * No injection, no deauth, no monitor-mode tricks — we merely count the
 * networks we can already see and visualize how crowded each 2.4GHz channel
 * is, so the user can pick a clean one (think WiFiAnalyzer's channel graph).
 * This is a COMPLEMENTARY view to app_wifi.c (which lists networks); here we
 * aggregate by channel into a bar chart.
 *
 * PLAIN-ENGLISH ALGORITHM
 * -----------------------
 *  1. SCAN: hal_wifi_scan() fills a static array with visible access points
 *     (ssid, rssi, chan). Called once on_start and again on every TAP.
 *  2. HISTOGRAM: walk the AP list; for each AP whose channel is a valid
 *     2.4GHz channel (1..13) increment counts[chan]. APs on 5GHz channels
 *     (e.g. 36/40/...) are simply ignored — out of range for this view.
 *  3. BEST / WORST PICK: scan counts[1..13]. The "best" (least congested)
 *     channel is the one with the smallest AP count; ties resolve to the
 *     lowest channel number. The "worst" is the largest count. These drive
 *     the green/orange highlighting and the "Best: ch N" hint.
 *  4. BAR LAYOUT: 13 equal slots span a centered horizontal band. Each bar
 *     sits on a common baseline and grows UP; its height is scaled to the
 *     busiest channel (tallest bar == max count). Bars are tallest through
 *     the middle of the round screen, where there's the most vertical room,
 *     so the whole chart stays inside the circle. Channel numbers label the
 *     bottom; the live count sits just above each bar.
 */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>

#define MAXAP    24   /* how many APs we keep from a scan */
#define NCHAN    13   /* 2.4GHz channels 1..13 */

/* ---- pure logic (unit-tested under SELFTEST) --------------------------- */

/* Build the per-channel histogram from an AP list.
 * counts[0] is unused; counts[1..13] hold AP counts per channel.
 * Writes *best (least congested channel, lowest # on ties) and
 * *worst (most congested channel, lowest # on ties). Returns the number of
 * APs that fell on a valid 2.4GHz channel (i.e. the total that was charted). */
static int netmon_histogram(const hal_wifi_ap *ap, int n,
                            int counts[NCHAN + 1], int *best, int *worst)
{
    int charted = 0;
    for (int c = 0; c <= NCHAN; c++) counts[c] = 0;

    for (int i = 0; i < n; i++) {
        int ch = ap[i].chan;
        if (ch >= 1 && ch <= NCHAN) { counts[ch]++; charted++; }
    }

    int bch = 1, wch = 1;
    for (int c = 2; c <= NCHAN; c++) {
        if (counts[c] < counts[bch]) bch = c;   /* strict < keeps lowest # */
        if (counts[c] > counts[wch]) wch = c;
    }
    if (best)  *best  = bch;
    if (worst) *worst = wch;
    return charted;
}

#ifndef SELFTEST

/* ---- app state --------------------------------------------------------- */

static hal_wifi_ap s_ap[MAXAP];
static int s_counts[NCHAN + 1];
static int s_total;    /* APs charted (valid 2.4GHz channels) */
static int s_best, s_worst;

static void rescan(void)
{
    int n = hal_wifi_scan(s_ap, MAXAP);
    if (n < 0) n = 0;
    if (n > MAXAP) n = MAXAP;
    s_total = netmon_histogram(s_ap, n, s_counts, &s_best, &s_worst);
}

static void on_start(void)
{
    rescan();
}

/* ---- drawing ----------------------------------------------------------- */

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    /* header */
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(34),
                       "CHANNELS", GFX_GREY);
    char hdr[24];
    snprintf(hdr, sizeof hdr, "%d NETWORKS", s_total);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(56),
                       hdr, GFX_DGREY);

    /* chart geometry (authored on the 240 grid, scaled to the panel) */
    const float span     = GFX_S(200);
    const float left     = GFX_CX - span * 0.5f;
    const float slotw    = span / NCHAN;
    const float barw     = slotw * 0.64f;
    const float baseline = GFX_S(170);
    const float maxbarh  = GFX_S(80);
    const float labely   = baseline + GFX_S(16);

    int maxc = s_counts[s_worst];   /* tallest bar scales to this */

    /* baseline rule */
    gfx_line(fb, left, baseline, left + span, baseline, GFX_S(1), GFX_DGREY);

    for (int ch = 1; ch <= NCHAN; ch++) {
        int cnt = s_counts[ch];
        float cx = left + (ch - 0.5f) * slotw;

        float h = 0.0f;
        if (maxc > 0) h = (float)cnt / (float)maxc * maxbarh;
        if (cnt > 0 && h < GFX_S(3)) h = GFX_S(3);   /* keep tiny bars visible */
        float top = baseline - h;

        /* colour: green = least congested, orange = most congested (only
         * when it actually stands out), grey/accent otherwise. */
        uint16_t col = GFX_ACCENT;
        if (cnt == s_counts[s_best]) col = GFX_GREEN;
        if (maxc > s_counts[s_best] && cnt == maxc) col = GFX_ORANGE;
        if (cnt == 0) col = GFX_GREEN;   /* empty channels are ideal picks */

        if (cnt == 0) {
            /* a faint stub so every channel reads as a slot on the axis */
            gfx_fill_rect(fb, (int)(cx - barw * 0.5f), (int)(baseline - GFX_S(2)),
                          (int)barw, (int)GFX_S(2), GFX_DGREY);
        } else {
            gfx_fill_rect(fb, (int)(cx - barw * 0.5f), (int)top,
                          (int)barw, (int)h, col);
            /* live count just above the bar */
            char cs[8];
            snprintf(cs, sizeof cs, "%d", cnt);
            font_draw_centered(fb, &font_small, (int)cx,
                               (int)(top - GFX_S(3)), cs, GFX_GREY);
        }

        /* channel number label along the bottom */
        char ls[4];
        snprintf(ls, sizeof ls, "%d", ch);
        uint16_t lcol = (ch == s_best) ? GFX_GREEN : GFX_GREY;
        font_draw_centered(fb, &font_small, (int)cx, (int)labely, ls, lcol);
    }

    /* best-channel hint + re-scan affordance */
    char bs[24];
    if (s_total > 0)
        snprintf(bs, sizeof bs, "Best: ch %d", s_best);
    else
        snprintf(bs, sizeof bs, "no networks");
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(202),
                       bs, GFX_GREEN);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(220),
                       "tap to re-scan", GFX_DGREY);
}

/* ---- events ------------------------------------------------------------ */

static int on_event(const wos_event *ev)
{
    if (ev->type == WOS_EV_TAP) { rescan(); return 1; }
    return 0;
}

const wos_app app_netmon = {
    "Channels", on_start, on_event, on_draw, GFX_GREEN, ICON_SIGNAL
};

#else  /* SELFTEST -------------------------------------------------------- */

#include <assert.h>

static hal_wifi_ap mk(int ch)
{
    hal_wifi_ap a;
    a.ssid[0] = 0;
    a.rssi = -60;
    a.chan = ch;
    return a;
}

int main(void)
{
    /* Known AP list. Valid 2.4GHz: ch1 x2, ch3 x1, ch6 x3, ch11 x1, ch13 x1.
     * The chan=36 entry (5GHz) must be ignored. */
    hal_wifi_ap ap[] = {
        mk(1), mk(1), mk(6), mk(11), mk(6), mk(6), mk(3), mk(13), mk(36)
    };
    int n = (int)(sizeof ap / sizeof ap[0]);

    int counts[NCHAN + 1], best, worst;
    int charted = netmon_histogram(ap, n, counts, &best, &worst);

    /* per-channel counts */
    assert(counts[1]  == 2);
    assert(counts[2]  == 0);
    assert(counts[3]  == 1);
    assert(counts[6]  == 3);
    assert(counts[11] == 1);
    assert(counts[13] == 1);

    /* 5GHz channel ignored -> 8 charted, not 9 */
    assert(charted == 8);

    /* least congested: first channel with count 0 is ch2 */
    assert(best == 2);
    /* most congested: ch6 (count 3) */
    assert(worst == 6);

    /* empty-input edge case: everything zero -> best=1, worst=1, charted=0 */
    int c2[NCHAN + 1], b2, w2;
    int ch2 = netmon_histogram(ap, 0, c2, &b2, &w2);
    assert(ch2 == 0 && b2 == 1 && w2 == 1);

    printf("SELFTEST OK: counts ch1=%d ch6=%d, best=ch%d worst=ch%d, charted=%d\n",
           counts[1], counts[6], best, worst, charted);
    return 0;
}

#endif
