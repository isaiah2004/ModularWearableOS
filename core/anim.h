/* anim.h — animation primitives. The house style is spring physics, not
 * fixed-duration tweens: every motion is interruptible mid-flight and
 * carries its velocity into the next gesture. */
#ifndef WATCH_ANIM_H
#define WATCH_ANIM_H

typedef struct {
    float pos;
    float vel;      /* units per second */
    float target;
    float omega;    /* stiffness; higher = snappier (12-20 feels right) */
} spring_t;

void spring_init(spring_t *s, float pos, float omega);
void spring_to(spring_t *s, float target);
/* Advance by dt seconds. Returns 1 while still moving, 0 once settled
 * (pos snapped exactly to target). Critically damped — no overshoot. */
int  spring_step(spring_t *s, float dt);

#endif
