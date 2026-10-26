#ifndef ORRERY_SHADE_Q16_H
#define ORRERY_SHADE_Q16_H
#include <stdint.h>
enum {
    ORRERY_SHADE_Q16_EMPTY,
    ORRERY_SHADE_Q16_BUILDING,
    ORRERY_SHADE_Q16_READY,
    ORRERY_SHADE_Q16_REJECTED
};
/* Background worker only. One admission/build attempt; never a UI fallback. */
unsigned orrery_shade_q16_prebake(void); /*0ready,~0unavailable/pending, else rejected entries */
void orrery_shade_q16_prewarm(void);     /* compatibility/background convenience */
int orrery_shade_q16_ready(void);
/*0none,1admission,2allocation,3validation; only terminal failures are reported. */
int orrery_shade_q16_rejection(void);
/* Acquire immutable multipliers only for a bitwise-identical512-float table.
 * Returned storage is permanent. Call once after READY/per card allocation,
 * not in a pixel loop. NULL means retain the original exact float shader. */
const uint32_t *orrery_shade_q16_get(const float coefficients[512]);
/* Original 512 quantized powf results, produced by the same background build.
 * Immutable after READY; NULL before publication or after rejection. Copying
 * these avoids repeating 512 powf calls on every card entry. */
const uint8_t *orrery_shade_lighting_get(void);
int orrery_shade_q16_state(void);
#endif
