"""Desktop decision replay must preserve modes and reject incomplete references."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which("gcc"), "gcc required")
class QualityTapeTests(unittest.TestCase):
    def test_keyed_replay_and_faults(self):
        source = r'''#include "nos_quality_tape.h"
#include <assert.h>
int main(int argc,char **argv) {
 assert(argc==3);
 if(!strcmp(argv[1],"record")) {
  qt_open(argv[2],0);
  assert(qt_decide(1,0,11,2)==2); assert(qt_decide(1,0,11,2)==2);
  assert(qt_decide(1,2,0,1)==1);
  assert(qt_decide(1,3,0x10000,0)==0); assert(qt_decide(1,3,0x10001,1)==1);
  assert(qt_decide(1,3,0x20000,1)==1); assert(qt_decide(1,4,0x10000,1)==1);
  assert(qt_decide(1,5,4,1)==1);
  assert(qt_decide(2,1,22,0)==0);
  qt_close(~0u);return 0;
 }
 qt_open(argv[2],1);
 if(!strcmp(argv[1],"missing")) return qt_decide(1,0,99,1);
 assert(qt_decide(1,2,0,0)==1); /* reference overrides today's adaptive input */
 assert(qt_decide(1,0,11,0)==2); /* keyed order and repeated calls may differ */
 assert(qt_decide(1,0,11,1)==2);
 if(!strcmp(argv[1],"badcall")) return qt_decide(1,3,0x30000,0);
 assert(qt_decide(1,3,0x10000,1)==0); assert(qt_decide(1,3,0x10001,0)==1);
 assert(qt_decide(1,3,0x20000,0)==1); assert(qt_decide(1,4,0x10000,0)==1);
 if(!strcmp(argv[1],"pblur_missing")) return qt_decide(1,5,3,0);
 if(strcmp(argv[1],"pblur_unused")) assert(qt_decide(1,5,4,0)==1);
 if(!strcmp(argv[1],"partial")) {qt_close(1);return 0;}
 if(strcmp(argv[1],"unused")) assert(qt_decide(2,1,22,2)==0);
 qt_close(~0u);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="nos-quality-") as tmp:
            tmp = Path(tmp)
            c, exe, tape = tmp / "check.c", tmp / "check.exe", tmp / "quality.txt"
            c.write_text(source)
            subprocess.run(["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Isim", str(c), "-o", str(exe)], cwd=ROOT, check=True, capture_output=True)
            def run(mode):
                return subprocess.run([str(exe), mode, str(tape)], capture_output=True, text=True)
            self.assertEqual(run("record").returncode, 0)
            self.assertEqual(run("replay").returncode, 0)
            self.assertEqual(run("partial").returncode, 0)
            for mode, diagnostic in [("missing", "missing decision"), ("unused", "unused decision"), ("badcall", "missing decision"), ("pblur_missing", "missing decision"), ("pblur_unused", "unused decision")]:
                result=run(mode)
                self.assertEqual(result.returncode, 3)
                self.assertIn(diagnostic, result.stderr)
            original=tape.read_text()
            tape.write_text(original + "2 1 00000016 0\n")
            self.assertIn("duplicate entry", run("replay").stderr)
            tape.write_text(original + "garbage\n")
            self.assertIn("malformed entry", run("replay").stderr)

            tape.write_text(original + "2 3 00010000 2\n")
            self.assertIn("invalid entry", run("replay").stderr)
            tape.write_text(original + "2 5 00000004 2\n")
            self.assertIn("invalid entry", run("replay").stderr)
            tape.write_text(original + "2 5 00000007 0\n")
            self.assertIn("invalid entry", run("replay").stderr)
            tape.write_text(original + "2 6 00000000 0\n")
            self.assertIn("invalid entry", run("replay").stderr)
            tape.write_text(original + "2 4 00010000 2\n")
            self.assertIn("invalid entry", run("replay").stderr)
            tape.write_text(original + "1 3 00010000 0\n")
            self.assertIn("invalid entry", run("replay").stderr)  # frame order remains strict

            tape.write_text(original.replace("2 1 00000016 0\n", "1 3 00010000 0\n2 1 00000016 0\n"))
            self.assertIn("duplicate entry", run("replay").stderr)

    def test_mandatory_cache_stops_are_not_replayed(self):
        # Compile each actual production work loop with admission/failure stubs.
        # A replay callback always returning continue must never bypass failure.
        # Explicitly select the runtime baker; immutable ROM has no bake work.
        common = r'''#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#define GFX_COST_MODEL 1
static unsigned nos_frame=7,calls,steps,mode,lastkey;
static uint32_t hal_micros(void){return 100;}
static int decide(unsigned f,int k,unsigned key,int proposed){(void)f;(void)k;(void)proposed;calls++;lastkey=key;return 0;}
static int (*nos_quality_decision)(unsigned,int,unsigned,int)=decide;
static unsigned bake_tape_frame=~0u,bake_tape_call;
'''
        settings=(ROOT/'nos/apps/set_settings.c').read_text()
        settings=settings[settings.index('static void rows_work(int home)\n{'):settings.index('static void rows_open(void)\n{')]
        launcher=(ROOT/'nos/apps/sys_launcher.c').read_text()
        launcher=launcher[launcher.index('void sys_launcher_prewarm(void)\n{'):launcher.index('/* ---- scrolling',launcher.index('void sys_launcher_prewarm(void)\n{'))]
        cases=[(settings,r'''static unsigned set_settings_bake_budget_us=750,set_settings_bake_max_steps=2;
static int row_deferred;static struct {int lut_ready;struct{int active;}job;int rom_bound;} cache={1,{1},0},*RC=&cache;
static int settings_rom_active(void){return 0;}static void rows_rom_sync(void){}
static int rows_init(void){return 1;}static void rows_palette(void){}
static void rows_palette_step(void){}static int rows_choose(int home){(void)home;return 0;}
static void row_start(int e){(void)e;}static void row_step_job(void){steps++;if(mode)row_deferred=1;}static void row_sharp_trim(void){}
''',r'''int main(void){mode=1;rows_work(0);assert(steps==1&&calls==0);mode=0;steps=0;rows_work(1);assert(steps==2&&calls==2&&(lastkey>>31));return 0;}
'''),(launcher,r'''static unsigned sys_launcher_prewarm_budget_us=750,sys_launcher_prewarm_max_steps=2;
static int s_failed,s_baked,s_memory_wait,s_tables_ready;
#define NOS_RENDER_SHARP 0
#define NVAR 1
#define NBLUR 1
static struct{struct{int ready,blurred,stage;}rows[1];}cache,*L=&cache;
static int napps(void){return 1;}static int bake_next(void){return 0;}
static void launcher_rom_sync(void){}static int launcher_adopt_rom(int i){(void)i;return 0;}
static void ensure_tables(void){steps++;if(mode==1)s_failed=1;if(mode==2)s_memory_wait=1;}
static void bake_blur_step(int i){(void)i;}static void bake_sharp_step(int i){(void)i;}
''',r'''int main(void){mode=1;sys_launcher_prewarm();assert(steps==1&&calls==0);s_failed=0;mode=2;steps=0;sys_launcher_prewarm();assert(steps==1&&calls==0);mode=0;steps=0;sys_launcher_prewarm();assert(steps==2&&calls==2);return 0;}
''')]
        with tempfile.TemporaryDirectory(prefix='nos-budget-') as tmp:
            for i,(body,stubs,main) in enumerate(cases):
                c=Path(tmp)/f'check{i}.c';exe=c.with_suffix('.exe')
                c.write_text(common+stubs+body+main)
                compiled=subprocess.run(['gcc','-std=c99','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
                subprocess.run([str(exe)],check=True,capture_output=True)

    def test_pblur_replays_only_admission_not_mandatory_guards(self):
        production = (ROOT / "nos/pblur.c").read_text()
        begin = production.index("int pblur_tick(int id, int vertical, pb_draw_fn draw)\n{")
        end = production.index("    pb_slot *s = &PB->slot[id];", begin)
        # Keep the actual production guard/admission/initialization prefix. The
        # remainder is irrelevant here and would require a whole renderer stub.
        body = production[begin:end] + "    return 99;\n}\n"
        common = r"""#include <stdint.h>
