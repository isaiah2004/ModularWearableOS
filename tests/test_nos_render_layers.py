import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class RenderLayersTests(unittest.TestCase):
    def test_cache_pixels_reuse_invalidation_and_allocation(self):
        with tempfile.TemporaryDirectory(prefix="nos-layers-") as tmp:
            binary = Path(tmp) / ("layers.exe" if os.name == "nt" else "layers")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DNOS_PSRAM_TRACKING", "-Icore", "-Inos", "tests/nos_render_layers_test.c", "nos/render_layers.c", "core/gfx.c", "-lm", "-o", str(binary)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for args in ([], ["0"], ["1"], ["2"], ["3"]):
                with self.subTest(allocation=args):
                    result = subprocess.run([str(binary), *args], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("match", result.stdout)

    def test_engine_composite_dim_cache_live_and_failed_allocation(self):
        engine = (ROOT / "nos/nos.c").read_text()
        subview = engine[engine.index("static int sub_fb("):engine.index("\nvoid nos_dim(")]
        start = engine.index("static void clear_sub(")
        end = engine.index("/* 8x8 Bayer", start)
        wrapper = engine[start:end]
        support = (ROOT / "tests/nos_render_layers_test.c").read_text().replace(
            "int main(int argc,char **argv)", "int legacy_main(int argc,char **argv)")
        harness = support + '\n#include "gfx_cost.h"\n#include "cost_profile.h"\n' + subview + wrapper + r"""
static void aa_scene(gfx_fb *fb) {
    gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, 0);
    gfx_fill_circle(fb, 231.375f, 230.625f, 70.25f, 0xffff);
    gfx_fill_circle(fb, 247.125f, 230.875f, 59.75f, 0xf800);
    gfx_fb local=*fb; local.dim=9;
    gfx_fill_circle(&local, 200.375f, 210.125f, 17.25f, 0x8410);
}
int main(int argc,char **argv) {
    if (argc>1) fail_at=atoi(argv[1]);
    nos_layers_init();
    const int dim_levels[]={0,1,7,16,23,24,32};
    for (int opaque=0;opaque<2;opaque++)
    for (int parent=0;parent<2;parent++)
    for (int mode=0;mode<3;mode++)
    for (int level=0;level<7;level++)
    for (int tx=-51;tx<100;tx+=59)
    for (int ty=-31;ty<100;ty+=47) {
        int keep=dim_levels[level];
        nos_screen s={.name="AA",.draw=aa_scene,.flags=opaque?SCR_OPAQUE_BG:0};
        nos_layers_invalidate();nos_layers_begin(1);
        for(int i=0;i<N;i++)actual[i]=expected[i]=(uint16_t)(i*47u);
        gfx_fb f=full(actual),ref=full(expected); f.dim=ref.dim=parent?9:0;
        f.cx0=ref.cx0=19;f.cx1=ref.cx1=411;
        f.y_base=ref.y_base=53;f.h=ref.h=301;
        nos_layer_cache_enabled=mode!=1; directional=mode==2;
        nos_debug_post_dim=0;
        draw_screen(&f,&s,11,60,425,333,tx,ty,keep);
        nos_debug_post_dim=1;
        draw_screen(&ref,&s,11,60,425,333,tx,ty,keep);
        assert(!memcmp(actual,expected,sizeof actual));
    }
    /* Untranslated full-width cold bands borrow the output buffer. Check AA,
     * nested ink dim, parent/final dim, and cache replay against live raster. */
    for (int level=0;level<7;level++) {
        nos_screen s={.name="AA",.draw=aa_scene};
        nos_layers_invalidate();nos_layers_begin(2);
        memset(actual,0,sizeof actual);memset(expected,0,sizeof expected);
        gfx_fb f=full(actual),ref=full(expected);
        nos_layer_cache_enabled=1;directional=0;
        nos_debug_post_dim=0;
        draw_screen(&f,&s,0,0,GFX_W,GFX_H,0,0,dim_levels[level]);
        nos_debug_post_dim=1;
        draw_screen(&ref,&s,0,0,GFX_W,GFX_H,0,0,dim_levels[level]);
        assert(!memcmp(actual,expected,sizeof actual));
        /* Reuse verifies that the stored band was sharp before final dim. */
        nos_debug_post_dim=0;
        draw_screen(&f,&s,0,0,GFX_W,GFX_H,0,0,32);
        nos_debug_post_dim=1;
        draw_screen(&ref,&s,0,0,GFX_W,GFX_H,0,0,32);
        assert(!memcmp(actual,expected,sizeof actual));
    }
    /* Final dim changes retained damage even when callback inks do not. */
    nos_screen s={.name="AA",.draw=aa_scene};
    gfx_rec a={0},b={0};gfx_fb f=full(actual);f.rec=&a;
    draw_screen(&f,&s,0,0,GFX_W,GFX_H,0,0,23);
    f.rec=&b;draw_screen(&f,&s,0,0,GFX_W,GFX_H,0,0,22);
    assert(a.n==b.n && a.n>0);
    assert(a.c[a.n-1].hash!=b.c[b.n-1].hash);
    assert(a.c[a.n-1].x0==0 && a.c[a.n-1].x1==GFX_W);
    puts("engine: final dim cache/live/allocation fallback, local ink, AA, clips and translated opaque views match");
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="nos-screen-dim-") as tmp:
            source = Path(tmp) / "dim.c"
            source.write_text(harness)
            binary = Path(tmp) / ("dim.exe" if os.name == "nt" else "dim")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DNOS_PSRAM_TRACKING", "-Icore", "-Inos", str(source), "nos/render_layers.c", "core/gfx.c", "-lm", "-o", str(binary)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for args in ([], ["0"], ["1"]):
                with self.subTest(allocation=args):
                    result = subprocess.run([str(binary), *args], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("match", result.stdout)

if __name__ == "__main__": unittest.main()
