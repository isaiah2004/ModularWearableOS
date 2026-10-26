"""Compile actual firmware command guard and ACK helper; no device/SDK access."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/alpha05/checks/native-stage-command'


def block(source,marker):
    start=source.index(marker);brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


@unittest.skipUnless(shutil.which('gcc'),'GCC required for actual command branch')
class NativeStageCommandTests(unittest.TestCase):
    def test_actual_outer_quiet_route_cannot_wake_before_native_validation(self):
        source=(ROOT/'firmware/main/nos_main.c').read_text()
        loop=source[source.index('    for (;;) {',source.index('void app_main(void)')):]
        start=loop.index('int quiet =')
        wake='if(mutate && s_pwr!=PWR_ON && !quiet)go_state(PWR_ON);'
        end=loop.index(wake,start)+len(wake)
        route=loop[start:end]
        branch=block(source,'else if (!strncmp(s,"native_stage",12))')
        ack=block(source,'static int cmd_printf(')
        fixture=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nos_frame_perf.h"
static nos_frame_capture s_capture;
static int nos_native_row_stage_mode,s_inj_active,s_touch_now,s_touch_any,s_cmd_ok;
static int wakes,fences,unknown,s_pwr;
enum {PWR_ON=0,PWR_OFF=2};
typedef struct {char text[192];} cmd_line;
static char output[256];
static int captured_vprintf(const char *format,va_list args){return vsnprintf(output,sizeof output,format,args);}
static void go_state(int state){wakes++;fences++;s_pwr=state;}
#define vprintf captured_vprintf
''' + ack + '\nstatic void dispatch(const char *s){if(0){} ' + branch + r''' else unknown++;}
static void route(const char *command,int mutate){cmd_line line;snprintf(line.text,sizeof line.text,"%s",command);
''' + route + r'''
 dispatch(line.text);
}
int main(void){
 const char *commands[]={"native_stage","native_stage 4","native_stage 1 extra","native_stagex 1","native_stage 0","native_stage 1","native_stage 2","native_stage 3"};
 for(unsigned i=0;i<sizeof commands/sizeof *commands;i++)for(int active=0;active<2;active++){
  s_pwr=PWR_OFF;nos_native_row_stage_mode=0;wakes=fences=unknown=0;s_cmd_ok=1;
  s_capture.state=active?NOS_CAPTURE_ACTIVE:NOS_CAPTURE_COMPLETE;route(commands[i],1);
  assert(s_pwr==PWR_OFF&&!wakes&&!fences&&!unknown);
  assert(nos_native_row_stage_mode==(!active&&i>=4?(int)i-4:0));
  assert(s_cmd_ok==(!active&&i>=4));
 }
 s_pwr=PWR_OFF;route("open Settings",1);assert(wakes==1&&fences==1&&s_pwr==PWR_ON&&unknown==1);
 puts("PASS actual outer native_stage route:16 awake/syntax/capture cases never wake or fence; ordinary app navigation still wakes");return 0;
}
'''
        OUT.mkdir(parents=True,exist_ok=True)
        env=os.environ | {key:str(OUT) for key in ('TEMP','TMP','TMPDIR')}
        with tempfile.TemporaryDirectory(prefix='route-',dir=OUT) as directory:
            path=Path(directory);c=path/'route.c';c.write_text(fixture)
            executable=path/('route.exe' if os.name=='nt' else 'route')
            compiled=subprocess.run(['gcc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'firmware/main'),str(c),'-o',str(executable)],
                cwd=path,env=env,capture_output=True,text=True,timeout=30)
            self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
            result=subprocess.run([str(executable)],cwd=path,env=env,capture_output=True,text=True,timeout=10)
            (OUT/'route.log').write_text(result.stdout+result.stderr)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('ordinary app navigation still wakes',result.stdout)

    def test_actual_strict_syntax_capture_touch_and_ack_before_mutation(self):
        firmware=ROOT/'firmware/main/nos_main.c';before=hashlib.sha256(firmware.read_bytes()).hexdigest()
        source=firmware.read_text()
        branch=block(source,'else if (!strncmp(s,"native_stage",12))')
        ack=block(source,'static int cmd_printf(')
        fixture=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define nos_frame_capture_live real_capture_live
#include "nos_frame_perf.h"
#undef nos_frame_capture_live
static nos_frame_capture s_capture;
static int nos_native_row_stage_mode,s_inj_active,s_touch_now,s_touch_any,s_cmd_ok;
static unsigned capture_checks,fences,unknown,syntax_cases,guard_cases;
static uint32_t s_exposure_ready[2],s_exposure_job[16];
static char output[256];
static int nos_frame_capture_live(const nos_frame_capture *capture){capture_checks++;return real_capture_live(capture);}
static int captured_vprintf(const char *format,va_list args){return vsnprintf(output,sizeof output,format,args);}
#define vprintf captured_vprintf
#define stripe_exposure_finish() (++fences,0u)
#define frame_drain() (++fences)
''' + ack + '\nstatic void dispatch(const char *s){if(0){} ' + branch + r''' else unknown++;}
static void reset(int initial,int state,int touches){
 memset(&s_capture,0,sizeof s_capture);s_capture.state=state;s_capture.perf.frames=100;
 s_capture.perf.painted=91;s_capture.start=123;s_capture.completed=456;
 nos_native_row_stage_mode=initial;s_inj_active=(touches&1)!=0;s_touch_now=(touches&2)!=0;s_touch_any=(touches&4)!=0;
 s_cmd_ok=1;capture_checks=fences=unknown=0;output[0]=0;
 s_exposure_ready[0]=7;s_exposure_ready[1]=11;
 for(unsigned i=0;i<16;i++)s_exposure_job[i]=100+i;
}
static void unchanged(const nos_frame_capture *before){
 assert(!memcmp(&s_capture,before,sizeof s_capture));assert(!fences);
 assert(s_exposure_ready[0]==7&&s_exposure_ready[1]==11);
 for(unsigned i=0;i<16;i++)assert(s_exposure_job[i]==100+i);
}
int main(void){
 const char *invalid[]={"native_stage","native_stage 4","native_stage -1","native_stage +1",
 "native_stage 01","native_stage 1x","native_stage 1 extra","native_stage 1 ",
 "native_stage\t1","native_stage  1","native_stagex 1","native_stage1","native_stage 1\n"};
 for(int initial=0;initial<4;initial++)for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
  reset(initial,NOS_CAPTURE_ACTIVE,7);nos_frame_capture before=s_capture;
  dispatch(invalid[i]);assert(nos_native_row_stage_mode==initial&&!s_cmd_ok&&!unknown&&!capture_checks);
  assert(!strcmp(output,"ERR native_stage values=0|1|2|3\n"));unchanged(&before);syntax_cases++;
 }
 for(int initial=0;initial<4;initial++)for(int desired=0;desired<4;desired++)
 for(int state=NOS_CAPTURE_EMPTY;state<=NOS_CAPTURE_ABORTED;state++)for(int touch=0;touch<8;touch++){
  reset(initial,state,touch);nos_frame_capture before=s_capture;
  char command[64];snprintf(command,sizeof command,"native_stage %d",desired);dispatch(command);assert(capture_checks==1&&!unknown);
  if(real_capture_live(&before)){
   assert(nos_native_row_stage_mode==initial&&!s_cmd_ok);assert(!strcmp(output,"ERR native_stage capture_active\n"));
  }else if(touch){
   assert(nos_native_row_stage_mode==initial&&!s_cmd_ok);assert(!strcmp(output,"ERR native_stage touch_active\n"));
  }else{
   assert(nos_native_row_stage_mode==desired&&s_cmd_ok);
   char expected[80];snprintf(expected,sizeof expected,"NOSDIAG native_stage=%d\n",desired);assert(!strcmp(output,expected));
  }
  assert(s_inj_active==((touch&1)!=0)&&s_touch_now==((touch&2)!=0)&&s_touch_any==((touch&4)!=0));
  unchanged(&before);guard_cases++;
 }
 reset(1,NOS_CAPTURE_ACTIVE,7);nos_frame_capture before=s_capture;dispatch("unrelated");
 assert(unknown==1&&nos_native_row_stage_mode==1&&s_cmd_ok&&!capture_checks&&!output[0]);unchanged(&before);
 printf("PASS actual native_stage guard: syntax=%u capture_touch_cases=%u typed_rejections=1 immutable_capture_jobs=1 fences=0\n",syntax_cases,guard_cases);
 return 0;
}
'''
        OUT.mkdir(parents=True,exist_ok=True)
        env=os.environ | {key:str(OUT) for key in ('TEMP','TMP','TMPDIR')}
        with tempfile.TemporaryDirectory(prefix='actual-',dir=OUT) as directory:
            path=Path(directory);c=path/'guard.c';c.write_text(fixture)
            executable=path/('guard.exe' if os.name=='nt' else 'guard')
            compiled=subprocess.run(['gcc','-O2','-std=c11','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'firmware/main'),str(c),'-o',str(executable)],
                cwd=path,env=env,capture_output=True,text=True,timeout=30)
            self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
            result=subprocess.run([str(executable)],cwd=path,env=env,capture_output=True,text=True,timeout=10)
            (OUT/'run.log').write_text(result.stdout+result.stderr)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('syntax=52 capture_touch_cases=768',result.stdout)
        self.assertEqual(hashlib.sha256(firmware.read_bytes()).hexdigest(),before)

    def test_actual_state_printf_reports_each_mode_once(self):
        source=(ROOT/'firmware/main/nos_main.c').read_text()
        state=block(source,'else if (!strcmp(s, "state"))')
        start=state.index('printf("NOSSTATE ')
        end=state.index(');',start)+2
        report=state[start:end]
        fixture=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef struct {const char *name;} app;
static app *nos_top(void){return NULL;}
static struct {int face;} nos_prefs;
static int s_pwr,s_no_sleep,s_perf,s_trace,nos_locked,nos_mblur,nos_layer_cache_enabled,nos_temporal_blur;
static int nos_history_uniform,nos_history_packed,s_exposure_worker,orrery_card_qz_exact,nos_debug_force_full;
static int orrery_card_lighting_cache,orrery_card_shade_q16,orrery_card_sun_fast,orrery_card_progressive;
static int orrery_card_motion_proxy,s_pipeline_enabled,blur_cache_packed,nos_native_row_stage_mode;
static unsigned nos_frame,orrery_card_render_budget_us,orrery_card_motion_step;
static int nos_transitioning(void){return 0;}
static int orrery_card_profile_enabled(void){return 0;}
static int orrery_shade_q16_ready(void){return 0;}
static int set_settings_shutter_get(void){return 0;}
static int board_pixels_rotation_mode_get(void){return 0;}
static int nos_damage_bands_get(void){return 0;}
static char output[1024];
static int capture_printf(const char *format,...) __attribute__((format(printf,1,2)));
static int capture_printf(const char *format,...){va_list args;va_start(args,format);
 int result=vsnprintf(output,sizeof output,format,args);va_end(args);return result;}
#define printf capture_printf
static void report(void){
''' + report + r'''
}
#undef printf
int main(void){
 for(int mode=0;mode<4;mode++){
  nos_native_row_stage_mode=mode;report();assert(!strncmp(output,"NOSSTATE ",9));
  char expected[32];snprintf(expected,sizeof expected," native_stage=%d\n",mode);
  char *found=strstr(output,expected);assert(found&&found[strlen(expected)]=='\0');
  assert(!strstr(found+2,"native_stage="));
 }
 puts("PASS actual NOSSTATE format/arguments: all4 native_stage values exactly once");return 0;
}
'''
        OUT.mkdir(parents=True,exist_ok=True)
        env=os.environ | {key:str(OUT) for key in ('TEMP','TMP','TMPDIR')}
        with tempfile.TemporaryDirectory(prefix='state-',dir=OUT) as directory:
            path=Path(directory);c=path/'state.c';c.write_text(fixture)
            executable=path/('state.exe' if os.name=='nt' else 'state')
            compiled=subprocess.run(['gcc','-O2','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(executable)],
                cwd=path,env=env,capture_output=True,text=True,timeout=30)
            self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
            result=subprocess.run([str(executable)],cwd=path,env=env,capture_output=True,text=True,timeout=10)
            (OUT/'state.log').write_text(result.stdout+result.stderr)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_public_benchmark_requires_and_preserves_native_stage_flag(self):
        spec=importlib.util.spec_from_file_location('native_stage_benchmark',ROOT/'tools/target_benchmark.py')
        benchmark=importlib.util.module_from_spec(spec);spec.loader.exec_module(benchmark)
        self.assertEqual(benchmark.GLOBAL_KEYS.count('native_stage'),1)
        state={key:'0' for key in benchmark.GLOBAL_KEYS}
        sections={section:[{field:'0'}] for section,field in benchmark.EXTRA_FLAGS.items()}
        snapshot={'state':state,'sections':sections}
        for mode in range(4):
            state['native_stage']=str(mode)
            self.assertEqual(benchmark.preserved_flags(snapshot)['native_stage'],mode)
        del state['native_stage']
        with self.assertRaises(benchmark.AdmissionError):benchmark.preserved_flags(snapshot)


if __name__=='__main__':unittest.main()
