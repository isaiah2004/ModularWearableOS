#include "anim.h"
#include <math.h>

void spring_init(spring_t *s, float pos, float omega)
{
    s->pos = s->target = pos;
    s->vel = 0;
    s->omega = omega;
}

void spring_to(spring_t *s, float target)
{
    s->target = target;
}

int spring_step(spring_t *s, float dt)
{
    /* closed-form critically damped spring — stable at any frame rate */
    float d = s->pos - s->target;
    float e = expf(-s->omega * dt);
    float t2 = (s->vel + s->omega * d) * dt;
    s->pos = s->target + (d + t2) * e;
    s->vel = (s->vel - s->omega * t2) * e;
    if (fabsf(s->pos - s->target) < 0.5f && fabsf(s->vel) < 10.0f) {
        s->pos = s->target;
        s->vel = 0;
        return 0;
    }
    return 1;
}
