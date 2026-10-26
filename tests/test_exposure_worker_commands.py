"""Compile actual firmware command guard and ACK helper; no device/SDK access."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/alpha05/checks/exposure-worker-command'


def block(source,marker):
    start=source.index(marker);brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


@unittest.skipUnless(shutil.which('gcc'),'GCC required for actual command branch')
class ExposureWorkerCommandTests(unittest.TestCase):
    def test_actual_outer_quiet_route_cannot_wake_before_worker_validation(self):
        source=(ROOT/'firmware/main/nos_main.c').read_text()
        loop=source[source.index('    for (;;) {',source.index('void app_main(void)')):]
        start=loop.index('int quiet =')
        wake='if(mutate && s_pwr!=PWR_ON && !quiet)go_state(PWR_ON);'
        end=loop.index(wake,start)+len(wake)
        route=loop[start:end]
        branch=block(source,'else if (!strncmp(s,"exposure_worker",15))')
        ack=block(source,'static int cmd_printf(')
        fixture=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nos_frame_perf.h"
#ifndef NOS_RENDER_SHARP
#define NOS_RENDER_SHARP 0 /* nos/render_config.h default; firmware/main/CMakeLists.txt sets 1 only for the SHARP build */
#endif
static nos_frame_capture s_capture;
static int s_exposure_worker,s_inj_active,s_touch_now,s_touch_any,s_cmd_ok;
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
 const char *commands[]={"exposure_worker","exposure_worker 2","exposure_worker 1 extra","exposure_workerx 1","exposure_worker 0","exposure_worker 1"};
 for(unsigned i=0;i<sizeof commands/sizeof *commands;i++)for(int active=0;active<2;active++){
  s_pwr=PWR_OFF;s_exposure_worker=0;wakes=fences=unknown=0;s_cmd_ok=1;
  s_capture.state=active?NOS_CAPTURE_ACTIVE:NOS_CAPTURE_COMPLETE;route(commands[i],1);
  assert(s_pwr==PWR_OFF&&!wakes&&!fences&&!unknown);
  assert(s_exposure_worker==(!active&&i==5));
  assert(s_cmd_ok==(!active&&i>=4));
 }
 s_pwr=PWR_OFF;route("open Settings",1);assert(wakes==1&&fences==1&&s_pwr==PWR_ON&&unknown==1);
 puts("PASS actual outer exposure_worker route:12 awake/syntax/capture cases never wake or fence; ordinary app navigation still wakes");return 0;
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
        branch=block(source,'else if (!strncmp(s,"exposure_worker",15))')
        ack=block(source,'static int cmd_printf(')
        fixture=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define nos_frame_capture_live real_capture_live
#include "nos_frame_perf.h"
#ifndef NOS_RENDER_SHARP
#define NOS_RENDER_SHARP 0 /* nos/render_config.h default; firmware/main/CMakeLists.txt sets 1 only for the SHARP build */
#endif
#undef nos_frame_capture_live
static nos_frame_capture s_capture;
static int s_exposure_worker,s_inj_active,s_touch_now,s_touch_any,s_cmd_ok;
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
 s_exposure_worker=initial;s_inj_active=(touches&1)!=0;s_touch_now=(touches&2)!=0;s_touch_any=(touches&4)!=0;
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
 const char *invalid[]={"exposure_worker","exposure_worker 2","exposure_worker -1","exposure_worker +1",
 "exposure_worker 01","exposure_worker 1x","exposure_worker 1 extra","exposure_worker 1 ",
 "exposure_worker\t1","exposure_worker  1","exposure_workerx 1","exposure_worker1","exposure_worker 1\n"};
 for(int initial=0;initial<2;initial++)for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
  reset(initial,NOS_CAPTURE_ACTIVE,7);nos_frame_capture before=s_capture;
  dispatch(invalid[i]);assert(s_exposure_worker==initial&&!s_cmd_ok&&!unknown&&!capture_checks);
  assert(!strcmp(output,"ERR exposure_worker values=0|1\n"));unchanged(&before);syntax_cases++;
 }
 for(int initial=0;initial<2;initial++)for(int desired=0;desired<2;desired++)
 for(int state=NOS_CAPTURE_EMPTY;state<=NOS_CAPTURE_ABORTED;state++)for(int touch=0;touch<8;touch++){
  reset(initial,state,touch);nos_frame_capture before=s_capture;
  dispatch(desired?"exposure_worker 1":"exposure_worker 0");assert(capture_checks==1&&!unknown);
  if(real_capture_live(&before)){
   assert(s_exposure_worker==initial&&!s_cmd_ok);assert(!strcmp(output,"ERR exposure_worker capture_active\n"));
  }else if(touch){
   assert(s_exposure_worker==initial&&!s_cmd_ok);assert(!strcmp(output,"ERR exposure_worker touch_active\n"));
  }else{
   assert(s_exposure_worker==desired&&s_cmd_ok);
   assert(!strcmp(output,desired?"NOSDIAG exposure_worker=1\n":"NOSDIAG exposure_worker=0\n"));
  }
  assert(s_inj_active==((touch&1)!=0)&&s_touch_now==((touch&2)!=0)&&s_touch_any==((touch&4)!=0));
  unchanged(&before);guard_cases++;
 }
 reset(1,NOS_CAPTURE_ACTIVE,7);nos_frame_capture before=s_capture;dispatch("unrelated");
 assert(unknown==1&&s_exposure_worker==1&&s_cmd_ok&&!capture_checks&&!output[0]);unchanged(&before);
 printf("PASS actual exposure_worker guard: syntax=%u capture_touch_cases=%u typed_rejections=1 immutable_capture_jobs=1 fences=0\n",syntax_cases,guard_cases);
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
            self.assertIn('syntax=26 capture_touch_cases=192',result.stdout)
        self.assertEqual(hashlib.sha256(firmware.read_bytes()).hexdigest(),before)


if __name__=='__main__':unittest.main()
