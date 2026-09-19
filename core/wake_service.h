#ifndef WATCH_WAKE_SERVICE_H
#define WATCH_WAKE_SERVICE_H
#include <stdint.h>
typedef struct {
    int wrist,pickup,turn,learned,learning,learn_result,valid,stage,last_reason;
    uint32_t wakes,learn_ms;
    float magnitude,speed,view_dot,bias[3];
} wake_info;
/* UI/main-loop ownership. No sensor IO or flash writes from drawing. */
void wake_service_init(int legacy_enabled);
int wake_service_tick(uint32_t now,int eligible);
int wake_service_set(const char *option,int value);
void wake_service_learn(void);
void wake_service_get(wake_info *out);
#endif
