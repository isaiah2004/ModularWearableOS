/* Independent full-image reference stays in card_raster_reference.h unchanged.
 * Force one tiny quantum per pump to expose unpublished stages/ownership. */
static unsigned progressive_tests(uint16_t *expected_pixels) {
    unsigned cases=0,used=nos_mem_used(),saved_cap=orrery_card_render_quanta;
    orrery_card_render_quanta=1;
    for(int ob=0;ob<OB_N;ob++)for(int angle=0;angle<3;angle++)
    for(int mode=0;mode<3;mode++)for(int free_mode=0;free_mode<2;free_mode++) {
        float lat=angle==0?-65:angle==1?0:35;int r=globe_radius(ob,lat);
        card_render_invalidate();C.preview=C.pending=C.anim_n=0;C.peek=0;release_pending=0;
        C.ob=ob;C.mode=mode;C.free=free_mode;C.spin=37.25f;C.tilt=7.5f;
        C.tex_ok=C.night_ok=C.ring_ok=1;prepare(C.b,ob,2460100.125);
        C.lut_r=0;while(!build_lut_reference(r,lat)){}
        if(ob==OB_SATURN)build_rings_reference(lat);
        sprite expected={expected_pixels,MAX_SIDE,236};render_globe_reference(&expected,C.b,C.spin,lat,1);
        card_render_invalidate();card_image_revision=100;C.key=100;C.globe.px[0]=0x5a5a;uint16_t *front=C.globe.px;
        assert(card_render_start(lat,1));
        unsigned steps=0,seen=0;
        C.dirty=1; /* later asset/clock invalidation must survive publication */
        while(R.stage!=CR_IDLE) {
            int phase=R.stage;seen|=1u<<phase;
            assert(C.globe.px==front && C.key==100 && C.globe.px[0]==0x5a5a);
            card_render_pump();assert(++steps<2000);
            if(R.stage!=CR_IDLE)assert(C.globe.px==front&&C.key==100);
        }
        assert((seen&(1u<<CR_LUT)) && (seen&(1u<<CR_CLEAR)) && (seen&(1u<<CR_SHADER)));
        if(mode==2)assert(seen&(1u<<CR_DOTMAX) && seen&(1u<<CR_DOT));
        if(ob==OB_SATURN)assert(seen&(1u<<CR_RING_CLEAR) && seen&(1u<<CR_RING) && seen&(1u<<CR_OVERLAY));
        assert(C.key==101 && C.dirty==1 && R.front_detail==1);
        assert(orrery_card_geometry_check()==0);
        assert(C.last_spin==37.25f && C.last_tilt==7.5f && C.last_mode==mode && C.last_free==free_mode);
        assert(C.globe.w==expected.w && C.globe.h==expected.h);
        assert(!memcmp(C.globe.px,expected_pixels,(size_t)expected.w*expected.h*2));
        assert(nos_mem_used()==used);cases++;
    }
    for(int detail=2;detail<=4;detail+=2)
    for(int ob=0;ob<OB_N;ob++)for(int angle=0;angle<3;angle++)for(int mode=0;mode<3;mode++) {
        float lat=angle==0?-65:angle==1?0:35;int r=globe_radius(ob,lat);
        card_render_invalidate();C.preview=C.pending=C.anim_n=0;C.peek=0;
        C.ob=ob;C.mode=mode;C.free=0;C.spin=17;C.tilt=0;
        prepare(C.b,ob,2460100.125);C.lut_r=0;
        while(!build_lut_motion_reference(r,lat,detail)){}
        if(ob==OB_SATURN)build_rings_motion_reference(lat,detail);
        sprite expected={expected_pixels,MAX_SIDE,236};
        render_globe_motion_reference(&expected,C.b,C.spin,lat,1,detail);
        card_render_invalidate();assert(card_render_start(lat,detail));unsigned steps=0;
        while(R.stage!=CR_IDLE){card_render_pump();assert(++steps<2000);}
        assert(R.front_detail==detail && expected.w==C.globe.w && expected.h==C.globe.h);
        assert(!memcmp(expected_pixels,C.globe.px,(size_t)expected.w*expected.h*2));
        /* Coarse geometry leaves holes: rebuilding exact must never reuse it. */
        C.lut_r=0;while(!build_lut_reference(r,lat)){}
        if(ob==OB_SATURN)build_rings_reference(lat);
        render_globe_reference(&expected,C.b,C.spin,lat,1);
        assert(card_render_start(lat,1));steps=0;
        while(R.stage!=CR_IDLE){card_render_pump();assert(++steps<2000);}
        assert(R.front_detail==1 && !memcmp(expected_pixels,C.globe.px,(size_t)expected.w*expected.h*2));
    }
    /* Asset adoption during a job must not mix old/new texture availability. */
    card_render_invalidate();C.ob=OB_EARTH;C.mode=C.free=0;C.spin=C.tilt=0;
    C.preview=C.pending=C.anim_n=0;C.peek=0;prepare(C.b,C.ob,2460100.125);
    C.tex_ok=C.night_ok=C.ring_ok=0;C.lut_r=0;while(!build_lut_reference(106,0)){}
    sprite fallback={expected_pixels,MAX_SIDE,236};render_globe_reference(&fallback,C.b,0,0,1);
    card_render_invalidate();assert(card_render_start(0,1));card_render_pump();
    C.tex_ok=C.night_ok=1;C.dirty=1;unsigned adoption_steps=0;
    while(R.stage!=CR_IDLE){card_render_pump();assert(++adoption_steps<2000);}
    assert(C.dirty==1 && C.tex_ok==1 && C.night_ok==1);
    assert(!memcmp(expected_pixels,C.globe.px,(size_t)fallback.w*fallback.h*2));
    render_globe_reference(&fallback,C.b,0,0,1);
    assert(card_render_start(0,1));adoption_steps=0;
    while(R.stage!=CR_IDLE){card_render_pump();assert(++adoption_steps<2000);}
    assert(!C.dirty && !memcmp(expected_pixels,C.globe.px,(size_t)fallback.w*fallback.h*2));
    unsigned old_generation=asset_generation,old_key=C.key;uint16_t*old_front=C.globe.px;
    assert(card_render_start(0,1));asset_generation++;card_render_pump();
    assert(R.stage==CR_IDLE&&C.key==old_key&&C.globe.px==old_front&&C.dirty==1);
    asset_generation=old_generation;
    /* Preview borrowing cancels every possible partial main-image phase. */
    for(int phase=CR_LUT;phase<=CR_DOT;phase++) {
        card_render_invalidate();C.preview=C.pending=C.anim_n=0;C.peek=0;
        C.ob=OB_SATURN;C.mode=2;C.free=0;C.spin=0;C.tilt=0;prepare(C.b,C.ob,2460100.125);
        assert(card_render_start(35,1));unsigned steps=0;
        while(R.stage!=phase && R.stage!=CR_IDLE){card_render_pump();assert(++steps<2000);}
        assert(R.stage==phase);uint16_t *front=C.globe.px;unsigned key=C.key;
        start_preview(1);assert(R.stage==CR_IDLE && C.preview && C.preview_key==0);
        prepare(C.nb,C.pending-1,C.b->jd);C.pending=0;
        render_preview_reference();int w=C.neighbour.w,h=C.neighbour.h;
        memcpy(expected_pixels,C.neighbour.px,(size_t)w*h*2);C.preview_key=0;
        card_preview_start();steps=0;
        while(R.stage!=CR_IDLE){card_render_pump();assert(++steps<1000);if(R.stage!=CR_IDLE)assert(C.preview_key==0);}
        assert(C.preview_key!=0 && C.globe.px==front && C.key==key);
        assert(!memcmp(expected_pixels,C.neighbour.px,(size_t)w*h*2));
        C.preview=0;C.pending=0;
    }
    /* A dirty-only job cancelled by preview must remain eligible after returning. */
    C.preview=0;C.pending=0;C.anim_n=0;C.peek=0;C.dirty=1;
    assert(card_render_start(0,1));card_render_pump();assert(C.dirty==0);
    start_preview(1);assert(C.dirty==1);C.preview=C.pending=0;card_render_cancel();
    assert(card_render_start(0,1));card_render_cancel();assert(C.dirty==1);
    /* Changing desired pose never mutates the in-flight snapshot bookkeeping. */
    card_render_invalidate();C.preview=C.pending=C.anim_n=0;C.peek=0;C.mode=C.free=0;
    C.spin=12;C.tilt=3;assert(card_render_start(0,2));C.spin=42;C.tilt=9;
    unsigned steps=0;while(R.stage!=CR_IDLE){card_render_pump();assert(++steps<2000);}
    assert(C.last_spin==12 && C.last_tilt==3 && R.front_detail==2);
    card_render_invalidate();assert(!R.stage && !card_window.active);
    orrery_card_render_quanta=saved_cap;
    C.ob=OB_N-1;C.mode=2;C.free=1;C.tex_ok=C.night_ok=C.ring_ok=1;
    prepare(C.b,C.ob,2460100.125);prepare(C.nb,C.ob,2460100.125);
    C.lut_r=0;while(!build_lut(globe_radius(C.ob,35),35)){}
    printf("PASS progressive %u exact settled images,198 coarse2/4 images+198 exact restorations,8 phase preview cancellations, snapshot freshness, atomic publication, zero allocation; one quantum/pump\n",cases);
    return cases;
}
