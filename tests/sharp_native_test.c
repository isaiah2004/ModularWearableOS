#include "../nos/apps/native_tile.h"
#include "../nos/render_history.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static unsigned allocations;
void *hal_big_alloc(uint32_t n){allocations++;return malloc(n);}
void hal_big_free(void *p){free(p);}
int main(void) {
    assert(nos_history_init());assert(nos_history_storage_bytes()==0 && allocations==0);
    uint8_t pixels[35],other[35];uint16_t lut[256],actual[466*8];
    for(unsigned i=0;i<35;i++){pixels[i]=(uint8_t)(i+1);other[i]=(uint8_t)(100+i);}
    for(unsigned i=0;i<256;i++)lut[i]=(uint16_t)(i*193u);
    sprite8 a={pixels,7,5},b={other,7,5};
    for(int base=0;base<8;base++)for(int pose=-220;pose<=220;pose+=110)for(int mix=0;mix<=256;mix+=64) {
        memset(actual,0,sizeof actual);
        gfx_fb fb={0};fb.px=actual;fb.h=8;fb.y_base=base;fb.cx0=2;fb.cx1=100;
        native_prepared_blit(&fb,&a,4,3,&b,6,4,(unsigned)mix,lut,7,pose,4,7,1,NULL,0);
        for(int y=base;y<base+8;y++)for(int x=0;x<466;x++) {
            uint16_t expected=(x>=4&&x<11&&y>=4&&y<7)?lut[pixels[(y-3)*7+x-4]]:0;
            assert(actual[(y-base)*466+x]==expected);
        }
    }
    memset(actual,0,sizeof actual);gfx_fb fb={0};fb.px=actual;fb.h=8;fb.cx1=466;
    native_tile_blit(&fb,&a,&b,64,4,1,lut,9,177,1000,0,8);
    for(int y=0;y<8;y++)for(int x=0;x<466;x++)assert(actual[y*466+x]==((x>=4&&x<11&&y>=1&&y<6)?lut[other[(y-1)*7+x-4]]:0));
    puts("PASS sharp sources, integer sampling, authored clipping, no history allocation");return 0;
}
