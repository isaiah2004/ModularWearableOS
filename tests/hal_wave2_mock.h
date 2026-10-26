/* Host-only ABI surface. Production HAL/OTA sources are compiled verbatim by
 * test_hal_wave2.py; only SDK/HW calls are mocked. No device/network access. */
#ifndef HAL_WAVE2_MOCK_H
#define HAL_WAVE2_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <pthread.h>
#include "hal.h"
#include "psram.h"
#ifdef _WIN32
#include <direct.h>
#define mkdir(path,mode) _mkdir(path)
static inline struct tm *hal_host_localtime_r(time_t t,struct tm *out)
{ *out=*localtime(&t);return out; }
#define localtime_r(epoch,out) hal_host_localtime_r((time_t)*(epoch),out)
#endif
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_WIFI_STATE 0x301
#define ESP_ERR_NVS_NO_FREE_PAGES 0x401
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x402
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS 0x501
typedef int esp_err_t;
const char *esp_err_to_name(esp_err_t e);
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(m) assert(pthread_mutex_lock(m)==0)
#define portEXIT_CRITICAL(m) assert(pthread_mutex_unlock(m)==0)
typedef struct { pthread_mutex_t mutex; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef uint32_t StackType_t;
typedef struct { char bytes[64]; } StaticTask_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define tskNO_AFFINITY -1
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s);
int xSemaphoreTake(SemaphoreHandle_t s,uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t s);
int xTaskCreate(void (*fn)(void*),const char *name,uint32_t size,void *arg,int priority,TaskHandle_t *handle);
int xTaskCreatePinnedToCore(void (*fn)(void*),const char *name,uint32_t size,void *arg,int priority,TaskHandle_t *handle,int core);
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 4096
#define CONFIG_BT_NIMBLE_PINNED_TO_CORE 0
#define configMAX_PRIORITIES 25
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void*),const char*,uint32_t,void*,int,StackType_t*,StaticTask_t*,int);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(uint32_t ticks);
uint32_t ulTaskNotifyTake(int clear,uint32_t ticks);
void xTaskNotifyGive(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
QueueHandle_t xQueueCreate(unsigned count,unsigned size);
int xQueueSend(QueueHandle_t q,const void *data,uint32_t ticks);
int xQueueSendToFront(QueueHandle_t q,const void *data,uint32_t ticks);
int xQueueReceive(QueueHandle_t q,void *data,uint32_t ticks);
int xQueueOverwrite(QueueHandle_t q,const void *data);
void vQueueDelete(QueueHandle_t q);
typedef struct mock_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void*);void *arg;const char *name; } esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
static inline uint32_t sleep_grid_now_ms(void) {return (uint32_t)(esp_timer_get_time()/1000);} /* sleep_grid.h: tick clock is device-only */
esp_err_t esp_timer_create(const esp_timer_create_args_t*,esp_timer_handle_t*);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t,uint64_t);
esp_err_t esp_timer_start_once(esp_timer_handle_t,uint64_t);
esp_err_t esp_timer_stop(esp_timer_handle_t);
uint32_t esp_random(void);
bool esp_ptr_internal(const void *p);
void esp_restart(void);
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
void *heap_caps_malloc(size_t n,unsigned caps);
void *heap_caps_calloc(size_t n,size_t s,unsigned caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 2
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
esp_err_t nvs_flash_init(void);
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;
typedef struct { int unit_id; } adc_oneshot_unit_init_cfg_t;
typedef struct { int atten,bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct { int unit_id,atten,bitwidth; } adc_cali_curve_fitting_config_t;
#define ADC_UNIT_1 1
#define ADC_ATTEN_DB_12 12
#define ADC_BITWIDTH_12 12
#define ADC_CHANNEL_3 3
#define ADC_CHANNEL_7 7
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t*,adc_oneshot_unit_handle_t*);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t,int,const adc_oneshot_chan_cfg_t*);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t,int,int*);
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t*,adc_cali_handle_t*);
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t,int,int*);
int usb_serial_jtag_is_connected(void);
int gpio_get_level(int pin);
int settimeofday(const struct timeval*,const void*);
int setenv(const char*,const char*,int);
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct {
    size_t content_len;int method;const uint8_t *body;size_t used;
    const char *token;int response,recv_calls;size_t max_recv;
} httpd_req_t;
typedef void *httpd_handle_t;
enum { HTTP_GET,HTTP_POST,HTTPD_400_BAD_REQUEST=400,HTTPD_403_FORBIDDEN=403,
       HTTPD_408_REQ_TIMEOUT=408,HTTPD_500_INTERNAL_SERVER_ERROR=500 };
