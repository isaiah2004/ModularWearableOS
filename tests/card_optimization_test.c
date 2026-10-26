/* Independent old full functions are retained below, outside production. */
#include "../nos/faces/orrery_card.c"
#include "card_prepare_reference.h"
#include "card_raster_reference.h"
#include "card_rings_reference.h"
#include "card_motion_reference.h"
#include <assert.h>
#include <time.h>
extern int sim_program_main(int,char **);
static body_state old_body,new_body;
#include "card_prepare_incremental_test.inc"
#include "card_progressive_test.inc"
#include "card_proxy_test.inc"
int main(int argc,char **argv)
{
    assert(sim_program_main(argc,argv)==0);
    assert(orrery_card_qz_exact == 1 && orrery_card_qz_check() == 0);
    assert(orrery_card_lighting_cache == 1 && orrery_card_lighting_check() == ~0u);
    assert(orrery_card_shade_q16==1 && orrery_card_shade_q16_check()==~0u);
    unsigned bodies=0,luts=0,globes=0,previews=0;
    clock_t before=0,after=0;
    for(int ob=0;ob<OB_N;ob++)for(int date=0;date<9;date++) {
        double jd=2451545.0+date*173.125;
        clock_t t=clock();prepare_reference(&old_body,ob,jd);before+=clock()-t;
        t=clock();prepare(&new_body,ob,jd);after+=clock()-t;
        assert(!memcmp(&old_body,&new_body,sizeof old_body));bodies++;
    }
    assert(card_prepare_incremental_tests()==99);
    assert(allocate() && C.coords && sizeof *C.coords==1896);
    nos_mem_stat stat;int accounted=0;
    for(unsigned i=0;i<nos_mem_stats(&stat,i);i++)if(!strcmp(stat.owner,"orrery_card_coords.h")) {
        assert(stat.bytes==sizeof *C.coords && stat.blocks==1);accounted=1;
    }
    assert(accounted);
    assert(C.lighting_coeff && sizeof *C.lighting_coeff==2048 && orrery_card_lighting_check()==0);
    accounted=0;
    for(unsigned i=0;i<nos_mem_stats(&stat,i);i++)if(!strcmp(stat.owner,"orrery_card_lighting.h")) {
        assert(stat.bytes==2048 && stat.blocks==1);accounted=1;
    }
    assert(accounted);
    unsigned unchanged=nos_mem_used(),allocation_errors=nos_mem_failures();
    nos_mem_fail_after(0);assert(orrery_card_lighting_check()==0 && nos_mem_used()==unchanged);
    assert(!card_lighting_new(C.lighting) && nos_mem_failures()==allocation_errors+1); /* validator allocates nothing */
    uint32_t saved;memcpy(&saved,&C.lighting_coeff->k[0],4);uint32_t corrupt=saved^1u;
    memcpy(&C.lighting_coeff->k[0],&corrupt,4);assert(orrery_card_lighting_check()==1);
    memcpy(&C.lighting_coeff->k[0],&saved,4);assert(orrery_card_lighting_check()==0);
    allocation_errors=nos_mem_failures();nos_mem_max_request(1);
    assert(!card_lighting_new(C.lighting) && nos_mem_failures()==allocation_errors);nos_mem_max_request(0);
    assert(orrery_shade_q16_prebake()==0 && orrery_card_shade_q16_check()==0);
    const uint32_t *multipliers=orrery_shade_q16_get(C.lighting_coeff->k);assert(multipliers);
    for(unsigned i=0;i<512;i++)for(unsigned c=0;c<65536;c++)
        assert(shade((uint16_t)c,C.lighting_coeff->k[i],0,0)==shade_q16((uint16_t)c,multipliers[i]));
    unchanged=nos_mem_used();allocation_errors=nos_mem_failures();nos_mem_fail_after(0);
    assert(orrery_card_shade_q16_check()==0 && nos_mem_used()==unchanged);
    assert(!card_lighting_new(C.lighting) && nos_mem_failures()==allocation_errors+1);
    memcpy(&saved,&C.lighting_coeff->k[0],4);corrupt=saved^1u;
    memcpy(&C.lighting_coeff->k[0],&corrupt,4);assert(orrery_card_shade_q16_check()==~0u);
    memcpy(&C.lighting_coeff->k[0],&saved,4);assert(orrery_card_shade_q16_check()==0);
    for(int r=1;r<=118;r++)for(int slot=0;slot<2;slot++) {
        const float *axis=card_coords_axis(C.coords,slot,r);
        assert(axis==card_coords_axis(C.coords,slot,r));
        for(int i=0;i<r*2;i++) {
            float x=(i+.5f-r)/r,y=-(i+.5f-r)/r,negative=-axis[i];
            assert(!memcmp(axis+i,&x,sizeof x) && !memcmp(&negative,&y,sizeof y));
        }
    }
    assert(texture_alloc(C.night));
    for(unsigned page=0;page<TEX_PAGES;page++)for(unsigned i=0;i<TEX_PAGE_BYTES/2;i++) {
        C.texture[page][i]=(uint16_t)((i*13u+page*173u)^0xA5A5u);
        C.night[page][i]=(uint16_t)((i*37u+page*43u)^0x5A5Au);
    }
    for(int i=0;i<2048;i++)C.ring_tex[i]=(uint8_t)(i*23+11);
    size_t lut_bytes=236u*236u*sizeof *C.lut;
    globe_lut *expected_lut=malloc(lut_bytes);assert(expected_lut);
    uint16_t *pixels=malloc(MAX_SIDE*236u*2u);assert(pixels);
    for(int ob=0;ob<OB_N;ob++)for(int angle=0;angle<3;angle++) {
        float lat=angle==0?-65:angle==1?0:35;
        int r=globe_radius(ob,lat);
        memset(C.lut,0,lut_bytes);C.lut_r=0;
        while(!build_lut_reference(r,lat)) {}
        memcpy(expected_lut,C.lut,lut_bytes);
        memset(C.lut,0,lut_bytes);C.lut_r=0;
        while(!build_lut(r,lat)) {}
        assert(!memcmp(expected_lut,C.lut,lut_bytes));luts++;
        prepare(C.b,ob,2460100.125);prepare(C.nb,ob,2460100.125);
        if(ob==OB_SATURN)build_rings(lat);
        for(int textured=0;textured<2;textured++)for(int mode=0;mode<3;mode++)for(int free_mode=0;free_mode<2;free_mode++) {
            C.tex_ok=C.night_ok=C.ring_ok=textured;
            C.mode=mode;C.free=free_mode;
            sprite expected={pixels,MAX_SIDE,236};
            render_globe_reference(&expected,C.b,37.25f,lat,textured);
            card_lighting *published=C.lighting_coeff;
            for(int sun=0;sun<2;sun++)for(int q16=0;q16<2;q16++)for(int qz=0;qz<2;qz++)for(int cached=0;cached<3;cached++) {
                orrery_card_sun_fast=sun;
                orrery_card_shade_q16=q16;
                orrery_card_qz_exact=qz;orrery_card_lighting_cache=cached!=0;
                C.lighting_coeff=cached==2?NULL:published; /* admitted and missing-table fallbacks */
                render_globe(&C.globe,C.b,37.25f,lat,textured);
                assert(expected.w==C.globe.w && expected.h==C.globe.h);
                assert(!memcmp(pixels,C.globe.px,(size_t)expected.w*expected.h*2));globes++;
            }
            C.lighting_coeff=published;orrery_card_qz_exact=1;orrery_card_lighting_cache=1;orrery_card_shade_q16=1;orrery_card_sun_fast=1;
        }
        for(int free_mode=0;free_mode<2;free_mode++) {
            C.free=free_mode;
            render_preview_reference();
            int w=C.neighbour.w,h=C.neighbour.h;
            memcpy(pixels,C.neighbour.px,(size_t)w*h*2);
            render_preview();assert(w==C.neighbour.w && h==C.neighbour.h);
            assert(!memcmp(pixels,C.neighbour.px,(size_t)w*h*2));previews++;
        }
    }
    assert(progressive_tests(pixels)==198);
    proxy_buffer_tests();
    /* Runtime legacy selection must match the same independent raster oracle. */
    orrery_card_qz_exact=0;
    sprite legacy={pixels,MAX_SIDE,236};
    render_globe_reference(&legacy,C.b,17.5f,0,1);render_globe(&C.globe,C.b,17.5f,0,1);
    assert(legacy.w==C.globe.w && legacy.h==C.globe.h && !memcmp(pixels,C.globe.px,(size_t)legacy.w*legacy.h*2));
    orrery_card_qz_exact=1;
    /* Allocation denial is optional: original division path retains identical pixels. */
    hal_big_free(C.coords);C.coords=0;
    unsigned failures=nos_mem_failures();nos_mem_max_request(1);
    assert(!card_coords_new() && nos_mem_failures()==failures);nos_mem_max_request(0);
    nos_mem_fail_after(0);assert(!card_coords_new());assert(nos_mem_failures()==failures+1);
    C.lut_r=0;while(!build_lut(106,0)) {}
    memcpy(expected_lut,C.lut,lut_bytes);C.lut_r=0;
    while(!build_lut_reference(106,0)) {}
    assert(!memcmp(expected_lut,C.lut,lut_bytes));
    sprite scalar={pixels,MAX_SIDE,236};
    render_globe_reference(&scalar,C.b,17.5f,0,1);render_globe(&C.globe,C.b,17.5f,0,1);
    assert(scalar.w==C.globe.w && scalar.h==C.globe.h && !memcmp(pixels,C.globe.px,(size_t)scalar.w*scalar.h*2));
    render_preview_reference();memcpy(pixels,C.neighbour.px,(size_t)C.neighbour.w*C.neighbour.h*2);
    render_preview();assert(!memcmp(pixels,C.neighbour.px,(size_t)C.neighbour.w*C.neighbour.h*2));
    unsigned used=nos_mem_used();release_buffers();assert(nos_mem_used()<used && !C.coords && !C.lighting_coeff && !C.shade_q16 && !C.shade_q16_checked);
    assert(orrery_card_lighting_check()==~0u);
    /* The new cache is optional even when its real allocator fails after all mandatory allocations. */
    for(int failure=24;failure<=25;failure++) {
        nos_mem_fail_after(failure);assert(allocate() && !C.failed);
        assert(failure==24?!C.coords:!C.lighting_coeff);
        prepare(C.b,OB_EARTH,2460100.125);C.lut_r=0;while(!build_lut(106,0)) {}
        C.tex_ok=C.night_ok=0;C.mode=C.free=0;
        render_globe_reference(&scalar,C.b,17.5f,0,0);render_globe(&C.globe,C.b,17.5f,0,0);
        assert(!memcmp(pixels,C.globe.px,(size_t)scalar.w*scalar.h*2));release_buffers();
        assert(!C.lighting_coeff && !C.coords);
    }
    assert(allocate() && C.lighting_coeff && orrery_card_lighting_check()==0);
    release_buffers();
    for(unsigned i=0;i<nos_mem_stats(&stat,i);i++)if(!strcmp(stat.owner,"orrery_card_lighting.h"))
        assert(stat.bytes==0 && stat.blocks==0);
    free(expected_lut);free(pixels);
    printf("PASS %u full body states, %u LUTs, %u globe and %u preview images; exact coordinate and65,536qz float bits; QZ/coefficient A/B and missing-cache fallbacks;512 coefficients and33554432 Q16shade color proofs; admission/failure/release. HOST ONLY prepare reference=%.3fms optimized=%.3fms\n",bodies,luts,globes,previews,1000.*before/CLOCKS_PER_SEC,1000.*after/CLOCKS_PER_SEC);
    return 0;
}
