#ifndef EXT_TEST_TASK_H
#define EXT_TEST_TASK_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
void vTaskDelay(TickType_t);
unsigned ulTaskNotifyTake(int,TickType_t);
/* firmware/main/sleep_grid.h is ESP_PLATFORM-only for the tick clock; host tests use a fixed one */
static inline unsigned sleep_grid_now_ms(void) {return 0;}
void xTaskNotifyGive(TaskHandle_t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
int xTaskCreatePinnedToCoreWithCaps(void (*)(void*),const char*,unsigned,void*,int,TaskHandle_t*,int,unsigned);
#endif
