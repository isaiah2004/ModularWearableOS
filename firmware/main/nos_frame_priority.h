/* Optional UI paint-only scheduler experiment. Default0 preserves inherited priority. */
#ifndef NOS_FRAME_PRIORITY_H
#define NOS_FRAME_PRIORITY_H
#include <string.h>
typedef struct { unsigned base,mode,paint,mismatches,paint_samples; int active; } nos_frame_priority;
typedef void (*nos_frame_priority_setter)(unsigned);
static inline void nos_frame_priority_init(nos_frame_priority *p,unsigned base){p->base=base;p->mode=0;p->paint=0;p->mismatches=0;p->paint_samples=0;p->active=0;}
static inline void nos_frame_priority_restore(nos_frame_priority *p,nos_frame_priority_setter set){
    if(p->active){set(p->base);p->active=0;}
}
/* Parsing/guards precede ALL mutations, including restoration. */
static inline int nos_frame_priority_command(nos_frame_priority *p,const char *line,int capture,int touch,nos_frame_priority_setter set){
    unsigned mode;
    if(!strcmp(line,"frame_priority 0"))mode=0;
    else if(!strcmp(line,"frame_priority 3"))mode=3;
    else return 1;
    if(capture)return 2;
    if(touch)return 3;
    nos_frame_priority_restore(p,set);p->mode=mode;return 0;
}
static inline void nos_frame_priority_begin(nos_frame_priority *p,int on,nos_frame_priority_setter set){
    if(on && p->mode==3 && !p->active && p->base!=3){set(3);p->active=1;}
}
/* Observe effective priority at the paint boundary, without output or enumeration. */
static inline void nos_frame_priority_observe(nos_frame_priority *p,int on,unsigned actual){
    p->paint=actual;p->paint_samples++;
    if(actual!=(on && p->mode==3 ? 3u : p->base))p->mismatches++;
}
#endif
