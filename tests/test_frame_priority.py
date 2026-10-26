from pathlib import Path
import subprocess,tempfile,unittest,re
ROOT=Path(__file__).resolve().parents[1]
class FramePriorityTests(unittest.TestCase):
 def test_state_machine(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build') as d:
   exe=Path(d)/'priority.exe';subprocess.run(['gcc','-O2','-std=c99','-Wall','-Wextra','-Werror','-Ifirmware/main','tests/nos_frame_priority_test.c','-o',str(exe)],cwd=ROOT,check=True);subprocess.run([str(exe)],check=True)
 def test_firmware_boundaries(self):
  s=(ROOT/'firmware/main/nos_main.c').read_text();main=s[s.index('void app_main(void)'):];loop=main[main.index('    for (;;) {'):]
  self.assertIn('nos_frame_priority_init(&s_frame_priority,(unsigned)uxTaskPriorityGet(NULL))',main)
  begin=loop.index('nos_frame_priority_begin(');finish=loop.index('int64_t completed = esp_timer_get_time();');restore=loop.index('nos_frame_priority_restore(',begin)
  self.assertLess(loop.index('nos_frame_priority_restore('),loop.index('nos_power_active_begin('))
  helper_start=s.index('static void calendar_preferences(void)');helper=s[helper_start:s.index('\nstatic ',helper_start+1)]
  self.assertIn('if(g_tz_offset_min!=nos_prefs.tz_min)',helper)
  self.assertLess(helper.index('g_tz_offset_min=nos_prefs.tz_min;'),helper.index('board_apply_timezone();'))
  self.assertLess(helper.index('board_apply_timezone();'),helper.index('board_calendar_ready(!prefs_loading());'))
  self.assertLess(loop.index('auto_brightness_tick();'),begin);self.assertLess(loop.index('calendar_preferences();'),begin)
  for housekeeping in ['calendar_preferences();','board_apply_timezone();','radio_preferences();','hal_setup_poll();','auto_brightness_tick();','hal_wifi_set_enabled(']:
   self.assertNotIn(housekeeping,loop[begin:restore])
  touch=re.search(r'\btouch_drain\(\s*mutate\s*&&\s*s_pwr\s*==\s*PWR_ON\s*\)',loop);self.assertIsNotNone(touch)
  self.assertIn('s_pwr==PWR_ON',loop[begin:begin+110]);self.assertLess(begin,loop.index('int64_t a ='));self.assertLess(begin,touch.start());self.assertLess(touch.start(),restore)
  self.assertLess(loop.index('frame_drain();',begin),finish);self.assertLess(finish,restore)
  self.assertLess(restore,loop.index('nos_pipeline_finish(',begin));self.assertLess(restore,loop.index('printf(',begin));self.assertLess(restore,loop.index('set_level(',begin));self.assertLess(restore,loop.index('nos_frame_wait_until(&s_pace_wait,',begin))
  arm_start=s.index('static int pace_arm(');arm=s[arm_start:s.index('\nstatic ',arm_start+1)]
  self.assertIn('return esp_timer_start_once(s_frame_timer,delay);',arm)
  self.assertIn('s_pace_wait={NULL,pace_now_us,pace_arm,pace_stop,pace_take,pace_fault}',s)
  self.assertNotIn('esp_timer_start_once(',loop[begin:restore]);self.assertNotIn('nos_frame_wait_until(',loop[begin:restore])
  # OFF/AOD each have one blocking exit and one service-owned KEEPWAKE exit, plus the ON-state G99 keep-awake cap exit;
  # all five release OTA admission and precede any priority elevation.
  self.assertEqual(loop[:begin].count('continue;'),5)
  self.assertEqual(loop[:begin].count('s_keep_released=top;'),1);cap=loop[loop.index('s_keep_released=top;'):];cap=cap[:cap.index('continue;')]
  self.assertIn('go_state(power_idle_target(',cap);self.assertIn('if(activity)ota_activity_end();',cap)
  self.assertEqual(loop[:begin].count('if(mutate && pending && (pending->flags&SCR_KEEPAWAKE) && pending!=s_keep_released) {go_state(PWR_ON);if(activity)ota_activity_end();continue;}'),2)
  self.assertEqual(loop[:begin].count('sleep_grid_delay(50)'),2)
  self.assertNotIn('continue;',loop[begin:restore]);self.assertNotIn('vTaskDelay(',loop[begin:restore]);self.assertNotIn('return;',loop[begin:restore]);self.assertNotIn('nos_frame_priority_begin(',loop[begin+30:])
  for command in ['state','info']:
   branch=s[s.index('else if (!strcmp(s, "'+command+'"))'):];match=re.search(r'\n    (?:} )?else\b',branch[1:]);branch=branch[:match.start()+1] if match else branch
   self.assertIn('if(!nos_frame_capture_live(&s_capture)){frame_priority_report();set_settings_carry_report();history_virtual_report();}',branch)
  self.assertIn('NOSSCHED v=1 mode=%u base=%u current=%u core=%d paint=%u mismatches=%u paint_samples=%u',s)
  observed=loop.index('nos_frame_priority_observe(');self.assertLess(begin,observed);self.assertLess(observed,loop.index('int64_t a ='));self.assertIn('(unsigned)uxTaskPriorityGet(NULL)',loop[observed:observed+150])
  self.assertIn('s_inj_active || s_touch_now || s_touch_any,frame_priority_set',s)
  carry=s[s.index('} else if (!strncmp(s,"settings_carry",14))'):s.index('} else if (!strncmp(s,"frame_priority",14))')];self.assertIn('strcmp(s,"settings_carry 0") && strcmp(s,"settings_carry 1")',carry);self.assertLess(carry.index('capture_active'),carry.index('set_settings_carry_set('));self.assertLess(carry.index('touch_active'),carry.index('set_settings_carry_set('))
  self.assertNotIn('frame_priority',s[s.index('printf("NOSSTATE'):s.index('settings_shutter_report();',s.index('printf("NOSSTATE'))])
if __name__=='__main__':unittest.main()


