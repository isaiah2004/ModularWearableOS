#include "ui_blur_test_support.h"

static void motion_tick(const void *id,int first,int last,float speed)
{
 unsigned before=FB_storage?FB.work_pixels:0;
 nos_frame++;
 ui_fblur_tick(id,0,speed/M_FPS,first,last,16,90,0,418,1,row,NULL);
 assert(FB.work_pixels-before<=6u*BC_SLICE*418u);
}
static void publication_oracle(const blur_item *s)
{
 if(!s || !s->level) return;
 static const int lengths[]={2,8,16,40};
 int length=lengths[s->level-1];
 unsigned p=(uint16_t)(0x8410+s->item);
 for(int y=0;y<90;y++) {
  unsigned count=0;
  for(int k=0;k<length;k++) count+=(unsigned)(y+k-length/2)<90;
  unsigned r=((p>>11)*count+length/2)/(unsigned)length;
  unsigned g=(((p>>5)&63)*count+length/2)/(unsigned)length;
  unsigned b=((p&31)*count+length/2)/(unsigned)length;
  uint16_t expected=(uint16_t)(r<<11|g<<5|b);
  for(int x=0;x<418;x++) assert(s->blur[y*418+x]==expected);
 }
}
static void check_all_publications(void)
{
 for(int i=0;i<BC_SLOTS;i++) if(FB.slots[i].blocks) publication_oracle(FB.slots+i);
}
static int at_level(int first,int last,int level)
{
 for(int i=first;i<=last;i++) {
  blur_item *s=fb_find(i);
  if(!s || s->level!=level) return 0;
 }
 return 1;
}
static unsigned settle(const void *id,int first,int last,float speed,int level)
{
 unsigned frames=0;
 do {
  motion_tick(id,first,last,speed);check_all_publications();
  assert(++frames<160);
 } while(!at_level(first,last,level));
 return frames;
}
int main(void)
{
 static int owner;
 used=3000000;
 unsigned warm=settle(&owner,0,5,290,2);
 /* Include the two nearest speculative incoming bands before starting motion. */
 for(unsigned i=0;i<100;i++) motion_tick(&owner,0,5,290);
 assert(at_level(0,7,2));
 void *payload=FB_payload;
 unsigned bytes=FB_alloc_bytes;
 unsigned incoming=0;
 /* Row8 was cold. Old visible publications remain usable until the new row
  * has its first complete shutter, before their level2->4 refinement starts. */
 do {
  motion_tick(&owner,2,8,1800);check_all_publications();
  for(int i=2;i<=7;i++) assert(fb_find(i) && fb_find(i)->level==2);
  assert(++incoming<40);
 } while(!fb_find(8) || !fb_find(8)->level);
 assert(fb_find(8)->level==4);
 unsigned refine=settle(&owner,2,8,1800,4);
 unsigned reverse=settle(&owner,0,6,-900,3);
 for(unsigned i=0;i<12;i++) {
  motion_tick(&owner,0,6,i&1?1800:-900);
  assert(published(&owner,0,6)==7);
  check_all_publications();
 }
 /* A cold tall band must publish even if the demanded shutter changes on
  * every frame. No completed snapshot may be needed to escape this cycle. */
 ui_fblur_invalidate(&owner,0,0);
 unsigned oscillating=0;
 do {
  motion_tick(&owner,0,6,oscillating&1?1800:-900);
  check_all_publications();
  assert(++oscillating<40);
 } while(!fb_find(0) || !fb_find(0)->level);
 assert(FB_payload==payload && FB_alloc_bytes==bytes);
 unsigned after_thresholds=settle(&owner,0,6,1800,4);
 motion_tick(&owner,0,6,0);
 assert(!ui_fblur_active(&owner) && published(&owner,0,6)==7);
 check_all_publications();
 assert(FB_payload==payload && FB_alloc_bytes==bytes && !failures);
 printf("PASS actual UI warm=%u incoming=%u refine=%u reverse=%u thresholds=%u; cold-visible priority, bounded6slice work, every published pixel, no arena resize\n",
        warm,incoming,refine,reverse,after_thresholds);
 ui_fblur_release();assert(used==3000000);
 return 0;
}
