#ifndef NOS_SYS_LAUNCHER_PAGE_H
#define NOS_SYS_LAUNCHER_PAGE_H
#include "nos.h"
/* Private HOME facade: shares the standalone launcher's state and sprite cache.
 * HOME owns horizontal navigation and all button actions. */
void sys_launcher_page_enter(void);
void sys_launcher_page_leave(void);
void sys_launcher_page_cancel(void);
void sys_launcher_page_tick(float dt);
void sys_launcher_page_draw(gfx_fb *fb);
int sys_launcher_page_input(const nos_input *in);
/* Read-only state for simulator assertions and serial diagnostics. */
void sys_home_nav_snapshot(int *page, float *offset, int *dragging);
float sys_launcher_page_scroll(void);
#endif
