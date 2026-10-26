/* Offline adapter: feeds the production motion engine unchanged. */
#include "workout_motion.h"
#include <stdio.h>
#include <stdint.h>
int main(void)
{
    workout_motion s;workout_motion_reset(&s);
    char line[512];unsigned t;float ax,ay,az,gx,gy,gz;
    puts("ms,valid_ms,movement_ms,bursts,episode_active,sample_anchor");
    while(fgets(line,sizeof line,stdin)) {
        if(sscanf(line,"%u,%f,%f,%f,%f,%f,%f",&t,&ax,&ay,&az,&gx,&gy,&gz)!=7)return 2;
        workout_motion_feed(&s,(uint32_t)t,ax,ay,az,gx,gy,gz);
        printf("%u,%u,%u,%u,%u,%u\n",t,s.valid_ms,s.movement_ms,s.bursts,s.active,s.have);
    }
    return ferror(stdin)?3:0;
}
