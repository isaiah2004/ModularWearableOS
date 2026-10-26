"""Compile the actual firmware/simulator command branches, with observable driver stubs."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
def block(text,marker):
 start=text.index(marker); brace=text.index('{',start); depth=1;end=brace+1
 while depth:
  if text[end]=='{':depth+=1
  elif text[end]=='}':depth-=1
  end+=1
 return text[start:end]
class HistoryVirtualCommandsTests(unittest.TestCase):
 def test_actual_dispatch_and_report(self):
  fw=(ROOT/'firmware/main/nos_main.c').read_text();sim=(ROOT/'sim/nos_sim.c').read_text()
  branch=block(fw,'else if (!strncmp(s,"history_virtual",15))')
  report=block(fw,'static void history_virtual_report(void)')
  simbranch=block(sim,'else if (!_stricmp(key,"history_virtual"))')
  source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint64_t virtual_pixels,avoided_bytes,materialize_bytes,classify_pixels;} nos_history_virtual_stats;
static nos_history_virtual_stats counters={4294967297ULL,8589934594ULL,17,51};
static int enabled,sets,reports,unknown,s_capture,s_inj_active,s_touch_now,s_touch_any;
static int nos_frame_capture_live(const int *p){return *p;}
static void nos_history_virtual_set(int e){sets++;enabled=e;if(!e)counters.materialize_bytes+=64;}
static int nos_history_virtual_get(void){return enabled;}
static const nos_history_virtual_stats *nos_history_virtual_get_stats(void){reports++;return &counters;}
'''+report+'\nstatic void dispatch(char *s){if(0){} '+branch+' else unknown++;}\nstatic int set_key(const char *key,const char *val,const char *rest){if(0){} '+simbranch+r''' else return 0;return 1;}
int main(void){
 const char *bad[]={"history_virtual","history_virtual 2","history_virtual -1","history_virtual 01","history_virtual +1","history_virtual 1x","history_virtual 1 extra","history_virtual\t1","history_virtual 1 ","history_virtualx 1"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){dispatch((char*)bad[i]);assert(!sets&&!reports&&!enabled);}
 dispatch("unrelated");assert(unknown==1&&!sets);
 s_capture=1;dispatch("history_virtual 0");s_capture=0;assert(!sets&&!reports&&!enabled&&counters.materialize_bytes==17);
 dispatch("history_virtual 1");assert(sets==1&&reports==1&&enabled==1);
 s_capture=1;dispatch("history_virtual 0");s_capture=0;assert(sets==1&&reports==1&&enabled==1);
 s_inj_active=1;dispatch("history_virtual 0");s_inj_active=0;
 s_touch_now=1;dispatch("history_virtual 0");s_touch_now=0;
 s_touch_any=1;dispatch("history_virtual 0");s_touch_any=0;assert(sets==1&&reports==1&&enabled==1);
 dispatch("history_virtual 0");assert(sets==2&&reports==2&&!enabled&&counters.materialize_bytes==81);
 const char *invalid[]={"","2","01","+1","1x","1 extra","1 "," 1"};
 for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){int n=sets;assert(!set_key("history_virtual",invalid[i],invalid[i]));assert(sets==n);}
 assert(!set_key("history_virtual","1","1 extra"));assert(!set_key("history_virtualx","1","1"));
 assert(set_key("history_virtual","1","1"));assert(enabled==1&&sets==3&&reports==3);
 assert(set_key("history_virtual","0","0"));assert(!enabled&&sets==4&&reports==4);
 puts("PASS actual firmware/simulator dispatch: exact syntax, all guards before mutation, post-set64bit report, disable ACK after materialization");return 0;
}
'''
  with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
   c=Path(d)/'dispatch.c';exe=Path(d)/'dispatch.exe';c.write_text(source)
   subprocess.run(['gcc','-O2','-std=c99','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
   p=subprocess.run([str(exe)],capture_output=True,text=True);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   self.assertIn('virtual_pixels=4294967297 avoided_bytes=8589934594 materialize_bytes=81 classify_pixels=51',p.stdout)
   self.assertIn('PASS actual firmware/simulator dispatch',p.stdout)
 def test_fenced_quiet_reporting_boundaries(self):
  s=(ROOT/'firmware/main/nos_main.c').read_text();loop=s[s.index('    for (;;) {',s.index('void app_main(void)')):]
  dispatch=loop.index('host_ui_dispatch(&line,mutate);');begin=loop.index('nos_frame_priority_begin(')
  self.assertLess(dispatch,begin)
  dispatcher=block(s,'static void host_ui_dispatch(');executor=block(s,'static void host_ui_execute(')
  self.assertIn('if(mutate || host_command_readonly(line->text)) {host_ui_execute(line);return;}',dispatcher)
  self.assertEqual(executor.count('cmd_exec('),1)
  self.assertLess(executor.index('printf("BEGIN '),executor.index('cmd_exec('))
  self.assertLess(executor.index('cmd_exec('),executor.index('host_done('))
  quiet=loop.index('int quiet =');wake=loop.index('if(mutate && s_pwr!=PWR_ON && !quiet)go_state(PWR_ON);')
  self.assertIn('!strncmp(line.text,"history_virtual",15)',loop[quiet:wake])
  self.assertLess(quiet,wake);self.assertLess(wake,dispatch)
  self.assertLess(loop.index('xSemaphoreTake(s_host_reply_lock,0)'),quiet)
  self.assertLess(dispatch,loop.index('xSemaphoreGive(s_host_reply_lock)'));self.assertLess(loop.index('xSemaphoreGive(s_host_reply_lock)'),begin)
  # Normal presentation completes its DMA fence before priority restoration;
  # the next iteration's queued command runs before any new elevated render.
  fence=loop.index('frame_drain();',begin);completed=loop.index('int64_t completed = esp_timer_get_time();',fence)
  self.assertLess(fence,completed);self.assertLess(completed,loop.index('nos_frame_priority_restore(',begin))
  for command in ['state','info']:
   b=block(s,'else if (!strcmp(s, "'+command+'"))')
   self.assertIn('if(!nos_frame_capture_live(&s_capture)){frame_priority_report();set_settings_carry_report();history_virtual_report();}',b)
  state=s[s.index('printf("NOSSTATE'):s.index('settings_shutter_report();',s.index('printf("NOSSTATE'))];self.assertNotIn('virtual',state)
if __name__=='__main__':unittest.main()


