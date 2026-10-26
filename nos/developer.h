#ifndef NOS_DEVELOPER_H
#define NOS_DEVELOPER_H
#include "gfx.h"
enum { NOS_DEV_FPS=1, NOS_DEV_STATS=2, NOS_DEV_TOUCH=4, NOS_DEV_SCREEN=8 };
extern unsigned nos_dev_overlays;
int nos_dev_te_available(void);
void nos_dev_frame(float elapsed_ms);
void nos_dev_touch(int x,int y,int down);
void nos_dev_draw(gfx_fb *fb);
#endif
