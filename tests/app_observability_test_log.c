#include "app_observability_test_log.h"
#include <assert.h>
app_test_event app_test_events[4096];
unsigned app_test_event_count;
static uint32_t sequence;
void app_test_trace_reset(void) { app_test_event_count = 0; sequence = 0; }
void diag_log_event(uint16_t code, int32_t a, int32_t b, int32_t c)
{
    assert(app_test_event_count < 4096);
    app_test_events[app_test_event_count++] = (app_test_event){code, a, b, c};
}
uint32_t diag_log_name_id(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}
uint32_t diag_action_request(uint16_t domain, uint16_t action, int32_t target)
{
    uint32_t request = ++sequence;
    diag_log_event(DIAG_ACTION_REQUEST, (int32_t)request, (int32_t)(((uint32_t)domain << 16) | action), target);
    return request;
}
void diag_action_context(uint32_t request, int32_t source, int32_t value)
{ if (request) diag_log_event(DIAG_ACTION_CONTEXT, (int32_t)request, source, value); }
void diag_action_result(uint32_t request, int32_t result, int32_t detail)
{ if (request) diag_log_event(DIAG_ACTION_RESULT, (int32_t)request, result, detail); }
uint32_t app_test_request(uint16_t domain, uint16_t action)
{
    int32_t key = (int32_t)(((uint32_t)domain << 16) | action);
    for (unsigned i = app_test_event_count; i; i--)
        if (app_test_events[i-1].code == DIAG_ACTION_REQUEST && app_test_events[i-1].b == key)
            return (uint32_t)app_test_events[i-1].a;
    return 0;
}
unsigned app_test_results(uint32_t request, int result)
{
    unsigned count = 0;
    for (unsigned i = 0; i < app_test_event_count; i++)
        if (app_test_events[i].code == DIAG_ACTION_RESULT && app_test_events[i].a == (int32_t)request &&
            app_test_events[i].b == result) count++;
    return count;
}
int app_test_target(uint32_t request)
{
    for (unsigned i = 0; i < app_test_event_count; i++)
        if (app_test_events[i].code == DIAG_ACTION_REQUEST && app_test_events[i].a == (int32_t)request)
            return app_test_events[i].c;
    assert(0); return 0;
}
