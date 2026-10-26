/* Independent monotone bisection oracle for elliptic solves and actual sky-pack
 * records. Numerical consistency only: this is not an astronomy accuracy test. */
#include "ephem.h"
#include "faces/orrery_data.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static const double pi = 3.14159265358979323846;
static double radians(double degrees)
{
    double d = fmod(degrees, 360.0);
    if (d < 0) d += 360.0;
    if (d > 180) d -= 360.0;
    return d * (pi / 180.0);
}
static double bisect(double M, double e)
{
    double lo = -pi, hi = pi;
    for (int i = 0; i < 100; i++) {
        double mid = (lo + hi) * .5;
        if (mid - e * sin(mid) < M) lo = mid; else hi = mid;
    }
    return (lo + hi) * .5;
}
static double distance(ep_vec a, ep_vec b)
{
    return hypot(hypot(a.x - b.x, a.y - b.y), a.z - b.z);
}
static ep_vec reference(const ep_conic *o, double jd)
{
    double a = o->q / (1 - o->e), b = a * sqrt(1 - o->e * o->e);
    double E = bisect(radians(.01720209895 / pow(a, 1.5) * (jd - o->tp) * 180 / pi), o->e);
    double u = a * (cos(E) - o->e), v = b * sin(E);
    double w = o->w * pi / 180, O = o->node * pi / 180, I = o->i * pi / 180;
    ep_vec r = {
        (cos(w)*cos(O)-sin(w)*sin(O)*cos(I))*u + (-sin(w)*cos(O)-cos(w)*sin(O)*cos(I))*v,
        (cos(w)*sin(O)+sin(w)*cos(O)*cos(I))*u + (-sin(w)*sin(O)+cos(w)*cos(O)*cos(I))*v,
        sin(w)*sin(I)*u + cos(w)*sin(I)*v
    };
    return r;
}
static unsigned samples;
static double max_exact, max_preview, max_residual;
static void catalog_case(const ep_conic *o, double jd)
{
    if (o->e < 0 || o->e >= 1 || fabs(o->e - 1) < .001) return;
    ep_conic_cache cache;
    ep_conic_prepare(&cache, o);
    ep_vec want = reference(o, jd);
    double exact = fmax(distance(ep_conic_pos(o, jd), want), distance(ep_conic_cached(&cache, jd), want));
    double preview = distance(ep_conic_preview(&cache, jd), want);
    if (exact > max_exact) max_exact = exact;
    if (preview > max_preview) max_preview = preview;
    if (!(exact < 2e-8 && preview < .002)) {
        fprintf(stderr, "catalog e=%.12g jd=%.9f exact=%.9g preview=%.9g\n", o->e, jd, exact, preview);
        assert(0);
    }
    samples++;
}
int main(void)
{
    const double es[] = {0, .2, .8, .9, .99, .9955, .9989, .999999, 1-1e-12};
    const double ms[] = {0, 1e-9, 1e-6, .001, .1, 1, 90, 179.99, 180, 270, 359.99, 360.001, 1000000, -1e-6, -90};
    for (unsigned i = 0; i < sizeof es / sizeof *es; i++)
        for (unsigned j = 0; j < sizeof ms / sizeof *ms; j++) {
            double M = radians(ms[j]), E = ep_kepler(ms[j], es[i]);
            double residual = fabs(E - es[i] * sin(E) - M);
            if (residual > max_residual) max_residual = residual;
            assert(isfinite(E) && residual < 5e-13);
            assert(fabs(E - bisect(M, es[i])) < 2e-9);
        }
    assert(isnan(ep_kepler(NAN, .5)) && isnan(ep_kepler(10, 1)) && isnan(ep_kepler(10, -.1)));
    for (double jd = EP_JD_MIN + 1; jd < EP_JD_MAX; jd += 31.75) {
        for (int k = 0; k < od_comets_n; k++) catalog_case(&od_comets[k].el, jd);
        for (int k = 0; k < od_small_n; k++) catalog_case(&od_small_bodies[k].el, jd);
    }
    /* The observed divergent case must be covered independently of the grid. */
    for (int k = 0; k < od_comets_n; k++) if (od_comets[k].el.e == .9955)
        catalog_case(&od_comets[k].el, 2465445.0);
    printf("PASS safeguarded Kepler: grid residual %.3g rad; %u actual catalog/date samples, "
           "public/cached error %.3g AU, preview error %.3g AU vs bisection (numerical only)\n",
           max_residual, samples, max_exact, max_preview);
    return 0;
}
