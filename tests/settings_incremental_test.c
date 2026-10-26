#include "../nos/apps/set_settings.c"
#include <windows.h>
#include <assert.h>
int settings_sim_main(int argc,char **argv);
#include "settings_bake_reference.h"
int main(int argc,char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    nos_native_menu_rom_set(0); /* explicit authored runtime-bake reference mode */
    assert(settings_sim_main(argc,argv)==0);
    assert(rows_init()); while(!RC->lut_ready)rows_palette_step();
    for(int e=0;e<NE;e++) {
        if(RC->job.active)row_cancel();
        RC->entry[e].sig^=1;assert(entry_bake_ref(e));
        entry_cache expected=RC->entry[e];
        for(int k=0;k<9;k++) {
            int bytes=expected.image[k].s.w*expected.image[k].s.h;
            if(bytes) {
                expected.image[k].s.px=malloc((size_t)bytes);assert(expected.image[k].s.px);
                memcpy(expected.image[k].s.px,RC->entry[e].image[k].s.px,(size_t)bytes);
            }
        }
        uint8_t *old=RC->entry[e].image[0].s.px;
        RC->entry[e].sig^=1;row_start(e);
        int steps=0;
        while(RC->job.active) {
            assert(RC->entry[e].image[0].s.px==old);
            row_step_job();assert(++steps<2000);
        }
        assert(RC->entry[e].ready && RC->entry[e].sig==entry_sig(e));
        for(int k=0;k<9;k++) {
            entry_image *a=&RC->entry[e].image[k],*b=&expected.image[k];
            assert(a->x==b->x && a->y==b->y && a->s.w==b->s.w && a->s.h==b->s.h);
            int bytes=a->s.w*a->s.h;
            assert(!bytes || !memcmp(a->s.px,b->s.px,(size_t)bytes));
            free(b->s.px);
        }
    }
    /* Admission leaves in-flight state retryable without touching the allocator. */
    row_start(2);while(RC->job.phase!=2)row_step_job();
    unsigned failures=nos_mem_failures();nos_mem_max_request(1);
    for(int i=0;i<1000;i++)row_step_job();
    assert(RC->job.active && RC->job.phase==2 && nos_mem_failures()==failures);
    nos_mem_max_request(0);while(RC->job.active)row_step_job();
    /* Failed staging preserves published images and cools down after its own frees. */
    RC->entry[2].sig^=1;row_start(2);while(RC->job.phase!=2)row_step_job();
    uint8_t *published=RC->entry[2].image[0].s.px;
    nos_mem_fail_after(0);row_step_job();assert(!RC->job.active);
    assert(nos_mem_failures()==failures+1 && RC->entry[2].image[0].s.px==published);
    for(int i=0;i<1000;i++)rows_work(0);
    assert(!RC->job.active && nos_mem_failures()==failures+1);
    nos_t+=1.0f;rows_work(0);assert(RC->job.active);
    row_cancel();
    /* A one-step call cannot drain a whole entry. */
    set_settings_bake_max_steps=1;rows_work(0);
    assert(RC->job.active && RC->job.phase==0 && RC->job.row==ROW_SLICE);
    row_cancel();set_settings_bake_max_steps=16;
    /* Invalidation cancels unpublished work without touching old published data. */
    row_start(NE-1);for(int i=0;i<35;i++)row_step_job();
    assert(RC->job.active);
    uint8_t *old=RC->entry[NE-1].image[0].s.px;
    RC->job.sig^=1;row_step_job();assert(!RC->job.active);
    assert(RC->entry[NE-1].image[0].s.px==old);
    row_start(1);for(int i=0;i<80;i++)row_step_job();
    unsigned used=nos_mem_used(),bytes=row_bytes;
    set_settings_cache_release();assert(!RC && !row_bytes && nos_mem_used()==used-bytes);
    puts("PASS incremental Settings: all36 entries x7 images match independent causal reference, atomic publication, invalidation, release");
    return 0;
}
