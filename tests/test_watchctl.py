import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("tools.watchctl", ROOT / "tools/watchctl.py")
watchctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(watchctl)


class FakePort:
    def __init__(self, lines):
        self.lines = iter(lines)
        self.written = b""

    def write(self, data):
        self.written += data

    def flush(self):
        pass

    def readline(self):
        return next(self.lines, b"")


class RuntimePort:
    """Real HostTransport/SerialLines framing; only the device is synthetic."""
    def __init__(self, *, legacy=False, awake=0):
        self.legacy, self.awake = legacy, awake
        self.rx, self.written, self.commands = bytearray(), bytearray(), []
        self.top, self.boot = 'HOME', 'a1b2c3d4'
        self.state = dict(pwr=0,locked=0,tx=0,perf=0,trace=0,face=8,mblur=2,history=1)
        self.runtime = {key:0 for key in watchctl.SMOKE_IDLE}
        self.runtime.update(v=1,imu_state=0,workout_ready=1,workout_state=0,workout_save=0,audio_status=0,capture_state=0)
        self.fault = None
        self.fault_used = False
        self.reboot_on_fault = False
        self.source, self.elf = 'a'*64, 'b'*64
        self.closed = False
        self.opened = False

    @property
    def in_waiting(self):return len(self.rx)

    def open(self):
        assert self.dtr is False and self.rts is False and self.port=='UNUSED'
        self.opened=True

    def close(self):self.closed=True
    def reset_input_buffer(self):self.rx.clear()
    def flush(self):pass
    def read(self,n):
        result=bytes(self.rx[:n]);del self.rx[:n];return result

    def payload(self,command):
        if command=='hostcaps':
            return ['ERR unknown: hostcaps'] if self.legacy else ['NOSHOST v=1 req=1 max_command=191 typed_errors=1']
        if command=='info':
            if self.legacy:return ['app=0/26 name=Clock']
            return ['NOS NOS 0.5.0-alpha board=1.43C depth=1 top='+self.top,
                    f'NOSBUILD source_sha256={self.source} source_build_id={self.source[:16]} elf_sha256={self.elf} metrics=nos-device-v1']
        if self.legacy:
            if command in ('board','imu','sd','apps','time') or command.startswith(('wifi ','tz ','settime ')):
                return [command.split()[0].upper()+' OK']
            raise AssertionError('Unsafe/unsupported legacy smoke command: '+command)
        if command=='state':
            return ['NOSSTATE '+ ' '.join(f'{key}={value}' for key,value in self.state.items())+
                    f' awake={self.awake} top={self.top}']
        if command=='runtime_state':return ['NOSRUNTIME '+' '.join(f'{key}={value}' for key,value in self.runtime.items())]
        if command=='prefs':return ['NOSPREFS ok=1 stored=1 match=1 hash=1234abcd stored_hash=1234abcd face=8 brightness=62']
        if command=='diag log':return ['NOSLOG v=1 boot='+self.boot+' ready=1 available=1']
        if command in ('sd','imu','sports','nav_state'):return [dict(sd='SD mounted=1',imu='IMU ok=1',sports='NOSSPORTS mode=0',nav_state='NOSNAV page=0 offset_q8=0 dragging=0')[command]]
        if command=='home':self.top='HOME';return []
        if command.startswith('open '):
            matches=[top for name,top in watchctl.SMOKE_APPS if command=='open '+name]
            assert len(matches)==1,command
            self.top=matches[0];return ['OPEN '+self.top]
        raise AssertionError('Unsupported NOS smoke command: '+command)

    def write(self,data):
        self.written.extend(data)
        if data==b'\x00\n':
            self.rx.extend(b'DONE ffffffff ok=1\nERR command input control\nDONE\n')
            return len(data)
        wire=data.decode().rstrip('\n')
        match=re.fullmatch(r'req ([0-9a-f]{8}) (.*)',wire)
        request,command=(match[1],match[2]) if match else (None,wire)
        self.commands.append(command)
        lines=self.payload(command)
        if request:lines=['BEGIN '+request,*lines]
        if command==self.fault and not self.fault_used:
            self.fault_used=True
            if self.reboot_on_fault:self.boot='deadbeef'
        else:lines.append('DONE '+request+' ok=1' if request else 'DONE')
        self.rx.extend(('\n'.join(lines)+'\n').encode())
        return len(data)


def fake_serial(port):
    serial=types.ModuleType('serial');serial.Serial=lambda **kwargs:port;serial.SerialException=OSError
    tools=types.ModuleType('serial.tools');ports=types.ModuleType('serial.tools.list_ports')
    ports.comports=lambda:[];tools.list_ports=ports
    return {'serial':serial,'serial.tools':tools,'serial.tools.list_ports':ports}


