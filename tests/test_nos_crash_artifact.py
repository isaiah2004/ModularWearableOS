"""Actual crash user-mode code with supported SDK API fault/bounds stubs.

The private SDK object check uses real installed headers; this host test checks
state, retention and read semantics. It never panics, connects or reads flash.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(name):
    text = (ROOT / "firmware/main/nos_heap_diag.c").read_text()
    match = re.search(r"^void " + name + r"\([^\n]*\)\s*\{|^int " + name + r"\([^\n]*\)\s*\{", text, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


class CrashArtifactTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("gcc"), "gcc required")
    def test_empty_invalid_read_errors_bounds_summary_and_explicit_clear(self):
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nos_heap_diag.h"
#include "diag_log.h"
#define ESP_PLATFORM 1
#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH 1
#define CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF 1
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_COREDUMP 3
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 10
#define ESP_ERR_INVALID_SIZE 11
typedef int esp_err_t;
typedef struct{uint32_t address,size;}esp_partition_t;
typedef struct{uint32_t exc_tcb;char exc_task[16];uint32_t exc_pc;
 struct{uint32_t bt[16],depth;int corrupted;}exc_bt_info;
 uint32_t core_dump_version;unsigned char app_elf_sha256[65];
 struct{uint32_t exc_cause,exc_vaddr,exc_a[16];}ex_info;}esp_core_dump_summary_t;
static nos_crash_info s_crash;
static esp_partition_t part={0xf80000,0x80000};
static unsigned char flash[2048];
static int present=1,read_error,image_error,check_error,summary_error,erase_error;
static int erase_calls,reads,events;static uint32_t image_bytes=512;
const esp_partition_t*esp_partition_find_first(int t,int s,const char*l){(void)t;(void)s;(void)l;return present?&part:NULL;}
esp_err_t esp_partition_read(const esp_partition_t*p,uint32_t at,void*out,uint32_t n){
 assert(p==&part&&at<=sizeof flash&&n<=sizeof flash-at);reads++;if(read_error)return 19;memcpy(out,flash+at,n);return 0;}
esp_err_t esp_core_dump_image_get(size_t*address,size_t*size){*address=part.address;*size=image_bytes;return image_error;}
esp_err_t esp_core_dump_image_check(void){return check_error;}
esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t*s){
 if(summary_error)return summary_error;
 s->exc_tcb=0x3fc88888;s->exc_pc=0x40001234;s->ex_info.exc_a[1]=0x3fc99999;
 s->core_dump_version=0x10002;
 s->ex_info.exc_cause=28;s->ex_info.exc_vaddr=0x17;
 strcpy(s->exc_task,"sensor");memset(s->app_elf_sha256,'a',64);s->app_elf_sha256[64]=0;
 s->exc_bt_info.depth=40;for(int i=0;i<16;i++)s->exc_bt_info.bt[i]=0x40001000u+(unsigned)i;
 return 0;}
esp_err_t esp_core_dump_image_erase(void){erase_calls++;if(erase_error)return erase_error;memset(flash,255,sizeof flash);return 0;}
uint32_t diag_log_name_id(const char*s){return (uint32_t)strlen(s);}
void diag_log_crash_state(uint32_t state,uint32_t pc,uint32_t build){(void)state;(void)pc;(void)build;}
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c){(void)code;(void)a;(void)b;(void)c;events++;}
'''
        code += "\n".join(function(name) for name in ("nos_crash_refresh", "nos_crash_get", "nos_crash_read", "nos_crash_clear"))
        code += r'''
int main(void){
 memset(flash,255,sizeof flash);nos_crash_refresh();assert(s_crash.state==NOS_CRASH_EMPTY&&!erase_calls&&!events);
 assert(nos_crash_read(0,flash,1)==-1);present=0;nos_crash_refresh();assert(s_crash.state==NOS_CRASH_IO_ERROR);
 present=1;read_error=1;nos_crash_refresh();assert(s_crash.state==NOS_CRASH_IO_ERROR);read_error=0;
 memset(flash,0x6b,sizeof flash);image_error=17;nos_crash_refresh();assert(s_crash.state==NOS_CRASH_INVALID&&!erase_calls);
 image_error=0;image_bytes=part.size+1;nos_crash_refresh();assert(s_crash.state==NOS_CRASH_INVALID);
 image_bytes=512;check_error=18;nos_crash_refresh();assert(s_crash.state==NOS_CRASH_INVALID&&!erase_calls);check_error=0;
 summary_error=20;nos_crash_refresh();assert(s_crash.state==NOS_CRASH_INVALID);summary_error=0;
 nos_crash_refresh();nos_crash_info info;nos_crash_get(&info);
 assert(info.state==NOS_CRASH_VALID&&info.pc==0x40001234&&info.sp==0x3fc99999&&info.cause==28&&info.vaddr==0x17);
 assert(info.depth==16&&info.format_version==0x10002&&strlen(info.elf_sha256)==64&&!strcmp(info.task_name,"sensor")&&events==2&&!erase_calls);
 unsigned char chunk[1024];int before=reads;
 assert(nos_crash_read(512,chunk,1)==-1&&nos_crash_read(0,chunk,1025)==-1&&reads==before);
 assert(nos_crash_read(500,chunk,1024)==12&&chunk[0]==0x6b);
 assert(nos_crash_read(0,chunk,512)==512&&!erase_calls);
 read_error=1;assert(nos_crash_read(0,chunk,32)==-1);read_error=0;
 erase_error=31;assert(!nos_crash_clear()&&erase_calls==1&&s_crash.state==NOS_CRASH_VALID);
 erase_error=0;assert(nos_crash_clear()&&erase_calls==2&&s_crash.state==NOS_CRASH_EMPTY);
 puts("PASS crash empty/corrupt/I/O states, bounded read-only artifact export, matching build/PC/task/backtrace and explicit-only clear");
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="nos-crash-") as directory:
            path = Path(directory)
            c = path / "test.c"
            binary = path / ("test.exe" if os.name == "nt" else "test")
            c.write_text(code)
            compile_result = subprocess.run([shutil.which("gcc"), "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                                             "-I" + str(ROOT / "firmware/main"), "-I" + str(ROOT / "nos"), str(c), "-o", str(binary)],
                                            capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS crash", result.stdout)


if __name__ == "__main__":
    unittest.main()
