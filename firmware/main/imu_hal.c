/* IMU platform workers: sampling never waits for SD or the display. */
#include "imu_backend.h"
#include "board.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <sys/time.h>
#include "sleep_grid.h"
static TaskHandle_t sampler, writer_task;
static SemaphoreHandle_t sample_lock, create_lock;
static StaticSemaphore_t create_lock_storage;
static portMUX_TYPE create_mux=portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t timer;
static unsigned rate;
static int64_t next_due;
/* The raw recorder/stream paces itself from the register reads: the board task then keeps
 * its legacy sampling (no FIFO batching) while this is non-zero. */
int imu_backend_active(void) { return __atomic_load_n(&rate,__ATOMIC_RELAXED)!=0; }
uint32_t imu_backend_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }
uint64_t imu_backend_utc_ms(void)
{
    struct timeval tv; gettimeofday(&tv,NULL);
    return tv.tv_sec>=1577836800 ? (uint64_t)tv.tv_sec*1000+tv.tv_usec/1000 : 0;
}
static void timer_cb(void *arg) { (void)arg; xTaskNotifyGive(sampler); }
static void sample_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        xSemaphoreTake(sample_lock,portMAX_DELAY);
        int64_t now=esp_timer_get_time();
        if (rate && now>=next_due) {
            unsigned period=1000000u/rate;
            unsigned missed=(unsigned)((now-next_due)/period);
            if (missed) imu_stream_missed(missed,0);
            next_due+=(int64_t)(missed+1)*period; /* skip late slots; no catch-up reads */
            hal_imu v;
            uint32_t ms=imu_backend_ms(); uint64_t utc=imu_backend_utc_ms();
            if (board_imu_raw(&v)) imu_stream_push(&v,ms,utc);
            else imu_stream_missed(1,1);
        }
        xSemaphoreGive(sample_lock);
    }
}
static void write_task(void *arg)
{
    (void)arg;
    for (;;) {
        imu_worker_step();
        hal_imu_record s; hal_imu_record_get(&s);
        if(s.state==HAL_IMU_RECORDING || s.state==HAL_IMU_STOPPING) ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));
        else sleep_grid_notify_wait(1000); /* idle: shared 50 ms wake grid; start/stop requests notify */
    }
}
static TaskHandle_t owned_task(TaskFunction_t fn, const char *name, unsigned bytes,
                              unsigned priority, const char *owner)
{
    /* Same static creation primitive as IDF's WithCaps helper, but allocate the
     * stack through NOS so its budget reservation and 1 MiB reserve are atomic.
     * FreeRTOS requires the small TCB in internal memory; both live until reboot. */
    StackType_t *stack=nos_mem_alloc(bytes,owner);
    if (!stack) return NULL;
    StaticTask_t *tcb=heap_caps_malloc(sizeof *tcb,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!tcb) { nos_mem_free(stack); return NULL; }
    TaskHandle_t task=xTaskCreateStaticPinnedToCore(fn,name,bytes,NULL,priority,stack,tcb,tskNO_AFFINITY);
    if (!task) { free(tcb); nos_mem_free(stack); }
    return task;
}
int imu_backend_wake(void)
{
    /* A newly created writer may run on the other core before owned_task()
     * returns its handle. Serialize every creator, including that worker's
     * recursive stream-start wake, until all handles have been published. */
    portENTER_CRITICAL(&create_mux);
    if(!create_lock) create_lock=xSemaphoreCreateMutexStatic(&create_lock_storage);
    portEXIT_CRITICAL(&create_mux);
    if(!create_lock || xSemaphoreTake(create_lock,portMAX_DELAY)!=pdTRUE) return 0;
    int ok=0;
    if (!sample_lock) sample_lock=xSemaphoreCreateMutex();
    if (!sample_lock) goto done;
    if (!sampler) {
        sampler=owned_task(sample_task,"imu-sample",4096,3,"imu.sample_stack");
        if (!sampler) goto done;
    }
    if (!timer) {
        const esp_timer_create_args_t args={ .callback=timer_cb, .name="imu-rate", .skip_unhandled_events=false };
        if (esp_timer_create(&args,&timer)!=ESP_OK) goto done;
    }
    if (!writer_task) {
        writer_task=owned_task(write_task,"imu-write",6144,1,"imu.writer_stack");
        if (!writer_task) goto done;
    }
    xTaskNotifyGive(writer_task);
    ok=1;
done:
    xSemaphoreGive(create_lock);
    return ok;
}
int imu_backend_start(unsigned hz)
{
    if (!timer || !board_imu_available()) return 0;
    xSemaphoreTake(sample_lock,portMAX_DELAY);
    rate=hz;
    next_due=esp_timer_get_time()+1000000u/hz; /* also rejects stale stop/start notifications */
    esp_err_t e=esp_timer_start_periodic(timer,1000000u/hz);
    if (e!=ESP_OK) rate=0;
    xSemaphoreGive(sample_lock);
    return e==ESP_OK;
}
void imu_backend_stop(void)
{
    if (!timer) return;
    esp_timer_stop(timer);
    xSemaphoreTake(sample_lock,portMAX_DELAY); rate=0; xSemaphoreGive(sample_lock);
}
