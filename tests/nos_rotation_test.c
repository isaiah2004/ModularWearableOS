#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "board_protocol.h"
typedef void (*rotate_fn)(const uint16_t*,int,uint16_t*,int,int,int);
static void baseline(const uint16_t *p,int stride,uint16_t *out,int w,int h,int swap) {
    board_pixels_rotation_mode_set(0);
    assert(board_pixels_rotation_mode_get()==0);
    board_pixels_ccw_swap(p,stride,out,w,h,swap);
}
static void paired(const uint16_t *p,int stride,uint16_t *out,int w,int h,int swap) {
    board_pixels_rotation_mode_set(1);
    assert(board_pixels_rotation_mode_get()==1);
    board_pixels_ccw_swap(p,stride,out,w,h,swap);
}
static void columns(const uint16_t *p,int stride,uint16_t *out,int w,int h,int swap) {
    board_pixels_rotation_mode_set(2);assert(board_pixels_rotation_mode_get()==2);
    board_pixels_ccw_swap(p,stride,out,w,h,swap);
}
static rotate_fn functions[]={baseline,paired,columns};
static const char *names[]={"scalar","paired","columns"};
static uint32_t rng=0x12345678u;
static uint32_t random32(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void one(int w,int h,int stride,int so,int dest_offset,int pattern,int swap)
{
    int absstride=stride<0?-stride:stride;
    size_t in_count=64+(size_t)absstride*(h?h:1)+(size_t)w;
    size_t out_count=64+(size_t)w*h;
    uint16_t *input=malloc(in_count*2),*backup=malloc(in_count*2),*actual=malloc(out_count*2),*expected=malloc(out_count*2);
    assert(input&&backup&&actual&&expected);
    for(size_t i=0;i<in_count;i++)input[i]=(uint16_t)random32();
    uint16_t *src=input+16+so+(stride<0?(h?h-1:0)*absstride:0);
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        uint16_t c=pattern==0?0:pattern==1?65535:pattern==2?0x1234:pattern==3?(uint16_t)((x/32)*0x1241):pattern==4?(uint16_t)random32():0;
        if(pattern==5 && x%32==31 && y==15)c=0xAA55;
        if(pattern==6 && (x%47<9 && y%7<3))c=0xBDF7;
        src[y*stride+x]=c;
    }
    memcpy(backup,input,in_count*2);
    for(size_t i=0;i<out_count;i++)expected[i]=0xCAFE;
    uint16_t *dst=expected+16+dest_offset;
    /* Independent destination-coordinate oracle, opposite loop order. */
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        uint16_t c=src[y*stride+x];
        dst[(w-1-x)*h+y]=swap?(uint16_t)((c>>8)|(c<<8)):c;
    }
    for(unsigned i=0;i<sizeof functions/sizeof functions[0];i++) {
        for(size_t k=0;k<out_count;k++)actual[k]=0xCAFE;
        functions[i](src,stride,actual+16+dest_offset,w,h,swap);
        if(memcmp(actual,expected,out_count*2)) {
            fprintf(stderr,"FAIL%s w%d h%d stride%d src%d dst%d pattern%d swap%d\n",names[i],w,h,stride,so,dest_offset,pattern,swap);abort();
        }
        assert(!memcmp(input,backup,in_count*2));
    }
    free(input);free(backup);free(actual);free(expected);
}
static void guard_pages(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    unsigned char *a=VirtualAlloc(0,page*6,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
    unsigned char *b=VirtualAlloc(0,page*6,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);
    assert(a&&b);DWORD old;
    assert(VirtualProtect(a+page,page*4,PAGE_READWRITE,&old));
    assert(VirtualProtect(b+page,page*4,PAGE_READWRITE,&old));
    uint16_t *src=(uint16_t*)(a+page*5-466*16*2),*dst=(uint16_t*)(b+page*5-466*16*2);
    for(int pattern=0;pattern<2;pattern++) {
        for(int i=0;i<466*16;i++)src[i]=pattern?(uint16_t)random32():0x1234;
        for(unsigned fn=0;fn<sizeof functions/sizeof functions[0];fn++)for(int swap=0;swap<2;swap++) {
            functions[fn](src,466,dst,466,16,swap);
            for(int x=0;x<466;x++)for(int y=0;y<16;y++) {
                uint16_t c=src[y*466+x];
                assert(dst[(465-x)*16+y]==(swap?(uint16_t)((c>>8)|(c<<8)):c));
            }
        }
    }
    assert(VirtualFree(a,0,MEM_RELEASE));assert(VirtualFree(b,0,MEM_RELEASE));
#endif
}
int main(void)
{
    assert(board_pixels_rotation_mode_get()==2);
    int widths[]={0,1,2,3,4,15,16,17,31,32,33,63,64,65,127,232,465,466};
    unsigned checks=0;
    for(unsigned k=0;k<sizeof widths/sizeof widths[0];k++)for(int h=0;h<=33;h++)for(int pad=0;pad<4;pad++)for(int off=0;off<4;off++)for(int swap=0;swap<2;swap++) {
        int w=widths[k],stride=w+pad;
        one(w,h,stride,off,(off+1)%4,(k+h+pad+off)%7,swap);checks++;
        if(w && h && !(checks%17)){one(w,h,-stride,off,off,6,swap);checks++;}
    }
    printf("PASS %u coordinate/canary/source-integrity cases x3 production modes\n",checks);
    guard_pages();puts("PASS source/destination inaccessible end-page guards, mixed and uniform, swap0/1");
    board_pixels_rotation_mode_set(0);
    return 0;
}