#include <assert.h>
#define PB_N 7
static int nos_mblur=2,nos_aod,transition,initialized,calls,answer,lastkind;
static unsigned nos_frame=1,pb_frame,lastkey,pblur_why[5];
static struct {uint32_t raster_us,wait_us,record_us;} nos_last;
typedef void (*pb_draw_fn)(void *);
static int nos_transitioning(void){return transition;}
static int pblur_busy(int id){(void)id;return 17;}
static int init(void){initialized++;return 0;}
static void draw(void *p){(void)p;}
#ifdef GFX_COST_MODEL
static int decide(unsigned frame,int kind,unsigned key,int proposed){
 (void)proposed;assert(frame==nos_frame);calls++;lastkind=kind;lastkey=key;return answer;
}
static int (*nos_quality_decision)(unsigned,int,unsigned,int)=decide;
#endif
"""
        check = r"""int main(void){
 nos_mblur=0;assert(pblur_tick(4,0,draw)==0);nos_mblur=2;
 nos_aod=1;assert(pblur_tick(4,0,draw)==0);nos_aod=0;
 transition=1;assert(pblur_tick(4,0,draw)==0);transition=0;
 assert(pblur_tick(-1,0,draw)==0 && pblur_tick(7,0,draw)==0);
 assert(pblur_tick(4,0,0)==0);assert(calls==0 && initialized==0);
 pb_frame=nos_frame;assert(pblur_tick(4,0,draw)==17 && calls==0);
 nos_frame++;nos_last.raster_us=30000;answer=0;
#ifdef GFX_COST_MODEL
 /* Replay continue overrides CPU admission, but cannot override init failure. */
 assert(pblur_tick(4,0,draw)==0 && initialized==1 && calls==1);
 assert(lastkind==5 && lastkey==4);
 nos_frame++;nos_last.raster_us=0;answer=1;
 assert(pblur_tick(4,0,draw)==1 && initialized==1 && calls==2);
 nos_quality_decision=0;nos_frame++;nos_last.raster_us=30000;
 assert(pblur_tick(4,0,draw)==1 && initialized==1 && calls==2);
#else
 assert(pblur_tick(4,0,draw)==1 && initialized==0 && calls==0);
 nos_frame++;nos_last.raster_us=0;
 assert(pblur_tick(4,0,draw)==0 && initialized==1 && calls==0);
#endif
 return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="nos-pblur-admission-") as tmp:
            source=Path(tmp)/"check.c";source.write_text(common+body+check)
            for model in (False,True):
                binary=Path(tmp)/("model.exe" if model else "device.exe")
                command=["gcc","-std=c99","-O2","-Wall","-Wextra","-Werror",
                         "-Wno-unused-parameter","-Wno-unused-variable"]
                if model:command.append("-DGFX_COST_MODEL")
                subprocess.run([*command,str(source),"-o",str(binary)],check=True,capture_output=True)
                subprocess.run([str(binary)],check=True,capture_output=True)
