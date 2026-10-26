#ifndef SLEEP_GRID_H
#define SLEEP_GRID_H
/* Shared wake grid for automatic light sleep (docs/LIGHT_SLEEP.md).
 *
 * While the display is not ON every periodic task rounds its next wake-up up to a
 * multiple of SLEEP_GRID_MS of the FreeRTOS tick clock (1 kHz, kept in step with
 * wall time across light sleep), so one CPU wake serves the main loop, the IMU batch
 * reader, the touch poller and the background workers together. A task that wakes a
 * few ms late must still land on the next grid point, not the one after, so the target
 * is rounded up from now + period - SLEEP_GRID_SLACK_MS: the delay is at least
 * period - SLEEP_GRID_SLACK_MS and at most period + SLEEP_GRID_MS - 1 ms. */
#include <stdint.h>
#define SLEEP_GRID_MS 50u
#define SLEEP_GRID_SLACK_MS 10u
static inline uint32_t sleep_grid_delay_ms(uint32_t now_ms,uint32_t period_ms)
{
    if(!period_ms) period_ms=1;
    uint32_t slack=period_ms>SLEEP_GRID_SLACK_MS?SLEEP_GRID_SLACK_MS:0;
    uint32_t target=now_ms+period_ms-slack,rem=target%SLEEP_GRID_MS;
    if(rem) target+=SLEEP_GRID_MS-rem;
    return target-now_ms; /* unsigned arithmetic also holds across a tick-counter wrap */
}
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static inline uint32_t sleep_grid_now_ms(void) {return (uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS);}
/* Block until the first grid point about period_ms ahead. */
static inline void sleep_grid_delay(uint32_t period_ms)
{
    vTaskDelay(pdMS_TO_TICKS(sleep_grid_delay_ms(sleep_grid_now_ms(),period_ms)));
}
/* Same, but a task notification ends the wait early; returns the notification count. */
static inline uint32_t sleep_grid_notify_wait(uint32_t period_ms)
{
    return ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(sleep_grid_delay_ms(sleep_grid_now_ms(),period_ms)));
}
#endif
#endif
