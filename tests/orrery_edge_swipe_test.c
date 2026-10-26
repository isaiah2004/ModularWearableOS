/* Actual Orrery input handler plus the normal HOME/input engine. No device I/O. */
#ifdef _WIN32
#include <windows.h>
#undef near
#undef far
#endif
#include "../nos/faces/face_orrery.c"
#include "apps/sys_launcher_page.h"
#include "nos_sim.h"

extern int sim_program_main(int, char **);
static unsigned checks;
#define CHECK(v) do { checks++; if (!(v)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #v); exit(2); \
} } while (0)

static void reset_controls(void)
{
    S.locked = 0; S.lockk = 0; S.drag = S.hold = S.time_touch = 0;
    S.rings_open = S.rings_pull = S.menu_open = S.menu_pull = 0;
    S.ringk = S.ringe = S.menuk = S.menue = S.subk = S.sube = 0;
    S.menu_sub = -1; S.nmh = 0; S.off = S.vel = 0; S.ta_n = 0;
    S.now_pill = 0; S.chip[0] = 0; S.platee = 0; S.d_ring = -1;
    S.drag_mode = DR_PAN; S.view = V_FACE; S.from = -1; S.tx = 1;
    S.zoom = 1; S.panx = S.pany = S.pvx = S.pvy = 0;
    s_target = s_edge = 0;
}
static nos_input down_at(int x, int y)
{
    nos_input in = {0};
    in.type = IN_DOWN; in.x = in.x0 = x; in.y = in.y0 = y;
    CHECK(face_orrery_input(&in));
    return in;
}
static nos_input down_polar(float angle, float radius)
{
    float x, y; polar_xy(angle, radius, &x, &y);
    return down_at((int)lroundf(x), (int)lroundf(y));
}
static void move(nos_input *in, int type, int dx, int dy)
{
    in->type = type; in->dx = dx; in->dy = dy;
    in->x = in->x0 + dx; in->y = in->y0 + dy;
    in->axis = abs(dx) >= abs(dy) ? AXIS_X : AXIS_Y;
    CHECK(face_orrery_input(in));
}
static void release(nos_input *in)
{
    in->type = IN_DRAG_END; CHECK(face_orrery_input(in));
    in->type = IN_UP; CHECK(face_orrery_input(in));
}
static void tap_at(int x, int y)
{
    nos_input in = down_at(x, y);
    in.type = IN_TAP; CHECK(face_orrery_input(&in));
    in.type = IN_UP; CHECK(face_orrery_input(&in));
}
static void check_closed(void) { CHECK(!S.rings_open && !S.menu_open); }

