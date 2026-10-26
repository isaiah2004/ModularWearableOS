/* Angled eyelids: compute one cutoff per column, fill the opaque span, then
 * blend at most one edge pixel. Same coverage as the original per-pixel mask. */
#ifndef WATCH_PIP_EYE_LID_H
#define WATCH_PIP_EYE_LID_H
#include "../gfx.h"
#include <math.h>

static void pip_eye_lid(gfx_fb *fb,float x,float y,float w,float h,float anger,int side)
{
    float top=y-h-GFX_S(5),span=anger*(h*.75f+w*.38f);
    int x0=(int)floorf(x-w-GFX_S(5)),x1=(int)ceilf(x+w+GFX_S(5));
    int y0=(int)floorf(top),y1=(int)ceilf(top+span+GFX_S(3));
    if(fb->rec) {
        unsigned hash=gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(
            gfx_hash_u(0x5049504c,(unsigned)side),x),y),w),h),anger);
        /* gfx_rec_cmd owns the framebuffer's screen translation. */
        gfx_rec_cmd(fb,x0,y0,x1,y1,hash);
        return;
    }
    int c0=fb->cx0,c1=fb->cx1;if(c1<=c0){c0=0;c1=GFX_W;}
    int bx0=x0+fb->ox,bx1=x1+fb->ox;
    if(bx0<c0)bx0=c0;
    if(bx1>c1)bx1=c1;
    if(bx0<0)bx0=0;
    if(bx1>GFX_W)bx1=GFX_W;
    int by0=y0+fb->oy-fb->y_base,by1=y1+fb->oy-fb->y_base;
    if(by0<0)by0=0;
    if(by1>fb->h)by1=fb->h;
    if(by1<=by0)return;
    for(int bx=bx0;bx<bx1;bx++) {
        float cutoff=top+anger*(h*.75f-side*((bx-fb->ox)+.5f-x)*.38f);
        /* floor(cutoff-.5) is the sole potentially translucent logical row.
         * Re-evaluate its coverage with the original expression to preserve
         * floating point rounding and RGB565 alpha quantization exactly. */
        int edge=(int)floorf(cutoff-.5f)+fb->oy-fb->y_base;
        int end=edge<by1?edge:by1;
        for(int by=by0;by<end;by++)fb->px[by*GFX_W+bx]=GFX_BLACK;
        if(edge>=by0 && edge<by1) {
            float coverage=cutoff-((edge+fb->y_base-fb->oy)+.5f);
            if(coverage>0) {
                uint16_t *pixel=&fb->px[edge*GFX_W+bx];
                *pixel=gfx_blend565(*pixel,GFX_BLACK,(uint8_t)(coverage*255));
            }
        }
    }
}
#endif
