/* energy.h — body profile and energy arithmetic. Pure and header-only so the host tests
 * (tests/energy_test.c) and the firmware use the same code; no I/O, no globals.
 * Every figure is an engineering estimate, never a measurement, and anything computed
 * from a default instead of the owner's value is flagged (ENERGY_ASSUME_*) so the UI can
 * say so. Units: kcal, kg, cm, years, m.
 *
 *   resting (BMR)  Mifflin-St Jeor: 10 kg + 6.25 cm - 5 age + {+5 male, -161 female};
 *                  an unset sex uses the midpoint (-78). Prorated by wall-clock time of day.
 *   step energy    NET walking cost 0.5 kcal/kg/km (ACSM horizontal term, above resting) x
 *                  stride; stride = 0.414 x height. Net, because resting is added separately.
 *   workout gross  Keytel et al. 2005 (heart rate, weight, age, sex) when HR is valid and
 *                  >= 90 bpm (the equation under-reads below that); otherwise MET x 3.5 x kg /
 *                  200 per minute. Wrist-movement families scale the category MET by the
 *                  measured movement fraction; others keep the category MET.
 *   daily ACTIVE   steps outside workouts x step energy + each workout's net energy; the
 *                  pedometer steps taken during a workout are subtracted, so they are never
 *                  counted twice. TOTAL = ACTIVE + resting so far today. */
#ifndef NOS_ENERGY_H
#define NOS_ENERGY_H
#include <stdint.h>
#include <math.h>

enum { ENERGY_SEX_UNSET, ENERGY_SEX_MALE, ENERGY_SEX_FEMALE };
#define ENERGY_DEF_MASS_KG 70
#define ENERGY_DEF_HEIGHT_CM 170
#define ENERGY_DEF_AGE 30
#define ENERGY_YEAR_FALLBACK 2026   /* when the clock is not set */
enum { ENERGY_ASSUME_MASS = 1, ENERGY_ASSUME_BODY = 2 };   /* height / age / sex defaulted */
typedef struct { uint8_t mass_kg, height_cm, age_years, sex; } energy_body;   /* 0 = unset */

/* workout energy_basis bits (stored with the session) */
enum { ENERGY_BASIS_HR = 1, ENERGY_BASIS_MOVE = 2, ENERGY_BASIS_MET = 4,
       ENERGY_BASIS_ASSUMED_MASS = 8, ENERGY_BASIS_ASSUMED_BODY = 16 };

static inline int energy_assumed(const energy_body *b)
{
    return (b->mass_kg ? 0 : ENERGY_ASSUME_MASS) |
           (b->height_cm && b->age_years && b->sex ? 0 : ENERGY_ASSUME_BODY);
}
static inline float energy_mass(const energy_body *b) { return b->mass_kg ? (float)b->mass_kg : (float)ENERGY_DEF_MASS_KG; }
static inline float energy_height(const energy_body *b) { return b->height_cm ? (float)b->height_cm : (float)ENERGY_DEF_HEIGHT_CM; }
static inline float energy_age(const energy_body *b) { return b->age_years ? (float)b->age_years : (float)ENERGY_DEF_AGE; }
/* Age in whole years from a stored birth year; 0 (unset) when there is none. */
static inline int energy_age_from_birth(int birth_year, int year_now)
{
    if (birth_year < 1900 || birth_year > 2100) return 0;
    if (year_now < 2024 || year_now > 2100) year_now = ENERGY_YEAR_FALLBACK;
    int a = year_now - birth_year;
    return a < 5 ? 5 : a > 120 ? 120 : a;
}
/* Resting energy for a whole day. Floored so a bad profile cannot report nonsense. */
static inline float energy_bmr_day(const energy_body *b)
{
    float s = b->sex == ENERGY_SEX_MALE ? 5.0f : b->sex == ENERGY_SEX_FEMALE ? -161.0f : -78.0f;
    float v = 10.0f * energy_mass(b) + 6.25f * energy_height(b) - 5.0f * energy_age(b) + s;
    return v < 800.0f ? 800.0f : v > 4000.0f ? 4000.0f : v;
}
static inline float energy_rest_min(const energy_body *b) { return energy_bmr_day(b) / 1440.0f; }
/* Resting energy accrued by `sec` seconds after local midnight. */
static inline float energy_rest_so_far(const energy_body *b, int sec)
{
    if (sec < 0) sec = 0;
    if (sec > 86400) sec = 86400;
    return energy_bmr_day(b) * (float)sec / 86400.0f;
}
static inline float energy_stride_m(const energy_body *b)
{
    if (!b->height_cm) return 0.75f;                       /* the old fixed stride */
    float k = b->sex == ENERGY_SEX_MALE ? 0.415f : b->sex == ENERGY_SEX_FEMALE ? 0.413f : 0.414f;
    float m = (float)b->height_cm * k / 100.0f;
    return m < 0.40f ? 0.40f : m > 1.20f ? 1.20f : m;
}
static inline float energy_step_kcal(const energy_body *b)
{ return 0.5f * energy_mass(b) * energy_stride_m(b) / 1000.0f; }

