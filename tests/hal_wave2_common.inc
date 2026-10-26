/* Shared deterministic SDK faults for the two actual-source test units. */
#include "hal_wave2_mock.h"
static int mock_voice_busy;
int voice_chat_active(void){return mock_voice_busy;}
int hal_display_mode(void){return 2;} /* nos_main.c: display ON */
static atomic_llong mock_us=1000000;
static int mock_alloc_fail, mock_alloc_count, mock_task_fail, mock_async_fail;
static int mock_complete_count,mock_reboots,mock_recv_stall,mock_recv_oversize;
static int mock_external_stack;
static ptrdiff_t mock_recv_stop=-1;
static size_t mock_recv_chunk=1500;
static void (*mock_recv_hook)(void),(*mock_reboot_hook)(void);
static void (*mock_delay_hook)(uint32_t);
static void (*mock_task_fn)(void*);
static void *mock_task_arg;
static struct { void *p;size_t n; } mock_allocs[32];
int64_t esp_timer_get_time(void) { return atomic_load(&mock_us); }
const char *esp_err_to_name(esp_err_t e) { return e==ESP_OK?"ok":"mock failure"; }
uint32_t esp_random(void) { static uint32_t seq=0x12345678;return ++seq; }
bool esp_ptr_internal(const void *p) {return p && !mock_external_stack;}
void *nos_mem_alloc(uint32_t n,const char *owner)
{
    mock_alloc_count++;
    if(mock_alloc_fail) {mock_alloc_fail--;return NULL;}
    unsigned char *p=malloc((size_t)n+16);assert(p);memset(p+n,0xA7,16);
    for(unsigned i=0;i<32;i++) if(!mock_allocs[i].p) {mock_allocs[i].p=p;mock_allocs[i].n=n;return p;}
    abort();
}
static void mock_canaries(void)
{
    for(unsigned i=0;i<32;i++) if(mock_allocs[i].p) {
        uint8_t *p=mock_allocs[i].p;
        for(unsigned j=0;j<16;j++) assert(p[mock_allocs[i].n+j]==0xA7);
    }
}
void nos_mem_free(void *p)
{
    if(!p) return;
    mock_canaries();
    for(unsigned i=0;i<32;i++) if(mock_allocs[i].p==p) {mock_allocs[i].p=NULL;free(p);return;}
    abort();
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)
{ assert(!pthread_mutex_init(&s->mutex,NULL));return s; }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{ StaticSemaphore_t *s=malloc(sizeof *s);assert(s);return xSemaphoreCreateMutexStatic(s); }
int xSemaphoreTake(SemaphoreHandle_t s,uint32_t ticks)
{ assert(s);return !(ticks?pthread_mutex_lock(&s->mutex):pthread_mutex_trylock(&s->mutex)); }
int xSemaphoreGive(SemaphoreHandle_t s) { assert(!pthread_mutex_unlock(&s->mutex));return pdTRUE; }
int xTaskCreate(void (*fn)(void*),const char *name,uint32_t size,void *arg,int p,TaskHandle_t *h)
{
    if(mock_task_fail) {mock_task_fail--;return 0;}
    assert(!mock_task_fn);mock_task_fn=fn;mock_task_arg=arg;if(h) *h=(void*)1;return pdPASS;
}
static void mock_run_task(void)
{ assert(mock_task_fn);void (*fn)(void*)=mock_task_fn;void *arg=mock_task_arg;mock_task_fn=NULL;mock_task_arg=NULL;fn(arg); }
void vTaskDelete(TaskHandle_t h) {}
void vTaskDelay(uint32_t ticks)
{ atomic_fetch_add(&mock_us,(int64_t)ticks*1000);if(mock_delay_hook) mock_delay_hook(ticks); }
void esp_restart(void) { mock_reboots++;if(mock_reboot_hook) mock_reboot_hook(); }
size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *name) { return r->token?strlen(r->token):0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *s,size_t n)
{ if(!r->token || strlen(r->token)>=n) return ESP_FAIL;strcpy(s,r->token);return ESP_OK; }
int httpd_req_recv(httpd_req_t *r,char *p,size_t n)
{
    r->recv_calls++;if(n>r->max_recv) r->max_recv=n;
    atomic_fetch_add(&mock_us,1000);
    if(mock_recv_hook) mock_recv_hook();
    if(mock_recv_stall) {atomic_fetch_add(&mock_us,1000000);return HTTPD_SOCK_ERR_TIMEOUT;}
    if(mock_recv_stop>=0 && r->used>=(size_t)mock_recv_stop) return 0;
    size_t take=n;if(take>mock_recv_chunk) take=mock_recv_chunk;
    if(take>r->content_len-r->used) take=r->content_len-r->used;
    if(r->body) memcpy(p,r->body+r->used,take);else memset(p,0x5C,take);
    r->used+=take;
    return mock_recv_oversize?(int)n+1:(int)take;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r,int e,const char *msg) { r->response=e;return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r,const char *s) { r->response=200;return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *s) { return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *s,const char *v) { return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r,const char *s,ptrdiff_t n) { r->response=200;return ESP_OK; }
esp_err_t httpd_req_async_handler_begin(httpd_req_t *r,httpd_req_t **a)
{ if(mock_async_fail) {mock_async_fail--;return ESP_FAIL;}*a=r;return ESP_OK; }
esp_err_t httpd_req_async_handler_complete(httpd_req_t *r) { mock_complete_count++;return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t h,const httpd_uri_t *u) { return ESP_OK; }
