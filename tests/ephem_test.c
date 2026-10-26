/* ephem_test.c - core/ephem.c against published almanac dates and a JPL Horizons vector.
 * gcc -O2 -std=c99 -Icore tests/ephem_test.c core/ephem.c -lm -o ephem_test */
#include "ephem.h"
#include <math.h>
#include <stdio.h>

static int fails;
static double jd_of(int y, int m, int d)   /* 0h UTC, proleptic Gregorian (Fliegel & Van Flandern) */
{
    long a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
    long jdn = d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
    return (double)jdn - 0.5;
}
static void near(const char *what, double got, double want, double tol)
{
    int ok = fabs(got - want) <= tol;
    printf("%s %-46s got %.4f want %.4f (+-%.4f)\n", ok ? "ok  " : "FAIL", what, got, want, tol);
    if (!ok) fails++;
}
/* the first event of `kind` for planet a (b = -1 or the pair partner) in the scan */
static double find(const ep_event *ev, int n, int kind, int a, int b)
{
    for (int i = 0; i < n; i++)
        if (ev[i].kind == kind && ev[i].a == a && (b < 0 || ev[i].b == b)) return ev[i].jd;
    return 0;
}

int main(void)
{
    /* Horizons: MRO (300 km over Mars) heliocentric ecliptic J2000 at 2026-10-14 0h TDB */
    ep_vec m = ep_helio(EP_MARS, 2461327.5);
    double d = sqrt(pow(m.x + 0.04762680908, 2) + pow(m.y - 1.572891438, 2) + pow(m.z - 0.03411595595, 2));
    near("Mars vs Horizons (au; Table 1 says 2e-4)", d, 0, 3e-4);

    ep_event ev[160];
    int n = ep_events(jd_of(2026, 9, 1), 200, 1, ev, 160);
    printf("     %d events in Sep 2026 - Mar 2027\n", n);
    near("Neptune opposition 2026-09-26", find(ev, n, EV_OPPOSITION, EP_NEPTUNE, -1), jd_of(2026, 9, 26), 1.0);
    near("Saturn opposition 2026-10-04", find(ev, n, EV_OPPOSITION, EP_SATURN, -1), jd_of(2026, 10, 4), 1.0);
    near("Venus inferior conjunction 2026-10-24", find(ev, n, EV_SOLAR, EP_VENUS, -1), jd_of(2026, 10, 24), 1.0);
    near("Uranus opposition 2026-11-25", find(ev, n, EV_OPPOSITION, EP_URANUS, -1), jd_of(2026, 11, 25), 1.0);
    near("Mars meets Jupiter 2026-11-16 (JS scan)", find(ev, n, EV_CONJUNCTION, EP_MARS, EP_JUPITER), jd_of(2026, 11, 16), 1.0);
    near("Jupiter opposition 2027-02-11", find(ev, n, EV_OPPOSITION, EP_JUPITER, -1), jd_of(2027, 2, 11), 1.0);
    near("Mars opposition 2027-02-19", find(ev, n, EV_OPPOSITION, EP_MARS, -1), jd_of(2027, 2, 19), 1.0);

    /* the chip filter (rank >= 2) never shows a lost-in-the-glare conjunction */
    int bad = 0, n2 = ep_events(jd_of(2027, 1, 1), 365, 2, ev, 160);
    for (int i = 0; i < n2; i++) if (ev[i].rank < 2) bad++;
    near("rank filter leaks", bad, 0, 0);
    char t[32]; ep_event e = { 0 }; e.kind = EV_CONJUNCTION; e.a = EP_MARS; e.b = EP_JUPITER;
    ep_event_title(&e, t, sizeof t);
    printf("%s title \"%s\"\n", t[0] == 'M' ? "ok  " : "FAIL", t);

    /* beyond the planets, against what the prototype (ephem.js) printed */
    double lon, lat, km; ep_moon(2461328.0, &lon, &lat, &km);
    near("Moon distance in range (km)", km > 356000 && km < 407000, 1, 0);
    int wax; double lit = ep_moon_lit(2461328.0, &wax);       /* 2026-10-14 12h: a thin waxing crescent */
    near("Moon lit fraction 14 Oct 2026 (JS: 0.15)", lit, 0.15, 0.05);
    near("Moon waxing", wax, 1, 0);
    ep_sat io = { 421800, 0.004, 0, 0, 0, 0, 1.769, 2451545.0 };
    ep_vec p = ep_sat_pos(&io, 2451545.0 + 1.769 / 4);
    near("Io a quarter orbit on (km from Jupiter)", sqrt(p.x * p.x + p.y * p.y + p.z * p.z), 421800, 3000);
    ep_conic halley = { 0.96714, 0.586, 162.26, 58.42, 111.33, 2446467.4 };   /* 1986 perihelion */
    ep_vec h = ep_conic_pos(&halley, 2446467.4);
    near("Halley at perihelion (au)", sqrt(h.x * h.x + h.y * h.y + h.z * h.z), 0.586, 0.001);
    ep_conic para = { 1.0000, 0.5, 30, 10, 20, 2461000.5 };
    ep_vec pp = ep_conic_pos(&para, 2461000.5 + 100);
    near("parabola moves outward (au > q)", sqrt(pp.x * pp.x + pp.y * pp.y + pp.z * pp.z) > 0.5, 1, 0);
    ep_conic hyp = { 6.1414, 1.356, 175.12, 322.17, 128.02, 2460978.00 };   /* 3I/ATLAS */
    ep_vec hp = ep_conic_pos(&hyp, 2461327.5);
    near("3I/ATLAS a year out (JS: 12.25 au)", sqrt(hp.x * hp.x + hp.y * hp.y + hp.z * hp.z), 12.25, 0.4);
    float ol, oo, sl, so; ep_vec none = { 0, 0, 0 };
    ep_geometry(EP_SATURN, 2461328.0, none, &ol, &oo, &sl, &so);
    near("Saturn ring opening (JS: -7.1 deg)", ol, -7.1, 0.3);
    ep_geometry(EP_VENUS, 2461328.0, none, &ol, &oo, &sl, &so);
    near("Venus sub-observer lat (JS: 8.75)", ol, 8.75, 0.3);

    printf(fails ? "FAIL ephem: %d\n" : "PASS ephem: Horizons + 7 almanac dates + moons, conics, globe axes\n", fails);
    return fails != 0;
}
