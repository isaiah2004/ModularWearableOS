#include "nos_frame_priority.h"
#include <assert.h>
#include <stdio.h>
static unsigned current,calls;
static void set(unsigned p){current=p;calls++;}
int main(void){
 nos_frame_priority p;current=1;nos_frame_priority_init(&p,current);
 for(int i=0;i<100;i++){nos_frame_priority_begin(&p,1,set);nos_frame_priority_restore(&p,set);nos_frame_priority_begin(&p,0,set);nos_frame_priority_restore(&p,set);}assert(!calls && current==1 && p.mode==0);
 const char *bad[]={"frame_priority","frame_priority 1","frame_priority 03","frame_priority +3","frame_priority 3 ","frame_priority 3 x","frame_priority\t3","frame_priority 4","frame_priority 0x3","frame_priorityx 3"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){assert(nos_frame_priority_command(&p,bad[i],0,0,set)==1);assert(!calls && !p.mode);}
 assert(nos_frame_priority_command(&p,"frame_priority 3",1,0,set)==2);assert(nos_frame_priority_command(&p,"frame_priority 3",0,1,set)==3);assert(!p.mode && !calls);
 assert(!nos_frame_priority_command(&p,"frame_priority 3",0,0,set));nos_frame_priority_begin(&p,0,set);assert(!calls);nos_frame_priority_begin(&p,1,set);assert(current==3 && calls==1 && p.active);nos_frame_priority_begin(&p,1,set);assert(calls==1);
 assert(nos_frame_priority_command(&p,"frame_priority 0",1,0,set)==2);assert(current==3 && p.mode==3 && calls==1);
 assert(nos_frame_priority_command(&p,"frame_priority 0",0,1,set)==3);assert(current==3 && p.mode==3 && calls==1);
 assert(!nos_frame_priority_command(&p,"frame_priority 0",0,0,set));assert(current==1 && calls==2 && !p.active && !p.mode);
 /* Every named exit restores once; repeated loop-top restoration is a no-op. */
 const char *paths[]={"normal_dma","pacer_wait","late_reloop","off_continue","aod_continue","command_disable"};
 for(unsigned i=0;i<sizeof paths/sizeof *paths;i++){assert(!nos_frame_priority_command(&p,"frame_priority 3",0,0,set));unsigned n=calls;nos_frame_priority_begin(&p,1,set);nos_frame_priority_restore(&p,set);nos_frame_priority_restore(&p,set);assert(current==1 && calls==n+2);}
 /* Preserve actual inherited baseline rather than hardcode priority1. */
 current=5;nos_frame_priority_init(&p,current);nos_frame_priority_begin(&p,1,set);assert(current==5);assert(!nos_frame_priority_command(&p,"frame_priority 3",0,0,set));nos_frame_priority_begin(&p,1,set);assert(current==3);nos_frame_priority_restore(&p,set);assert(current==5);
 current=3;nos_frame_priority_init(&p,current);assert(!nos_frame_priority_command(&p,"frame_priority 3",0,0,set));unsigned n=calls;nos_frame_priority_begin(&p,1,set);nos_frame_priority_restore(&p,set);assert(current==3 && calls==n);
 nos_frame_priority_init(&p,1);nos_frame_priority_observe(&p,1,1);assert(p.paint==1 && !p.mismatches);
 assert(!nos_frame_priority_command(&p,"frame_priority 3",0,0,set));nos_frame_priority_observe(&p,1,3);assert(p.paint==3 && !p.mismatches);
 nos_frame_priority_observe(&p,1,1);assert(p.paint==1 && p.mismatches==1 && p.paint_samples==3);nos_frame_priority_observe(&p,0,1);assert(p.mismatches==1);
 assert(!nos_frame_priority_command(&p,"frame_priority 0",0,0,set));nos_frame_priority_observe(&p,1,1);assert(p.paint==1 && p.mismatches==1 && p.paint_samples==5);
 puts("PASS frame priority: default/off no setters; exact commands/guards; bounded window; all exits restore inherited baseline");return 0;
}

