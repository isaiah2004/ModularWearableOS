/* ephem.h - where the planets are, for the Orrery face. A port of the JS the prototype
 * (docs/SOLAR_FACE.html) was validated with: JPL "Approximate Positions of the Planets"
 * Table 1 (valid 1800-2050) and a day-step event scan with parabolic refinement.
 *
 * Time is a Julian date in double (planet longitudes grow 149 000 deg a century; float
 * would lose the arcminutes). Positions are heliocentric ecliptic J2000, au.
 * Cost: one planet is a Kepler solve (~5 iterations) and a rotation: call it a few
 * times a minute, never per pixel. */
#ifndef EPHEM_H
#define EPHEM_H
#include <stdint.h>

enum { EP_MERCURY, EP_VENUS, EP_EARTH, EP_MARS, EP_JUPITER, EP_SATURN, EP_URANUS, EP_NEPTUNE, EP_N };
/* The UI's supported approximate-planet interval: 1800-01-01..2050-01-01 UTC.
 * This bounds model use; it does not assert accuracy for the bundled sky pack. */
#define EP_JD_MIN 2378496.5
#define EP_JD_MAX 2469807.5

typedef struct { double x, y, z; } ep_vec;

/* Julian date of a Unix time (UTC seconds) */
double ep_jd_unix(double unix_s);
/* Safeguarded elliptic Kepler solve. Mean anomaly in degrees; result [-pi,pi].
 * Invalid/nonfinite inputs or e outside [0,1) return NaN. */
double ep_kepler(double mean_deg, double eccentricity);
/* heliocentric ecliptic position of planet p at jd */
ep_vec ep_helio(int p, double jd);
/* planet p's eccentric anomaly at jd, radians 0..2pi: ep_orbit's point i sits at E = 2 pi i / n,
 * so a motion streak is a run of orbit points (no extra maths per frame) */
double ep_ecc_anomaly(int p, double jd);
/* n points of planet p's orbit (true shape, at jd's elements) into xyz[3n], au */
void   ep_orbit(int p, double jd, int n, float *xyz);

/* a planet as seen from Earth */
typedef struct {
    ep_vec helio;        /* heliocentric, au */
    double dist;         /* from Earth, au */
    double lon, lat;     /* geocentric ecliptic, deg */
    double elong;        /* angle from the Sun, deg */
    int    evening;      /* 1 east of the Sun (evening sky), 0 morning */
    double lit;          /* illuminated fraction 0..1 */
} ep_body;
typedef struct { double jd; ep_vec earth; ep_body b[EP_N]; double sun_lon; } ep_sky;
void ep_sky_at(double jd, ep_sky *out);

/* events: what the face highlights. kind EV_*; a, b = planets for non-line-up
 * events (b = -1 if one); line-ups use mask exclusively. value is separation /
 * elongation in deg; n is the number of line-up members. */
enum { EV_OPPOSITION, EV_CONJUNCTION, EV_ELONGATION, EV_SOLAR, EV_LINEUP };
typedef struct { double jd; uint8_t kind, a; int8_t b; uint8_t n, rank, evening, inferior; uint16_t mask; float value; } ep_event;
/* Scan [jd0, jd0 + days] (whole days); returns how many were written (<= cap), sorted.
 * Only rank >= min_rank (1 hidden ones, 2 worth a chip, 3 big ones). One sky per day of
 * double maths (soft-float on the S3: cost still to be measured on the board), so the face
 * runs it in slices with ep_scan_* below. */
int ep_events(double jd0, int days, int min_rank, ep_event *out, int cap);
/* The same scan in slices, for a frame budget: begin once, then step a few days per frame
 * until it returns 1 (out is then sorted and holds sc.n events). */
typedef struct {
    double jd0; int days, i, min_rank, n, cap, done;
    ep_sky S[3]; int run_on[2]; ep_event run_ev[2]; ep_event *out;
} ep_scan;
void ep_scan_begin(ep_scan *sc, double jd0, int days, int min_rank, ep_event *out, int cap);
int  ep_scan_step(ep_scan *sc, int max_days);
/* short upper-case title of an event, e.g. "MARS + JUPITER", "SATURN OPPOSITION" */
void ep_event_title(const ep_event *e, char *out, int n);

/* ---- beyond the planets (ports of the prototype's conic / satPos / moon / geometry) ---- */
/* any body at heliocentric h as seen from Earth (planets, Pluto, asteroids, comets) */
ep_body ep_observe(ep_vec h, ep_vec earth);
/* an asteroid or comet from perihelion elements (ecliptic J2000, deg; tp = JD of perihelion):
 * ellipse, near-parabola (Barker) or hyperbola. Heliocentric au. */
typedef struct { double e, q, i, node, w, tp; } ep_conic;
ep_vec ep_conic_pos(const ep_conic *o, double jd);
typedef struct { double px[3],py[3],a,b,n; const ep_conic *el; } ep_conic_cache;
void ep_conic_prepare(ep_conic_cache *c,const ep_conic *o);
ep_vec ep_conic_cached(const ep_conic_cache *c,double jd);
ep_vec ep_helio_anomaly(int p,double jd,double *anomaly);
/* Visual motion only; exact APIs remain authoritative at rest/events/cards. */
ep_vec ep_helio_preview(int p,double jd,double *anomaly);
ep_vec ep_conic_preview(const ep_conic_cache *c,double jd);
/* a moon or orbiter round its planet (a km, deg, P days, M at epoch): planet-centred km */
typedef struct { double a, e, i, node, w, M, P, epoch; } ep_sat;
ep_vec ep_sat_pos(const ep_sat *o, double jd);
/* the Moon, geocentric ecliptic: lon/lat deg, distance km (~0.3 deg); lit fraction, waxing */
void   ep_moon(double jd, double *lon, double *lat, double *km);
double ep_moon_lit(double jd, int *waxing);
/* ecliptic -> equatorial J2000 */
ep_vec ep_ecl2eq(ep_vec v);
/* bodies the globe knows beyond the planets */
enum { EPB_SUN = EP_N, EPB_MOON, EPB_PLUTO, EPB_N };
/* where you and the Sun stand over a body right now (IAU spin axis and prime meridian):
 * planetocentric lat/lon in deg of the sub-observer and sub-solar points. For Earth the
 * observer is put just past the day side (no location yet); pluto_helio only for Pluto. */
void ep_geometry(int body, double jd, ep_vec pluto_helio, float *obs_lat, float *obs_lon, float *sun_lat, float *sun_lon);
/* a vector (ecliptic) seen face-on from above body's north pole: x, y in the equator plane */
void ep_pole_frame(int body, ep_vec v, float *x, float *y);

extern const char *const ep_name[EP_N];        /* "MERCURY" ... */
extern const double ep_period_days[EP_N];
#endif
