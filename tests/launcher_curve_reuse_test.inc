/* Included after the actual launcher source, with test-only call counters. */
#include <stdio.h>
extern int sim_program_main(int,char **);
static uint16_t reference_pixels[GFX_W*GFX_H],cached_pixels[GFX_W*GFX_H];
static unsigned cases,invalidations,cadence;
static unsigned long long old_sqrt_total,new_sqrt_total;
static unsigned long long bytes_hash(const void *data,size_t bytes)
{
    const unsigned char *p=data;unsigned long long h=1469598103934665603ULL;
    for(size_t i=0;i<bytes;i++)h=(h^p[i])*1099511628211ULL;
    return h;
}
static void frame(uint16_t *pixels,gfx_rec *rec)
{
    gfx_fb record={.h=GFX_H,.cx1=GFX_W,.rec=rec};l_draw(&record);
    memset(pixels,0,sizeof reference_pixels);
    for(int y=0;y<GFX_H;y+=16) {
        gfx_fb fb={.px=pixels+y*GFX_W,.h=GFX_H-y<16?GFX_H-y:16,.cx1=GFX_W,.y_base=y};
        l_draw(&fb);
    }
}
static void compare(const char *kind,int cached)
{
    gfx_rec a={0},b={0};unsigned char published[sizeof geometry];
    memcpy(published,&geometry,sizeof geometry);
    unsigned memory=nos_mem_used(),failures=nos_mem_failures();
    curve_reference_direct=1;curve_sqrt_calls=curve_trail_calls=0;
    frame(reference_pixels,&a);
    unsigned before=curve_sqrt_calls;
    curve_reference_direct=0;curve_sqrt_calls=curve_trail_calls=0;
    frame(cached_pixels,&b);
    assert(!memcmp(reference_pixels,cached_pixels,sizeof reference_pixels));
    assert(!memcmp(&a,&b,sizeof a));
    assert(!memcmp(published,&geometry,sizeof geometry)); /* no draw-side publication */
    if(nos_mem_used()!=memory || nos_mem_failures()!=failures)
        fprintf(stderr,"CURVE memory case=%u before=%u after=%u failures=%u/%u\n",cases,memory,nos_mem_used(),failures,nos_mem_failures());
    assert(nos_mem_used()==memory && nos_mem_failures()==failures);
#if CURVE_REUSE_CANDIDATE
    if(cached)assert(!curve_trail_calls && !curve_sqrt_calls);
    else {assert(curve_trail_calls>0);invalidations++;}
#else
    (void)cached;
#endif
    assert(curve_sqrt_calls<=before);
    old_sqrt_total+=before;new_sqrt_total+=curve_sqrt_calls;
    printf("CURVE {\"case\":%u,\"kind\":\"%s\",\"cadence\":%u,\"exposure_q8\":%d,\"direction\":%d,\"pixels\":\"%016llx\",\"records\":\"%016llx\",\"commands\":%d,\"old_sqrt\":%u,\"draw_sqrt\":%u}\n",
        cases++,kind,cadence,native_exposure,native_direction,bytes_hash(cached_pixels,sizeof cached_pixels),bytes_hash(&b,sizeof b),b.n,before,curve_sqrt_calls);
}
static unsigned publish(float x,int exposure,int direction)
{
    S.x=x;native_pose=(int)lroundf((floorf(x+.5f)-x)*256);native_exposure=exposure;native_direction=direction;
    curve_sqrt_calls=curve_trail_calls=0;geometry_prepare(floorf(x+.5f));
    return curve_sqrt_calls;
}
int main(int argc,char **argv)
{
    assert(!sim_program_main(argc,argv));nos_t=1000;nos_frame=500;nos_mblur=2;
    assert(launcher_rom_active());l_enter();S.t_enter=nos_t-10;P.id=-1;P.down=0;P.until=0;
    launcher_rom_sync();
    assert(ensure_tables());
    for(int i=0;i<napps();i++)if(!L->rows[i].ready)assert(launcher_adopt_rom(i));
    /* First title/font/coverage use can initialize unrelated shared owners.
     * Settle them before asserting that repeated rendering adds no allocation. */
    publish(368,30*256,-1);gfx_rec warm={0};frame(cached_pixels,&warm);
    unsigned prepared=publish(368,30*256,-1);
    compare("representative",1);
#if CURVE_REUSE_CANDIDATE
    assert(prepared==20 && old_sqrt_total==152 && !new_sqrt_total);
#endif
    printf("WORK preparation_sqrt=%u geometry_bytes=%u\n",prepared,(unsigned)sizeof geometry);
    const int fps[]={15,30,60,90};
    const float offsets[]={368.25f,414.5f};
    const float travel[]={1.0f/256,7.5f,15,22.5f,30,45,60,75,90};
    for(unsigned f=0;f<4;f++)for(unsigned x=0;x<2;x++)for(unsigned e=0;e<9;e++)for(int dir=-1;dir<=1;dir+=2) {
        float dt=1.0f/fps[f],fr=dt*M_FPS;
        cadence=(unsigned)fps[f];
        nos_frame++;nos_t+=dt;S.mode=M_IDLE;S.x=offsets[x];
        native_previous=S.x+dir*travel[e]*(fr>.5f?fr:1.0f);
        l_tick(dt);
        compare("cadence-direction-density",1);
    }
    /* A second pose/clock/source can arrive without advancing nos_frame/time.
     * The publication stays immutable and every changed lookup is exact. */
    cadence=0;
    publish(368.25f,30*256,-1);S.x+=1;compare("same-frame-scroll",0);
    publish(368.25f,30*256,-1);native_direction=1;compare("same-frame-direction",0);
    publish(368.25f,30*256,-1);native_exposure=75*256;compare("same-frame-exposure",0);
    publish(368.25f,22*256,-1);native_exposure++;compare("same-level-new-exposure",0);
    publish(368.25f,30*256,-1);S.t_enter-=.01;compare("same-frame-enter-clock",0);
    publish(368.25f,30*256,-1);nos_t+=.001;compare("same-frame-clock",0);
    publish(368.25f,30*256,-1);geometry.valid=0;compare("draw-only-no-publication",0);
    publish(368.25f,30*256,-1);
    int row=(int)floorf(S.x/ROW_P+.5f);L->rows[row].blurred=0;
    compare("same-frame-sharp-source",0);L->rows[row].blurred=1;
    for(int direction=-1;direction<=1;direction+=2) {
        publish(-69.25f,60*256,direction);compare("prefix-culling",1);
        publish(max_x()+69.25f,90*256,direction);compare("suffix-culling",1);
    }
    printf("PASS exact launcher curve reuse: %u full-pixel/record cases, %u within-frame invalidations; draw sqrt %llu -> %llu. No target timing claim.\n",
           cases,invalidations,old_sqrt_total,new_sqrt_total);
    return 0;
}
