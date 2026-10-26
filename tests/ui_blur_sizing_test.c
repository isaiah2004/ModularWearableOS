#include "ui_blur_test_support.h"

static void capacity_cases(void)
{
 static int owner;
 const unsigned occupied[]={3000000,5051330,5051330-440720};
 const unsigned expected[]={7,5,7};
 for(unsigned i=0;i<sizeof occupied/sizeof occupied[0];i++) {
  used=occupied[i];run(&owner,0,6,418,90);
  assert(published(&owner,0,6)==expected[i]);
  assert(FB.stage_blocks==19 && nos_mem_available()>=512u*1024u);
  printf("occupied=%u arena_total=%u published=%u stage_blocks=%d reserve=%u\n",
         occupied[i],FB_alloc_bytes,published(&owner,0,6),FB.stage_blocks,nos_mem_available());
  for(int j=0;j<7;j++) {
   blur_item *s=fb_find(j);
   if(s) assert(s->blocks==29 && s->blur_blocks==19);
  }
  ui_fblur_release();assert(used==occupied[i]);
 }
 assert(!failures);
 puts("PASS initial-stage reservation and observed-budget capacity without HOME reclamation");
}

int main(void)
{
    static int a,b;
    static const nos_screen native={.flags=SCR_NATIVE_CACHE};
    /* Reject invalid height before metadata/payload allocation or arithmetic. */
    ui_fblur_tick(&a,0,25,0,5,16,INT_MAX,0,418,1,row,NULL);
    assert(!FB_storage && !FB_payload && used==0);
    assert(fb_payload_target(466,INT_MAX,24,0,15,16)==BC_BYTES-BC_HEADER);
    assert(fb_payload_target(32,32,24,0,0,16)==BC_DEFAULT_BYTES-BC_HEADER);

    used=3000000;
    run(&a,0,5,418,90);
    /* Independent layout: sharp418*(90+48)*2 ->29 blocks, published
     * 418*90*2 ->19 blocks; eight items plus one shared19-block stage. */
    assert(FB_alloc_bytes==BC_HEADER+(8u*48u+19u)*BC_BLOCK);
    assert(published(&a,0,5)==6 && nos_mem_available()>=512u*1024u);
    run(&a,1,7,418,90);
    assert(published(&a,1,7)==7);
    ui_fblur_release();assert(used==3000000);

    /* Reserve admission can fit fewer than the visible set. Do not evict a
     * published visible item just to rotate the next cold item into its place. */
    used=5051330;
    run(&a,0,5,418,90);
    assert(published(&a,0,5)==5 && nos_mem_available()>=512u*1024u);
    blur_item *saved=fb_find(0);
    assert(saved && saved->level);
    unsigned generation=saved->generation;
    uint16_t pixel=saved->blur[0];
    run(&a,0,5,418,90);
    assert(saved->owner==&a && saved->generation==generation);
    assert(saved->level && saved->blur[0]==pixel && published(&a,0,5)==5);
    ui_fblur_release();

    /* A small first owner retains its allocation when a larger owner follows. */
    used=3000000;
    run(&b,0,0,32,32);
    unsigned bytes=FB_alloc_bytes;
    void *payload=FB_payload;
    assert(bytes==BC_DEFAULT_BYTES);
    run(&a,0,5,418,90);
    assert(FB_payload==payload && FB_alloc_bytes==bytes);
    assert(published(&a,0,5)==4);
    ui_fblur_release();

    /* More memory returning mid-gesture does not replace a published arena. */
    used=5400000;
    run(&b,0,0,418,90);
    saved=fb_find(0);assert(saved && saved->level);
    generation=saved->generation;pixel=saved->blur[0];
    bytes=FB_alloc_bytes;payload=FB_payload;
    used-=1000000;
    run(&b,0,5,418,90);
    assert(FB_payload==payload && FB_alloc_bytes==bytes);
    assert(saved->owner==&b && saved->generation==generation);
    assert(saved->level && saved->blur[0]==pixel && published(&b,0,5)==3);

    /* Existing native-cache lifecycle releases payload; the next owner can
     * recompute its demand. This is not a resize of a live publication. */
    top=&native;ui_fblur_idle();assert(!FB_payload);
    top=NULL;used=3000000+BC_HEADER;
    run(&a,0,5,418,90);
    assert(published(&a,0,5)==6);
    ui_fblur_release();

    /* Reserve denial is optional admission, not a permanent allocation error. */
    used=budget-BC_HEADER-512u*1024u;
    run(&a,0,5,418,90);
    assert(FB_storage && !FB_payload && !FB_alloc_failed);
    used-=2000000;
    run(&a,0,5,418,90);
    assert(FB_payload && published(&a,0,5)==6);
    ui_fblur_release();assert(!failures);
    capacity_cases();
    puts("PASS demand sizing, reserve, pinning, publication and lifecycle retry");
    return 0;
}