class WatchControlTests(unittest.TestCase):
    def cli(self,port,*args):
        stdout,stderr=io.StringIO(),io.StringIO()
        with patch.dict(sys.modules,fake_serial(port)),patch.object(watchctl.time,'sleep'), \
             contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
            code=watchctl.main(['--port','UNUSED','--timeout','0.02',*args])
        self.assertTrue(port.closed)
        return code,stdout.getvalue(),stderr.getvalue()

    def test_collects_response_amid_boot_logging(self):
        port = FakePort([b"I (123) main: running\r\n", b"BOARD sd=0\r\n", b"DONE\r\n"])
        self.assertEqual(watchctl.execute(port, "board"), ["I (123) main: running", "BOARD sd=0"])
        self.assertEqual(port.written, b"board\n")

    def test_timeout_does_not_claim_success(self):
        with self.assertRaises(TimeoutError):
            watchctl.execute(FakePort([]), "board", timeout=0.001)

    def test_rejects_overflow_and_embedded_commands(self):
        for command in ["", "x" * 192, "home\nopen 3", "home\rsleep"]:
            port = FakePort([])
            with self.assertRaises(ValueError):
                watchctl.execute(port, command)
            self.assertEqual(port.written, b"")

    def test_failed_correlated_results_preserve_typed_error_and_redact_credentials(self):
        from tools.host_transport import HostTransport, DeviceError
        from tests.test_host_transport import Link
        transport = HostTransport(Link())
        transport.active = {'id': '12345678', 'command': 'wifi {"password":"secret"}',
                            'lines': ['ERR unknown: wifi {"password":"secret"}']}
        transport.correlated = True
        record = watchctl.result_record(transport, transport.active['command'],
                                        error=DeviceError('SDPUT ERR protected 16'))
        self.assertFalse(record['ok'])
        self.assertEqual(record['request_id'], '12345678')
        self.assertEqual(record['error']['errno'], 16)
        self.assertNotIn('secret', str(record))

    def test_nos_smoke_uses_current_names_and_preserves_both_awake_values(self):
        for awake in (0,1):
            with self.subTest(awake=awake):
                port=RuntimePort(awake=awake)
                code,out,err=self.cli(port,'--smoke')
                self.assertEqual(code,0,err)
                self.assertEqual(port.awake,awake)
                self.assertEqual(port.top,'HOME')
                self.assertEqual([c for c in port.commands if c.startswith('open ')],['open '+name for name,_ in watchctl.SMOKE_APPS])
                self.assertFalse(any(c.startswith(('awake','wifi ','tz ','settime ','tap ','swipe ','ext ')) or c in ('board','apps','left','right','top','bottom','close','time') for c in port.commands))
                self.assertIn('"awake_preserved": '+str(awake),out)
                self.assertIn('"home_verified": true',out)

    def test_record_parser_preserves_screen_spaces_and_real_hex_boot_format(self):
        line='NOSSTATE pwr=0 awake=1 top=BATTERY HISTORY face=8 locked=0 tx=0'
        self.assertEqual(watchctl.smoke_record([line],'NOSSTATE')['top'],'BATTERY HISTORY')
        self.assertEqual(watchctl.smoke_boot(['NOSLOG v=1 boot=a1b2c3d4 ready=1']),'a1b2c3d4')
        source=(ROOT/'nos/diag_log.c').read_text()
        self.assertIn('boot=%08lx',source)
        for lines in ([line,line],['NOSSTATE awake=1 awake=0 top=HOME']):
            with self.assertRaises(ValueError):watchctl.smoke_record(lines,'NOSSTATE')

    def test_active_unknown_locked_and_unresolved_states_refuse_before_mutation(self):
        cases=[('runtime',key,1) for key in watchctl.SMOKE_IDLE]
        cases += [('runtime','imu_state',-1),('runtime','workout_ready',0),('runtime','workout_state',2),
                  ('runtime','workout_save',-1),('runtime','audio_status',-1),('runtime','capture_state',1),
                  ('state','locked',1),('state','tx',1),('state','pwr',2),('runtime','imu_state',None)]
        for area,key,value in cases:
            with self.subTest(area=area,key=key,value=value):
                port=RuntimePort();target=getattr(port,area)
                if value is None:target.pop(key)
                else:target[key]=value
                code,_,_=self.cli(port,'--smoke')
                self.assertEqual(code,1)
                self.assertFalse(any(c=='home' or c.startswith('open ') for c in port.commands))
                self.assertEqual(port.awake,0)

    def test_unknown_open_outcome_resyncs_then_observes_before_home_without_replay(self):
        port=RuntimePort(awake=1);port.fault='open Energy'
        code,_,err=self.cli(port,'--smoke')
        self.assertEqual(code,1,err)
        self.assertEqual(port.commands.count('open Energy'),1)
        self.assertEqual(port.top,'HOME')
        self.assertEqual(port.awake,1)
        self.assertIn(b'\x00\n',port.written)
        after=port.commands[port.commands.index('open Energy')+1:]
        self.assertLess(after.index('hostcaps'),after.index('home'))
        self.assertLess(after.index('state'),after.index('home'))
        self.assertLess(after.index('runtime_state'),after.index('home'))

    def test_unknown_home_outcome_is_observed_without_repeating_home(self):
        port=RuntimePort();port.fault='home'
        code,_,_=self.cli(port,'--smoke')
        self.assertEqual(code,1)
        self.assertEqual(port.commands.count('home'),1)
        self.assertEqual(port.top,'HOME')
        self.assertEqual(port.awake,0)

    def test_known_read_timeout_resyncs_and_retries_once_before_smoke_navigation(self):
        port=RuntimePort();port.fault='state'
        code,_,err=self.cli(port,'--smoke')
        self.assertEqual(code,0,err)
        self.assertEqual(port.commands[:5],['hostcaps','info','state','hostcaps','state'])
        self.assertIn(b'\x00\n',port.written)
        self.assertEqual(port.top,'HOME')
        self.assertEqual(port.awake,0)

    def test_owner_starting_during_smoke_is_left_running_and_cleanup_refuses_navigation(self):
        port=RuntimePort();original=port.payload
        def reply(command):
            lines=original(command)
            if command=='open Energy':port.runtime['audio_active']=1
            return lines
        port.payload=reply
        code,_,err=self.cli(port,'--smoke')
        self.assertEqual(code,1)
        self.assertIn('audio_active',err)
        self.assertNotIn('home',port.commands[port.commands.index('open Energy')+1:])
        self.assertEqual(port.runtime['audio_active'],1)
        self.assertEqual(port.awake,0)

    def test_boot_change_after_unknown_mutation_blocks_cleanup_navigation(self):
        port=RuntimePort();port.fault='open Energy';port.reboot_on_fault=True
        code,_,err=self.cli(port,'--smoke')
        self.assertEqual(code,1)
        self.assertIn('rebooted',err)
        self.assertEqual(port.commands.count('open Energy'),1)
        self.assertNotIn('home',port.commands[port.commands.index('open Energy')+1:])
        self.assertEqual(port.awake,0)

    def test_nos_legacy_wifi_and_time_flags_refuse_before_prompt_or_mutation(self):
        for args in (('--sync-time',),('--wifi-ssid','Synthetic network')):
            with self.subTest(args=args),patch.object(watchctl.getpass,'getpass',side_effect=AssertionError('Must not prompt NOS')):
                port=RuntimePort()
                code,_,err=self.cli(port,*args)
                self.assertEqual(code,1)
                self.assertIn('legacy WOS commands',err)
                self.assertEqual(port.commands,['hostcaps','info'])

    def test_explicit_legacy_wos_retains_readonly_diagnostics_and_legacy_time(self):
        port=RuntimePort(legacy=True)
        code,out,err=self.cli(port,'--smoke')
        self.assertEqual(code,0,err)
        self.assertEqual(port.commands,['hostcaps','info','board','imu','sd','apps','info'])
        self.assertIn('"runtime": "WOS"',out)
        port=RuntimePort(legacy=True)
        code,_,err=self.cli(port,'--sync-time')
        self.assertEqual(code,0,err)
        self.assertTrue(any(c.startswith('tz ') for c in port.commands))
        self.assertTrue(any(c.startswith('settime ') for c in port.commands))
        self.assertEqual(port.commands[-1],'time')

    def test_unsupported_or_conflicting_runtime_and_build_identity_refuse_navigation(self):
        for kind in ('unknown','conflict','short-elf','short-source'):
            with self.subTest(kind=kind):
                port=RuntimePort();original=port.payload
                def reply(command):
                    lines=original(command)
                    if command=='info':
                        if kind=='unknown':return ['UNKNOWN firmware']
                        if kind=='conflict':return lines+['app=0/26 name=Clock']
                        if kind=='short-elf':return [line.replace('b'*64,'b'*8) for line in lines]
                        return [line.replace('a'*64,'a'*8) for line in lines]
                    return lines
                port.payload=reply
                code,_,_=self.cli(port,'--smoke')
                self.assertEqual(code,1)
                self.assertFalse(any(c=='home' or c.startswith('open ') for c in port.commands))

    @unittest.skipUnless(shutil.which('gcc'),'host GCC required for actual dispatch contract')
    def test_smoke_navigation_compiles_actual_firmware_dispatch_and_registry_names(self):
        source=(ROOT/'firmware/main/nos_main.c').read_text()
        registry=(ROOT/'nos/apps/registry.c').read_text()
        expected=[('Energy','ENERGY'),('Battery history','BATTERY HISTORY'),('Sports','SPORTS'),('Developer tools','DEVELOPER TOOLS')]
        self.assertEqual(list(watchctl.SMOKE_APPS),expected)
        def block(marker):
            start=source.index(marker);brace=source.index('{',start);depth=1;end=brace+1
            while depth:
                depth+=(source[end]=='{')-(source[end]=='}');end+=1
            return source[start:end]
        names=re.findall(r'\{\s*"([^"]+)"\s*,\s*DOT_ICON_\w+\s*,\s*&(scr_\w+)\s*\}',registry)
        selected={name:symbol for name,symbol in names}
        bodies='\n'.join((ROOT/'nos/apps'/name).read_text() for name in ('fit_energy.c','fit_workout.c','set_developer.c'))
        for name,top in expected:
            symbol=selected[name]
            self.assertRegex(bodies,r'const nos_screen '+symbol+r'\s*=\s*\{\s*"'+re.escape(top)+'"')
        for command in ('info','runtime_state','state','diag log','prefs','sd','imu','sports','nav_state'):
            self.assertRegex(source,r'!strcmp\(\s*s\s*,\s*'+re.escape(json.dumps(command))+r'\s*\)')
        # 'board' became a real read-only cmd_exec diagnostic (board_print_info; watchctl issues it), so it must exist there;
        # the open/home/awake dispatch compiled below must still not claim it (dispatch("board") stays unknown).
        self.assertRegex(source,r'if\(!strcmp\(s,"board"\)\)\s*\{board_print_info\(\);return 1;\}')
        resolver=block('static const nos_screen *screen_by_name(')
        open_branch=block('else if (!strncmp(s, "open ", 5))')
        awake_branch=block('else if (!strcmp(s, "awake"))')
        home=re.search(r'else if \(!strcmp\(s, "home"\)\) nos_home\(\);',source)[0]
        symbols=set(symbol for _,symbol in names)|set(re.findall(r'&(scr_\w+)',resolver))
        labels={symbol:name.upper() for name,symbol in names}
        labels.update({selected[name]:top for name,top in expected})
        definitions='\n'.join('static const nos_screen '+symbol+'={'+json.dumps(labels.get(symbol,symbol))+'};' for symbol in sorted(symbols))
        actual_array=re.search(r'const nos_app nos_app_list\[\] = \{.*?\n\};',registry,re.S)[0]
        actual_array=re.sub(r'DOT_ICON_\w+','0',actual_array)
        probes='\n'.join('dispatch('+json.dumps('open '+name)+');assert(!strcmp(last_name,'+json.dumps(top)+'));' for name,top in expected)
        text=r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
