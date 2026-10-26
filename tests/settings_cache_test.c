/* Link against the simulator with its main renamed to settings_sim_main, excluding
 * its set_settings.o: this TU exposes the entry cache to scalar/reference checks. */
#include "../nos/apps/set_settings.c"
#include <windows.h>

int settings_sim_main(int argc, char **argv);
/* Tests explicitly drain the bounded production state machine; app ticks never do. */
static int entry_bake(int e)
{
    if (!rows_init()) return 0;
    while (!RC->lut_ready) rows_palette_step();
    if (RC->entry[e].ready && RC->entry[e].sig == entry_sig(e)) return 1;
    row_cancel(); row_start(e);
    for (int n = 0; RC->job.active && n < 2000; n++) row_step_job();
    return !RC->job.active && RC->entry[e].ready && RC->entry[e].sig == entry_sig(e);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static int slow_scroll_continuity(uint16_t *pixels,uint16_t *previous)
{
    S.arm_e=-1;S.flash_e=-1;S.L.pressed=-1;S.L.dragging=1;S.L.sc.v=0;
    S.L.bar_alpha=0;S.build_t0=nos_t-100;
    tw_set(&S.arm_tw[0],0);tw_set(&S.arm_tw[1],0);tw_set(&S.grey_tw,0);
    for(int i=0;i<I_COUNT;i++){S.vt0[i]=nos_t-100;tw_set(&S.tog[i],S.tog[i].to);}
    float base=80;row_prev=base;
    gfx_fb fb={0};fb.px=pixels;fb.h=GFX_H;fb.cx1=GFX_W;
    float previous_off=-999;unsigned fractional_pose=0,moving=0,held=0;
    for(int frame=0;frame<120;frame++) {
        S.L.sc.x=base+frame*.25f;
        rows_tick(1.f/40);CHECK(row_level==0);
        moving+=row_step>0;held+=row_step==0;
        memset(pixels,0,GFX_W*GFX_H*sizeof *pixels);settings_draw(&fb);
        float off=list_off();
        /* Continuous placement changes fractional ink even while the rounded
         * layout coordinate stays fixed. It must no longer stall for3/4 frames. */
        if(frame && off==previous_off){CHECK(memcmp(pixels,previous,GFX_W*GFX_H*sizeof *pixels));fractional_pose++;}
        memcpy(previous,pixels,GFX_W*GFX_H*sizeof *pixels);previous_off=off;
    }
    CHECK(fractional_pose>=80 && moving>=25 && held>=80);
    S.L.dragging=0;
    printf("PASS: Settings fractional continuity %u changing subpixel frame pairs (%u rounded moving,%u rounded held)\n",fractional_pose,moving,held);
    return 0;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
#endif
    /* The simulator configures _set_abort_behavior for the available Windows CRTs
     * before booting. Checks here return errors instead of invoking assert/abort. */
    nos_native_menu_rom_set(0); /* explicit authored runtime-bake reference mode */
    CHECK(settings_sim_main(argc, argv) == 0);
    CHECK(RC && row_bytes < ROW_CACHE_LIMIT);
    for (int e = 0; e < NE; e++) CHECK(entry_bake(e));
    unsigned bakes = row_bakes;
    for (int e = 0; e < NE; e++) CHECK(entry_bake(e));
    CHECK(row_bakes == bakes);  /* unchanged signatures do no raster work */

    for (int e = 0; e < NE; e++) {
        RC->entry[e].sig ^= 1;  /* force one fresh source for the scalar comparison */
        CHECK(entry_bake(e));
        int h = units_of(E[e].kind) * UNIT;
        for (int k = 1; k < 7; k++) {
            entry_image *im = &RC->entry[e].image[k];
            int len = row_blur_len[(k - 1)%3], support=len/2, direction=k<=3?1:-1;
            CHECK(im->x >= 0 && im->x + im->s.w <= ROW_W);
            CHECK(im->y >= 0 && im->y + im->s.h <= h);
            for (int y = 0; y < h; y++) for (int x = 0; x < ROW_W; x++) {
                uint64_t sum = 0, total=0;
                for (int age=0;age<=support;age++) {
                    uint64_t remaining=(uint64_t)(support+1-age);
                    uint64_t weight=(age+1)*remaining*remaining*remaining;
                    int sy=y+direction*age;
                    total+=weight;
                    if (sy >= 0 && sy < h) sum += weight*RC->grey[sy * ROW_W + x];
                }
                int actual = x >= im->x && x < im->x + im->s.w &&
                             y >= im->y && y < im->y + im->s.h ?
                    im->s.px[(y - im->y) * im->s.w + x - im->x] : 0;
                CHECK((uint64_t)actual == (sum + total / 2) / total);
            }
        }
    }

    /* An armed Reset must keep its label/countdown during a fast fling instead of
     * using its cached plain pill. Compare its region against a direct live draw. */
    int e = NE - 1, h = units_of(E[e].kind) * UNIT;
    int y = UI_CY - h / 2;
    S.L.sc.x = (float)(LTOP + S.start[e] * UNIT - y);
    S.build_t0 = -100; S.arm_e = e; S.arm_t = nos_t;
    tw_set(&S.arm_tw[1], 1); row_level = 1; row_step = 20;
    entry_draw_prepare(); /* publish the synthetic armed state before drawing */
    uint16_t *actual = calloc(GFX_W * GFX_H, sizeof(uint16_t));
    uint16_t *expected = calloc(GFX_W * GFX_H, sizeof(uint16_t));
    CHECK(actual && expected);
    sprite a = { actual, GFX_W, GFX_H }, b = { expected, GFX_W, GFX_H };
    gfx_fb af = sprite_fb(&a), bf = sprite_fb(&b);
    settings_draw(&af);
    draw_entry(&bf, e, (float)y, (float)h, 0);
    for (int sy = y; sy < y + h; sy++) for (int x = UI_ROW_X0; x <= UI_ROW_X1; x++)
        CHECK(actual[sy * GFX_W + x] == expected[sy * GFX_W + x]);
    CHECK(!slow_scroll_continuity(actual,expected));
    free(actual); free(expected);

    unsigned used = nos_mem_used(), bytes = row_bytes;
    set_settings_cache_release();
    CHECK(!RC && !row_bytes && nos_mem_used() == used - bytes);
    puts("PASS: Settings scalar blur/crop, unchanged signatures, armed fling, complete release");
    return 0;
}
