"""Synthetic regression for the physical 42-byte V3 migration incident.

No user checkpoint/credentials are embedded. Original backups and the physical
card are never accessed. All fixture/compiler/temp paths stay under private build.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/"build/alpha05/checks/prefs-v3-physical"


def synthetic_bank(sequence,brightness):
    # Explicit old uint16-aligned layout, independent of the current sizeof.
    payload=bytearray(42)
    payload[:20]=bytes([3,1,73,1,31,brightness,1,0,1,0,1,1,8,1,60,0,0,1,0,1])
    struct.pack_into("<hH",payload,20,-240,90)
    payload[24:]=bytes([3,1,0,10,1,3,1,1,2,3,1,2,73,1,0x81,90,0x82,88])
    body=struct.pack("<4I",0x4e4f5350,1,sequence,len(payload))+payload+bytes(256-len(payload))
    checksum=2166136261
    for value in body:checksum=((checksum^value)*16777619)&0xffffffff
    return body+struct.pack("<I",checksum)


BANKS=(synthetic_bank(25,62),synthetic_bank(24,100))


def function(source,name):
    marker="int "+name+"("
    start=source.index(marker);brace=source.index("{",start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=="{")-(source[end]=="}");end+=1
    return source[start:end]


def compile_harness(directory,prefs_source=None):
    directory=Path(directory)
    hal=(ROOT/"firmware/main/hal_esp32.c").read_text(encoding="utf-8")
    (directory/"actual_prefs_hal.h").write_text("\n\n".join(function(hal,name) for name in
        ("hal_store_load_pending","hal_store_save_checked","hal_store_load")),encoding="utf-8")
    exe=directory/("migration.exe" if os.name=="nt" else "migration")
    env=dict(os.environ,TEMP=str(directory),TMP=str(directory),TMPDIR=str(directory))
    command=["gcc","-std=c11","-O2","-Wall","-Wextra","-Werror","-DBOARD_HAS_SD=1","-Icore","-Inos","-I"+str(directory),
             "tests/prefs_v3_checkpoint_test.c","-x","c",str(prefs_source or ROOT/"nos/prefs.c"),
             "nos/prefs_disk.c","-o",str(exe)]
    result=subprocess.run(command,cwd=ROOT,env=env,capture_output=True,text=True)
    (directory/"compile.log").write_text(subprocess.list2cmdline(command)+"\n"+result.stdout+result.stderr)
    if result.returncode:raise AssertionError(result.stdout+result.stderr)
    return exe,env


@unittest.skipUnless(shutil.which("gcc"),"GCC required")
class LegacyCheckpointTests(unittest.TestCase):
    def test_actual_42_byte_checkpoint_migrates_via_real_hal_and_disk(self):
        OUT.mkdir(parents=True,exist_ok=True)
        self.assertTrue(OUT.resolve().is_relative_to(ROOT.resolve()))
        for data in BANKS:
            self.assertEqual(len(data),276)
        with tempfile.TemporaryDirectory(prefix="legacy42-",dir=OUT) as directory:
            path=Path(directory);watch=path/"watch";watch.mkdir()
            for slot,data in enumerate(BANKS):(watch/f"settings.{slot}.bin").write_bytes(data)
            exe,env=compile_harness(path)
            result=subprocess.run([str(exe),str(watch)],cwd=path,env=env,capture_output=True,text=True,timeout=30)
            (OUT/"physical-checkpoint.log").write_text(result.stdout+result.stderr)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS actual42-byte V3 physical checkpoint migration",result.stdout)


if __name__=="__main__":unittest.main()
