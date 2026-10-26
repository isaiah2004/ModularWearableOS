/* Session-only developer overlays; rates describe engine calls, not optical FPS. */
#include "developer.h"
#include "nos.h"
#include "ui.h"
#include <stdio.h>
unsigned nos_dev_overlays;
/* Optional platform capability: simulators and boards without a TE route show N/A. */
extern int hal_display_te_available(void) __attribute__((weak));
int nos_dev_te_available(void)
{return hal_display_te_available && hal_display_te_available();}
static float elapsed, fps;
static unsigned frames;
static uint32_t raster_us,dirty_px;
static int tx,ty,down;
void nos_dev_frame(float ms) {
    if(ms<=0)return;
    elapsed+=ms;frames++;
    if(elapsed>=500){fps=1000.0f*frames/elapsed;elapsed=0;frames=0;raster_us=nos_last.raster_us;dirty_px=nos_last.dirty_px;}
}
void nos_dev_touch(int x,int y,int pressed){tx=x;ty=y;down=pressed;}
static void line(gfx_fb *fb,int y,const char *text) {
    gfx_fill_rect(fb,88,y-UI_F_SMALL->ascent-2,290,UI_F_SMALL->line_height+4,C_BG);
    ui_text_fit(fb,UI_F_SMALL,UI_CX,y,text,C_PRI,UI_CENTER,280);
    /* Diagnostic readouts must not inherit previous scene pixels. */
    ui_fblur_coverage_claim(fb,88,y-UI_F_SMALL->ascent-2,378,y-UI_F_SMALL->ascent+UI_F_SMALL->line_height+2);
}
void nos_dev_draw(gfx_fb *fb) {
    if(!nos_dev_overlays || nos_aod)return;
    char text[64];int y=54;
    if(nos_dev_overlays & NOS_DEV_FPS){snprintf(text,sizeof text,"LOOP %.1f FPS",fps);line(fb,y,text);y+=23;}
    if(nos_dev_overlays & NOS_DEV_STATS){
        snprintf(text,sizeof text,"RASTER %.1f MS  %u PX",raster_us/1000.0,(unsigned)dirty_px);
        line(fb,y,text);y+=23;
    }
    if(nos_dev_overlays & NOS_DEV_SCREEN){const nos_screen *s=nos_top();snprintf(text,sizeof text,"%s / DEPTH %d",s?s->name:"?",nos_depth());line(fb,y,text);}
    if((nos_dev_overlays & NOS_DEV_TOUCH) && down){
        gfx_ring(fb,tx,ty,16,2,C_PRI);
        gfx_line(fb,tx-22,ty,tx+22,ty,1,C_PRI);
        gfx_line(fb,tx,ty-22,tx,ty+22,1,C_PRI);
    }
}
