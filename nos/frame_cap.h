/* Presentation limits are independent of the authored animation clock. */
#ifndef NOS_FRAME_CAP_H
#define NOS_FRAME_CAP_H
#include <stdint.h>
#define NOS_FRAME_CAP_DEFAULT 60
static inline int nos_frame_cap_valid(unsigned fps)
{
    return fps==0 || fps==24 || fps==30 || fps==40 || fps==48 || fps==60;
}
/* Round up: a capped start must never run faster than the selected rate. */
static inline uint32_t nos_frame_cap_period(unsigned fps)
{
    if(!nos_frame_cap_valid(fps))fps=NOS_FRAME_CAP_DEFAULT;
    return fps?(1000000u+fps-1u)/fps:0;
}
#endif
