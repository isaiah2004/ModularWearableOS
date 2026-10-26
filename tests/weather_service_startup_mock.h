#ifndef WEATHER_SERVICE_STARTUP_MOCK_H
#define WEATHER_SERVICE_STARTUP_MOCK_H
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void weather_test_enter(portMUX_TYPE *lock);
void weather_test_exit(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) weather_test_enter(lock)
#define portEXIT_CRITICAL(lock) weather_test_exit(lock)
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
typedef void (*TaskFunction_t)(void *);
int xTaskCreate(TaskFunction_t entry,const char *name,unsigned bytes,void *arg,int priority,void *handle);
void vTaskDelay(unsigned ticks);
static inline void sleep_grid_delay(unsigned ms) {vTaskDelay(ms);} /* firmware/main/sleep_grid.h, device build only */
int64_t esp_timer_get_time(void);
void weather_test_log(const char *level,const char *tag,const char *format,...);
#define ESP_LOGI(tag,...) weather_test_log("I",tag,__VA_ARGS__)
#define ESP_LOGW(tag,...) weather_test_log("W",tag,__VA_ARGS__)
#define ESP_LOGE(tag,...) weather_test_log("E",tag,__VA_ARGS__)
const char *esp_err_to_name(esp_err_t error);
typedef struct { int event_id,data_len;void *data,*user_data; } esp_http_client_event_t;
#define HTTP_EVENT_ON_DATA 1
typedef struct {
    const char *url;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void *);
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    int buffer_size,buffer_size_tx;
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
esp_err_t esp_crt_bundle_attach(void *config);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
#endif