#define HTTPD_RESP_USE_STRLEN -1
#define HTTPD_SOCK_ERR_TIMEOUT -3
typedef struct { const char *uri;int method;esp_err_t (*handler)(httpd_req_t*); } httpd_uri_t;
typedef struct { int recv_wait_timeout,max_uri_handlers,stack_size,task_caps,max_open_sockets,lru_purge_enable; } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
int httpd_req_recv(httpd_req_t*,char*,size_t);
size_t httpd_req_get_hdr_value_len(httpd_req_t*,const char*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*,const char*,char*,size_t);
esp_err_t httpd_resp_send_err(httpd_req_t*,int,const char*);
esp_err_t httpd_resp_sendstr(httpd_req_t*,const char*);
esp_err_t httpd_resp_send_chunk(httpd_req_t*,const char*,ptrdiff_t);
esp_err_t httpd_resp_set_type(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*);
esp_err_t httpd_resp_set_status(httpd_req_t*,const char*);
esp_err_t httpd_req_async_handler_begin(httpd_req_t*,httpd_req_t**);
esp_err_t httpd_req_async_handler_complete(httpd_req_t*);
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t*);
esp_err_t httpd_start(httpd_handle_t*,const httpd_config_t*);
typedef struct { uint8_t ssid[33];int8_t rssi;uint8_t primary; } wifi_ap_record_t;
typedef struct { int show_hidden,scan_type; } wifi_scan_config_t;
typedef struct { int placeholder; } wifi_init_config_t;
typedef struct { uint8_t ssid[32],password[64];struct { int authmode; } threshold; } mock_sta;
typedef struct { uint8_t ssid[32],password[64];unsigned ssid_len;int channel,max_connection,authmode; } mock_ap;
typedef union { mock_sta sta;mock_ap ap; } wifi_config_t;
typedef struct { char cc[3];int schan,nchan,policy; } wifi_country_t;
typedef struct { uint32_t filter_mask; } wifi_promiscuous_filter_t;
typedef int wifi_promiscuous_pkt_type_t;
typedef int wifi_second_chan_t;
typedef struct { struct { uint32_t sig_len;int8_t rssi; } rx_ctrl; } wifi_promiscuous_pkt_t;
typedef void (*wifi_promiscuous_cb_t)(void*,wifi_promiscuous_pkt_type_t);
typedef struct { unsigned reason; } wifi_event_sta_disconnected_t;
typedef struct { struct { uint32_t ip; } ip_info; } ip_event_got_ip_t;
typedef void *esp_netif_t;
typedef struct {int ap;} esp_netif_config_t;
typedef void *esp_event_handler_instance_t;
#define ESP_NETIF_DEFAULT_WIFI_AP() ((esp_netif_config_t){.ap=1})
#define ESP_NETIF_DEFAULT_WIFI_STA() ((esp_netif_config_t){.ap=0})
esp_netif_t *esp_netif_new(const esp_netif_config_t*);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t*);
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t*);
void esp_netif_destroy_default_wifi(void*);
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void);
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void);
typedef const void *esp_event_base_t;
extern const void *WIFI_EVENT,*IP_EVENT;
#define WIFI_EVENT_SCAN_DONE 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
#define ESP_EVENT_ANY_ID -1
#define WIFI_REASON_AUTH_FAIL 1
#define WIFI_REASON_HANDSHAKE_TIMEOUT 2
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 3
#define WIFI_REASON_NO_AP_FOUND 4
#define WIFI_SCAN_TYPE_ACTIVE 1
#define WIFI_MODE_STA 1
#define WIFI_MODE_APSTA 3
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_AUTH_WPA2_PSK 2
#define WIFI_AUTH_OPEN 0
#define WIFI_STORAGE_RAM 1
#define WIFI_COUNTRY_POLICY_MANUAL 1
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_PROMIS_FILTER_MASK_ALL 0xffff
#define WIFI_PKT_MGMT 1
#define WIFI_PKT_DATA 2
#define WIFI_PKT_CTRL 3
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define IPSTR "%u"
#define IP2STR(p) (*(p))
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t,int32_t,void (*)(void*,esp_event_base_t,int32_t,void*),void*,void*);
esp_err_t esp_wifi_init(const wifi_init_config_t*);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_get_mac(int,uint8_t*);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int,const wifi_config_t*);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_country(const wifi_country_t*);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t*);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t*,wifi_ap_record_t*);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t*,bool);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*);
esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t*);
esp_err_t esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t);
esp_err_t esp_wifi_set_promiscuous(bool);
esp_err_t esp_wifi_get_channel(uint8_t*,wifi_second_chan_t*);
esp_err_t esp_wifi_set_channel(uint8_t,wifi_second_chan_t);
#define ESP_SNTP_OPMODE_POLL 1
int esp_sntp_enabled(void);
void esp_sntp_restart(void);
void esp_sntp_setoperatingmode(int);
void esp_sntp_setservername(int,const char*);
void esp_sntp_set_time_sync_notification_cb(void (*)(struct timeval*));
void sntp_set_time_sync_notification_cb(void (*)(struct timeval*));
void esp_sntp_init(void);
typedef struct cJSON { char *valuestring;double valuedouble;int valueint; } cJSON;
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_AddArrayToObject(cJSON*,const char*);
void cJSON_AddBoolToObject(cJSON*,const char*,int);
void cJSON_AddNumberToObject(cJSON*,const char*,double);
void cJSON_AddStringToObject(cJSON*,const char*,const char*);
void cJSON_AddItemToArray(cJSON*,cJSON*);
char *cJSON_PrintUnformatted(const cJSON*);
void cJSON_Delete(cJSON*);
cJSON *cJSON_ParseWithLengthOpts(const char*,size_t,const char**,int);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON*,const char*);
int cJSON_IsString(const cJSON*);
int cJSON_IsNumber(const cJSON*);
int cJSON_IsBool(const cJSON*);
int cJSON_IsTrue(const cJSON*);
double cJSON_GetNumberValue(const cJSON*);
typedef struct { int type; } ble_uuid_t;
typedef struct { ble_uuid_t u;uint8_t value[16]; } ble_uuid128_t;
#define BLE_UUID128_INIT(...) { {128}, {__VA_ARGS__} }
typedef struct { uint8_t type,val[6]; } ble_addr_t;
struct os_mbuf { uint8_t data[540];uint16_t size; };
struct ble_gatt_access_ctxt { int op;struct os_mbuf *om; };
struct ble_gatt_chr_def { const ble_uuid_t *uuid;int (*access_cb)(uint16_t,uint16_t,struct ble_gatt_access_ctxt*,void*);unsigned flags;uint16_t *val_handle; };
struct ble_gatt_svc_def { int type;const ble_uuid_t *uuid;const struct ble_gatt_chr_def *characteristics; };
struct ble_hs_adv_fields { unsigned flags;uint8_t *name,name_len;int name_is_complete;const ble_uuid128_t *uuids128;int num_uuids128,uuids128_is_complete; };
struct ble_gap_disc_params { int passive,filter_duplicates,itvl,window; };
struct ble_gap_adv_params { int conn_mode,disc_mode,itvl_min,itvl_max; };
struct ble_gap_upd_params { int itvl_min,itvl_max,latency,supervision_timeout,min_ce_len,max_ce_len; };
struct ble_gap_conn_desc { uint16_t conn_handle;struct {int bonded,encrypted,authenticated;} sec_state;int conn_itvl,conn_latency;ble_addr_t peer_id_addr; };
struct ble_gap_disc_desc { ble_addr_t addr;uint8_t *data,length_data;int8_t rssi; };
struct ble_gap_event { int type;union {
    struct ble_gap_disc_desc disc;
    struct { int status;uint16_t conn_handle; } connect,enc_change,conn_update;
    struct { int reason;struct ble_gap_conn_desc conn; } disconnect;
    struct { uint16_t conn_handle; } repeat_pairing;
    struct { struct {int action;} params; } passkey;
    struct { uint16_t value; } mtu;
    struct { uint16_t attr_handle;int cur_notify; } subscribe;
}; };
struct ble_npl_event { void (*fn)(struct ble_npl_event*);void *arg;int queued; };
struct ble_npl_eventq { int unused; };
struct ble_npl_callout { struct ble_npl_event event; };
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 2
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_FOREVER -1
#define BLE_HS_ENOENT 2
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 3
#define BLE_HS_EALREADY 4
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_OWN_ADDR_RANDOM 1
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 1
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_GATT_CHR_F_READ 1
#define BLE_GATT_CHR_F_WRITE 2
#define BLE_GATT_CHR_F_WRITE_NO_RSP 4
#define BLE_GATT_CHR_F_NOTIFY 8
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_INVALID_OFFSET 7
#define BLE_ATT_ERR_VALUE_NOT_ALLOWED 19
#define BLE_ATT_ERR_READ_NOT_PERMITTED 2
enum { BLE_GAP_EVENT_DISC=1,BLE_GAP_EVENT_DISC_COMPLETE,BLE_GAP_EVENT_CONNECT,
 BLE_GAP_EVENT_DISCONNECT,BLE_GAP_EVENT_ENC_CHANGE,BLE_GAP_EVENT_REPEAT_PAIRING,
 BLE_GAP_EVENT_PASSKEY_ACTION,BLE_GAP_EVENT_CONN_UPDATE,BLE_GAP_EVENT_MTU,
 BLE_GAP_EVENT_SUBSCRIBE,BLE_GAP_EVENT_ADV_COMPLETE };
