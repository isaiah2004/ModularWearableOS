static void proxy_buffer_detail_tests(unsigned detail) {
    orrery_card_motion_step=detail;
    uint16_t *full=malloc(GFX_W*GFX_H*2),*stripe=malloc(GFX_W*GFX_H*2);assert(full&&stripe);
    for(int i=0;i<GFX_W*GFX_H;i++)full[i]=stripe[i]=0xbeef;
    V.active=1;V.spin=2;V.lat=2;V.key=0x11223344;R.front_r=106;R.front_lat=0;R.front_detail=1;
    C.ob=OB_EARTH;C.mode=R.front_mode=0;C.free=R.front_free=0;
    gfx_fb fb={0};fb.px=full;fb.h=GFX_H;fb.cx1=GFX_W;fb.ox=-13;fb.oy=3;fb.dim=9;
    card_proxy_blit(&fb,&C.globe,233-C.globe.w/2,214-C.globe.h/2);
    for(int y=0;y<GFX_H;y+=7) {
        gfx_fb row=fb;row.px=stripe+y*GFX_W;row.y_base=y;row.h=GFX_H-y<7?GFX_H-y:7;
        card_proxy_blit(&row,&C.globe,233-C.globe.w/2,214-C.globe.h/2);
    }
    assert(!memcmp(full,stripe,GFX_W*GFX_H*2));
    /* Packed translated/partial subviews and untouched edge columns/canaries. */
    for(int y=160;y<270;y+=7)for(int x=120;x<340;x+=17) {
        uint16_t packed[2+17*7];for(unsigned i=0;i<sizeof packed/2;i++)packed[i]=0xbeef;
        gfx_fb sub=fb;sub.px=packed+1;sub.stride=17;sub.x_base=x;sub.cx0=x+1;sub.cx1=x+16;sub.y_base=y;sub.h=7;
        card_proxy_blit(&sub,&C.globe,233-C.globe.w/2,214-C.globe.h/2);
        assert(packed[0]==0xbeef&&packed[1+17*7]==0xbeef);
        for(int yy=0;yy<7;yy++)for(int xx=0;xx<17;xx++)
            assert(packed[1+yy*17+xx]==(xx>0&&xx<16?full[(y+yy)*GFX_W+x+xx]:0xbeef));
    }
    gfx_rec rec={0};gfx_fb recorded={0};recorded.rec=&rec;recorded.h=GFX_H;recorded.cx1=GFX_W;
    C.key=123;orrery_card_progressive=0;
    page_globe(&recorded,C.b,&C.globe,C.key,0);assert(rec.n>0);
    gfx_rec expected={0};gfx_fb er=recorded;er.rec=&expected;
    sprite_blit(&er,&C.globe,233-C.globe.w/2,214-C.globe.h/2,C.key);
    assert(rec.c[0].hash==expected.c[0].hash);
    orrery_card_progressive=1;orrery_card_motion_proxy=0;rec.n=0;
    page_globe(&recorded,C.b,&C.globe,C.key,0);assert(rec.c[0].hash==expected.c[0].hash);
    orrery_card_motion_proxy=1;V.active=0;
    float old_olat=C.b->olat,old_tilt=C.tilt,old_front_lat=R.front_lat;
    int old_ready=C.b->ready;C.b->ready=1;C.b->olat=6;C.tilt=0;R.front_lat=6;V.lat=0;
    assert(card_proxy_lat_error()==0);C.tilt=2;assert(card_proxy_lat_error()==2);
    C.b->ready=0;assert(card_proxy_lat_error()==0);
    C.b->olat=old_olat;C.tilt=old_tilt;R.front_lat=old_front_lat;C.b->ready=old_ready;
    free(full);free(stripe);
    printf("PASS proxy detail%u full/7row/translated packed/dim9 clips and canaries, baseline/proxy-off gates\n",detail);
}
static void proxy_buffer_tests(void) {
    unsigned saved=orrery_card_motion_step;
    proxy_buffer_detail_tests(2);
    proxy_buffer_detail_tests(4);
    orrery_card_motion_step=saved;
}
