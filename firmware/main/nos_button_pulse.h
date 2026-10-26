/* Serial button injection spans UI samples; physical button state is ORed by host. */
#ifndef NOS_BUTTON_PULSE_H
#define NOS_BUTTON_PULSE_H
#include <stdint.h>
typedef struct { uint32_t started; int active, sampled; } nos_button_pulse;
static inline void nos_button_pulse_start(nos_button_pulse *p, uint32_t now)
{ p->started = now; p->active = 1; p->sampled = 0; }
static inline int nos_button_pulse_sample(nos_button_pulse *p, uint32_t now)
{
    if (!p->active) return 0;
    /* Even if the first UI sample was delayed, expose a down before its up. */
    if (p->sampled && (uint32_t)(now - p->started) >= 80) { p->active = 0; return 0; }
    p->sampled = 1;
    return 1;
}
#endif
