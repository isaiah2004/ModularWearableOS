#include "tuning.h"
#include "sports.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE tune_mux=portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&tune_mux)
#define UNLOCK() portEXIT_CRITICAL(&tune_mux)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif
static sports_tuning cfg={1,.10f,.60f,8,250};
static unsigned cfg_revision=1;
unsigned tuning_revision(void) { unsigned value;LOCK();value=cfg_revision;UNLOCK();return value; }
void tuning_init(void)
{
    sports_tuning c;
    if(hal_store_load("sportstune",&c,sizeof c) && c.version==1 &&
       c.walk_g>=.04f && c.walk_g<=.20f && c.knock_g>=.20f && c.knock_g<=1.2f &&
       c.walk_peaks>=4 && c.walk_peaks<=12 && c.quiet_ms>=80 && c.quiet_ms<=400) {
        LOCK();cfg=c;UNLOCK();
    }
}
void tuning_get(sports_tuning *out) { LOCK();*out=cfg;UNLOCK(); }
int tuning_adjust(int feature,int dir)
{
    sports_tuning c;tuning_get(&c);
    if(dir==0)c=(sports_tuning){1,.10f,.60f,8,250};
    else if(feature==0) {
        c.walk_g+=dir*.01f;if(c.walk_g<.04f)c.walk_g=.04f;if(c.walk_g>.20f)c.walk_g=.20f;
    } else if(feature==1) {
        c.knock_g+=dir*.05f;if(c.knock_g<.20f)c.knock_g=.20f;if(c.knock_g>1.2f)c.knock_g=1.2f;
    } else if(feature==2) {
        c.walk_peaks+=dir;if(c.walk_peaks<4)c.walk_peaks=4;if(c.walk_peaks>12)c.walk_peaks=12;
    } else if(feature==3) {
        c.quiet_ms+=dir*25;if(c.quiet_ms<80)c.quiet_ms=80;if(c.quiet_ms>400)c.quiet_ms=400;
    } else {
        return 0;
    }
    if(!hal_store_save_checked("sportstune",&c,sizeof c))return 0;
    LOCK();cfg=c;if(++cfg_revision==0)cfg_revision=1;UNLOCK();
    float v[]={(float)feature,c.walk_g,c.knock_g,(float)c.walk_peaks,(float)c.quiet_ms};sports_emit(SP_TUNE,v,5);
    return 1;
}
void tuning_session(int feature,int start,int expected)
{
    float v[]={(float)feature,(float)start,(float)expected};sports_emit(SP_LABEL,v,3);
}
