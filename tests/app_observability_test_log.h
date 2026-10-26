/* Private semantic-event collector; linked only into callback harnesses. */
#ifndef APP_OBSERVABILITY_TEST_LOG_H
#define APP_OBSERVABILITY_TEST_LOG_H
#include "diag_log.h"
typedef struct { uint16_t code; int32_t a, b, c; } app_test_event;
extern app_test_event app_test_events[4096];
extern unsigned app_test_event_count;
void app_test_trace_reset(void);
uint32_t app_test_request(uint16_t domain, uint16_t action);
unsigned app_test_results(uint32_t request, int result);
int app_test_target(uint32_t request);
#endif
