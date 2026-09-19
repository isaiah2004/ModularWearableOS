/* One continuous rounded rectangle for Pip's eyes. The old union of integer
 * rectangles and fractional circles left seams where their edges met. */
#ifndef WATCH_PIP_EYE_SHAPE_H
#define WATCH_PIP_EYE_SHAPE_H

#include "../gfx.h"
#include <math.h>

/* Screen coordinates are finite and well within int range. Avoid software
 * libm rounding in scanline setup; coverage below remains unchanged. */
static int pip_floor(float x) { int i=(int)x;return i-(x<(float)i); }
static int pip_ceil(float x) { int i=(int)x;return i+(x>(float)i); }

static void pip_eye_blob(gfx_fb *fb, float cx, float cy,
                         float half_width, float half_height, uint16_t color)
{
    float hw=half_width, hh=half_height;
    if(hw<=0 || hh<=0) return;
    if(fb->rec) {
        unsigned hash=gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(
            gfx_hash_u(0x50495042u,color),cx),cy),hw),hh);
        gfx_rec_cmd(fb,pip_floor(cx-hw-1)+fb->ox,
            pip_floor(cy-hh-1)+fb->oy,
            pip_ceil(cx+hw+1)+fb->ox,
            pip_ceil(cy+hh+1)+fb->oy,hash);
        return;
    }
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0){c0=0;c1=GFX_W;}
    if(c0<0)c0=0;
    if(c1>GFX_W)c1=GFX_W;
    int y0=pip_floor(cy-hh-1)+fb->oy-fb->y_base;
    int y1=pip_ceil(cy+hh+1)+fb->oy-fb->y_base;
    if(y0<0)y0=0;
    if(y1>fb->h)y1=fb->h;
    if(c1<=c0 || y1<=y0 || cx+hw+1+fb->ox<c0 || cx-hw-1+fb->ox>=c1)return;

    float r=(hw<hh?hw:hh)*.62f, r2=r*r;
    float outside=r+.5f, outside2=outside*outside;
    float inside=r>.5f?r-.5f:0, inside2=inside*inside;
    float corner_x=hw-r, corner_y=hh-r;
    for(int by=y0;by<y1;by++) {
        float dy=fabsf((float)(by+fb->y_base-fb->oy)+.5f-cy);
        float qy=dy-corner_y;
        if(qy>=outside)continue;
        /* Outer extent bounds coverage work; the conservative inner extent
         * below turns the solid middle into a plain RGB565 span fill. */
        /* The fast root is used ONLY for conservative loop bounds, never AA.
         * A .01 pixel outer margin exceeds its error at screen-sized radii.
         * The inner bound already has a full pixel of slack on both sides. */
        float extent=qy>0 ? corner_x+gfx_fast_sqrt(outside2-qy*qy)+.01f : hw+.5f;
        int x0=pip_floor(cx-extent)+fb->ox;
        int x1=pip_ceil(cx+extent)+fb->ox;
        if(x0<c0)x0=c0;
        if(x1>c1)x1=c1;
        if(x1<=x0)continue;
        uint16_t *row=&fb->px[by*GFX_W];
        int solid0=x0,solid1=x0;
        if(hw>=.5f && hh-dy>=.5f && (qy<=0 || qy<=inside)) {
            float inner=qy>0 ? corner_x+gfx_fast_sqrt(inside2-qy*qy) : hw-.5f;
            /* Keep one pixel of slack at both ends. These pixels still run
             * through the original coverage equations, so float rounding at
             * the analytic boundary cannot turn an AA pixel into a solid one.
             * There are at most two square roots per row, none per pixel. */
            solid0=pip_ceil(cx-inner+.5f)+fb->ox;
            solid1=pip_floor(cx+inner-.5f)+fb->ox;
            if(solid0<x0)solid0=x0;
            if(solid1>x1)solid1=x1;
            if(solid1<solid0)solid0=solid1=x0;
        }
        for(int bx=solid0;bx<solid1;bx++)row[bx]=color;
        for(int fringe=0;fringe<2;fringe++) {
          int start=fringe ? solid1 : x0,end=fringe ? x1 : solid0;
          for(int bx=start;bx<end;bx++) {
            float dx=fabsf((float)(bx-fb->ox)+.5f-cx);
            float qx=dx-corner_x, coverage;
            if(qx<=0 || qy<=0) {
                float a=hw-dx,b=hh-dy;
                coverage=.5f+(a<b?a:b);
            } else {
                float d2=qx*qx+qy*qy;
                if(d2<=inside2){row[bx]=color;continue;}
                if(d2>=outside2)continue;
                /* Pade distance near the circular rim: exact at radius r,
                 * smooth across pixel boundaries, and avoids per-pixel sqrt.
                 * For Pip's radii its AA error is well below one alpha step. */
                coverage=.5f+2*r*(r2-d2)/(d2+3*r2);
            }
            if(coverage>=1)row[bx]=color;
            else if(coverage>0)row[bx]=gfx_blend565(row[bx],color,
                (uint8_t)(coverage*255.0f));
          }
        }
    }
}
#endif
