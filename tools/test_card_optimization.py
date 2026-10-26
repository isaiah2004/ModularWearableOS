"""Independent original card state/LUT/raster equivalence; requires built simulator objects."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc",default=shutil.which("gcc") or "gcc")
    parser.add_argument("--out",type=Path,default=ROOT/"build/card_optimization")
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    flags=["-O2","-std=c99","-DNOS_PSRAM_TRACKING","-DGFX_COST_MODEL","-Icore","-Inos","-Inos/fonts","-Isim"]
    main_obj=args.out/"sim_main.o"
    subprocess.run([args.cc,*flags,"-Dmain=sim_program_main","-c","sim/nos_sim.c","-o",str(main_obj)],cwd=ROOT,check=True)
    shade_obj=args.out/"shade_q16.o"
    subprocess.run([args.cc,*flags,"-c","nos/faces/orrery_shade_q16.c","-o",str(shade_obj)],cwd=ROOT,check=True)
    objects=[p for p in (ROOT/"build/nos_obj").glob("*.o") if p.name not in ("sim_nos_sim.o","nos_faces_orrery_card.o","nos_faces_orrery_shade_q16.o")]
    objects.append(shade_obj)
    binary=args.out/"card_optimization_test.exe"
    subprocess.run([args.cc,*flags,"tests/card_optimization_test.c",str(main_obj),*map(str,objects),"-o",str(binary),"-lgdi32","-luser32","-lwinmm","-lm"],cwd=ROOT,check=True)
    script=args.out/"boot.txt";script.write_text("face 8\nwait 3000\n")
    result=subprocess.run([str(binary.resolve()),"--script",str(script.resolve()),"--out",str((args.out/"frames").resolve()),"--no-media"],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    log=args.out/"oracle.log";log.write_bytes(result.stdout)
    assert result.returncode==0,f"Card oracle failed: {log}"
    print(result.stdout.decode(errors="replace").splitlines()[-1])

if __name__=="__main__":main()