static void test_openers(void)
{
    for (int menu = 0; menu < 2; menu++) {
        float angle = menu ? 90 : TAB_PHI;
        int dx = menu ? -100 : -75, dy = menu ? 0 : 80;
        /* Edge taps and old broad interior hit areas never open a control. */
        for (int radius = 190; radius <= 232; radius += 2) {
            reset_controls();
            nos_input in = down_polar(angle, (float)radius);
            in.type = IN_TAP; CHECK(face_orrery_input(&in));
            in.type = IN_UP; CHECK(face_orrery_input(&in));
            check_closed();
        }
        /* Actual origin decides admission, including either side of 22 px rim. */
        for (int radius = 186; radius <= 240; radius++) {
            reset_controls();
            nos_input in = down_polar(angle, (float)radius);
            float rx = in.x0 - CX, ry = in.y0 - CY;
            int edge = rx * rx + ry * ry >= 211 * 211 && rx * rx + ry * ry <= 233 * 233;
            move(&in, IN_DRAG_START, dx / 4, dy / 4);
            move(&in, IN_DRAG, dx, dy);
            release(&in);
            CHECK((menu ? S.menu_open : S.rings_open) == edge);
            CHECK(!(menu ? S.rings_open : S.menu_open));
        }
        /* An outward/tangential initial drag stays rejected after changing direction. */
        const int wrong[4][2] = {{20, 0}, {0, -20}, {0, 20}, {20, 20}};
        for (int k = 0; k < 4; k++) {
            reset_controls();
            nos_input in = down_polar(angle, 225);
            move(&in, IN_DRAG_START, wrong[k][0], wrong[k][1]);
            move(&in, IN_DRAG, dx, dy); release(&in); check_closed();
        }
        /* Interior-origin drags cannot acquire an opener by visiting the edge. */
        reset_controls();
        nos_input in = down_polar(angle, 200);
        move(&in, IN_DRAG_START, 24, -24);
        move(&in, IN_DRAG, dx, dy); release(&in); check_closed();
        /* Short pulls and returning to the origin cancel. */
        for (int reverse = 0; reverse < 2; reverse++) {
            reset_controls(); in = down_polar(angle, 225);
            move(&in, IN_DRAG_START, dx / 4, dy / 4);
            move(&in, IN_DRAG, reverse ? dx : dx / 4, reverse ? dy : dy / 4);
            if (reverse) move(&in, IN_DRAG, 0, 0);
            release(&in); check_closed();
        }
        /* An UP without DRAG_END cancels even beyond the normal commit point. */
        reset_controls(); in = down_polar(angle, 225);
        move(&in, IN_DRAG_START, dx / 4, dy / 4);
        move(&in, IN_DRAG, dx, dy);
        in.type = IN_UP; CHECK(face_orrery_input(&in));
        CHECK(!S.menu_pull && !S.rings_pull && !S.drag && !s_edge); check_closed();
        S.ringk = S.ringe = S.menuk = S.menue = 0;
        move(&in, IN_DRAG_START, dx, dy); release(&in); check_closed();
    }
    puts("PASS edge openers: taps, radial boundary, direction lock, interior origin, reversal, cancellation");
}

static void test_ownership(void)
{
    const float phases[] = {.005f, .1f, .32f, .68f, .9f, 1};
    for (int closing = 0; closing < 2; closing++)
    for (unsigned phase = 0; phase < sizeof phases / sizeof phases[0]; phase++)
    for (int radius = 186; radius <= 232; radius += 2) {
        reset_controls(); S.rings_open = !closing; S.ringk = S.ringe = phases[phase];
        nos_input in = down_polar(90, (float)radius);
        CHECK(!s_edge && s_target != 7);
        move(&in, IN_DRAG_START, -20, 0);
        move(&in, IN_DRAG, -100, 0); release(&in);
        CHECK(!S.menu_open && !S.menu_pull && S.menuk == 0);
    }
    /* A previously eligible DOWN is not a license to open behind a new control. */
    for (int menu = 0; menu < 2; menu++) {
        reset_controls(); nos_input in = down_polar(menu ? 90 : TAB_PHI, 225);
        CHECK(s_edge == (menu ? 7 : 3));
        if (menu) { S.rings_open = 1; S.ringk = S.ringe = .4f; }
        else { S.menu_open = 1; S.menuk = S.menue = .4f; }
        move(&in, IN_DRAG_START, -25, menu ? 0 : 25);
        move(&in, IN_DRAG, -100, menu ? 0 : 100); release(&in);
        CHECK(!(menu ? S.menu_open : S.rings_open));
    }
    /* Closing menu keeps ownership until completely gone, too. */
    for (unsigned phase = 0; phase < sizeof phases / sizeof phases[0]; phase++) {
        reset_controls(); S.menuk = S.menue = phases[phase];
        nos_input in = down_polar(TAB_PHI, 225);
        move(&in, IN_DRAG_START, -25, 25); move(&in, IN_DRAG, -100, 100);
        release(&in); CHECK(!S.rings_open);
    }
    /* Existing visible ring bands retain both taps and tangent scrubbing. */
    for (int ring = 0; ring < 3; ring++) {
        reset_controls(); S.rings_open = 1; S.ringk = S.ringe = 1;
        nos_input in = down_polar(90, RING_R[ring]);
        CHECK(s_target == 6 && S.d_ring == ring);
        in.type = IN_TAP; CHECK(face_orrery_input(&in));
        CHECK(S.ta_n && !S.menu_open);
        in.type = IN_UP; CHECK(face_orrery_input(&in));
        in = down_polar(90, RING_R[ring]);
        move(&in, IN_DRAG_START, -2, -20); CHECK(S.drag == 4);
        move(&in, IN_DRAG, -10, -40); release(&in);
        CHECK(S.off != 0 && S.rings_open && !S.menu_open);
    }
    reset_controls(); S.rings_open = 1; S.ringk = S.ringe = 1;
    nos_input grip = down_polar(TAB_PHI, RING_R[0]);
    CHECK(s_target == 4);
    grip.type = IN_TAP; CHECK(face_orrery_input(&grip)); CHECK(!S.rings_open);
    reset_controls(); S.rings_open = 1; S.ringk = S.ringe = 1;
    grip = down_polar(TAB_PHI, RING_R[0]);
    move(&grip, IN_DRAG_START, -20, 20); CHECK(S.drag == 6);
    move(&grip, IN_DRAG, -100, 100); release(&grip); CHECK(!S.rings_open);

    /* Visible menu buttons and existing close interactions still work. */
    reset_controls(); S.menu_open = 1; S.menuk = S.menue = 1; menu_layout();
    int line = -1;
    for (int i = 0; i < S.nmh; i++) if (S.mhit[i].id == M_LINES && S.mhit[i].key < 0) line = i;
    CHECK(line >= 0); int old_lines = S.lines;
    tap_at((int)lroundf(S.mhit[line].x), (int)lroundf(S.mhit[line].y));
    CHECK(S.lines != old_lines && S.menu_open);
    tap_at(CX, CY); CHECK(!S.menu_open);
    reset_controls(); S.menu_open = 1; S.menuk = S.menue = 1; menu_layout();
    nos_input menu = down_at(430, 233);
    move(&menu, IN_DRAG_START, 20, 0); CHECK(S.drag == 8);
    move(&menu, IN_DRAG, 100, 0); release(&menu); CHECK(!S.menu_open);

    reset_controls(); menu = down_polar(90, 225);
    move(&menu, IN_DRAG_START, -25, 0); CHECK(S.menu_pull);
    menu.type = IN_BTN; CHECK(face_orrery_input(&menu));
    CHECK(S.locked && !s_edge && !S.menu_pull && !S.rings_pull);
    puts("PASS ownership: all dial phases block the menu, pending requests, rings, menu actions, close controls, lock");
}

