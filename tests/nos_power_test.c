#include "nos_power.h"
#include "esp_pm.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct mock_pm_lock {int held,deleted,type;};
#define NLOCKS 4 /* ui, prewarm (CPU_FREQ_MAX), display, xfer (NO_LIGHT_SLEEP) */
static struct mock_pm_lock locks[NLOCKS];
static esp_pm_config_t config={240,240,false};
static int configure_calls,create_calls,acquire_calls,release_calls;
static int fail_config,fail_create,fail_acquire,fail_release,fail_dfs,fail_sleep_cfg;
static esp_pm_sleep_cbs_register_config_t registered;
static int register_calls,before_calls,after_calls;
static int64_t last_after;
int esp_clk_cpu_freq(void) {
 return (locks[0].held || locks[1].held?config.max_freq_mhz:config.min_freq_mhz)*1000000;
}
/* IDF enters light sleep only when the light-sleep configuration is on and no lock is held. */
static int sleep_possible(void) {
 if(!config.light_sleep_enable) return 0;
 for(int i=0;i<NLOCKS;i++) if(locks[i].held) return 0;
 return 1;
}
esp_err_t esp_pm_configure(const void *p) {
 if(++configure_calls==fail_config) return ESP_FAIL;
 const esp_pm_config_t *c=p;
 assert((c->max_freq_mhz==240 || c->max_freq_mhz==80) && (c->min_freq_mhz==80 || c->min_freq_mhz==240));
 assert(!c->light_sleep_enable || c->min_freq_mhz==80);
 if(fail_dfs && c->min_freq_mhz==80) return ESP_FAIL;
 if(fail_sleep_cfg && c->light_sleep_enable) return ESP_ERR_NOT_SUPPORTED;
 config=*c;return ESP_OK;
}
esp_err_t esp_pm_lock_create(int type,int arg,const char *name,esp_pm_lock_handle_t *out) {
 assert(arg==0 && name && create_calls<NLOCKS);
 assert(type==(create_calls<2?ESP_PM_CPU_FREQ_MAX:ESP_PM_NO_LIGHT_SLEEP));
 if(++create_calls==fail_create) return ESP_ERR_NO_MEM;
 locks[create_calls-1].type=type;
 *out=&locks[create_calls-1];return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock) {
 assert(lock && !lock->deleted);
 if(++acquire_calls==fail_acquire) return ESP_FAIL;
 lock->held++;assert(lock->held==1);return ESP_OK;
}
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock) {
 assert(lock && lock->held==1 && !lock->deleted);
 if(++release_calls==fail_release) return ESP_FAIL;
 lock->held--;return ESP_OK;
}
esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t lock) {
 assert(lock && !lock->held);lock->deleted=1;return ESP_OK;
}
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *cbs) {
 assert(cbs && cbs->enter_cb && cbs->exit_cb);
 registered=*cbs;register_calls++;return ESP_OK;
}
static void hook_before(void) {before_calls++;}
static void hook_after(int64_t slept_us) {after_calls++;last_after=slept_us;}
static int any_lock_held(void) {for(int i=0;i<NLOCKS;i++) if(locks[i].held) return 1;return 0;}
int main(int argc,char **argv) {
 const char *mode=argc>1?argv[1]:"normal";
 if(!strcmp(mode,"config-fail")) fail_config=1;
 if(!strcmp(mode,"dfs-fail")) fail_dfs=1;        /* light sleep and plain DFS both refused */
 if(!strcmp(mode,"create-fail")) fail_create=1;
 if(!strcmp(mode,"second-create-fail")) fail_create=2;
 if(!strcmp(mode,"third-create-fail")) fail_create=3;
 if(!strcmp(mode,"fourth-create-fail")) fail_create=4;
 if(!strcmp(mode,"acquire-fail")) fail_acquire=1;
 if(!strcmp(mode,"display-acquire-fail")) fail_acquire=2;
 if(!strcmp(mode,"sleep-fail")) fail_sleep_cfg=1; /* only the light-sleep configuration fails */
 nos_power_init();
 nos_power_stats s=nos_power_get_stats();
 if(strcmp(mode,"normal") && strcmp(mode,"sleep-fail") && strncmp(mode,"runtime-",8)) {
  assert(!s.enabled && s.error && s.min_mhz==240 && s.cpu_mhz==240 && !s.light_sleep);
  assert(!config.light_sleep_enable);
  nos_power_active_begin();nos_power_active_end();
  nos_power_display_hold(1);nos_power_display_hold(0);nos_power_xfer_hold(1);nos_power_xfer_hold(0);
  assert(!locks[0].held && !locks[1].held && !locks[3].held);
  puts("PM initialization failure retains fixed240");return 0;
 }
 assert(s.enabled && s.ui_held && s.cpu_mhz==240 && s.display_held && !s.xfer_held);
 if(!strcmp(mode,"sleep-fail")) {
  /* Light sleep refused: DFS stays enabled, sleep reports off, no callbacks, runtime keeps off. */
  assert(!s.light_sleep && !config.light_sleep_enable && !register_calls);
  nos_power_display_hold(0);nos_power_active_end();assert(!sleep_possible());
  assert(nos_power_set_max_mhz(80)==0 && !config.light_sleep_enable);
  puts("light sleep refused: DFS-only retained");return 0;
 }
 assert(s.light_sleep && config.light_sleep_enable && config.min_freq_mhz==80 && register_calls==1);
 assert(!sleep_possible());
 int acq=acquire_calls;nos_power_active_begin();assert(acquire_calls==acq);
 nos_power_active_end();assert(esp_clk_cpu_freq()==80000000);
 if(!strcmp(mode,"runtime-acquire-fail")) {
  fail_acquire=acquire_calls+1;nos_power_active_begin();
  s=nos_power_get_stats();assert(!s.enabled && s.cpu_mhz==240 && !s.ui_held && !s.light_sleep);return 0;
 }
 if(!strcmp(mode,"runtime-release-fail")) {
  nos_power_active_begin();fail_release=release_calls+1;nos_power_active_end();
  s=nos_power_get_stats();assert(!s.enabled && s.cpu_mhz==240 && s.ui_held);
  nos_power_active_end();assert(!locks[0].held);return 0;
 }
 /* Display lock: idempotent, and the only thing standing between an idle watch and sleep. */
 acq=acquire_calls;nos_power_display_hold(1);assert(acquire_calls==acq && !sleep_possible());
 nos_power_display_hold(0);assert(!nos_power_get_stats().display_held && sleep_possible());
 nos_power_display_hold(0);assert(sleep_possible());
 nos_power_xfer_hold(1);assert(nos_power_get_stats().xfer_held && !sleep_possible());
 nos_power_xfer_hold(1);nos_power_xfer_hold(0);assert(sleep_possible() && !nos_power_get_stats().xfer_held);
 /* A UI scope also blocks sleep; the frame pacing loop must not sleep while ON. */
 nos_power_active_begin();assert(!sleep_possible());nos_power_active_end();assert(sleep_possible());
 nos_power_display_hold(1);nos_power_active_begin();nos_power_active_end();assert(!sleep_possible());
 nos_power_display_hold(0);
 /* Mode changes keep the light-sleep flag, so no retention buffer is freed and re-allocated. */
 assert(nos_power_set_max_mhz(80)==0 && config.light_sleep_enable && sleep_possible());
 assert(nos_power_set_max_mhz(240)==0 && config.light_sleep_enable);
 assert(nos_power_set_max_mhz(80)==0);
 /* Sleep accounting: only exits that really slept (>=500 us) count. */
 nos_power_set_sleep_hooks(hook_before,hook_after);
 registered.enter_cb(20000,NULL);assert(before_calls==1);
 registered.exit_cb(100,NULL);assert(after_calls==1 && last_after==100);
 uint32_t ms,entries;nos_power_sleep_stats(&ms,&entries);assert(ms==0 && entries==0);
 registered.exit_cb(20000,NULL);registered.exit_cb(700,NULL);registered.exit_cb(800,NULL);
 nos_power_sleep_stats(&ms,&entries);assert(ms==21 && entries==3 && after_calls==4);
 s=nos_power_get_stats();assert(s.sleep_ms==21 && s.sleep_entries==3);
 /* Sensor ceiling applies even with both owners locked, and wake restores 240. */
 assert(nos_power_set_max_mhz(80)==0);
 nos_power_active_begin();nos_power_bake_begin();
 assert(esp_clk_cpu_freq()==80000000 && nos_power_get_stats().max_mhz==80);
 assert(nos_power_set_max_mhz(240)==0 && esp_clk_cpu_freq()==240000000);
 nos_power_active_end();nos_power_bake_end();
 assert(nos_power_set_max_mhz(120)!=0);
 /* Independent prewarmer keeps both cores protected when UI blocks. */
 nos_power_bake_begin();assert(esp_clk_cpu_freq()==240000000);
 nos_power_active_begin();nos_power_active_end();assert(esp_clk_cpu_freq()==240000000);
 nos_power_bake_end();assert(esp_clk_cpu_freq()==80000000);
 /* ON render, OFF continue, stable AOD poll and AOD paint share one balanced scope. */
 for(int state=0;state<4;state++) for(int frame=0;frame<50;frame++) {
  nos_power_active_begin();assert(esp_clk_cpu_freq()==240000000);
  if(state==1 || (state==2 && frame%50)) {nos_power_active_end();continue;}
  /* Tick/raster/prepaint/core1 rotation and final DMA fence are all protected. */
  assert(nos_power_get_stats().ui_held);
  nos_power_active_end();assert(esp_clk_cpu_freq()==80000000);
 }
 s=nos_power_get_stats();
 assert(!s.ui_held && !s.bake_held && s.ui_acquires==s.ui_releases && s.bake_acquires==s.bake_releases);
 assert(!any_lock_held() && sleep_possible());
 nos_power_report();puts("PM lock scopes, idempotence, independent prewarm, idle80 and light sleep gating passed");return 0;
}
