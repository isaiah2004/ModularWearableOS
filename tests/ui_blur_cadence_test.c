/* Physical-speed ladders and actual retained RGB565 output across cadences. */
#include "../nos/ui_blur.c"
#include <assert.h>
#include <stdlib.h>

uint32_t nos_frame;
double nos_t; float nos_dt;
int nos_mblur=2;
uint32_t hal_micros(void) { return 0; }
void *hal_big_alloc(uint32_t n) { return calloc(1,n); }
const nos_screen *nos_top(void) { return 0; }
int nos_transitioning(void) { return 0; }
void nos_ink_dim(gfx_fb *fb,float k) { fb->dim=32-(int)((32-fb->dim)*k+0.5f); }

#define W 32
#define H 48
static const char owner;
static unsigned digest=2166136261u;
static uint16_t color(int row) {
    static const uint16_t colors[]={0xf800,0x07e0,0x001f,0xffff,0x8410,0xffe0,0x0000};
    return colors[(row+98)%7];
}
static void row_draw(gfx_fb *fb,int i,int x0,int y,int x1,int h,void *ctx) {
    (void)i;(void)ctx;
    for(int r=-24;r<h+24;r++) gfx_fill_rect(fb,x0,y+r,x1-x0,1,color(r));
}
static uint16_t reference(int row,int length) {
    if(!length) return color(row);
    unsigned r=0,g=0,b=0;
    for(int j=0;j<length;j++) {
        unsigned c=color(row-length/2+j);
        r+=c>>11;g+=(c>>5)&63;b+=c&31;
    }
    return (uint16_t)(((r+length/2)/length)<<11|((g+length/2)/length)<<5|(b+length/2)/length);
}
static float normalized(float speed,int skipped) {
    float elapsed=(float)skipped/M_FPS;
    float delta=speed*elapsed;
    return delta/(elapsed*M_FPS); /* same contract as actual scrolling callers */
}
static void ladders(void) {
    const float public_edges[]={360,840,1680};
    const float early_edges[]={150,300,960,1920};
    const float cache_edges[]={15,150,720,1800};
    for(int sign=-1;sign<=1;sign+=2) {
        assert(!ui_fblur_level(0) && !ui_fblur_level_early(0) && !fb_level(0));
        for(int i=0;i<3;i++) {
            assert(ui_fblur_level(sign*(public_edges[i]-.25f)/M_FPS)==i);
            assert(ui_fblur_level(sign*public_edges[i]/M_FPS)==i+1);
        }
        for(int i=0;i<4;i++) {
            assert(ui_fblur_level_early(sign*(early_edges[i]-.25f)/M_FPS)==i);
            assert(ui_fblur_level_early(sign*early_edges[i]/M_FPS)==i+1);
            assert(fb_level(sign*(cache_edges[i]-.25f)/M_FPS)==i);
            assert(fb_level(sign*cache_edges[i]/M_FPS)==i+1);
        }
    }
}
static void warm_raster(float speed,int skipped,int sign,int mode,int held,int early,
                        int public_level,int early_level,int cache_level) {
    ui_fblur_release();
    float step=normalized(sign*speed,skipped);
    assert(ui_fblur_level(step)==public_level);
    assert(ui_fblur_level_early(step)==early_level);
    assert(fb_level(step)==cache_level);
    int requested=early?(early_level<3?early_level+1:4):cache_level;
    int selected=mode && !(held && mode<2)?requested:0;
    nos_mblur=mode;
    for(int tick=0;tick<16;tick++) {
        nos_frame+=(unsigned)skipped;
        nos_t+=(float)skipped/M_FPS;
        /* Selection must not depend on a second, unrelated global dt. */
        nos_dt=tick&1?.731f:.001f;
        ui_fblur_tick(&owner,held,step,0,0,1,H,0,W,1|(early?UI_FBLUR_EARLY:0),row_draw,0);
    }
    assert(FB_view.level==selected && ui_fblur_active(&owner)==(selected!=0));
    assert(ui_fblur_ready(&owner,0,0));
    blur_item *item=fb_find(0);assert(item);
    if(selected) assert(item->level==selected);
    uint16_t pixels[W*H];memset(pixels,0,sizeof pixels);
    gfx_fb fb={0};fb.px=pixels;fb.stride=W;fb.cx1=W;fb.h=H;
    assert(ui_fblur_draw_at(&fb,&owner,0,0,0,1));
    int length=selected?blur_cache_length(selected):0;
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        assert(pixels[y*W+x]==reference(y,length));
        digest=(digest^pixels[y*W+x])*16777619u;
    }
    int spans[2][2];
    assert(ui_fblur_row_spans(H/2,spans,2)==(selected!=0));
    if(selected) assert(spans[0][0]==0 && spans[0][1]==W);
}
int main(void) {
    ladders();
    const float speeds[]={150,290,900,1800};
    const int public_levels[]={0,0,2,3},early_levels[]={1,1,2,3},cache_levels[]={2,2,3,4};
    const int skipped[]={1,3,7};
    for(int i=0;i<4;i++) for(int k=0;k<3;k++)
    for(int sign=-1;sign<=1;sign+=2) for(int mode=0;mode<=2;mode++)
    for(int held=0;held<=1;held++) for(int early=0;early<=1;early++)
        warm_raster(speeds[i],skipped[k],sign,mode,held,early,
                    public_levels[i],early_levels[i],cache_levels[i]);
    ui_fblur_release();
    printf("PASS cadence-independent blur ladders, skipped ticks, held policy, RGB565 raster digest=%08x\n",digest);
    return 0;
}
