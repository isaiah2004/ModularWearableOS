/* Exercise actual scheduling while app animation advances on every tick.
 * Warmup in nos_blur_animation_test freezes state only for pixel/revision tests. */
#define main frozen_pixel_checks
#include "nos_blur_animation_test.c"
#undef main
int main(void)
{
    dot_prebake_all();setup();test_transitioning=0;
#if APP_CASE == 1
    for(int frame=0;frame<120;frame++) {
        W.frame=frame;nos_frame++;nos_t+=1.f/M_FPS;
        ui_fblur_tick(&W,1,290.f/M_FPS,0,5,NBANDS,BAND_H,BAND_X0,BAND_X1,1,band_draw,NULL);
    }
    blur_item *visible=fb_find(4),*incoming=fb_find(6),*animated=fb_find(1);
    printf("Weather: visible4_level%d incoming6_level%d animated1_level%d\n",
        visible?visible->level:-1,incoming?incoming->level:-1,animated?animated->level:-1);
    fflush(stdout);
    assert(visible && visible->level && incoming && incoming->level);
    /* Stabilization backoff must recover after the authored icon stops. */
    for(int frame=0;frame<120;frame++) {
        nos_frame++;nos_t+=1.f/M_FPS;
        ui_fblur_tick(&W,1,290.f/M_FPS,0,5,NBANDS,BAND_H,BAND_X0,BAND_X1,1,band_draw,NULL);
        animated=fb_find(1);
        if(animated && animated->level && fb_validate(animated)) break;
    }
    assert(animated && animated->level && fb_validate(animated));
    assert(animated->sig==signature(1));
    /* Re-render the immutable final frame independently, then use a scalar
     * RGB565 channel box oracle for the published directional image. */
    int rows=animated->h+2*animated->context;
    memset(expected,0,(size_t)rows*animated->w*sizeof *expected);
    gfx_fb f={0};f.px=expected;f.h=rows;f.stride=animated->w;f.x_base=animated->x0;
    f.cx0=animated->x0;f.cx1=f.cx0+animated->w;
    int y=UI_CY-animated->h/2;f.y_base=y-animated->context;
    band_draw(&f,1,f.cx0,y,f.cx1,animated->h,NULL);
    assert(!memcmp(animated->sharp,expected,(size_t)rows*animated->w*sizeof *expected));
    int length=blur_cache_length(animated->level);
    for(int row=0;row<animated->h;row++) for(int x=0;x<animated->w;x++) {
        unsigned r=0,g=0,b=0;
        for(int j=0;j<length;j++) {
            unsigned c=expected[(row+animated->context-length/2+j)*animated->w+x];
            r+=c>>11;g+=(c>>5)&63;b+=c&31;
        }
        uint16_t pixel=(uint16_t)(((r+length/2)/length)<<11|((g+length/2)/length)<<5|(b+length/2)/length);
        assert(animated->blur[row*animated->w+x]==pixel);
    }
    puts("Weather: stopped icon recovered with exact sharp and scalar-filter pixels");
#elif APP_CASE == 2
    for(int frame=0;frame<24;frame++) {
        nos_t=1.1f+(float)frame/M_FPS;nos_frame++;
        ui_fblur_tick(&A,1,290.f/M_FPS,0,5,(A.nunits*AU+89)/90,BAND_H,20,446,1,band_draw,NULL);
    }
    /* All24ticks remain inside the active1.2second glint window. Progress,
     * rather than animation settling, must let the later static band work. */
    blur_item *visible=fb_find(4);
    printf("About: static_visible4_rows%d level%d\n",visible?visible->sharp_rows:-1,visible?visible->level:-1);
    fflush(stdout);
    assert(visible && visible->sharp_rows>0);
#else
#error Scheduling check supports Weather and About
#endif
    ui_fblur_release();puts("PASS actual animated ticks allow later static cache progress");
    return 0;
}
