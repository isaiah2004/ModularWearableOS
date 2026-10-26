#include "nos_power.h"
#include "esp_pm.h"
#include "esp_err.h"
#include "esp_private/esp_clk.h"
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#include <stdio.h>
#include <stdbool.h>

/* IDF: components/esp_pm/pm_impl.c esp_pm_configure() and esp_pm_impl_init().
 * All modes start at CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ; explicit fixed fallback
 * prevents a partial initialization from leaving idle DFS active without locks.
 * https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/power_management.html
 *
 * Light sleep: DFS is configured with light_sleep_enable on the 1.43-B (the 1.43C
 * keeps it off). esp_pm_configure ignores a failed CPU-retention allocation, which
 * only costs sleep current. Any held lock keeps the PM mode above light sleep, so the
 * display lock (UI frame pacing waits) and the transfer lock make sleep opt-out. */
static esp_pm_lock_handle_t s_ui,s_bake,s_display,s_xfer;
static int s_initialized,s_enabled,s_error,s_ui_held,s_bake_held,s_display_held,s_xfer_held;
static int s_max_mhz=240,s_light_sleep;
static unsigned s_ui_acquires,s_ui_releases,s_bake_acquires,s_bake_releases;
/* Written only from the idle-task sleep callbacks (serialised by IDF's switch lock). */
static uint32_t s_sleep_ms,s_sleep_rem_us,s_sleep_entries;
static void (*s_hook_before)(void);
static void (*s_hook_after)(int64_t slept_us);

