/* Daily Activity energy merge in the real service (act_energy): profile-driven step energy and
 * stride, workout net energy added without double counting its steps, resting energy prorated by
 * the time of day. Only clocks, storage and the workout history are mocked. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../nos/svc.c"
nos_time_t nos_t;
nos_prefs_t nos_prefs;
static uint32_t clock_ms=1000;
static uint64_t clock_epoch=1792497600ULL;   /* an instant in 2026 */
static hal_datetime now_dt={.year=2026,.month=10,.day=24,.hour=12};
static uint32_t stub_ped,stub_since;static float stub_net;static int stub_count;
uint32_t hal_millis(void) {return clock_ms;}
uint64_t hal_epoch_seconds(void) {return clock_epoch;}
void hal_get_datetime(hal_datetime *out) {*out=now_dt;}
int workout_day_energy(uint32_t since,uint32_t *ped,float *net) {stub_since=since;*ped=stub_ped;*net=stub_net;return stub_count;}
#define NEAR(a,b,t) (fabsf((float)(a)-(float)(b))<=(t))
int main(void)
{
    /* Unset profile: the old 0.75 m stride, 70 kg assumed, everything flagged as assumed. */
    memset(&nos_prefs,0,sizeof nos_prefs);
    act_publish(10000);
    assert(NEAR(s_act.km,7.5f,.001f) && NEAR(s_act.kcal,10000*0.02625f,.01f));
    assert(s_act.kcal_assumed==(ENERGY_ASSUME_MASS|ENERGY_ASSUME_BODY));
    float rest_unset=1534.5f*0.5f;                  /* noon: half of the day's resting energy */
    assert(NEAR(s_act.kcal_rest,rest_unset,.05f) && NEAR(s_act.kcal_total,s_act.kcal+rest_unset,.05f));
    assert(stub_since==0 || stub_since==(uint32_t)(clock_epoch-12*3600));   /* local midnight */

    /* A complete profile: weight 80 kg, 180 cm, born 1996 (age 30 in 2026), male. */
    nos_prefs.sports_mass_kg=80;nos_prefs.body_height_cm=180;nos_prefs.body_birth_year=1996;nos_prefs.body_sex=1;
    act_publish(10000);
    float stride=1.80f*0.415f,sk=0.5f*80*stride/1000.0f;
    assert(s_act.kcal_assumed==0);
    assert(NEAR(s_act.km,10000*stride/1000.0f,.001f) && NEAR(s_act.kcal,10000*sk,.01f));
    assert(NEAR(s_act.kcal_rest,1780.0f*0.5f,.05f));            /* 800+1125-150+5 = 1780 */
    assert(s_act.kcal_workout==0 && s_act.workouts_today==0);

    /* A workout adds its net energy and its own steps leave the step term: no double counting. */
    stub_ped=2500;stub_net=150.0f;stub_count=1;
    act_publish(10000);
    assert(NEAR(s_act.kcal,7500*sk+150.0f,.01f) && NEAR(s_act.kcal_workout,150.0f,.001f) && s_act.workouts_today==1);
    assert(NEAR(s_act.kcal_total,s_act.kcal+s_act.kcal_rest,.001f));
    /* The same steps counted without the merge would be higher by the workout's step share minus its net. */
    assert(s_act.kcal>10000*sk-2500*sk);
    /* Pedometer reset mid-day (fewer steps than the workout recorded): never negative. */
    act_publish(1000);assert(NEAR(s_act.kcal,150.0f,.01f) && s_act.kcal>=0);

    /* Time of day: midnight has no resting energy yet; the end of the day has all of it. */
    stub_ped=0;stub_net=0;stub_count=0;
    now_dt.hour=0;act_publish(0);assert(s_act.kcal_rest==0.0f);
    now_dt.hour=23;now_dt.minute=59;now_dt.second=59;act_publish(0);assert(NEAR(s_act.kcal_rest,1780.0f,.1f));
    /* No trusted clock: no resting energy and no workout merge. */
    clock_epoch=0;stub_since=12345;now_dt.hour=12;now_dt.minute=now_dt.second=0;
    stub_ped=500;stub_net=99;act_publish(1000);
    assert(s_act.kcal_rest==0.0f && stub_since==12345 && NEAR(s_act.kcal,1000*sk,.01f) && s_act.kcal_total==s_act.kcal);
    puts("PASS svc energy: profile stride/step energy, workout merge without double counting, resting by time of day, no-clock fallback");
    return 0;
}
