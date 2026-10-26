/* Exercise actual manual wrist setter/learn activation and unsupported GPS UI. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../nos/apps/set_settings.c"
#include "settings_semantic_draw_stubs.h"

nos_time_t nos_t = 10;
uint32_t nos_frame = 1;
const nos_face *const nos_faces[] = {NULL};
const int nos_face_count = 0;
static svc_sys_t sys;
static wake_info wake;
static int wrist_saves, fail_wrist_save, learns, learned_hand[2], feedback;
static char toast[64];

uint32_t hal_millis(void) { return 1000; }
int hal_phone_connected(void) { return 0; }
hal_conn hal_status(int feature) { (void)feature; return HAL_CONN_OFF; }
const svc_sys_t *svc_sys(void) { return &sys; }
int hal_battery_percent(void) { return 50; }
void hal_get_datetime(hal_datetime *out) { *out=(hal_datetime){.year=2026,.month=10,.day=21,.hour=12}; }
void ui_fmt_time(char *out,int n,int h,int m) { snprintf(out,(size_t)n,"%02d:%02d",h,m); }
void wake_service_get(wake_info *out) { *out=wake; }
int wake_service_set(const char *key,int value)
{
    assert(!strcmp(key,"wrist") && value>=0 && value<=1);
    wrist_saves++;
    if (fail_wrist_save) return 0;
    wake.wrist=value; return 1;
}
void wake_service_learn(void) { learns++; learned_hand[wake.wrist]++; wake.learning=1; }
void nos_feedback(int kind) { feedback=kind; }
void nos_toast(const char *s) { snprintf(toast,sizeof toast,"%s",s); }
/* Navigation/hardware changes outside the exercised rows must never run. The
 * nonescaping push mock also lets LTO discard unrelated screen renderers. */
void nos_push(const nos_screen *screen,int tx) { (void)screen; (void)tx; assert(0); }
void set_modules_open_sd(void) { assert(0); }
void set_connectivity_open(int bt) { (void)bt; assert(0); }
void hal_store_save(const char *key,const void *data,int size) { (void)key; (void)data; (void)size; assert(0); }
/* Manual wrist/GPS edits only mark RAM preferences dirty. Retained Reset and
 * Restart branches now reference the real checked prefs_flush path; provide its
 * exact HAL interfaces without allowing those unexercised branches to do I/O. */
int hal_store_save_checked(const char *key,const void *data,int size)
{ (void)key; (void)data; (void)size; assert(0 && "unexpected preference checkpoint write"); return 0; }
int hal_store_load(const char *key,void *data,int size)
{ (void)key; (void)data; (void)size; assert(0 && "unexpected preference checkpoint read"); return 0; }
void hal_set_backlight(uint8_t level) { (void)level; assert(0); }
void hal_audio_volume(int pct) { (void)pct; assert(0); }
void hal_haptic_set_strength(int pct) { (void)pct; assert(0); }
const char *ui_module_short(uint32_t ext) { (void)ext; return "MODULE"; }
int hal_lte_get(hal_lte *out) { memset(out,0,sizeof *out); return 0; }
uint64_t hal_epoch_seconds(void) { return 0; }
nos_time_t nos_enter_t(void) { return nos_t; }
void nos_pop(void) { assert(0); }
int hal_set_datetime_checked(const hal_datetime *dt) { (void)dt; assert(0); return 0; }
void hal_set_datetime(const hal_datetime *dt) { (void)dt; assert(0); }
void nos_brightness_override(int level) { (void)level; assert(0); }
uint32_t hal_ext_present(void) { return S.ext; }
int hal_light_get(hal_light *out) { memset(out,0,sizeof *out); return 0; }
void hal_haptic(int pattern) { (void)pattern; assert(0); }
void hal_sfx(int sound) { (void)sound; assert(0); }

int main(int argc,char **argv)
{
    enum { LEARN_ENTRY=7, GPS_ENTRY=20 };
    assert(E[LEARN_ENTRY].id==I_LEARN && E[GPS_ENTRY].id==I_GPS && NE==36);
    prefs_defaults();
    if (argc < 2 || strcmp(argv[1],"wrist")) {
    uint8_t original=nos_prefs.gps_auto;
    assert(!prefs_pending() && !tog_pref(I_GPS));
    S.ext=EXT_GPS; activate(GPS_ENTRY);
    assert(nos_prefs.gps_auto==original && !prefs_pending() && feedback==FB_LIMIT);
    assert(!strcmp(toast,"GPS AUTO UNAVAILABLE"));
    S.ext=0; activate(GPS_ENTRY);
    assert(nos_prefs.gps_auto==original && !prefs_pending());
    S.build_t0=nos_t-100;
    for (int overlay=0;overlay<2;overlay++) {
        memset(&test_draw,0,sizeof test_draw); row_overlay=overlay;
        gfx_fb fb={.h=GFX_H}; draw_entry(&fb,GPS_ENTRY,180,90,0);
        assert(strstr(test_draw.text,"N/A") && !test_draw.pills && !test_draw.circles);
    }
    row_overlay=0;
    puts("PASS DEV-002: GPS-auto is explicitly unavailable with/without hardware; activation cannot mutate the legacy preference or request demand");
    }
    if (argc > 1 && !strcmp(argv[1],"gps")) return 0;

    nos_prefs.units_metric=1;
    nos_prefs.wrist_refs[0]=23; nos_prefs.wrist_refs[1]=42;
    nos_prefs.wrist_left=1;
    uint32_t revision=prefs_wrist_revision();
    flip(I_WRIST);
    assert(!nos_prefs.wrist_left && prefs_wrist_source()==PREFS_WRIST_SOURCE_MANUAL);
    assert(prefs_wrist_revision()==revision+1 && prefs_pending());
    /* No host loop intervenes: learn must select the authoritative hand now. */
    wake.wrist=0; activate(LEARN_ENTRY);
    assert(wrist_saves==1 && learns==1 && learned_hand[1]==1 && learned_hand[0]==0);
    assert(!strcmp(toast,"HOLD RIGHT WRIST STILL"));
    update_values(); assert(!strcmp(S.val[I_LEARN],"Right holding"));
    activate(LEARN_ENTRY); assert(wrist_saves==1 && learns==2);
    flip(I_WRIST);
    assert(nos_prefs.wrist_left && prefs_wrist_revision()==revision+2);
    activate(LEARN_ENTRY);
    assert(wrist_saves==2 && learns==3 && learned_hand[0]==1 && learned_hand[1]==2);
    assert(!strcmp(toast,"HOLD LEFT WRIST STILL"));
    assert(nos_prefs.wrist_refs[0]==23 && nos_prefs.wrist_refs[1]==42);
    /* If synchronization cannot be saved, do not learn the other hand. */
    wake.wrist=1; wake.learning=0; S.learn_wait=0; fail_wrist_save=1;
    activate(LEARN_ENTRY);
    assert(wrist_saves==3 && learns==3 && !S.learn_wait && feedback==FB_ERROR);
    assert(!strcmp(toast,"WRIST NOT SAVED: RETRY"));
    puts("PASS DEV-003: both manual wrist edits advance real preference metadata; immediate learning selects the matching hand, preserves wrist references, and refuses a failed wrist save");
    return 0;
}
