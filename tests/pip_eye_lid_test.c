#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../core/apps/pip_eye_lid.h"
#include "pip_eye_lid_reference.h"
#define GUARD 32
#define CAP (GFX_W*GFX_H+2*GUARD)
static uint16_t old_pixels[CAP],new_pixels[CAP];
static unsigned seed=0x587aac31u;
static unsigned random_u(void)
{ seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
int main(void)
{
    for(int test=0;test<4500;test++) {
        float x=(int)(random_u()%70000)/100.0f-100;
        float y=(int)(random_u()%70000)/100.0f-100;
        float w=(random_u()%11000+1)/100.0f,h=(random_u()%11000+1)/100.0f;
        float anger=test%7==0?0:test%7==1?1:(random_u()%10001)/10000.0f;
        int ox=(int)(random_u()%201)-100,oy=(int)(random_u()%201)-100;
        int ybase=(int)(random_u()%GFX_H),rows=1+(int)(random_u()%64);
        if(rows>GFX_H-ybase)rows=GFX_H-ybase;
        if(test<100){x=158.37f;y=248.63f;w=51.43f;h=65.1f;ox=oy=ybase=0;rows=GFX_H;}
        int cx0=(int)(random_u()%500)-16,cx1=cx0+1+(int)(random_u()%GFX_W);
        if(test<100){cx0=0;cx1=GFX_W;}
        int count=rows*GFX_W+2*GUARD;
        for(int i=0;i<count;i++)old_pixels[i]=new_pixels[i]=(uint16_t)(i*37u^(i>>3)^0xa52du);
        gfx_fb a={.px=old_pixels+GUARD,.ox=ox,.oy=oy,.y_base=ybase,.h=rows,.cx0=cx0,.cx1=cx1};
        gfx_fb b=a;b.px=new_pixels+GUARD;
        int side=test&1?1:-1;
        pip_eye_lid_reference(&a,x,y,w,h,anger,side);
        pip_eye_lid(&b,x,y,w,h,anger,side);
        if(memcmp(old_pixels,new_pixels,(size_t)count*2)) {
            fprintf(stderr,"Lid mismatch case %d anger %g\n",test,anger);return 1;
        }
        for(int i=0;i<GUARD;i++) {
            assert(new_pixels[i]==(uint16_t)(i*37u^(i>>3)^0xa52du));
            int tail=count-GUARD+i;
            assert(new_pixels[tail]==(uint16_t)(tail*37u^(tail>>3)^0xa52du));
        }
        gfx_rec ra={0},rb={0};a.rec=&ra;b.rec=&rb;a.px=b.px=NULL;
        pip_eye_lid_reference(&a,x,y,w,h,anger,side);pip_eye_lid(&b,x,y,w,h,anger,side);
        assert(!memcmp(&ra,&rb,sizeof ra));
    }
    puts("PASS Pip lids: 4500 exact RGB565/reference comparisons, both slopes, fractional coverage, clips, translations, stripes, guards and damage records");
    return 0;
}
