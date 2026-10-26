#include "../nos/faces/orrery_card.c"
#include "card_stripe_reference.h"
#include <assert.h>
#include <time.h>
extern int sim_program_main(int, char **);
static uint16_t a[466 * 466 + 32], b[466 * 466 + 32];
static gfx_rec ra, rb;
int main(int argc, char **argv) {
    assert(!sim_program_main(argc, argv));
    assert(!orrery_card_profile_enabled());
    orrery_card_profile_set(1, 1);
    card_profile_token token = {hal_micros() - 32u, 77, 3, 2, 1};
    card_profile_end(CP_READ, token);
    assert(card_profile[CP_READ].calls == 1 && card_profile[CP_READ].max_us >= 32);
    assert(card_profile[CP_READ].max_frame == 77 && card_profile[CP_READ].max_ob == 3);
    orrery_card_profile_set(0, 0);
    assert(!orrery_card_profile_enabled());
    token.enabled = 0;
    card_profile_end(CP_READ, token);
    assert(card_profile[CP_READ].calls == 1);
    orrery_card_profile_set(0, 1);
    assert(!card_profile[CP_READ].calls);
    /* moons.bin is read through the asset worker (sim: synchronous): every body below sees the loaded block */
    for (int i = 0; i < 64 && od_moons_use(EP_SATURN) <= 0; i++) od_sd_pump();
    assert(od_moons_ready());
    C.mode = 0;
    C.load_stage = 3;
    body_state body;
    sprite empty = {0};
    unsigned cases = 0;
    clock_t old = 0, fast = 0, ot[3] = {0}, ft[3] = {0};
    void (*legacy[4])(gfx_fb *, const body_state *) = {0, page_heat_reference, page_moons_reference,
                                                       page_facts_reference};
    void (*opt[4])(gfx_fb *, const body_state *) = {0, page_heat, page_moons, page_facts};
    for (int ob = 0; ob < OB_N; ob++) {
        prepare(&body, ob, 2461000.5);
        prepare_extra(&body);
        for (int page = 0; page < 4; page++) {
            memset(&ra, 0, sizeof ra);
            memset(&rb, 0, sizeof rb);
            gfx_fb fa = {0}, fb = {0};
            fa.h = 16;
            fa.y_base = 220;
            fa.oy = -33;
            fa.ox = 17;
            fa.cx0 = 25;
            fa.cx1 = 425;
            fa.dim = 9;
            fa.rec = &ra;
            fb = fa;
            fb.rec = &rb;
            if (page) {
                legacy[page](&fa, &body);
                opt[page](&fb, &body);
            } else {
                page_globe_reference(&fa, &body, &empty, 0, 0);
                page_globe(&fb, &body, &empty, 0, 0);
            }
            assert(!memcmp(&ra, &rb, sizeof ra));
        }
        for (int page = 0; page < 4; page++)
            for (int shift = -33; shift <= 33; shift += 33)
                for (int dim = 0; dim <= 9; dim += 9)
                    for (int band = 0; band < 3; band++) {
                        int h = band == 0 ? 1 : band == 1 ? 16 : 466;
                        for (int y = 0; y < 466; y += h) {
                            memset(a, 0x5a, sizeof a);
                            memset(b, 0x5a, sizeof b);
                            gfx_fb fa = {0}, fb = {0};
                            fa.px = a + 16;
                            fa.h = h < 466 - y ? h : 466 - y;
                            fa.y_base = y;
                            fa.oy = shift;
                            fa.ox = 17;
                            fa.cx0 = 25;
                            fa.cx1 = 425;
                            fa.dim = dim;
                            fb = fa;
                            fb.px = b + 16;
                            clock_t t = clock();
                            if (page)
                                legacy[page](&fa, &body);
                            else
                                page_globe_reference(&fa, &body, &empty, 0, 0);
                            ot[band] += clock() - t;
                            old += clock() - t;
                            t = clock();
                            if (page)
                                opt[page](&fb, &body);
                            else
                                page_globe(&fb, &body, &empty, 0, 0);
                            ft[band] += clock() - t;
                            fast += clock() - t;
                            assert(!memcmp(a, b, sizeof a));
                            cases++;
                        }
                    }
    }
    printf("PASS %u original/candidate translated clipped dimmed page/stripe images and canaries; "
           "row guards add no raster storage. HOST aggregate old %.3fms fast %.3fms\n",
           cases, 1000. * old / CLOCKS_PER_SEC, 1000. * fast / CLOCKS_PER_SEC);
    for (int k = 0; k < 3; k++)
        printf("HOST band %d old %.3fms fast %.3fms\n", k, 1000. * ot[k] / CLOCKS_PER_SEC,
               1000. * ft[k] / CLOCKS_PER_SEC);
    return 0;
}
