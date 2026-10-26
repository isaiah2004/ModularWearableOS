/* Exercise the real optional allocator and cache, with a tracked memory budget. */
#include "../nos/ui_blur.c"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t nos_frame;
double nos_t;
int nos_mblur=2;
static unsigned used,failures;
static const unsigned budget=6815744;
static const nos_screen *top;
static uint32_t test_clock_us;

const nos_screen *nos_top(void) {return top;}
int nos_transitioning(void) {return 0;}
uint32_t hal_micros(void) {return test_clock_us;}
void nos_ink_dim(gfx_fb *fb,float k) {fb->dim=32-(int)((32-fb->dim)*k+.5f);}
uint32_t nos_mem_available(void) {return used<budget?budget-used:0;}
typedef struct {size_t bytes;uint64_t alignment;} allocation;
void *nos_mem_alloc(uint32_t bytes,const char *owner)
{
    (void)owner;
    if(bytes>nos_mem_available()) {failures++;return NULL;}
    allocation *p=malloc(bytes+sizeof *p);
    assert(p);
    p->bytes=bytes;used+=bytes;
    return p+1;
}
void nos_mem_free(void *ptr)
{
    if(!ptr) return;
    allocation *p=(allocation *)ptr-1;
    used-=(unsigned)p->bytes;
    free(p);
}
static void row(gfx_fb *fb,int item,int x0,int y,int x1,int h,void *ctx)
{
    (void)ctx;
    gfx_fill_rect(fb,x0,y,x1-x0,h,(uint16_t)(0x8410+item));
}
static inline void run(const void *id,int first,int last,int width,int height)
{
    for(int frame=0;frame<240;frame++) {
        nos_frame++;
        ui_fblur_tick(id,0,1000.0f/M_FPS,first,last,16,height,0,width,1,row,NULL);
    }
}
static inline unsigned published(const void *id,int first,int last)
{
    unsigned count=0;
    for(int i=first;i<=last;i++) {
        blur_item *s=blur_cache_find(&FB,id,i,FB_view.W,FB_view.H,FB_view.C,0,0);
        count+=s && s->level!=0;
    }
    return count;
}

/* The target proof is also compiled separately with the actual Xtensa flags. */
typedef char blur_header_fits[sizeof(fb_storage)<=BC_HEADER?1:-1];
