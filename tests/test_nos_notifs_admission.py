"""Exercise the production notification arena admission/lifetime path in isolation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class NotificationAdmissionTests(unittest.TestCase):
    def test_cold_fallback_exposure_keeps_physical_speed_across_cadences(self):
        source = (ROOT / "nos/apps/sys_notifs.c").read_text()
        tick = source[source.index("static void band_tick(float dt)\n{"):source.index("/* a glide:")]
        cache = (ROOT / "nos/ui_blur.c").read_text()
        ladder = cache[cache.index("int ui_fblur_level_early(float step)"):cache.index("static int fb_level(")]
        harness = r"""
#include "ui.h"
#include "svc.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#define GAP 12.0f
int nos_mblur=2;
static struct {
    struct {float x;int dragging;} sc;
    float band_prev,band_step;
    int band_lv,n,clearing,sw,out_i,fly,gap_i,dragging;
    uint64_t now;
} N;
static struct {unsigned sig[SVC_NOTIF_MAX];} *NL;
static float card_in(int n) {(void)n;return 1;}
static float card_hi(int i) {(void)i;return 100;}
static unsigned card_sig(const svc_notif *v,float h,uint64_t now) {(void)v;(void)h;(void)now;return 0;}
const svc_notif *svc_notif_get(int i) {(void)i;return 0;}
void sys_notifs_prewarm(void) {}
void nos_blur_note(int kind,int moving,int fast,int blurred) {(void)kind;(void)moving;(void)fast;(void)blurred;}
""" + ladder + tick + r"""
int main(void) {
    const float speeds[]={660,1320};
    const int skips[]={1,2,3,7};
    for(int speed=0;speed<2;speed++) for(int skip=0;skip<4;skip++)
    for(int direction=-1;direction<=1;direction+=2) {
        N.n=1;N.sw=N.out_i=N.gap_i=-1;N.band_prev=2000;
        float dt=(float)skips[skip]/M_FPS,total=0;
        for(int frame=1;frame<=4;frame++) {
            float previous=N.band_prev;
            N.sc.x=2000+direction*speeds[speed]*dt*frame;
            band_tick(dt);
            /* The actual rounded source movement owns exposure. At 40Hz,
             * 660px/s alternates 16/17px ticks; the mean remains exactly22. */
            float measured=fabsf(N.band_prev-previous)/(dt*30.0f);
            assert(fabsf(N.band_step-measured)<.001f);
            assert(N.band_lv==(speed?3:2)); /* cached ladder still receives NOS_FPS units */
            total+=N.band_step;
        }
        assert(fabsf(total/4-speeds[speed]/30.0f)<.001f);
        if(skips[skip]==2) {
            /* Exact physical 660/1320px/s samples produce the authored k3/k4
             * tail, including40Hz where the former normalized step was wrong. */
            float tau=N.band_step*.25f;
            int k=tau>=11?4:tau>=5.5f?3:tau>=2.8f?2:1;
            assert(k==(speed?4:3));
        }
    }
    puts("PASS notification cold fallback reference exposure and cached speed ladder");
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="nos-notifs-exposure-") as tmp:
            src = Path(tmp) / "exposure.c"
            src.write_text(harness)
            for cadence in (30, 40, 60):
                with self.subTest(cadence=cadence):
                    binary = Path(tmp) / f"exposure-{cadence}.exe"
                    result = subprocess.run(
                        ["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                         f"-DNOS_FPS={cadence}", "-Icore", "-Inos", str(src), "-lm", "-o", str(binary)],
                        cwd=ROOT, capture_output=True, text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("PASS notification cold fallback", result.stdout)

    def test_budget_failure_cooldown_release_and_recovery(self):
        source = (ROOT / "nos/apps/sys_notifs.c").read_text()
        state = source[source.index("static kblur *s_kb;"):source.index("void sys_notifs_blur_mem(")]
        admission = source[source.index("void sys_notifs_prewarm(void)"):source.index("    if (n > SVC_NOTIF_MAX)")]+"}\n"
        harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
typedef double nos_time_t;
#define NOS_PSRAM_TRACKING 1
#define KB_W 400
#define KB_MAXH 200
typedef int kblur;
static double nos_t;
static uint32_t room;
static int calls, fail=1, transition, count=1, freed;
uint32_t nos_mem_available(void) { return room; }
int nos_transitioning(void) { return transition; }
int svc_notif_count(void) { return count; }
void hal_big_free(void *p) { if(p)freed++;free(p); }
kblur *kblur_new(int w,int h,int slots,uint32_t bytes) {
    assert(w==400 && h==200 && slots==24 && bytes==896u*1024u);
    calls++;return fail ? NULL : malloc(sizeof(kblur));
}
"""+state+admission+r"""
int main(void) {
    room=1408u*1024u-1;
    for(int i=0;i<1000;i++)sys_notifs_prewarm();
    assert(calls==0 && !s_kb);
    room++;sys_notifs_prewarm();assert(calls==1 && !s_kb);
    for(int i=0;i<1000;i++)sys_notifs_prewarm();
    assert(calls==1);
    nos_t=0.999f;sys_notifs_prewarm();assert(calls==1);
    nos_t=1.0f;sys_notifs_prewarm();assert(calls==2);
    room++;fail=0;sys_notifs_prewarm();assert(calls==3 && s_kb);
    for(int i=0;i<1000;i++) { sys_notifs_prewarm(); } assert(calls==3);
    sys_notifs_blur_release();assert(!s_kb && freed==1);
    transition=1;sys_notifs_prewarm();assert(calls==3);
    transition=0;count=0;sys_notifs_prewarm();assert(calls==3);
    count=1;sys_notifs_prewarm();assert(calls==4 && s_kb);
    sys_notifs_blur_release();return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="nos-notifs-admission-") as tmp:
            src = Path(tmp)/"test.c"
            binary = Path(tmp)/("test.exe" if os.name=="nt" else "test")
            src.write_text(harness)
            result=subprocess.run(["gcc","-std=c11","-O2","-Wall","-Wextra","-Werror",str(src),"-o",str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)

if __name__ == "__main__": unittest.main()