static void delete_locks(void)
{
    esp_pm_lock_handle_t *all[]={&s_ui,&s_bake,&s_display,&s_xfer};
    for(unsigned i=0;i<sizeof all/sizeof all[0];i++) if(*all[i]) {esp_pm_lock_delete(*all[i]);*all[i]=NULL;}
}
static void fixed_fallback(esp_err_t cause)
{
    const esp_pm_config_t fixed={.max_freq_mhz=240,.min_freq_mhz=240,.light_sleep_enable=false};
    __atomic_store_n(&s_enabled,0,__ATOMIC_RELEASE);
    __atomic_store_n(&s_max_mhz,240,__ATOMIC_RELEASE);
    __atomic_store_n(&s_light_sleep,0,__ATOMIC_RELEASE);
    __atomic_store_n(&s_error,(int)cause,__ATOMIC_RELEASE);
    esp_err_t restored=esp_pm_configure(&fixed);
    /* With PM disabled at compile time IDF leaves the configured boot clock.
     * Otherwise a failed restore cannot safely continue at an unknown frequency. */
    if(restored!=ESP_OK && esp_clk_cpu_freq()!=240000000) ESP_ERROR_CHECK(restored);
    printf("NOSPM fallback=fixed240 error=%d restore=%d cpu_mhz=%d\n",(int)cause,(int)restored,esp_clk_cpu_freq()/1000000);
}
#if defined(CONFIG_PM_LIGHT_SLEEP_CALLBACKS)
static esp_err_t IRAM_ATTR sleep_enter_cb(int64_t sleep_time_us,void *arg)
{
    (void)sleep_time_us;(void)arg;
    if(s_hook_before) s_hook_before();
    return ESP_OK;
}
static esp_err_t IRAM_ATTR sleep_exit_cb(int64_t slept_us,void *arg)
{
    (void)arg;
    if(slept_us>=500) { /* anything shorter means IDF rejected the sleep */
        uint32_t rem=s_sleep_rem_us+(uint32_t)(slept_us%1000),ms=s_sleep_ms+(uint32_t)(slept_us/1000);
        if(rem>=1000) {rem-=1000;ms++;}
        __atomic_store_n(&s_sleep_rem_us,rem,__ATOMIC_RELAXED);
        __atomic_store_n(&s_sleep_ms,ms,__ATOMIC_RELAXED);
        __atomic_store_n(&s_sleep_entries,s_sleep_entries+1,__ATOMIC_RELAXED);
    }
    if(s_hook_after) s_hook_after(slept_us);
    return ESP_OK;
}
static void register_sleep_callbacks(void)
{
    esp_pm_sleep_cbs_register_config_t cbs={.enter_cb=sleep_enter_cb,.exit_cb=sleep_exit_cb};
    esp_err_t e=esp_pm_light_sleep_register_cbs(&cbs);
    if(e!=ESP_OK) printf("NOSPM sleep callbacks error=%d (sleep counters stay zero)\n",(int)e);
}
#else
static void register_sleep_callbacks(void) {}
#endif
void nos_power_init(void)
{
    if(s_initialized) return;
    s_initialized=1;
    const esp_pm_config_t fixed={.max_freq_mhz=240,.min_freq_mhz=240,.light_sleep_enable=false};
    esp_err_t e=esp_pm_configure(&fixed);
    if(e!=ESP_OK) {fixed_fallback(e);return;}
    struct {esp_pm_lock_handle_t *h;int type;const char *name;} locks[]={
        {&s_ui,ESP_PM_CPU_FREQ_MAX,"nos-ui"},{&s_bake,ESP_PM_CPU_FREQ_MAX,"nos-prewarm"},
        {&s_display,ESP_PM_NO_LIGHT_SLEEP,"nos-display"},{&s_xfer,ESP_PM_NO_LIGHT_SLEEP,"nos-xfer"}};
    for(unsigned i=0;i<sizeof locks/sizeof locks[0];i++) {
        e=esp_pm_lock_create(locks[i].type,0,locks[i].name,locks[i].h);
        if(e!=ESP_OK) {delete_locks();fixed_fallback(e);return;}
    }
    e=esp_pm_lock_acquire(s_ui);
    if(e!=ESP_OK) {delete_locks();fixed_fallback(e);return;}
    s_ui_held=1;s_ui_acquires++;
    /* Boot is display ON: sleep stays blocked until the main task releases this. */
    e=esp_pm_lock_acquire(s_display);
    if(e!=ESP_OK) {
        esp_pm_lock_release(s_ui);s_ui_held=0;s_ui_releases++;
        delete_locks();fixed_fallback(e);return;
    }
    s_display_held=1;
    int sleep_wanted=1;
#if defined(CONFIG_WATCH_BOARD_143C)
    sleep_wanted=0; /* the audio board keeps the previous no-sleep power policy */
#endif
    esp_pm_config_t dfs={.max_freq_mhz=240,.min_freq_mhz=80,.light_sleep_enable=sleep_wanted!=0};
    e=esp_pm_configure(&dfs);
    if(e!=ESP_OK && sleep_wanted) { /* light sleep unsupported: keep DFS */
        printf("NOSPM light_sleep configure error=%d, retrying DFS only\n",(int)e);
        sleep_wanted=0;dfs.light_sleep_enable=false;
        e=esp_pm_configure(&dfs);
    }
    if(e!=ESP_OK) {
        fixed_fallback(e);
        esp_err_t r1=esp_pm_lock_release(s_display);
        esp_err_t r2=esp_pm_lock_release(s_ui);
        if(r1==ESP_OK) s_display_held=0;
        if(r2==ESP_OK) {s_ui_held=0;s_ui_releases++;}
        if(r1==ESP_OK && r2==ESP_OK) delete_locks();
        return;
    }
    __atomic_store_n(&s_light_sleep,sleep_wanted,__ATOMIC_RELEASE);
    if(sleep_wanted) register_sleep_callbacks();
    __atomic_store_n(&s_enabled,1,__ATOMIC_RELEASE);
    printf("NOSPM enabled=1 max_mhz=240 min_mhz=80 light_sleep=%d\n",sleep_wanted);
}
void nos_power_active_begin(void)
{
    if(!__atomic_load_n(&s_enabled,__ATOMIC_ACQUIRE) || s_ui_held) return;
    esp_err_t e=esp_pm_lock_acquire(s_ui);
    if(e!=ESP_OK) {fixed_fallback(e);return;}
    s_ui_held=1;s_ui_acquires++;
}
void nos_power_active_end(void)
{
    if(!s_ui_held) return;
    esp_err_t e=esp_pm_lock_release(s_ui);
    if(e!=ESP_OK) {fixed_fallback(e);return;}
    s_ui_held=0;s_ui_releases++;
}
void nos_power_bake_begin(void)
{
    if(!__atomic_load_n(&s_enabled,__ATOMIC_ACQUIRE) || __atomic_load_n(&s_bake_held,__ATOMIC_ACQUIRE)) return;
    esp_err_t e=esp_pm_lock_acquire(s_bake);
    if(e!=ESP_OK) {fixed_fallback(e);return;}
    __atomic_store_n(&s_bake_held,1,__ATOMIC_RELEASE);
    __atomic_add_fetch(&s_bake_acquires,1,__ATOMIC_RELAXED);
}
void nos_power_bake_end(void)
{
    if(!__atomic_load_n(&s_bake_held,__ATOMIC_ACQUIRE)) return;
    esp_err_t e=esp_pm_lock_release(s_bake);
    if(e!=ESP_OK) {fixed_fallback(e);return;}
    __atomic_store_n(&s_bake_held,0,__ATOMIC_RELEASE);
    __atomic_add_fetch(&s_bake_releases,1,__ATOMIC_RELAXED);
}
/* A single owner task per no-sleep lock: the held flag is only written by that task. */
static void no_sleep_hold(esp_pm_lock_handle_t lock,int *held,int on)
{
    if(!__atomic_load_n(&s_enabled,__ATOMIC_ACQUIRE) || !lock) return;
    if(on) {
        if(*held) return;
        esp_err_t e=esp_pm_lock_acquire(lock);
        if(e!=ESP_OK) {fixed_fallback(e);return;}
        __atomic_store_n(held,1,__ATOMIC_RELEASE);
    } else {
        if(!*held) return;
        esp_err_t e=esp_pm_lock_release(lock);
        if(e!=ESP_OK) {fixed_fallback(e);return;}
        __atomic_store_n(held,0,__ATOMIC_RELEASE);
    }
}
void nos_power_display_hold(int on) {no_sleep_hold(s_display,&s_display_held,on);}
void nos_power_xfer_hold(int on) {no_sleep_hold(s_xfer,&s_xfer_held,on);}
void nos_power_set_sleep_hooks(void (*before)(void),void (*after)(int64_t slept_us))
{
    s_hook_after=after;
    s_hook_before=before;
}
void nos_power_sleep_stats(uint32_t *sleep_ms,uint32_t *entries)
{
    if(sleep_ms) *sleep_ms=__atomic_load_n(&s_sleep_ms,__ATOMIC_RELAXED);
    if(entries) *entries=__atomic_load_n(&s_sleep_entries,__ATOMIC_RELAXED);
}
int nos_power_set_max_mhz(int mhz)
{
    if(mhz!=80 && mhz!=240)return -1;
    if(!__atomic_load_n(&s_enabled,__ATOMIC_ACQUIRE))return s_error?s_error:-1;
    /* Keep the init light-sleep flag: toggling it would free and re-allocate the
     * CPU retention buffer (internal RAM) on every OFF/ON transition. */
    const esp_pm_config_t cfg={.max_freq_mhz=mhz,.min_freq_mhz=80,.light_sleep_enable=__atomic_load_n(&s_light_sleep,__ATOMIC_ACQUIRE)!=0};
    esp_err_t e=esp_pm_configure(&cfg);
    if(e!=ESP_OK){fixed_fallback(e);return (int)e;}
    __atomic_store_n(&s_max_mhz,mhz,__ATOMIC_RELEASE);
    return 0;
}
nos_power_stats nos_power_get_stats(void)
{
    nos_power_stats st={0};
    st.enabled=__atomic_load_n(&s_enabled,__ATOMIC_ACQUIRE);
    st.error=__atomic_load_n(&s_error,__ATOMIC_ACQUIRE);
    st.cpu_mhz=esp_clk_cpu_freq()/1000000;st.max_mhz=__atomic_load_n(&s_max_mhz,__ATOMIC_ACQUIRE);st.min_mhz=st.enabled?80:240;
    st.ui_held=s_ui_held;st.ui_acquires=s_ui_acquires;st.ui_releases=s_ui_releases;
    st.bake_held=__atomic_load_n(&s_bake_held,__ATOMIC_ACQUIRE);
    st.bake_acquires=__atomic_load_n(&s_bake_acquires,__ATOMIC_RELAXED);
    st.bake_releases=__atomic_load_n(&s_bake_releases,__ATOMIC_RELAXED);
    st.light_sleep=__atomic_load_n(&s_light_sleep,__ATOMIC_ACQUIRE);
    st.display_held=__atomic_load_n(&s_display_held,__ATOMIC_ACQUIRE);
    st.xfer_held=__atomic_load_n(&s_xfer_held,__ATOMIC_ACQUIRE);
    nos_power_sleep_stats(&st.sleep_ms,&st.sleep_entries);
    return st;
}
void nos_power_report(void)
{
    nos_power_stats s=nos_power_get_stats();
    printf("NOSPM enabled=%d cpu_mhz=%d min_mhz=%d max_mhz=%d light_sleep=%d ui_held=%d bake_held=%d ui_acquires=%u ui_releases=%u bake_acquires=%u bake_releases=%u display_held=%d xfer_held=%d sleep_ms=%u sleep_entries=%u error=%d\n",
        s.enabled,s.cpu_mhz,s.min_mhz,s.max_mhz,s.light_sleep,s.ui_held,s.bake_held,s.ui_acquires,s.ui_releases,s.bake_acquires,s.bake_releases,s.display_held,s.xfer_held,(unsigned)s.sleep_ms,(unsigned)s.sleep_entries,s.error);
}
