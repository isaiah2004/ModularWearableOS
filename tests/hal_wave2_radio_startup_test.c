/* Reuse the complete existing HAL SDK surface; startup faults below exercise
 * production functions, not a second initialization model. */
#define main existing_radio_main
#include "hal_wave2_radio_cover_test.c"
#undef main

static const char *startup_fault;
static unsigned netif_news,netif_destroys,sta_attaches,ap_attaches,sta_handlers,ap_handlers;
static unsigned wifi_driver_inits,wifi_event_regs,ip_event_regs,wifi_starts,weather_starts,ble_inits,ble_deinits,host_tasks,host_notifies;
static unsigned retained_driver,retained_ble;
static size_t internal_free=100000,internal_largest=64000,external_free=1000000;
static int preferences_loading,updater_busy;
static int weather_ready;
static int fault(const char *step) {if(startup_fault && !strcmp(step,startup_fault)){startup_fault=NULL;return 1;}return 0;}
size_t heap_caps_get_free_size(unsigned caps) {return caps&MALLOC_CAP_INTERNAL?internal_free:external_free;}
size_t heap_caps_get_largest_free_block(unsigned caps) {return caps&MALLOC_CAP_INTERNAL?internal_largest:external_free;}
void *heap_caps_calloc(size_t n,size_t s,unsigned caps){return nos_mem_alloc((uint32_t)(n*s),"radio-test");}
void *heap_caps_malloc(size_t n,unsigned caps){return nos_mem_alloc((uint32_t)n,"radio-test");}
void heap_caps_free(void *p){nos_mem_free(p);}
int xTaskCreatePinnedToCore(void (*fn)(void*),const char *name,uint32_t bytes,void *arg,int priority,TaskHandle_t *handle,int core)
{assert(fn==ble_host_task && bytes==4096 && core==0);if(fault("host-task"))return 0;host_tasks++;*handle=(void*)2;return pdPASS;}
uint32_t ulTaskNotifyTake(int clear,uint32_t ticks){return 0;}
void xTaskNotifyGive(TaskHandle_t task){assert(task);host_notifies++;}
QueueHandle_t xQueueCreate(unsigned n,unsigned bytes){if(fault("queue"))return NULL;assert(n==1 && bytes==sizeof(wifi_cred_t));return (void*)1;}
int xQueueReceive(QueueHandle_t q,void *p,uint32_t t){return pdFALSE;}
int xQueueSendToFront(QueueHandle_t q,const void *p,uint32_t t){return pdTRUE;}
void vQueueDelete(QueueHandle_t q){}
esp_err_t esp_netif_init(void){return fault("netif")?ESP_FAIL:ESP_OK;}
esp_err_t esp_event_loop_create_default(void){return fault("loop")?ESP_FAIL:ESP_OK;}
esp_netif_t *esp_netif_new(const esp_netif_config_t *cfg)
{if(fault(cfg->ap?"ap-new":"sta-new"))return NULL;netif_news++;return (esp_netif_t*)(uintptr_t)(cfg->ap?4:3);}
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *p){sta_attaches++;return fault("attach")?ESP_FAIL:ESP_OK;}
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *p){ap_attaches++;return fault("ap-attach")?ESP_FAIL:ESP_OK;}
void esp_netif_destroy_default_wifi(void *p){assert(p);netif_destroys++;}
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void){sta_handlers++;return fault("sta-handler")?ESP_FAIL:ESP_OK;}
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void){ap_handlers++;return fault("ap-handler")?ESP_FAIL:ESP_OK;}
esp_err_t esp_event_handler_instance_register(esp_event_base_t b,int32_t id,void (*fn)(void*,esp_event_base_t,int32_t,void*),void *arg,void *out)
{if(b==WIFI_EVENT){if(fault("wifi-event"))return ESP_FAIL;wifi_event_regs++;}else{if(fault("ip-event"))return ESP_FAIL;ip_event_regs++;}*(void**)out=(void*)5;return ESP_OK;}
const void *WIFI_EVENT=(void*)1,*IP_EVENT=(void*)2;
esp_err_t esp_wifi_init(const wifi_init_config_t *cfg){wifi_driver_inits++;if(fault("driver"))return ESP_ERR_NO_MEM;assert(!retained_driver);retained_driver=1;return ESP_OK;}
esp_err_t esp_wifi_set_storage(int mode){return fault("storage")?ESP_FAIL:ESP_OK;}
esp_err_t esp_wifi_set_country(const wifi_country_t *c){return fault("country")?ESP_FAIL:ESP_OK;}
esp_err_t esp_wifi_set_mode(int mode){return fault("mode")?ESP_FAIL:ESP_OK;}
esp_err_t esp_wifi_start(void){wifi_starts++;return fault("start")?ESP_ERR_NO_MEM:ESP_OK;}
esp_err_t esp_wifi_stop(void){return ESP_OK;}
esp_err_t esp_wifi_get_mac(int iface,uint8_t *m){memset(m,0,6);return ESP_OK;}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out){return ESP_FAIL;}
int weather_service_start(void){weather_starts++;if(fault("weather"))return 0;weather_ready=1;return 1;}
int weather_service_ready(void){return weather_ready;}
esp_err_t nimble_port_init(void){ble_inits++;if(fault("nimble"))return ESP_ERR_NO_MEM;assert(!retained_ble);retained_ble=1;return ESP_OK;}
esp_err_t nimble_port_deinit(void){ble_deinits++;if(fault("ble-cleanup"))return ESP_FAIL;retained_ble=0;return ESP_OK;}
void nimble_port_run(void){}
void ble_svc_gap_init(void){}
void ble_svc_gatt_init(void){}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *p){return fault("gatt-count")?BLE_HS_ENOMEM:0;}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *p){return fault("gatt-add")?BLE_HS_ENOMEM:0;}
int ble_svc_gap_device_name_set(const char *s){return fault("name")?BLE_HS_ENOMEM:0;}
void ble_store_config_init(void){}
int prefs_loading(void){return preferences_loading;}
int prefs_pending(void){return 0;}
void prefs_save(void){}
nos_prefs_t nos_prefs;
void hal_storage_get(hal_storage *s){memset(s,0,sizeof *s);}
int ota_busy(void){return updater_busy;}
int ota_external_activity_begin(void){return 1;}
void ota_external_activity_end(void){}
void ota_session_begin(void){}
void ota_session_end(void){}
int board_sd_asset_acquire(void){return 0;}
void board_sd_asset_release(void){}
void board_sd_asset_lock(void){}
void board_sd_asset_unlock(void){}
prefs_disk_read_status prefs_disk_load_status(const char *dir,void *p,size_t n){return PREFS_DISK_MISSING;}
int prefs_disk_save(const char *dir,const void *p,size_t n){return 0;}
int prefs_disk_write_allowed(const char *dir){return 1;}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *out){return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n){return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n){return ESP_FAIL;}
esp_err_t nvs_commit(nvs_handle_t h){return ESP_FAIL;}
void nvs_close(nvs_handle_t h){}
void board_clock_changed(time_t epoch){}
int esp_sntp_enabled(void){return 0;}
void esp_sntp_setoperatingmode(int mode){}
void esp_sntp_setservername(int n,const char *name){}
void sntp_set_time_sync_notification_cb(void (*fn)(struct timeval*)){}
void esp_sntp_init(void){}
void hal_weather_refresh(void){}
esp_err_t httpd_start(httpd_handle_t *h,const httpd_config_t *cfg){*h=(void*)1;return ESP_OK;}
void ota_register_web(httpd_handle_t h){}
static void later(void){atomic_fetch_add(&mock_us,5000001);mock_task_fn=NULL;}
int main(int argc,char **argv)
{
    const char *which=argc>1?argv[1]:"off";
    assert(!s_ble_enabled && !s_wifi_enabled);
    if(!strcmp(which,"off")) {
        hal_ble_set_enabled(0);hal_wifi_set_enabled(0);hal_esp32_start_radios();hal_setup_poll();
        assert(!netif_news && !wifi_driver_inits && !ble_inits && !host_tasks && !mock_task_fn && !weather_starts && !hal_esp32_radios_pending());
        char text[96];hal_ble_device_status(text,sizeof text);assert(strstr(text,"off") && !strstr(text,"retained"));
    } else if(!strcmp(which,"guard")) {
        hal_wifi_set_enabled(1);hal_ble_set_enabled(1);assert(!netif_news && !ble_inits);
        internal_free=1000;internal_largest=500;hal_esp32_start_radios();
        assert(!netif_news && !ble_inits && s_wifi_startup.deferred && s_ble_startup.deferred && hal_esp32_radios_pending());
        unsigned skipped=s_wifi_startup.deferred;radio_poll();assert(s_wifi_startup.deferred==skipped);
        hal_wifi_set_enabled(0);hal_ble_set_enabled(0);assert(!hal_esp32_radios_pending());
    } else if(!strcmp(which,"gates")) {
        hal_wifi_set_enabled(1);preferences_loading=1;hal_esp32_start_radios();assert(!netif_news);
        preferences_loading=0;updater_busy=1;radio_poll();assert(!netif_news);
        updater_busy=0;radio_poll();assert(s_wifi_initialized && !hal_esp32_radios_pending());
    } else if(!strcmp(which,"later")) {
        hal_esp32_start_radios();assert(!netif_news && !ble_inits);
        hal_wifi_set_enabled(1);assert(!netif_news);hal_setup_poll();assert(s_wifi_initialized && weather_starts==1);
        mock_task_fn=NULL;hal_ble_set_enabled(1);assert(!ble_inits);hal_setup_poll();assert(s_ble_initialized && !s_ble_ready);
        ble_on_sync();drain();assert(!hal_esp32_radios_pending());
    } else if(!strcmp(which,"start-again")) {
        hal_wifi_set_enabled(1);hal_esp32_start_radios();assert(s_wifi_initialized);
        hal_wifi_set_enabled(0);startup_fault="start";hal_wifi_set_enabled(1);
        assert(s_wifi_start_pending && hal_esp32_radios_pending());later();radio_poll();
        assert(!s_wifi_start_pending && !hal_esp32_radios_pending() && wifi_driver_inits==1);
    } else if(!strcmp(which,"weather")) {
        startup_fault="weather";hal_wifi_set_enabled(1);hal_esp32_start_radios();
        assert(s_wifi_initialized && !weather_ready && weather_starts==1 &&
               s_weather_startup.failures==1 && hal_esp32_radios_pending());
        hal_setup_poll();assert(weather_starts==1);
        internal_free=1;internal_largest=1;later();hal_setup_poll();
        assert(weather_starts==1 && s_weather_startup.deferred==1 && hal_esp32_radios_pending());
        hal_wifi_set_enabled(0);later();hal_setup_poll();
        assert(weather_starts==1 && !hal_esp32_radios_pending());
        internal_free=100000;internal_largest=64000;hal_wifi_set_enabled(1);later();hal_setup_poll();
        assert(weather_ready && weather_starts==2 && !hal_esp32_radios_pending() &&
               !s_weather_startup.stage && !s_weather_startup.error && wifi_driver_inits==1);
        hal_setup_poll();assert(weather_starts==2);
    } else if(!strcmp(which,"ap-only")) {
        hal_esp32_start_radios();assert(!s_wifi_enabled && !hal_esp32_radios_pending());
        assert(!hal_setup_set(1) && s_wifi_on_demand && hal_esp32_radios_pending());
        hal_setup_poll();assert(s_wifi_initialized && s_wifi_worker_ready && !s_wifi_enabled &&
                                !weather_starts && !weather_ready && !hal_esp32_radios_pending());
        /* A latched AP-only intent requires its radio worker, never Weather. */
        s_wifi_on_demand=1;assert(!hal_esp32_radios_pending());
        s_wifi_worker_ready=0;assert(hal_esp32_radios_pending());
        s_wifi_worker_ready=1;s_wifi_start_pending=1;assert(hal_esp32_radios_pending());
        s_wifi_start_pending=0;s_wifi_on_demand=0;
        assert(hal_setup_set(1) && s_setup_active && !hal_esp32_radios_pending());
        hal_setup_poll();assert(!weather_starts && wifi_driver_inits==1);
        assert(hal_setup_set(0) && !s_setup_active && !hal_esp32_radios_pending());
    } else if(!strcmp(which,"ap")) {
        hal_wifi_set_enabled(1);hal_esp32_start_radios();startup_fault="ap-new";
        assert(!hal_setup_set(1) && !s_ap_netif);startup_fault="ap-attach";
        assert(!hal_setup_set(1) && !s_ap_netif);startup_fault="ap-handler";
        assert(!hal_setup_set(1) && !s_ap_netif);assert(hal_setup_set(1) && s_ap_netif && s_setup_active);
        unsigned made=netif_news;assert(hal_setup_set(0));assert(hal_setup_set(1) && netif_news==made && wifi_driver_inits==1);
    } else if(!strcmp(which,"ble-cleanup")) {
        startup_fault="gatt-add";hal_ble_set_enabled(1);hal_esp32_start_radios();assert(s_ble_cleanup_pending && retained_ble);
        hal_ble_set_enabled(0);internal_free=1;internal_largest=1;startup_fault="ble-cleanup";later();radio_poll();
        assert(s_ble_cleanup_pending && hal_esp32_radios_pending());later();radio_poll();radio_poll();
        assert(!s_ble_cleanup_pending && !retained_ble && !s_ble_host_task && !hal_esp32_radios_pending());
    } else if(!strncmp(which,"wifi-",5)) {
        startup_fault=which+5;if(!strcmp(startup_fault,"task")){startup_fault=NULL;mock_task_fail=1;}
        hal_wifi_set_enabled(1);hal_esp32_start_radios();
        assert(!s_wifi_initialized && s_wifi_startup.failures==1 && hal_esp32_radios_pending());
        unsigned attempts=s_wifi_startup.attempts;radio_poll();assert(s_wifi_startup.attempts==attempts);
        later();radio_poll();assert(s_wifi_initialized && s_wifi_worker_ready && !hal_esp32_radios_pending());
        assert(wifi_event_regs==1 && ip_event_regs==1 && netif_news-netif_destroys==1 && weather_starts==1);
        unsigned inits=wifi_driver_inits;later();radio_poll();hal_wifi_set_enabled(0);hal_wifi_set_enabled(1);radio_poll();
        assert(wifi_driver_inits==inits && !hal_esp32_radios_pending());
    } else if(!strncmp(which,"ble-",4)) {
        if(!strcmp(which+4,"callout"))mock_callout_init_fail=1;else startup_fault=which+4;
        hal_ble_set_enabled(1);hal_esp32_start_radios();
        assert(!s_ble_initialized && s_ble_startup.failures==1 && hal_esp32_radios_pending() && !host_notifies);
        unsigned inits=ble_inits;radio_poll();assert(ble_inits==inits);
        later();radio_poll();if(!s_ble_initialized){later();radio_poll();}
        assert(s_ble_initialized && host_tasks==1 && host_notifies==1 && hal_esp32_radios_pending());
        ble_on_sync();drain();assert(!hal_esp32_radios_pending());
        inits=ble_inits;hal_ble_set_enabled(0);drain();hal_ble_set_enabled(1);drain();radio_poll();assert(ble_inits==inits);
    } else return 2;
    mock_canaries();hal_esp32_radio_report();printf("PASS lazy checked radio startup %s\n",which);return 0;
}
