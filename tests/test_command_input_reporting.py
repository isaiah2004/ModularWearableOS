"""Actual host rejection helper, parser metadata, privacy and binary export."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


def actual_helper():
    source = (ROOT / "firmware/main/nos_main.c").read_text()
    start = source.index("static void host_input_reject(const command_input *input)")
    brace = source.index("{", start); end = brace + 1; depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}"); end += 1
    return source[start:end]


@unittest.skipUnless(shutil.which("gcc"), "GCC required")
class CommandInputReportingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base = ROOT / "build/alpha05/checks/command-input-reporting"
        base.mkdir(parents=True, exist_ok=True)
        cls.out = Path(tempfile.mkdtemp(prefix="reporting-", dir=base))
        helper = actual_helper()
        prelude = r'''
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "command_input.h"
#include "diag_log.h"
static char captured[512];static size_t used;
static struct {uint16_t code;int32_t a,b,c;} events[2];static unsigned count;
static int captured_printf(const char *format,...) __attribute__((format(printf,1,2)));
static int captured_printf(const char *format,...){
 va_list args;va_start(args,format);int n=vsnprintf(captured+used,sizeof captured-used,format,args);va_end(args);
 assert(n>=0 && (size_t)n<sizeof captured-used);used+=(size_t)n;return n;
}
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c){
 assert(count<2);events[count].code=code;events[count].a=a;events[count].b=b;events[count++].c=c;
}
#define printf captured_printf
'''
        tail = r'''
#undef printf
static void feed(command_input *input,const unsigned char *p,size_t n){
 command_input_reset(input);int result=0;
 for(size_t i=0;i<n;i++){result=command_input_push(input,p[i]);if(i+1<n)assert(result==COMMAND_INPUT_NONE);}
 assert(result==COMMAND_INPUT_REJECTED);
}
static void dump(const char *name,const command_input *input){
 count=0;used=0;memset(captured,0,sizeof captured);host_input_reject(input);
 printf("WIRE %s ",name);for(size_t i=0;i<used;i++)printf("%02x",(unsigned char)captured[i]);putchar('\n');
 for(unsigned i=0;i<count;i++)printf("EVENT %s %u %d %d %d\n",name,(unsigned)events[i].code,
 events[i].a,events[i].b,events[i].c);
}
int main(void){
 command_input input;
 const unsigned char barrier[]={0,'\n'};feed(&input,barrier,sizeof barrier);dump("standalone",&input);
 const unsigned char secret[]="wifi {\"password\":\"synthetic-secret\"}";
 unsigned char bad[sizeof secret+2];memcpy(bad,secret,sizeof secret-1);bad[sizeof secret-1]=0;
 bad[sizeof secret]='x';bad[sizeof secret+1]='\n';feed(&input,bad,sizeof bad);dump("secret",&input);
 const unsigned char multiple[]={'a',0,'b',0,'\n'};feed(&input,multiple,sizeof multiple);dump("multiple",&input);
 const unsigned char cr[]={'a','\r','b','\n'};feed(&input,cr,sizeof cr);dump("cr",&input);
 unsigned char oversized[COMMAND_INPUT_CAP+6];memset(oversized,'x',sizeof oversized);
 oversized[sizeof oversized-1]='\n';feed(&input,oversized,sizeof oversized);dump("overflow",&input);
 command_input_reset(&input);input.reason=COMMAND_INPUT_BAD_CONTROL;input.bytes=UINT32_MAX;
 input.first_bad_offset=input.first_nul_offset=input.last_nul_offset=input.nul_count=UINT32_MAX;
 dump("saturated",&input);
 command_input_reset(&input);input.bytes=UINT32_MAX-1;
 assert(command_input_push(&input,'\r')==COMMAND_INPUT_NONE);
 assert(command_input_push(&input,'x')==COMMAND_INPUT_NONE);
 assert(command_input_push(&input,'\n')==COMMAND_INPUT_REJECTED);dump("saturated_cr",&input);
 return 0;
}
'''
        source = cls.out / "actual-reporting.c"
        source.write_text(prelude + helper + tail)
        binary = cls.out / "actual-reporting.exe"
        command = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wformat=2", "-Inos",
                   str(source), "nos/command_input.c", "-o", str(binary)]
        compiled = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
        (cls.out / "compile.log").write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode: raise AssertionError(compiled.stdout + compiled.stderr)
        run = subprocess.run([str(binary)], cwd=cls.out, capture_output=True, text=True, timeout=15)
        (cls.out / "run.log").write_text(run.stdout + run.stderr)
        if run.returncode: raise AssertionError(run.stdout + run.stderr)
        cls.output = run.stdout; cls.wires = {}; cls.events = {}
        for line in cls.output.splitlines():
            kind, name, data = line.split(" ", 2)
            if kind == "WIRE": cls.wires[name] = bytes.fromhex(data).decode("ascii")
            else:
                assert kind == "EVENT"
                cls.events.setdefault(name, []).append(tuple(map(int, data.split())))
        (cls.out / "source.json").write_text(json.dumps({"helper_sha256": hashlib.sha256(helper.encode()).hexdigest(),
                "command_input_sha256": hashlib.sha256((ROOT / "nos/command_input.c").read_bytes()).hexdigest()}, indent=2) + "\n")

    def test_actual_helper_numeric_events_and_wire_prefix(self):
        self.assertEqual(self.events["standalone"], [(0x203, 1, 1, 0)])
        self.assertEqual(self.events["multiple"], [(0x203, 1, 4, 0), (0x204, 2, 2, 2)])
        self.assertEqual(self.events["cr"], [(0x203, 1, 3, 13), (0x204, 2, 0, 0)])
        self.assertEqual(self.events["overflow"], [(0x203, 2, 197, 0), (0x204, 192, 0, 0)])
        self.assertEqual(self.events["saturated"], [(0x203, 1, 2147483647, 0),
                                                  (0x204, 2147483647, 2147483647, 2147483647)])
        self.assertEqual(self.events["saturated_cr"], [(0x203, 1, 2147483647, 13), (0x204, 2147483647, 0, 0)])
        for name, wire in self.wires.items():
            with self.subTest(name=name):
                self.assertRegex(wire, r"\AERR command input reason=\d+ bytes=\d+ bad=\d+ bad_offset=\d+ nul_count=\d+ first_nul=\d+ last_nul=\d+\nDONE\n\Z")
                self.assertEqual(wire.count("\n"), 2)
        self.assertIn("bad_offset=2 nul_count=2 first_nul=2 last_nul=4", self.wires["multiple"])

    def test_synthetic_sensitive_payload_never_enters_reporting(self):
        self.assertNotRegex(self.output, r"wifi|password|synthetic-secret|hash")
        secret_wire = self.wires["secret"]
        self.assertNotRegex(secret_wire, r"wifi|password|synthetic-secret|hash")
        self.assertEqual(len(self.events["secret"]), 2)
        self.assertEqual(self.events["secret"][1][0], 0x204)
        self.assertEqual(self.events["secret"][1][-1], 1)

    def test_actual_numeric_events_binary_export_known_schema_and_raw_values(self):
        spec = importlib.util.spec_from_file_location("command_input_reporting_export", ROOT / "tools/diag_export.py")
        export = importlib.util.module_from_spec(spec); spec.loader.exec_module(export)
        records = []
        expected = [event for events in self.events.values() for event in events]
        for sequence, (code, a, b, c) in enumerate(expected, 1):
            raw = struct.pack("<IHHIIIIHHiii", 0x31474C44, 1, 44, 123, sequence, sequence * 10, 0,
                              code, 0, a, b, c)
            records.append(raw + struct.pack("<I", zlib.crc32(raw)))
        header = struct.pack("<7I", 0x46474C44, 1, 32, 1, 123, 0, 0)
        path = self.out / "diag-rejections.bin"
        path.write_bytes(header + struct.pack("<I", zlib.crc32(header)) + b"".join(records))
        report = export.load([path])
        (self.out / "export.json").write_text(json.dumps(report, indent=2) + "\n")
        self.assertEqual(report["errors"], [])
        self.assertEqual([(r["code"], r["a"], r["b"], r["c"]) for r in report["events"]], expected)
        self.assertTrue(all(r["event"] == "COMMAND_INPUT_DETAIL" for r in report["events"] if r["code"] == 0x204))
        self.assertEqual(report["schema"]["arguments"]["COMMAND_INPUT_DETAIL"],
                         ["first_bad_offset_1based_INT32_saturated", "first_NUL_offset_1based_INT32_saturated", "NUL_count_INT32_saturated"])


if __name__ == "__main__":
    unittest.main()
