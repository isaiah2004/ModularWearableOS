"""USB diagnostic wrappers: actual GNU symbol wrapping, wire CRC and failure accounting."""
import re
import shutil
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('gcc'), 'gcc required')
class UsbDiag(unittest.TestCase):
    def test_wrapping_and_wire(self):
        with tempfile.TemporaryDirectory() as temp:
            d = Path(temp)
            (d/'nos_usb_diag_host.h').write_text('''#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
extern unsigned task_id,isr;
static inline int xPortInIsrContext(void){return (int)isr;}
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void*)(uintptr_t)task_id;}
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#define pdMS_TO_TICKS(n) (n)
int64_t esp_timer_get_time(void);
int usb_serial_jtag_write_bytes(const void *,size_t,TickType_t);
''')
            (d/'sdk.c').write_text('''#include "nos_usb_diag_host.h"
#include <stdio.h>
unsigned task_id=1,isr;
int result=1,connected=1; unsigned last_ticks;const void *last_src;size_t last_size;
int64_t clock_us;
int64_t esp_timer_get_time(void){return clock_us++;}
bool usb_serial_jtag_is_connected(void){return connected;}
int usb_serial_jtag_write_bytes(const void *p,size_t n,TickType_t t){last_src=p;last_size=n;last_ticks=t;clock_us+=t?7:2;if(result==2)fwrite(p,1,n,stdout);return result==2?(int)n:result;}
''')
            (d/'caller.c').write_text('''#include "nos_usb_diag_host.h"
extern bool usb_serial_jtag_is_connected(void);
int external_connected(void){return usb_serial_jtag_is_connected();}
int external_write(const void *p,size_t n,TickType_t t){return usb_serial_jtag_write_bytes(p,n,t);}
''')
            (d/'main.c').write_text('''#include "nos_usb_diag.h"
#include "nos_usb_diag_host.h"
#include <assert.h>
extern int result,connected;extern unsigned last_ticks;extern const void *last_src;extern size_t last_size;
int external_connected(void);int external_write(const void*,size_t,TickType_t);
int main(void){nos_usb_diag_stats s;const char data[]="abcd";
connected=0;nos_usb_diag_reply_begin();assert(!external_connected());nos_usb_diag_reply_end();result=0;assert(external_write(data,4,17)==0);nos_usb_diag_snapshot(&s);assert(s.connected_checks==0&&s.wait_calls==0);
nos_usb_diag_start();assert(!external_connected());connected=1;assert(external_connected());
result=0;assert(external_write(data,4,0)==0);assert(last_src==data&&last_size==4&&last_ticks==0);
result=-1;assert(external_write(data,4,19)==-1);assert(last_ticks==19);
result=1;assert(external_write(data,4,7)==1);result=2;assert(external_write(data,4,0)==4);
nos_usb_diag_snapshot(&s);assert(s.connected_checks==2&&s.connected_false==1);assert(s.zero_calls==2&&s.zero_fail==1&&s.zero_us==6&&s.zero_max_us==3);assert(s.wait_calls==2&&s.wait_fail==2&&s.wait_us==16&&s.wait_max_us==8);
nos_usb_diag_usb_line();nos_usb_diag_queue_accepted();nos_usb_diag_queue_failed();nos_usb_diag_dispatched();
assert(!nos_usb_diag_command("usb_diagonal 1"));
/* Failed and short sends are never retried; next snapshot reports both. */
result=0;assert(nos_usb_diag_command("usb_diag 13"));result=1;assert(nos_usb_diag_command("usb_diag 14"));
nos_usb_diag_snapshot(&s);assert(s.reply_calls==2&&s.reply_incomplete==2&&s.reply_last_accepted==1&&s.reply_last_requested>1);
result=2;assert(nos_usb_diag_command("usb_diag 4294967295"));assert(last_ticks==50);
assert(nos_usb_diag_command("usb_diag 4294967296"));assert(nos_usb_diag_command("usb_diag -1"));assert(nos_usb_diag_command("usb_diag"));
/* Guard defaultoff, scope-owner and ISR exclusion, then exact off restoration. */
connected=0;nos_usb_diag_reply_begin();assert(!external_connected());nos_usb_diag_reply_end();
assert(nos_usb_diag_command("usb_guard 1 101"));
assert(!external_connected());nos_usb_diag_reply_begin();assert(external_connected());
task_id=2;assert(!external_connected());nos_usb_diag_reply_end();
task_id=1;isr=1;assert(!external_connected());isr=0;assert(external_connected());
result=0;assert(external_write(data,4,37)==0);assert(last_ticks==37&&last_src==data&&last_size==4);result=2;
nos_usb_diag_reply_end();assert(!external_connected());
nos_usb_diag_snapshot(&s);assert(s.connected_overrides==2&&s.guard_enabled==1);
assert(nos_usb_diag_command("usb_guard 2 102"));nos_usb_diag_snapshot(&s);assert(s.guard_enabled==1);
assert(nos_usb_diag_command("usb_guard 0 103"));nos_usb_diag_reply_begin();assert(!external_connected());nos_usb_diag_reply_end();
nos_usb_diag_snapshot(&s);assert(s.guard_enabled==0&&s.connected_overrides==2&&s.connected_checks==10&&s.connected_false==9);return 0;}
''')
            exe=d/'probe.exe'
            subprocess.run(['gcc','-std=c99','-O2','-Wall','-Wextra','-Werror','-DNOS_USB_DIAG_HOST','-I'+str(d),'-I'+str(ROOT/'firmware/main'),str(ROOT/'firmware/main/nos_usb_diag.c'),str(d/'sdk.c'),str(d/'caller.c'),str(d/'main.c'),'-Wl,--wrap=usb_serial_jtag_is_connected','-Wl,--wrap=usb_serial_jtag_write_bytes','-o',str(exe)],check=True,capture_output=True)
            raw=subprocess.run([str(exe)],check=True,capture_output=True).stdout
            # Ignore the initial successful plain write from the argument-preservation test.
            frames=raw[4:].splitlines();self.assertEqual(len(frames),7)
            for index,line in enumerate(frames):
                m=re.fullmatch(rb'USBDIAG (\d+):(.*) crc=([0-9a-f]{8})',line);self.assertIsNotNone(m)
                payload=m[2];self.assertEqual(int(m[1]),len(payload));self.assertEqual(int(m[3],16),zlib.crc32(payload))
                fields=dict(item.split(b'=') for item in payload.split())
                self.assertEqual(fields[b'v'],b'2');self.assertEqual(fields[b'valid'],b'1' if index in (0,4,6) else b'0')
                if index==4:self.assertEqual(fields[b'guard'],b'1')
                if index==5:self.assertEqual(fields[b'guard'],b'1')
                if index==6:self.assertEqual(fields[b'guard'],b'0');self.assertEqual(fields[b'overrides'],b'2')
                if index==0:
                    self.assertEqual(fields[b'nonce'],b'4294967295');self.assertEqual(fields[b'ri'],b'2');self.assertEqual(fields[b'rc'],b'2')
                    self.assertEqual(fields[b'rx'],b'1');self.assertEqual(fields[b'qa'],b'1');self.assertEqual(fields[b'qf'],b'1');self.assertEqual(fields[b'ui'],b'1')
                changed=bytearray(payload);changed[len(changed)//2]^=1
                self.assertNotEqual(zlib.crc32(changed),int(m[3],16))

if __name__=='__main__':unittest.main()
