#ifndef EXT_TEST_FREERTOS_H
#define EXT_TEST_FREERTOS_H
#include <windows.h>
typedef SRWLOCK portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED SRWLOCK_INIT
#define portENTER_CRITICAL(p) AcquireSRWLockExclusive(p)
#define portEXIT_CRITICAL(p) ReleaseSRWLockExclusive(p)
typedef unsigned UBaseType_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xffffffffu
#define tskNO_AFFINITY -1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#endif
