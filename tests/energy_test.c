/* energy_test.c — body profile energy arithmetic (nos/energy.h).
 * gcc -std=c99 -O2 -Wall -Wextra -Werror -Inos tests/energy_test.c -lm -o energy_test */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "energy.h"

#define NEAR(a,b,t) (fabsf((float)(a)-(float)(b))<=(t))

int main(void)
{
    energy_body none={0,0,0,0}, male={80,180,30,ENERGY_SEX_MALE}, female={60,165,25,ENERGY_SEX_FEMALE};
    /* Assumed-value flags: mass alone, or any of height/age/sex, marks the figure as defaulted. */
    assert(energy_assumed(&none)==(ENERGY_ASSUME_MASS|ENERGY_ASSUME_BODY));
    assert(energy_assumed(&male)==0 && energy_assumed(&female)==0);
    energy_body w={75,0,0,0};assert(energy_assumed(&w)==ENERGY_ASSUME_BODY);
    energy_body h={0,175,30,ENERGY_SEX_MALE};assert(energy_assumed(&h)==ENERGY_ASSUME_MASS);

    /* Mifflin-St Jeor: 10*80 + 6.25*180 - 5*30 + 5 = 1780; female 10*60+6.25*165-125-161 = 1345.25. */
    assert(NEAR(energy_bmr_day(&male),1780.0f,.01f));
    assert(NEAR(energy_bmr_day(&female),1345.25f,.01f));
    /* Unset everything: 70 kg / 170 cm / 30 y, sex midpoint (-78): 700+1062.5-150-78 = 1534.5. */
    assert(NEAR(energy_bmr_day(&none),1534.5f,.01f));
    assert(NEAR(energy_rest_min(&male),1780.0f/1440.0f,.0001f));
    /* A silly profile cannot report nonsense. */
    energy_body tiny={20,100,120,ENERGY_SEX_FEMALE};assert(energy_bmr_day(&tiny)>=800.0f);
    energy_body huge={250,230,5,ENERGY_SEX_MALE};assert(energy_bmr_day(&huge)<=4000.0f);

    /* Prorated by seconds since local midnight; clamped to the day. */
    assert(energy_rest_so_far(&male,0)==0.0f);
    assert(NEAR(energy_rest_so_far(&male,43200),890.0f,.01f));
    assert(NEAR(energy_rest_so_far(&male,86400),1780.0f,.01f));
    assert(NEAR(energy_rest_so_far(&male,99999),1780.0f,.01f));
    assert(energy_rest_so_far(&male,-5)==0.0f);

    /* Age from the stored birth year; the clock year is trusted only when plausible. */
    assert(energy_age_from_birth(1990,2026)==36);
    assert(energy_age_from_birth(0,2026)==0);
    assert(energy_age_from_birth(1850,2026)==0);
    assert(energy_age_from_birth(2026,2026)==5);           /* floor: a profile of a newborn is a typo */
    assert(energy_age_from_birth(1900,2100)==120);
    assert(energy_age_from_birth(1990,1970)==energy_age_from_birth(1990,ENERGY_YEAR_FALLBACK)); /* unset clock */

    /* Stride is 0.414 x height (sex-adjusted), 0.75 m when height is unset. */
    assert(NEAR(energy_stride_m(&none),0.75f,.0001f));
    assert(NEAR(energy_stride_m(&male),1.80f*0.415f,.0005f));
    assert(NEAR(energy_stride_m(&female),1.65f*0.413f,.0005f));
    energy_body hn={70,170,30,ENERGY_SEX_UNSET};assert(NEAR(energy_stride_m(&hn),1.70f*0.414f,.0005f));
    /* Net step energy: 0.5 kcal/kg/km. Unset reproduces 70 kg x 0.75 m = 0.02625 per step. */
    assert(NEAR(energy_step_kcal(&none),0.02625f,.0001f));
    assert(NEAR(energy_step_kcal(&male),0.5f*80.0f*(1.80f*0.415f)/1000.0f,.0001f));
    assert(energy_step_kcal(&male)>energy_step_kcal(&female)); /* heavier and taller */

    /* Keytel: published coefficients, kJ -> kcal. 80 kg male age 30 at 140 bpm. */
    assert(NEAR(energy_keytel_min(&male,140),(-55.0969f+0.6309f*140+0.1988f*80+0.2017f*30)/4.184f,.001f));
    assert(NEAR(energy_keytel_min(&female,140),(-20.4022f+0.4472f*140-0.1263f*60+0.0740f*25)/4.184f,.001f));
    /* Unset sex: midpoint of the two equations, between them. */
    energy_body m70={70,0,30,ENERGY_SEX_MALE},f70={70,0,30,ENERGY_SEX_FEMALE},u70={70,0,30,ENERGY_SEX_UNSET};
    float km=energy_keytel_min(&m70,100),kf=energy_keytel_min(&f70,100),ku=energy_keytel_min(&u70,100);
    assert(kf<ku && ku<km && NEAR(ku,0.5f*(km+kf),.001f));
    assert(NEAR(ku,5.457f,.01f));
    assert(energy_keytel_min(&f70,20)>=0.0f);               /* never negative */
    assert(energy_keytel_min(&male,160)>energy_keytel_min(&male,120));

    /* MET energy: MET x 3.5 x kg / 200 kcal/min. Badminton 5.5 at 70 kg = 6.7375. */
    assert(NEAR(energy_met_min(5.5f,70.0f),6.7375f,.0001f));
    /* Movement scaling: 1.3 MET idle, the category MET at 50 %, capped at 1.5x of the span. */
    assert(NEAR(energy_met_scaled(55,0),1.3f,.001f));
    assert(NEAR(energy_met_scaled(55,50),5.5f,.001f));
    assert(NEAR(energy_met_scaled(55,25),3.4f,.001f));
    assert(NEAR(energy_met_scaled(55,100),1.3f+4.2f*1.5f,.001f));
    assert(NEAR(energy_met_scaled(55,255),5.5f,.001f));     /* unknown motion: unscaled */
    assert(NEAR(energy_met_scaled(12,0),1.2f,.001f));       /* below idle: untouched */

    /* Session rate selection and basis flags. */
    int basis=0;
    float r=energy_session_rate(&m70,140,55,1,60,&basis);
    assert(basis==ENERGY_BASIS_HR && NEAR(r,energy_keytel_min(&m70,140),.001f));
    basis=0;r=energy_session_rate(&m70,89,55,1,50,&basis);  /* below 90 bpm: HR under-reads */
    assert(basis==ENERGY_BASIS_MOVE && NEAR(r,energy_met_min(5.5f,70.0f),.001f));
    basis=0;r=energy_session_rate(&m70,0,55,1,255,&basis);   /* motion unknown */
    assert(basis==ENERGY_BASIS_MET && NEAR(r,energy_met_min(5.5f,70.0f),.001f));
    basis=0;r=energy_session_rate(&m70,0,70,0,50,&basis);    /* not a movement family */
    assert(basis==ENERGY_BASIS_MET && NEAR(r,energy_met_min(7.0f,70.0f),.001f));
    basis=0;r=energy_session_rate(&m70,0,0,0,50,&basis);     /* nothing to go on */
    assert(basis==0 && r==0.0f);
    basis=0;r=energy_session_rate(&m70,150,0,0,255,&basis);  /* HR works without a category MET */
    assert(basis==ENERGY_BASIS_HR && r>0.0f);
    basis=0;r=energy_session_rate(&m70,230,55,1,50,&basis);  /* HR out of range falls back */
    assert(basis==ENERGY_BASIS_MOVE);
    /* HR path never reports below the resting rate. */
    energy_body old={50,150,90,ENERGY_SEX_FEMALE};basis=0;
    r=energy_session_rate(&old,90,0,0,255,&basis);assert(r>=energy_rest_min(&old));
    /* Rate is capped. */
    energy_body big={250,230,60,ENERGY_SEX_MALE};basis=0;
    assert(energy_session_rate(&big,220,0,0,255,&basis)<=30.0f);

    /* Daily merge: the pedometer steps taken inside workouts are removed from the step term. */
    float sk=0.03f;
    assert(NEAR(energy_daily_active(1000,0,sk,0),30.0f,.001f));            /* no workouts */
    assert(NEAR(energy_daily_active(1000,400,sk,50),18.0f+50.0f,.001f));   /* 600 outside + 50 net */
    assert(NEAR(energy_daily_active(1000,1000,sk,50),50.0f,.001f));        /* all steps inside */
    assert(NEAR(energy_daily_active(300,900,sk,50),50.0f,.001f));          /* more inside than counted: clamp */
    assert(NEAR(energy_daily_active(1000,100,sk,-5),27.0f,.001f));         /* bad net ignored */
    /* The workout adds its net energy only once: total = active + resting so far. */
    float active=energy_daily_active(8000,2500,energy_step_kcal(&male),120.0f);
    float total=active+energy_rest_so_far(&male,36000);
    assert(NEAR(total-active,1780.0f*36000.0f/86400.0f,.01f));
    assert(NEAR(active,5500.0f*energy_step_kcal(&male)+120.0f,.01f));
    puts("PASS energy: BMR, stride, step kcal, Keytel, MET scaling, rate selection, daily merge without double counting");
    return 0;
}
