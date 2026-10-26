#include "nos_touch_contact.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {int16_t x,y;uint8_t down;uint32_t ms;} touch_smp;
static touch_smp queued[8];
static unsigned at,count,downs,ups,cancels;
static int s_swallow,s_touch_any;
static void enqueue(int down)
{assert(count<8);queued[count]=(touch_smp){233,233,(uint8_t)down,count*10};count++;s_touch_any=down==1;}
static int touch_pop(touch_smp *out)
{if(at==count){at=count=0;return 0;}*out=queued[at++];return 1;}
static void nos_touch_at(int x,int y,int down,uint32_t ms)
{(void)ms;assert(x==233&&y==233);if(down==2)cancels++;else if(down)downs++;else ups++;}
/* ACTUAL FUNCTIONS */
int main(void)
{
    nos_touch_contact contact={0};
    assert(nos_touch_contact_sample(&contact,1,100)==1);
    s_swallow=1;enqueue(1);touch_drain(1);assert(s_swallow&&!downs&&!ups&&!cancels);
    assert(nos_touch_contact_sample(&contact,-1,110)==-1);
    int lost=nos_touch_contact_sample(&contact,-1,260);assert(lost==2);
    enqueue(lost);touch_drain(1);assert(!s_swallow&&cancels==1&&!downs&&!ups);
    /* The real release is consumed by the contact filter. No queued UP can
     * rescue a wake-suppression latch: cancellation itself must clear it. */
    assert(nos_touch_contact_sample(&contact,0,280)==-1);
    assert(nos_touch_contact_sample(&contact,1,300)==1);
    enqueue(1);touch_drain(1);assert(downs==1&&!s_touch_any);
    enqueue(0);touch_drain(1);assert(ups==1);
    s_swallow=1;enqueue(1);enqueue(0);touch_drain(1);
    assert(!s_swallow&&downs==1&&ups==1&&cancels==1); /* normal waking tap stays spent */
    s_swallow=1;enqueue(2);touch_drain(0);
    assert(!s_swallow&&cancels==1); /* dropping physical samples cannot cancel USB input */
    puts("PASS actual touch drain: failed waking contact cancels without tap, clears suppression, accepts next gesture; normal wake and injected input preserved");
}
