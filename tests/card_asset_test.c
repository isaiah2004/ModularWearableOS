#include "../nos/faces/orrery_card.c"
#include <assert.h>
extern int sim_program_main(int, char **);
extern void hal_asset_sim_delay(int);
static uint32_t card_bytes(void) {
    nos_mem_stat stat;
    for (unsigned i = 0; i < nos_mem_stats(&stat, i); i++)
        if (!strcmp(stat.owner, "orrery_card.c"))
            return stat.bytes;
    return 0;
}
static void finish(void) {
    for (int i = 0; i < 160 && hal_asset_busy(); i++)
        orrery_card_asset_poll();
    assert(!hal_asset_busy());
}
static void begin(int ob) {
    release_pending = 0;
    assert(allocate());
    body_reset(ob);
}
int main(int argc, char **argv) {
    assert(!sim_program_main(argc, argv));
    hal_asset_sim_delay(2);
    begin(OB_EARTH);
    load_tick();
    assert(hal_asset_busy() && asset_pending_generation && !C.texture[0] && !C.tex_ok);
    for (int i = 0; i < 10; i++)
        orrery_card_asset_poll();
    assert(!C.texture[0] && !C.tex_ok);
    unsigned old = asset_pending_generation;
    release_buffers();
    assert(!C.b && hal_asset_busy() && card_bytes() == TEX_BYTES);
    begin(OB_MARS);
    assert(!C.texture[0]);
    load_tick();
    assert(asset_generation != old);
    finish();
    if (!C.tex_ok) {
        load_tick();
        finish();
    }
    assert(C.tex_ok && C.texture[0] && C.load_ob == OB_MARS);
    for (unsigned p = 0; p < TEX_PAGES; p++)
        for (unsigned i = 0; i < TEX_PAGE_BYTES / 2; i++)
            assert(C.texture[p][i] == 0x3333);
    release_buffers();
    finish();
    assert(!card_bytes());
    begin(OB_EARTH);
    load_tick();
    finish();
    assert(C.tex_ok);
    load_tick();
    assert(hal_asset_busy() && !C.night[0] && !C.night_ok);
    release_buffers();
    assert(card_bytes() == TEX_BYTES);
    hal_asset_cancel();
    hal_asset_wait();
    orrery_card_asset_poll();
    assert(!hal_asset_busy() && !card_bytes());
    begin(OB_EARTH);
    load_tick();
    finish();
    assert(C.tex_ok);
    load_tick();
    finish();
    assert(C.night_ok && C.night[0] && C.load_stage == 2);
    for (unsigned p = 0; p < TEX_PAGES; p++)
        for (unsigned i = 0; i < TEX_PAGE_BYTES / 2; i++)
            assert(C.night[p][i] == 0x2222);
    assert(asset_status.code == ASSET_OK && asset_status.ob == OB_EARTH &&
           asset_status.stage == 1 && asset_status.adopted);
    release_buffers();
    finish();
    assert(!card_bytes());
    begin(OB_SATURN);
    load_tick();
    finish();
    load_tick();
    load_tick();
    assert(hal_asset_busy() && !C.ring_tex);
    finish();
    assert(C.ring_ok && C.ring_tex && C.ring_tex[0] == 0x77);
    release_buffers();
    finish();
    assert(!card_bytes());
    begin(OB_SUN);
    load_tick();
    finish();
    assert(!C.tex_ok && C.texture[0] && C.load_stage == 1);
    release_buffers();
    finish();
    assert(!card_bytes());
    printf("PASS async card: unpublished pages, delayed exit/reentry generation, exact assets, "
           "night/ring cancellation, off-style reaping, shutdown drain, missing asset, complete "
           "release\n");
}
