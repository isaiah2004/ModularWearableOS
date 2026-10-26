"""Compile isolated C checks against production geometry; no simulator/device needed."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which("gcc"), "gcc is required for C geometry equivalence")
class RenderGeometryTests(unittest.TestCase):
    def compile_run(self, source, extra=()):
        with tempfile.TemporaryDirectory(prefix="nos-geometry-") as tmp:
            binary = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            subprocess.run(["gcc", "-O2", "-std=c11", "-Icore", "-Inos", str(source),
                            *extra, "core/gfx.c", "-lm", "-o", str(binary)],
                           cwd=ROOT, check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], cwd=ROOT, check=True,
                                    capture_output=True, text=True)
            self.assertIn("match", result.stdout)

    def test_ring_pixels_hashes_and_allocation_fallback(self):
        self.compile_run(ROOT / "tests/nos_ring_cache_test.c", ["nos/dot_glyphs.c"])

    def test_visible_lap_ranges(self):
        stopwatch = (ROOT / "nos/apps/time_stopwatch.c").read_text()
        start = stopwatch.index("    int r0 = (int)floorf(off / ROW_H)")
        stop = stopwatch.index("        float yc =", start)
        lap_loop = stopwatch[start:stop]
        harness = r"""
#include <assert.h>
#include <math.h>
#include <stdio.h>
#define ROW_H 36.0f
#define LIST_Y0 194.0f
#define LIST_Y1 302.0f
int main(void) {
    int got[256], expected[256];
    for (int n = 0; n < 180; n += 7)
    for (int blur = 0; blur < 2; blur++)
    for (float off = -97.25f; off < n * ROW_H + 120; off += 13.25f) {
        int ng = 0, ne = 0;
""" + lap_loop + r"""
            got[ng++] = r;
        }
        for (int j = 0; j <= n; j++) {
            int r = blur ? (j < n ? j + 1 : 0) : j;
            float y = LIST_Y0 + r * ROW_H - off;
            if (y + ROW_H <= LIST_Y0 || y >= LIST_Y1) continue;
            expected[ne++] = r;
        }
        assert(ng == ne);
        for (int i = 0; i < ne; i++) assert(got[i] == expected[i]);
    }
    puts("lap culling: visible primitive sequence and compositing order match");
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="nos-visible-source-") as tmp:
            path = Path(tmp) / "visible.c"
            path.write_text(harness)
            self.compile_run(path)

    def test_trace_group_culling_preserves_order_and_bounds(self):
        source = (ROOT / "nos/apps/fit_heart.c").read_text()
        start = source.index("typedef struct { float x, y, d; uint16_t c; } hdot;")
        end = source.index("/* -------------------------------------------------------- vertical pager", start)
        production = source[start:end]
        harness = r"""
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"
static float qz(float v, float steps) { return floorf(v * steps + 0.5f) / steps; }
void *hal_big_alloc(uint32_t n) { return malloc(n); }
typedef struct { float x,y,d; uint16_t c; } paint;
static paint calls[512], ref[512];
static int nc;
void dot_disc(gfx_fb *fb, float x, float y, float d, uint16_t c)
{ (void)fb; calls[nc++] = (paint){x,y,d,c}; }
""" + production + r"""
int main(void)
{
    dlist l = {0}; dl_alloc(&l, 384); dl_reset(&l);
    for (int i = 0; i < 384; i++)
        dl_add(&l, (i % 40) * 7.25f, (i * 37 % 500) * 0.75f - 64,
               (i % 21) * 0.25f, (uint16_t)(i * 91));
    dl_band *bands = l.band;
    for (int fallback = 0; fallback < 2; fallback++) {
        l.band = fallback ? NULL : bands;
        for (int off = -467; off <= 467; off += 41)
        for (int base = 0; base < GFX_H; base += 7)
        for (int height = 1; height <= 34; height += 11) {
            gfx_fb fb = {0}; fb.h = height; fb.y_base = base; fb.oy = -13;
            memset(calls, 0, sizeof calls); memset(ref, 0, sizeof ref); nc = 0;
            dl_draw(&fb, &l, (float)off + 0.75f);
            int nr = 0, o = (int)((float)off + 0.75f); float lo = (float)(base - fb.oy), hi = lo + height;
            for (int i = 0; i < l.n; i++) {
                hdot *p = &l.d[i]; float y = p->y + (float)o, e = p->d * 0.5f + 1;
                if (y + e <= lo || y - e >= hi) continue;
                ref[nr++] = (paint){p->x,y,p->d,p->c};
            }
            assert(nc == nr);
            for (int j = 0; j < nr; j++) {
                assert(calls[j].x == ref[j].x && calls[j].y == ref[j].y);
                assert(calls[j].d == ref[j].d && calls[j].c == ref[j].c);
            }
        }
    }
    puts("trace culling: ordered primitive sequence and allocation fallback match");
    free(bands); free(l.d); return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="nos-trace-source-") as tmp:
            path = Path(tmp) / "trace.c"
            path.write_text(harness)
            self.compile_run(path)

if __name__ == "__main__":
    unittest.main()