/* Keytel 2005 gross kcal/min (valid for exercise heart rates). Never negative. */
static inline float energy_keytel_min(const energy_body *b, int bpm)
{
    float w = energy_mass(b), a = energy_age(b), h = (float)bpm;
    float male = (-55.0969f + 0.6309f * h + 0.1988f * w + 0.2017f * a) / 4.184f;
    float female = (-20.4022f + 0.4472f * h - 0.1263f * w + 0.0740f * a) / 4.184f;
    float v = b->sex == ENERGY_SEX_MALE ? male : b->sex == ENERGY_SEX_FEMALE ? female : 0.5f * (male + female);
    return v < 0 ? 0 : v;
}
/* Gross kcal/min of a MET value for a body of `mass` kg. */
static inline float energy_met_min(float met, float mass) { return met * 3.5f * mass / 200.0f; }
/* Movement-scaled MET: idle standing 1.3 MET up to 1.5x the category MET, reaching the
 * category value at a 50 % movement fraction. `move_pct` 0..100; 255 or more = unknown
 * (valid motion not available), which keeps the unscaled category MET. */
static inline float energy_met_scaled(int met_tenths, int move_pct)
{
    float met = (float)met_tenths * 0.1f;
    if (move_pct > 100 || met <= 1.3f) return met;
    float x = (float)move_pct / 50.0f;
    if (x > 1.5f) x = 1.5f;
    return 1.3f + (met - 1.3f) * x;
}
/* Gross session kcal/min and the basis used. `scalable` = a wrist-movement family. */
static inline float energy_session_rate(const energy_body *b, int bpm, int met_tenths, int scalable,
                                        int move_pct, int *basis)
{
    float rest = energy_rest_min(b), v;
    if (bpm >= 90 && bpm <= 220) {
        v = energy_keytel_min(b, bpm);
        if (v < rest) v = rest;
        *basis |= ENERGY_BASIS_HR;
    } else if (met_tenths) {
        if (scalable && move_pct <= 100) {
            v = energy_met_min(energy_met_scaled(met_tenths, move_pct), energy_mass(b));
            *basis |= ENERGY_BASIS_MOVE;
        } else {
            v = energy_met_min((float)met_tenths * 0.1f, energy_mass(b));
            *basis |= ENERGY_BASIS_MET;
        }
    } else return 0;
    return v > 30.0f ? 30.0f : v;
}
/* ACTIVE energy for the day: steps outside workouts, plus each workout's net energy.
 * `session_steps` are the pedometer steps already inside those workouts. */
static inline float energy_daily_active(int steps, uint32_t session_steps, float step_kcal, float session_net_kcal)
{
    float outside = steps > (int)session_steps ? (float)(steps - (int)session_steps) : 0.0f;
    return outside * step_kcal + (session_net_kcal > 0 ? session_net_kcal : 0.0f);
}
#endif
