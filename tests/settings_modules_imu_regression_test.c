/* Real Modules sampling, tick, text and draw paths with failed/retained IMU data. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../nos/apps/set_modules.c"
#include "settings_semantic_draw_stubs.h"

nos_time_t nos_t=10;
nos_prefs_t nos_prefs;
static hal_imu reading;
static int read_ok;
uint32_t hal_ext_present(void) { return 0; }
int hal_imu_read(hal_imu *out) { if (read_ok) *out=reading; return read_ok; }
void hal_get_datetime(hal_datetime *out) { *out=(hal_datetime){.year=2026,.month=10,.day=21,.hour=12}; }
void hal_storage_get(hal_storage *out) { memset(out,0,sizeof *out); }
void hal_flash_info_get(hal_flash_info *out) { memset(out,0,sizeof *out); }
void hal_power_get(hal_power *out) { *out=(hal_power){.rail_mv=3700,.battery_percent=50,.sample_valid=1}; }
int hal_battery_mv(void) { return 3700; }
int hal_battery_percent(void) { return 50; }
hal_conn hal_status(int feature) { (void)feature; return HAL_CONN_OFF; }
uint64_t hal_epoch_seconds(void) { return 1792540800; }
const char *ui_weekday3(int day) { (void)day; return "WED"; }
const char *ui_month3(int month) { (void)month; return "OCT"; }
void ui_fmt_time(char *out,int n,int h,int m) { snprintf(out,(size_t)n,"%02d:%02d",h,m); }
int hal_gps_get(hal_gps *out) { memset(out,0,sizeof *out); return 0; }
int hal_lte_get(hal_lte *out) { memset(out,0,sizeof *out); return 0; }
int hal_env_get(hal_env *out) { memset(out,0,sizeof *out); return 0; }
int hal_light_get(hal_light *out) { memset(out,0,sizeof *out); return 0; }
int hal_mag_get(hal_mag *out) { memset(out,0,sizeof *out); return 0; }
int hal_hr_get(hal_hr *out) { memset(out,0,sizeof *out); return 0; }
void ui_trail_reset(ui_trail *trail,float x,float y) {}
void ui_trail_push(ui_trail *trail,float x,float y) {}
int hal_phone_connected(void) { return 0; }
const svc_sys_t *svc_sys(void) { static svc_sys_t sys; return &sys; }
int nos_touch_travel(void) { return 0; }
int hal_mic_wave(uint8_t out[32]) { memset(out,0,32); return 0; }
int hal_light_gesture(void) { return GEST_NONE; }
void hal_mag_calibrate(int on) { (void)on; assert(0); }
void nos_feedback(int kind) { (void)kind; assert(0); }
void nos_toast(const char *s) { (void)s; assert(0); }
void prog_bar(gfx_fb *f,int style,float x,float y,float w,float h,int n,float frac,uint16_t on,uint16_t off) {}

static void draw_assert(int available)
{
    memset(&test_draw,0,sizeof test_draw);
    gfx_fb fb={0}; detail_draw(&fb);
    if (available) {
        assert(!test_draw.absent && test_draw.dots==2);
        assert(strstr(test_draw.text,"TILT") && strstr(test_draw.text,"GYRO"));
        assert(!strstr(test_draw.text,"No reply"));
    } else {
        assert(test_draw.absent==1 && !test_draw.dots);
        assert(strstr(test_draw.text,"No reply") && strstr(test_draw.text,"MOTION DATA UNAVAILABLE"));
        assert(!strstr(test_draw.text,"TILT") && !strstr(test_draw.text,"GYRO"));
        assert(!strcmp(D.hero.to,UI_NONE) && !strcmp(D.hero.from,UI_NONE));
        assert(!D.line[0][0] && !D.line[1][0] && !D.line[2][0]);
    }
    list_values();
    assert((V.ok[M_IMU]!=0)==available);
    if (!available) assert(!strcmp(V.val[M_IMU],"No reply"));
}

int main(void)
{
    D.m=M_IMU; D.t0=nos_t-2; D.sig=-1;
    /* Cold failure and subsequent failure both hide numeric vectors/bubble. */
    read_ok=0; detail_tick(.025f); draw_assert(0);
    reading=(hal_imu){.ax=.2f,.ay=-.3f,.az=.9327f,.gx=1,.gy=2,.gz=3}; read_ok=1;
    detail_tick(.025f); draw_assert(1);
    hal_imu retained=V.imu;
    float bubble_x=D.bub_x.x,bubble_y=D.bub_y.x;
    D.next=nos_t+30; read_ok=0; detail_tick(.025f);
    assert(!memcmp(&retained,&V.imu,sizeof retained));  /* failed getter kept last data */
    assert(D.bub_x.x==bubble_x && D.bub_y.x==bubble_y); draw_assert(0);
    reading=(hal_imu){.ax=-.3f,.ay=.2f,.az=.9327f}; read_ok=1;
    detail_tick(.025f); draw_assert(1);
    float bx,by; bubble_target(&bx,&by);
    assert(D.bub_x.x==bx && D.bub_y.x==by);  /* recovery starts from fresh motion */
    for (int axis=0;axis<6;axis++) {
        reading=(hal_imu){.az=1};
        switch (axis) {
        case 0: reading.ax=NAN; break; case 1: reading.ay=NAN; break;
        case 2: reading.az=NAN; break; case 3: reading.gx=INFINITY; break;
        case 4: reading.gy=INFINITY; break; case 5: reading.gz=INFINITY; break;
        }
        detail_tick(.025f); assert(!V.imu_ok); draw_assert(0);
        reading=(hal_imu){.az=1}; detail_tick(.025f); draw_assert(1);
    }
    puts("PASS DEV-005: cold/read failure and six nonfinite axes hide retained Motion numbers/bubble immediately; list/detail agree and fresh recovery resumes automatically");
    return 0;
}