static void raw_sample(int x, int y, int down)
{
    sim_vclock_ms += 25;
    nos_touch_at(x, y, down, (uint32_t)sim_vclock_ms);
    nos_frame_run(25);
}
static void raw_swipe(int x, int y, int dx, int dy)
{
    raw_sample(x, y, 1);
    for (int k = 1; k <= 8; k++) raw_sample(x + dx * k / 8, y + dy * k / 8, 1);
    raw_sample(x + dx, y + dy, 0);
}
static void test_input_engine(void)
{
    nos_home_now(); nos_prefs.face = 8; nos_aod = 0; nos_touch_predict = 0;
    reset_controls();
    raw_sample(458, 233, 1); raw_sample(450, 237, 1); raw_sample(450, 237, 0);
    check_closed(); CHECK(!S.drag);
    raw_swipe(458, 233, -120, 0); CHECK(S.menu_open && !S.rings_open);
    reset_controls();
    raw_sample(386, 68, 1); raw_sample(380, 73, 1); raw_sample(380, 73, 0);
    check_closed();
    raw_swipe(386, 68, -80, 90); CHECK(S.rings_open && !S.menu_open);
    for (int k = 0; k < 12; k++) { sim_vclock_ms += 25; nos_frame_run(25); }
    CHECK(S.ringe == 1);
    raw_swipe(458, 233, -120, 0); CHECK(S.rings_open && !S.menu_open);
    int page, dragging; float offset;
    sys_home_nav_snapshot(&page, &offset, &dragging);
    CHECK(page == 0 && offset == 0 && !dragging && nos_depth() == 1);
    puts("PASS real input engine: sub-slop touches, both edge swipes, dial/menu overlap, HOME ownership");
}

int main(int argc, char **argv)
{
    int rc = sim_program_main(argc, argv); if (rc) return rc;
    CHECK(s_orr && S.list);
    test_openers(); test_ownership(); test_input_engine();
    printf("PASS Orrery edge swipe regressions: %u checks\n", checks);
    return 0;
}