extern struct mock_ble_hs_cfg { int sm_bonding,sm_sc,sm_io_cap;void (*sync_cb)(void);void (*reset_cb)(int); } ble_hs_cfg;
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void);
void ble_npl_event_init(struct ble_npl_event*,void (*)(struct ble_npl_event*),void*);
void ble_npl_eventq_put(struct ble_npl_eventq*,struct ble_npl_event*);
int ble_npl_callout_init(struct ble_npl_callout*,struct ble_npl_eventq*,void (*)(struct ble_npl_event*),void*);
int ble_npl_callout_reset(struct ble_npl_callout*,uint32_t);
uint32_t ble_npl_time_ms_to_ticks32(uint32_t);
esp_err_t nimble_port_init(void);
esp_err_t nimble_port_deinit(void);
void nimble_port_run(void);
void nimble_port_freertos_init(void (*)(void*));
void nimble_port_freertos_deinit(void);
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields*,const uint8_t*,uint8_t);
int ble_gap_disc_active(void);
int ble_gap_disc(uint8_t,int32_t,const struct ble_gap_disc_params*,int (*)(struct ble_gap_event*,void*),void*);
int ble_gap_disc_cancel(void);
int ble_gap_adv_active(void);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields*);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields*);
int ble_gap_adv_start(uint8_t,const ble_addr_t*,int32_t,const struct ble_gap_adv_params*,int (*)(struct ble_gap_event*,void*),void*);
int ble_gap_adv_stop(void);
int ble_gap_conn_active(void);
int ble_gap_connect(uint8_t,const ble_addr_t*,int32_t,const void*,int (*)(struct ble_gap_event*,void*),void*);
int ble_gap_conn_cancel(void);
int ble_gap_terminate(uint16_t,uint8_t);
int ble_gap_conn_find(uint16_t,struct ble_gap_conn_desc*);
int ble_gap_update_params(uint16_t,const struct ble_gap_upd_params*);
int ble_gap_unpair(const ble_addr_t*);
int ble_hs_id_set_rnd(const uint8_t*);
int ble_hs_util_ensure_addr(int);
int ble_hs_id_infer_auto(int,uint8_t*);
int ble_hs_id_copy_addr(uint8_t,uint8_t*,int*);
int ble_hs_mbuf_to_flat(const struct os_mbuf*,void*,uint16_t,uint16_t*);
struct os_mbuf *ble_hs_mbuf_from_flat(const void*,uint16_t);
int ble_gatts_notify_custom(uint16_t,uint16_t,struct os_mbuf*);
int os_mbuf_append(struct os_mbuf*,const void*,uint16_t);
int os_mbuf_copydata(const struct os_mbuf*,int,int,void*);
#define OS_MBUF_PKTLEN(m) ((m)->size)
void ble_svc_gap_init(void);
void ble_svc_gatt_init(void);
int ble_svc_gap_device_name_set(const char*);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def*);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def*);
typedef unsigned esp_ota_handle_t;
typedef struct { const char *label;size_t size;uint32_t address; } esp_partition_t;
typedef struct { char version[32],date[16],time[16]; } esp_app_desc_t;
typedef int esp_ota_img_states_t;
#define ESP_OTA_IMG_PENDING_VERIFY 1
#define OTA_WITH_SEQUENTIAL_WRITES 0xffff
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t*);
esp_err_t esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t*);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_get_partition_description(const esp_partition_t*,esp_app_desc_t*);
esp_err_t esp_ota_begin(const esp_partition_t*,size_t,esp_ota_handle_t*);
esp_err_t esp_ota_write(esp_ota_handle_t,const void*,size_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t*);
typedef void *esp_https_ota_handle_t;
typedef struct { const char *url;esp_err_t (*crt_bundle_attach)(void*);int timeout_ms,keep_alive_enable,buffer_size; } esp_http_client_config_t;
typedef struct { const esp_http_client_config_t *http_config; } esp_https_ota_config_t;
esp_err_t esp_crt_bundle_attach(void*);
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t*,esp_https_ota_handle_t*);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t);
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t);
#endif
