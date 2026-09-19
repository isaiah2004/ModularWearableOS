#include "notif.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    uint32_t before=notif_sequence();
    for(int i=0;i<12;i++) {
        char text[20];snprintf(text,sizeof text,"Arrival %d",i);
        notif_push("Test",text,0);
        assert(notif_sequence()==before+(uint32_t)i+1);
    }
    assert(notif_count()==8);
    assert(!strcmp(notif_get(0)->title,"Arrival 11"));
    assert(!strcmp(notif_get(7)->title,"Arrival 4"));
    notif_clear();assert(notif_count()==0 && notif_sequence()==before+12);
    notif_push("Test","After clearing",0);assert(notif_sequence()==before+13);
    puts("PASS notifications: arrivals survive saturation and clearing");
    return 0;
}