typedef struct {const char *name;} nos_screen;
typedef struct {const char *name;int icon;const nos_screen *scr;} nos_app;
#define GFX_W 466
#define GFX_H 466
#define PWR_ON 0
static int opens,homes,errors,unknown,s_no_sleep=1;
static char last_name[32];
static void go_state(int state){assert(state==PWR_ON);}
static void nos_open(const nos_screen *screen,int x,int y){assert(x==233&&y==233);opens++;strcpy(last_name,screen->name);}
static void nos_home(void){homes++;}
static int test_printf(const char *format,...){if(!strncmp(format,"ERR ",4))errors++;return 0;}
#define printf test_printf
'''+definitions+'\n'+actual_array+'\nconst int nos_app_count=sizeof nos_app_list/sizeof nos_app_list[0];\n'+resolver+ \
            '\nstatic void dispatch(char *s){if(0){} '+open_branch+' '+home+' '+awake_branch+' else unknown++;}\nint main(void){\n'+probes+r'''
assert(opens==4);dispatch("home");assert(homes==1);
dispatch("open 16");assert(errors==1&&opens==4);
dispatch("board");dispatch("awake 1");assert(unknown==2&&s_no_sleep==1);
dispatch("awake");assert(!s_no_sleep);dispatch("awake");assert(s_no_sleep);
puts("PASS actual NOS named dispatch/registry, space names, awake toggle only and rejected legacy commands");return 0;}
'''
        parent=ROOT/'build/alpha05/checks/watchctl-smoke-contract';parent.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='dispatch-',dir=parent) as directory:
            directory=Path(directory);c=directory/'dispatch.c';c.write_text(text)
            binary=directory/('dispatch.exe' if os.name=='nt' else 'dispatch')
            env=dict(os.environ,TEMP=str(directory),TMP=str(directory),TMPDIR=str(directory))
            compile=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)],env=env,capture_output=True,text=True)
            (parent/'compile.log').write_text(compile.stdout+compile.stderr)
            self.assertEqual(compile.returncode,0,compile.stderr)
            result=subprocess.run([str(binary)],env=env,capture_output=True,text=True,timeout=10)
            (parent/'run.log').write_text(result.stdout+result.stderr)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS actual NOS',result.stdout)


if __name__ == "__main__":
    unittest.main()
