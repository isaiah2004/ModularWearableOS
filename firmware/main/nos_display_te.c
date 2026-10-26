#include "nos_display_te.h"
#include "pins.h"
#ifdef PIN_LCD_TE
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TE_WAIT_US 25000
static StaticSemaphore_t s_edge_storage;
static SemaphoreHandle_t s_edge;
static portMUX_TYPE s_lock=portMUX_INITIALIZER_UNLOCKED;
static nos_display_te_stats s_stats={.gpio=PIN_LCD_TE,.awake=1};
static int s_waiting;
static int64_t s_last_edge;

static void IRAM_ATTR te_isr(void *arg)
{
    (void)arg;
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_lock);
    int64_t period=now-s_last_edge;
    if(s_last_edge && period>=5000 && period<=50000)s_stats.period_us=(uint32_t)period;
    s_last_edge=now;s_stats.edges++;
    int notify=s_waiting;
    portEXIT_CRITICAL_ISR(&s_lock);
    BaseType_t woken=pdFALSE;
    if(notify)xSemaphoreGiveFromISR(s_edge,&woken);
    if(woken)portYIELD_FROM_ISR();
}

static void te_irq_update(void)
{
    portENTER_CRITICAL(&s_lock);
    int on=s_stats.ready && s_stats.enabled && s_stats.awake;
    /* Never reuse a pre-sleep/pre-enable edge or its measured period. */
    s_last_edge=0;s_stats.period_us=0;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t error=on?gpio_intr_enable(PIN_LCD_TE):gpio_intr_disable(PIN_LCD_TE);
    if(error!=ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_stats.error=error;s_stats.ready=0;
        portEXIT_CRITICAL(&s_lock);
    }
}

void nos_display_te_init(void)
{
    s_edge=xSemaphoreCreateBinaryStatic(&s_edge_storage);
    gpio_config_t input={.pin_bit_mask=1ULL<<PIN_LCD_TE,.mode=GPIO_MODE_INPUT,
        .pull_down_en=GPIO_PULLDOWN_ENABLE,.intr_type=GPIO_INTR_POSEDGE};
    esp_err_t error=gpio_config(&input);
    if(error==ESP_OK)error=gpio_intr_disable(PIN_LCD_TE);
    if(error==ESP_OK) {
        error=gpio_install_isr_service(0); /* shared with touch/button */
        if(error==ESP_ERR_INVALID_STATE)error=ESP_OK;
    }
    if(error==ESP_OK)error=gpio_isr_handler_add(PIN_LCD_TE,te_isr,NULL);
    s_stats.ready=error==ESP_OK && s_edge!=NULL;
    s_stats.error=error;
    if(s_stats.ready)te_irq_update();
}

void nos_display_te_set_enabled(int enabled)
{
    portENTER_CRITICAL(&s_lock);s_stats.enabled=enabled!=0;portEXIT_CRITICAL(&s_lock);
    if(s_stats.ready)te_irq_update();
}

void nos_display_te_set_awake(int awake)
{
    portENTER_CRITICAL(&s_lock);s_stats.awake=awake!=0;portEXIT_CRITICAL(&s_lock);
    if(s_stats.ready)te_irq_update();
}

void nos_display_te_wait(void)
{
    if(!s_edge)return;
    while(xSemaphoreTake(s_edge,0)==pdTRUE) {}
    int64_t begin=esp_timer_get_time(),edge=0;
    portENTER_CRITICAL(&s_lock);
    int active=s_stats.ready && s_stats.enabled && s_stats.awake;
    uint32_t sequence=s_stats.edges;
    if(active){s_waiting=1;s_stats.waits++;}
    portEXIT_CRITICAL(&s_lock);
    if(!active)return;
    for(;;) {
        int64_t remaining=TE_WAIT_US-(esp_timer_get_time()-begin);
        if(remaining<=0)break;
        TickType_t ticks=pdMS_TO_TICKS((remaining+999)/1000);
        if(!ticks)ticks=1;
        if(xSemaphoreTake(s_edge,ticks)!=pdTRUE)break;
        portENTER_CRITICAL(&s_lock);
        /* An in-flight give from an earlier wait cannot authorize a frame. */
        if(s_stats.edges!=sequence && s_last_edge>=begin)edge=s_last_edge;
        portEXIT_CRITICAL(&s_lock);
        if(edge)break;
    }
    int64_t end=esp_timer_get_time();
    uint32_t waited=(uint32_t)(end-begin);
    portENTER_CRITICAL(&s_lock);
    s_waiting=0;s_stats.last_wait_us=waited;s_stats.wait_total_us+=waited;
    if(waited>s_stats.max_wait_us)s_stats.max_wait_us=waited;
    if(edge) {
        s_stats.synced++;s_stats.last_phase_us=(uint32_t)(end-edge);
        if(s_stats.last_phase_us>s_stats.max_phase_us)s_stats.max_phase_us=s_stats.last_phase_us;
    } else s_stats.timeouts++;
    portEXIT_CRITICAL(&s_lock);
}

nos_display_te_stats nos_display_te_snapshot(void)
{
    portENTER_CRITICAL(&s_lock);
    nos_display_te_stats stats=s_stats;
    int64_t age=s_last_edge?esp_timer_get_time()-s_last_edge:UINT32_MAX;
    stats.edge_age_us=age>=UINT32_MAX?UINT32_MAX:(uint32_t)age;
    portEXIT_CRITICAL(&s_lock);
    return stats;
}
#else
void nos_display_te_init(void) {}
void nos_display_te_set_enabled(int enabled) {(void)enabled;}
void nos_display_te_set_awake(int awake) {(void)awake;}
void nos_display_te_wait(void) {}
nos_display_te_stats nos_display_te_snapshot(void)
{return (nos_display_te_stats){.gpio=-1};}
#endif
