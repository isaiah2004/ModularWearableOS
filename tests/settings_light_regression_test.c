/* Fixed assertions replacing diag_constant_light.c, plus malformed/missing data. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../nos/apps/set_diag.c"

nos_time_t nos_t = 10;
static hal_light light;
static int available, polls;
int hal_light_get(hal_light *out) { *out=light; polls++; return available; }
void hal_log(const char *s) { (void)s; }
void hal_gps_enable(int on) { (void)on; }
void hal_hr_enable(int on) { (void)on; }
void hal_lte_enable(int on) { (void)on; }
int svc_track_active(void) { return 0; }

static void reset(float lux, int prox, int valid, int present)
{
    memset(&T, 0, sizeof T); memset(R, 0, sizeof R);
    R[D_LIGHT].st=ST_RUN;
    light=(hal_light){.valid=valid,.lux=lux,.prox=prox}; available=present; polls=0;
}
static void run(void)
{
    for (int i=0; i<160 && R[D_LIGHT].st==ST_RUN; i++) {
        R[D_LIGHT].el=(i+1)*.025f;
        test_light(&R[D_LIGHT]);
    }
}
int main(void)
{
    reset(0,0,1,1); run();
    assert(polls==160 && R[D_LIGHT].st==ST_PASS);
    assert(!strcmp(R[D_LIGHT].why,"0 LUX, PLAUSIBLE"));
    assert(strstr(R[D_LIGHT].rd[1],"POLLS") && !strstr(R[D_LIGHT].rd[1],"READS"));
    reset(125,128,1,1); run(); assert(R[D_LIGHT].st==ST_PASS);
    reset(10,0,1,1);
    for (int i=0; i<160; i++) {
        light.lux=(float)i; light.prox=i;
        R[D_LIGHT].el=(i+1)*.025f; test_light(&R[D_LIGHT]);
    }
    assert(R[D_LIGHT].st==ST_PASS);
    const float invalid_lux[]={NAN,INFINITY,-1};
    for (unsigned i=0;i<sizeof invalid_lux/sizeof invalid_lux[0];i++) {
        reset(invalid_lux[i],0,1,1); run(); assert(R[D_LIGHT].st==ST_FAIL);
    }
    reset(1,-1,1,1); run(); assert(R[D_LIGHT].st==ST_FAIL);
    reset(1,256,1,1); run(); assert(R[D_LIGHT].st==ST_FAIL);
    reset(1,1,0,1); run(); assert(R[D_LIGHT].st==ST_FAIL && T.lt_n==0);
    reset(1,1,1,0); run(); assert(R[D_LIGHT].st==ST_FAIL && T.lt_n==0);
    /* A malformed value after good polls also fails, instead of escaping extrema. */
    reset(2,2,1,1); R[D_LIGHT].el=1; test_light(&R[D_LIGHT]);
    light.lux=NAN; test_light(&R[D_LIGHT]); assert(R[D_LIGHT].st==ST_FAIL);
    assert(!strcmp(DI[D_LIGHT].check[1],"PLAUSIBILITY ONLY"));
    puts("PASS DEV-006: stable darkness/proximity passes explicit plausibility scope; changing samples pass; nonfinite/range/missing data fail without claiming sample liveness");
    return 0;
}
