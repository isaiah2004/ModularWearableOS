import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class LauncherBakeTests(unittest.TestCase):
    def test_bounded_jobs_match_original_and_fail_without_retry_storm(self):
        src = (ROOT / "nos/apps/sys_launcher.c").read_text()
        declarations = src[:src.index("enum { M_IDLE")]
        jobs = src[src.index("/* ---- sprite baking"):src.index("/* ---- scrolling")]
        trim = src[src.index("static int launcher_rom_active("):src.index("/* ---- sprite baking")]
        registry = (ROOT / "nos/apps/registry.c").read_text()
        registry = registry[registry.index("const nos_app nos_app_list[]"):registry.index("/* the last app")]
        registry = re.sub(r"&scr_\w+", "NULL", registry)
        ui = (ROOT / "nos/ui.c").read_text()
        text = ui[ui.index("static const font_glyph *glyph("):ui.index("\nint ui_label(")]
        harness = declarations + registry + r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "gfx_cost.h"
#include "cost_profile.h"
#include "fonts/nos_fonts.h"
const font_t *UI_F_TITLE=&nf_title;
const font_t *UI_F_BODY=&nf_title,*UI_F_LABEL=&nf_title;
int nos_native_menu_rom_enabled=0;
int native_menu_assets_compatible(void){return 0;}
int native_menu_launcher_matches(unsigned e,const char*n,int i){(void)e;(void)n;(void)i;return 0;}
const native_asset_desc *native_menu_asset(unsigned f,unsigned e,unsigned l,unsigned v){(void)f;(void)e;(void)l;(void)v;return NULL;}
sprite8 native_menu_sprite(const native_asset_desc*d){(void)d;sprite8 s={0};return s;}
const uint16_t *native_menu_launcher_palette(int l,int p){(void)l;(void)p;return NULL;}
const int16_t *native_menu_launcher_curve(void){return NULL;}
static int allocations, fail_after=-1;
static unsigned long long bytes;
static uint32_t available=UINT32_MAX;
uint32_t nos_mem_available(void) { return available; }
void *hal_big_alloc(uint32_t n) {
    int i=allocations++;
    if(fail_after>=0 && i>=fail_after)return NULL;
    void *p=malloc(n);if(p)bytes+=n;return p;
}
uint32_t hal_micros(void) {static uint32_t t; t+=1000;return t;}
static int napps(void) {return nos_app_count<MAXAPP?nos_app_count:MAXAPP;}
""" + text + "\n#define NOS_PSRAM_TRACKING 1\n" + trim + jobs + '\n#include "launcher_bake_reference.h"\n' + r"""
static void same_sprite(const sprite8 *a,const sprite8 *b) {
    assert((a->px!=NULL)==(b->px!=NULL));
    if(a->px){assert(a->w==b->w && a->h==b->h);assert(!memcmp(a->px,b->px,(size_t)a->w*a->h));}
}
static int done(void) {
    if(s_failed)return 1;
    if(!s_tables_ready)return 0;
    for(int i=0;i<napps();i++)if(!L->rows[i].failed && !L->rows[i].blurred)return 0;
    return 1;
}
int main(int argc,char **argv) {
    /* Isolate launcher cache failures from the separate dot-mask allocator. */
    dot_prebake_all();
    gfx_rec rec={0}; gfx_fb warm={0}; warm.rec=&rec; warm.h=GFX_H; warm.cx1=GFX_W;
    dot_icon(&warm,DOT_ICON_STEPS,20,20,3,2.75f,0xffff);
    allocations=0;bytes=0;
    if(argc>1)fail_after=atoi(argv[1]);
    S.x=12*ROW_P;S.v=0;
    available=LAUNCHER_RESERVE;
    for(int i=0;i<50;i++)sys_launcher_prewarm();
    assert(!L && !s_failed && allocations==0); /* budget miss never reaches allocator */
    available=UINT32_MAX;
    sys_launcher_prewarm_budget_us=750;sys_launcher_prewarm_max_steps=16;
    /* Fake time advances1ms/check: budget permits exactly one bounded stage. */
    int calls=0,first=-1;
    while(!done() && calls++<30000){
        sys_launcher_prewarm();
        if(L)for(int i=0;i<napps();i++)if(L->rows[i].ready && first<0)first=i;
        if(L)for(int i=0;i<napps();i++){
            row_spr *r=&L->rows[i];
            if(r->ready){assert(r->stage>=NVAR+1);for(int k=0;k<NVAR;k++)assert(r->icon[k].px);assert(r->name.px);}
            if(r->blurred)assert(r->stage==NVAR+1+2*NBLUR);
        }
    }
    assert(calls<30000);
    if(fail_after>=0){
        int count=allocations;
        for(int i=0;i<1000;i++)sys_launcher_prewarm();
        assert(allocations==count); /* every failed item is parked, including blur failures */
        assert(count<=fail_after+nos_app_count+4);
        puts("launcher bake: allocation failure is terminal per item with bounded retries");return 0;
    }
    assert(first==12);assert(s_baked==napps());
    /* Every authored16-level anchor remains exact; new levels interpolate
     * those channel codes, rather than rounding a new5-bit grey independently. */
    assert(NLEV==64);
    for(int anchor=0;anchor<=16;anchor++)for(int i=0;i<256;i++)
        assert(L->lut[anchor*4][i]==grey565((i*anchor+8)/16));
    for(int anchor=0;anchor<16;anchor++)for(int fraction=1;fraction<4;fraction++)
    for(int i=0;i<256;i++) {
        uint16_t a=grey565((i*anchor+8)/16),b=grey565((i*(anchor+1)+8)/16);
        uint16_t c=L->lut[anchor*4+fraction][i],previous=L->lut[anchor*4+fraction-1][i];
        const int shifts[3]={11,5,0},masks[3]={31,63,31};
        for(int channel=0;channel<3;channel++) {
            int lo=(a>>shifts[channel])&masks[channel],hi=(b>>shifts[channel])&masks[channel];
            int actual=(c>>shifts[channel])&masks[channel];
            int prior=(previous>>shifts[channel])&masks[channel];
            double t=fraction/4.0;
            assert(actual==(int)floor(lo+(hi-lo)*t+0.5));
            assert(actual>=lo&&actual<=hi&&actual>=prior);
        }
    }
    for(int i=0;i<256;i++)assert(L->lut_press[i]==grey565(i<=26?i*51/26:51+(i-26)*(255-51)/(255-26)));
    for(int y=0;y<(int)(sizeof L->curve/sizeof L->curve[0]);y++){
        float d=y,r=UI_R;assert(L->curve[y]==(int)(CURVE_K*(d<r?r-sqrtf(r*r-d*d):r)+0.5f));
    }
    unsigned long long total=bytes;
    /* Account for the actual registry and all eight directional trails. The
     * previous fixed18-app/four-box budget no longer described this fixture.
     * Admission against LAUNCHER_RESERVE is checked above and after trim. */
    unsigned long long pixels=0;
    for(int i=0;i<napps();i++) {
        const row_spr *r=&L->rows[i];
        for(int k=0;k<NVAR;k++)pixels+=(unsigned)r->icon[k].w*r->icon[k].h;
        pixels+=(unsigned)r->name.w*r->name.h;
        for(int k=0;k<NBLUR;k++) {
            pixels+=(unsigned)r->bicon[k].s.w*r->bicon[k].s.h;
            pixels+=(unsigned)r->bname[k].s.w*r->bname[k].s.h;
        }
    }
    assert(total>=sizeof *L+pixels);
    assert(total-sizeof *L-pixels<64u*1024u); /* bounded incidental lazy dot masks */
    void *actual=L;
    void *reference=calloc(1,sizeof *L);assert(reference);
    L=reference;
    for(int i=0;i<napps();i++)assert(reference_bake_app(i));
    for(int i=0;i<napps();i++){
        row_spr expected=L->rows[i];L=actual;row_spr *a=&L->rows[i];
        for(int k=0;k<NVAR;k++)same_sprite(&a->icon[k],&expected.icon[k]);
        same_sprite(&a->name,&expected.name);
        assert(a->name_base==expected.name_base && a->name_w==expected.name_w && a->name_h==expected.name_h);
        for(int k=0;k<NBLUR;k++){
            same_sprite(&a->bicon[k].s,&expected.bicon[k].s);same_sprite(&a->bname[k].s,&expected.bname[k].s);
            assert(a->bicon[k].y0==expected.bicon[k].y0 && a->bname[k].y0==expected.bname[k].y0);
        }
        L=reference;
    }
    L=actual;int count=allocations;sys_launcher_prewarm();assert(allocations==count);
    /* Memory pressure can trim a distant level even while its scratch job is active. */
    J.item=0;J.stage=NVAR+1;J.phase=3;
    uint8_t *sharp_kept=L->rows[0].icon[0].px;
    uint8_t *visible_kept=L->rows[12].bicon[0].s.px;
    sys_launcher_trim();
    assert(J.item<0 && L->rows[0].ready && L->rows[0].stage==NVAR+1);
    assert(L->rows[0].icon[0].px==sharp_kept);
    assert(L->rows[12].bicon[0].s.px==visible_kept && L->rows[12].blurred);
    for(int k=0;k<NBLUR;k++)assert(!L->rows[0].bicon[k].s.px && !L->rows[0].bname[k].s.px);
    available=LAUNCHER_RESERVE;
    count=allocations;
    for(int i=0;i<1000;i++)sys_launcher_prewarm();
    assert(allocations==count && !done());
    available=UINT32_MAX;
    int rebuild=0;while(!done() && rebuild++<30000)sys_launcher_prewarm();assert(rebuild<30000);
    L=reference;row_spr expected0=L->rows[0];L=actual;
    for(int k=0;k<NBLUR;k++){
        same_sprite(&L->rows[0].bicon[k].s,&expected0.bicon[k].s);
        same_sprite(&L->rows[0].bname[k].s,&expected0.bname[k].s);
    }
    visible_kept=L->rows[12].bicon[0].s.px;
    sys_launcher_card_prepare();
    assert(!L->rows[0].ready && !L->rows[0].icon[0].px && L->rows[0].stage==0);
    assert(L->rows[12].ready && L->rows[12].bicon[0].s.px==visible_kept);
    rebuild=0;while(!done() && rebuild++<30000)sys_launcher_prewarm();assert(rebuild<30000);
    same_sprite(&L->rows[0].icon[0],&expected0.icon[0]);
    printf("launcher bake: %d single-slice calls, sharp/blur pixels+cropping match original, trim/resume, focus priority, %llu bytes\n",calls,total);
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="nos-launcher-bake-") as tmp:
            source = Path(tmp) / "bake.c"
            source.write_text(harness)
            binary = Path(tmp) / ("bake.exe" if os.name == "nt" else "bake")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable", "-Werror", "-Icore", "-Inos", "-Itests", str(source), "nos/dot.c", "nos/dot_glyphs.c", "core/gfx.c", "core/sprite.c", "nos/fonts/nf_title.c", "-lm", "-o", str(binary)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for args in ([],["0"],["1"],["4"],["12"],["50"],["100"]):
                with self.subTest(allocation=args):
                    result = subprocess.run([str(binary),*args], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("launcher bake:", result.stdout)
    def test_sharp_build_skips_blur_baking_with_and_without_the_sd_pack(self):
        src = (ROOT / "nos/apps/sys_launcher.c").read_text()
        declarations = src[:src.index("enum { M_IDLE")]
        jobs = src[src.index("/* ---- sprite baking"):src.index("/* ---- scrolling")]
        trim = src[src.index("static int launcher_rom_active("):src.index("/* ---- sprite baking")]
        registry = (ROOT / "nos/apps/registry.c").read_text()
        registry = registry[registry.index("const nos_app nos_app_list[]"):registry.index("/* the last app")]
        registry = re.sub(r"&scr_\w+", "NULL", registry)
        ui = (ROOT / "nos/ui.c").read_text()
        text = ui[ui.index("static const font_glyph *glyph("):ui.index("\nint ui_label(")]
        stubs = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "gfx_cost.h"
#include "cost_profile.h"
#include "fonts/nos_fonts.h"
#if REAL_PACK
const font_t *UI_F_TITLE=&nf_title,*UI_F_BODY=&nf_body,*UI_F_LABEL=&nf_label;
uint16_t nos_c_red=0xD0C4;
void nos_layers_invalidate(void){}
void nos_history_reset(void){}
void nos_invalidate(void){}
uint32_t hal_millis(void){return 0;}
const char *hal_sd_root(void){return "none";}
#else
const font_t *UI_F_TITLE=&nf_title;
const font_t *UI_F_BODY=&nf_title,*UI_F_LABEL=&nf_title;
int nos_native_menu_rom_enabled=0;
int native_menu_assets_compatible(void){return 0;}
int native_menu_launcher_matches(unsigned e,const char*n,int i){(void)e;(void)n;(void)i;return 0;}
const native_asset_desc *native_menu_asset(unsigned f,unsigned e,unsigned l,unsigned v){(void)f;(void)e;(void)l;(void)v;return NULL;}
sprite8 native_menu_sprite(const native_asset_desc*d){(void)d;sprite8 s={0};return s;}
const uint16_t *native_menu_launcher_palette(int l,int p){(void)l;(void)p;return NULL;}
const int16_t *native_menu_launcher_curve(void){return NULL;}
#endif
static int allocations;
static unsigned long long bytes;
uint32_t nos_mem_available(void) { return UINT32_MAX; }
void *hal_big_alloc(uint32_t n) { allocations++; void *p=malloc(n); if(p)bytes+=n; return p; }
uint32_t hal_micros(void) {static uint32_t t; t+=1000;return t;}
static int napps(void) {return nos_app_count<MAXAPP?nos_app_count:MAXAPP;}
"""
        main = r"""
static void same_sprite(const sprite8 *a,const sprite8 *b) {
    assert(a->px && b->px && a->w==b->w && a->h==b->h);
    assert(!memcmp(a->px,b->px,(size_t)a->w*a->h));
}
static int done(void) {
    if(s_failed)return 1;
    if(!s_tables_ready)return 0;
    for(int i=0;i<napps();i++)if(!L->rows[i].failed && !L->rows[i].blurred)return 0;
    return 1;
}
int main(int argc,char **argv) {
    assert(NOS_RENDER_SHARP==1);
#if REAL_PACK
    assert(argc>1);{static char env[600];snprintf(env,sizeof env,"NOS_MENU_PACK=%s",argv[1]);putenv(env);}
    assert(native_menu_assets_compatible());
    native_menu_pack_info pi;native_menu_pack_status(&pi);assert(pi.sharp==1 && pi.records==169);
#else
    (void)argc;(void)argv;
#endif
    dot_prebake_all();
    allocations=0;bytes=0;
    S.x=12*ROW_P;S.v=0;
    sys_launcher_prewarm_budget_us=750;sys_launcher_prewarm_max_steps=16;
    int calls=0;
    while(!done() && calls++<30000)sys_launcher_prewarm();
    assert(calls<30000 && !s_failed && s_baked==napps());
    unsigned long long total=bytes;
    for(int i=0;i<napps();i++) {
        const row_spr *r=&L->rows[i];
        assert(r->ready && r->blurred && r->stage==NVAR+1+2*NBLUR);
        for(int k=0;k<NVAR;k++)assert(r->icon[k].px);
        assert(r->name.px);
        for(int k=0;k<NBLUR;k++)assert(!r->bicon[k].s.px && !r->bname[k].s.px); /* no blur sprites exist */
#if REAL_PACK
        assert(r->rom);
        for(int k=0;k<NVAR;k++)assert(r->icon[k].px==native_menu_sprite(native_menu_asset(1,(unsigned)i,(unsigned)k,0)).px);
#else
        assert(!r->rom);
#endif
    }
    unsigned long long sharp=0;
    for(int i=0;i<napps();i++){
        for(int k=0;k<NVAR;k++)sharp+=(unsigned)L->rows[i].icon[k].w*L->rows[i].icon[k].h;
        sharp+=(unsigned)L->rows[i].name.w*L->rows[i].name.h;
    }
#if REAL_PACK
    (void)sharp;
    assert(total<sizeof *L+64u*1024u); /* immutable level-0 art: no pixel allocations */
#else
    assert(total>=sizeof *L+sharp && total-sizeof *L-sharp<64u*1024u); /* sharp rows only, no ~3 MB of blur */
#endif
    void *actual=L;void *reference=calloc(1,sizeof *L);assert(reference);
    L=reference;
    for(int i=0;i<napps();i++)assert(reference_bake_app(i));
    for(int i=0;i<napps();i++){
        row_spr expected=L->rows[i];L=actual;
        for(int k=0;k<NVAR;k++)same_sprite(&L->rows[i].icon[k],&expected.icon[k]);
        same_sprite(&L->rows[i].name,&expected.name);
        assert(L->rows[i].name_base==expected.name_base && L->rows[i].name_w==expected.name_w && L->rows[i].name_h==expected.name_h);
        L=reference;
    }
    L=actual;
    /* Trim and a rebuilt row stay blur-free and complete. */
    sys_launcher_trim();
    int again=0;while(!done() && again++<30000)sys_launcher_prewarm();
    assert(again<30000 && s_baked==napps());
    for(int i=0;i<napps();i++)for(int k=0;k<NBLUR;k++)assert(!L->rows[i].bicon[k].s.px && !L->rows[i].bname[k].s.px);
    printf("launcher sharp: %d calls, %s, %llu bytes, no blur sprites\n",calls,REAL_PACK?"level-0 pack adopted":"dynamic sharp bake",total);
    return 0;
}
"""
        common = ["-O2", "-std=c11", "-DNOS_RENDER_SHARP=1", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable", "-Werror", "-Icore", "-Inos", "-Itests"]
        with tempfile.TemporaryDirectory(prefix="nos-launcher-sharp-") as tmp:
            for real in (0, 1):
                source = Path(tmp) / f"sharp{real}.c"
                source.write_text(f"#define REAL_PACK {real}\n" + declarations + registry + stubs + text + "\n#define NOS_PSRAM_TRACKING 1\n" + trim + jobs +
                                  '\n#include "launcher_bake_reference.h"\n' + main)
                binary = Path(tmp) / f"sharp{real}.exe"
                extra = ["nos/native_menu_assets.c", "nos/fonts/nf_body.c", "nos/fonts/nf_label.c"] if real else []
                result = subprocess.run(["gcc", *common, str(source), "nos/dot.c", "nos/dot_glyphs.c", "core/gfx.c", "core/sprite.c", "nos/fonts/nf_title.c",
                                         *extra, "-lm", "-o", str(binary)], cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                pack = ROOT / "assets/sd/menu/native_menu_sharp.pack"
                with self.subTest(sd_pack=bool(real)):
                    result = subprocess.run([str(binary), str(pack)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("launcher sharp:", result.stdout)
if __name__ == "__main__": unittest.main()
