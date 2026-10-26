#ifndef NOS_PANEL_ACK_H
#define NOS_PANEL_ACK_H
#include <stdint.h>
/* One command owner serializes request/ack access. Failed or stale callbacks
 * never advance applied state. The host retains ownership until this ACK. */
typedef struct { uint32_t request,ack; int requested,applied,error; } nos_panel_ack;
static inline uint32_t nos_panel_request(nos_panel_ack *s,int value)
{
    if(!++s->request)++s->request;
    s->requested=value;return s->request;
}
static inline int nos_panel_complete(nos_panel_ack *s,uint32_t request,int error)
{
    if(request!=s->request || request==s->ack)return 0;
    s->error=error;if(!error)s->applied=s->requested;
    s->ack=request;return 1;
}
#endif
