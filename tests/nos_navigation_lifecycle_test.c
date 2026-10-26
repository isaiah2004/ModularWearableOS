/* Actual HOME lifecycle with an instrumented face, linked to simulator services. */
#include <assert.h>
#include <stdio.h>
#define nos_faces navigation_test_faces
#define nos_face_input navigation_test_face_input
#include "../nos/apps/sys_home.c"
#undef nos_faces
#undef nos_face_input
static unsigned ticks;
static face_ctx last_context;
static void test_tick(const face_ctx *c,float dt) { (void)dt; ticks++; last_context=*c; }
static const nos_face test_face={"TEST",0,test_tick,0,0};
const nos_face *const navigation_test_faces[]={&test_face,&test_face,&test_face,&test_face,&test_face,&test_face,&test_face,&test_face,&test_face};
int navigation_test_face_input(const nos_face *f,const nos_input *in) {
    (void)f; return in->type==IN_DOWN; /* locked Orrery consumes DOWN only */
}
extern int sim_program_main(int,char **);
static void settle(void) {
    for(int n=0;n<100;n++){nos_t+=.025f;home_tick(.025f);}
    assert(!H.dragging && !H.settling);
}
int main(int argc,char **argv) {
    assert(!sim_program_main(argc,argv));
    nos_prefs.face=1;nos_aod=0;
    const int physical_pages[]={2,3,4,5,6,0,1};
    for(int slot=FIRST_SLOT;slot<=LAST_SLOT;slot++) {
        int page=physical_pages[slot-FIRST_SLOT];
        assert(slot_page(slot)==page && page_slot(page)==slot);
        H.x=slot*PAGE_W;assert(nearest_page()==page);
    }
    H.x=PAGE_W;H.alias=0;H.dragging=H.settling=0;H.covered=0;
    H.apps_visible=0;home_enter();assert(H.apps_visible);
    nos_input down={0};down.type=IN_DOWN;down.x=233;down.y=233;
    assert(home_input(&down) && H.apps_touch);
    home_tick(.025f);assert(H.apps_touch && H.apps_visible);
    down.type=IN_UP;home_input(&down);
    unsigned before=ticks;
    for(int n=0;n<4800;n++) {nos_t+=.025f;home_tick(.025f);}
    assert(ticks==before); /* two simulated minutes hidden, zero face work */
    H.covered=1; /* outgoing Apps is still rendered underneath an app iris */
    home_tick(.025f);assert(ticks==before);
    H.covered=0;
    H.x=PAGE_W-1;nos_t+=.025f;home_tick(.025f);
    assert(ticks==before+1 && last_context.t==nos_t);
    H.x=0;H.dragging=H.grabbed=H.settling=H.apps_touch=0;
    nos_input in={0};in.type=IN_DOWN;in.x=110;in.y=233;
    assert(home_input(&in));assert(!H.grabbed);
    in.type=IN_DRAG_START;in.axis=AXIS_X;in.dx=-20;
    assert(home_input(&in));assert(H.grabbed&&H.dragging&&H.grab_page==0);
    in.type=IN_DRAG;in.dx=-300;assert(home_input(&in));assert(H.x==280);
    in.type=IN_DRAG_END;in.vx=-1000;assert(home_input(&in));
    settle();
    assert(nearest_page()==1 && H.x==PAGE_W && !H.dragging);
    /* Return button, then a rightward finger must reach the last original widget. */
    in.type=IN_BTN;assert(home_input(&in));settle();assert(at_face());
    in.type=IN_DOWN;assert(home_input(&in));
    in.type=IN_DRAG_START;in.axis=AXIS_X;in.dx=20;assert(home_input(&in));
    in.type=IN_DRAG;in.dx=300;assert(home_input(&in));assert(H.x==-280);
    in.type=IN_DRAG_END;in.vx=1000;assert(home_input(&in));settle();
    assert(nearest_page()==6 && H.x==-PAGE_W);
    /* Buttons from every widget return through one slot without reordering widgets. */
    for(int page=2;page<=6;page++) {
        H.x=page_slot(page)*PAGE_W;H.alias=0;
        in.type=IN_BTN;assert(home_input(&in));
        assert(H.alias==page && H.x==-PAGE_W);
        assert(slot_page(-1)==page && real_pos()==page_slot(page));
        settle();assert(at_face() && !H.alias && nearest_page()==0);
    }
    /* AOD collapses either side to face, cancels touch, and wakes on face. */
    for(int slot=FIRST_SLOT;slot<=LAST_SLOT;slot++) {
        H.x=slot*PAGE_W;H.last_aod=0;nos_aod=1;home_tick(.025f);
        assert(H.x==0 && nearest_page()==0 && !H.dragging && !H.grabbed && !H.alias);
        assert(H.aod_out==(slot==0));
        nos_aod=0;home_tick(.025f);assert(at_face());
    }
    puts("PASS navigation lifecycle: physical order, both swipe directions, buttons, AOD, hidden face refresh");
    return 0;
}
