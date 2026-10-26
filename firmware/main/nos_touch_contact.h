#ifndef NOS_TOUCH_CONTACT_H
#define NOS_TOUCH_CONTACT_H
#include <stdint.h>
#define NOS_TOUCH_INVALID_HOLD_MS 150u
typedef struct {uint32_t invalid_since;int down,invalid,suppressed;} nos_touch_contact;
/* -1 produces no event, 0/1 are confirmed physical states, 2 cancels a lost
 * contact. After cancellation, require an actual release before a new gesture. */
static inline int nos_touch_contact_sample(nos_touch_contact *s,int sample,uint32_t now)
{
    if(sample<0) {
        if(!s->invalid){s->invalid=1;s->invalid_since=now;}
        if(s->down && now-s->invalid_since>=NOS_TOUCH_INVALID_HOLD_MS) {
            s->down=0;s->suppressed=1;return 2;
        }
        return -1;
    }
    s->invalid=0;
    if(s->suppressed) {if(!sample)s->suppressed=0;return -1;}
    s->down=sample!=0;return s->down;
}
#endif
