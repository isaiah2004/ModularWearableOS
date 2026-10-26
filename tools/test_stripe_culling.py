"""Build isolated simulator objects and exact old/new stripe callback oracles.
No shared nos_obj writes, device access, or flashing. Output: build/stripe-culling.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--cc",default=shutil.which("gcc"))
    ap.add_argument("--out",type=Path,default=ROOT/"build/stripe-culling")
    ap.add_argument("--skip-build",action="store_true",help="reuse only this runner's isolated objects")
    args=ap.parse_args()
    if not args.cc: raise SystemExit("gcc required")
    out=args.out.resolve();obj=out/"objects";obj.mkdir(parents=True,exist_ok=True)
    flags=["-O2","-std=c99","-Wall","-Wextra","-Wno-unused-parameter","-Wno-format-truncation",
           "-DNOS_PSRAM_TRACKING","-DGFX_COST_MODEL","-Icore","-Inos","-Inos/fonts","-Isim"]
    core=["asset_job","gfx","font","sprite","anim","notif","battery","pedometer","walk","motion_wake","wake_service","knock","ephem"]
    sources=sorted((ROOT/"nos").rglob("*.c"))+[ROOT/"core"/(n+".c") for n in core]
    sources+=sorted((ROOT/"core/fonts").glob("*.c"))+sorted((ROOT/"sim").glob("nos_*.c"))
    def object_path(source): return obj/(source.relative_to(ROOT).as_posix().replace("/","_")[:-2]+".o")
    def run(command):
        result=subprocess.run([str(v) for v in command],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        if result.returncode: raise RuntimeError(result.stdout.decode(errors="replace"))
        return result.stdout
    if not args.skip_build:
        def compile_one(source): run([args.cc,*flags,"-c",source,"-o",object_path(source)])
        with ThreadPoolExecutor(max_workers=min(os.cpu_count() or 4,16)) as pool:
            list(pool.map(compile_one,sources))
    objects=[object_path(s) for s in sources]
    if not all(p.exists() for p in objects): raise RuntimeError("isolated objects missing; omit --skip-build")
    sim=out/"nos_sim.exe"
    run([args.cc,*objects,"-o",sim,"-lgdi32","-luser32","-lwinmm","-lm"])
    renamed=out/"sim_test_main.o"
    run([args.cc,*flags,"-Dmain=sim_program_main","-c","sim/nos_sim.c","-o",renamed])
    boot=out/"boot.txt";boot.write_text("wait 100\n")
    for app in ("sys_notifs","set_settings"):
        harness=out/(app+"_test.o");exe=out/(app+"_test.exe")
        run([args.cc,*flags,"-c",ROOT/"tests"/(app+"_stripe_test.c"),"-o",harness])
        deps=[p for p in objects if p.name not in ("sim_nos_sim.o","nos_apps_"+app+".o")]
        run([args.cc,*deps,renamed,harness,"-o",exe,"-lgdi32","-luser32","-lwinmm","-lm"])
        log=run([exe,"--script",boot,"--out",out/app,"--no-media","--quiet"])
        (out/(app+".log")).write_bytes(log)
        print(log.decode(errors="replace"),end="")
    print("isolated simulator:",sim)
if __name__=="__main__":main()
