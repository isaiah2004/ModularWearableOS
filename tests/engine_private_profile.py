"""Profile actual Apps/Settings paths without touching shared simulator outputs.

Run after test_nos_engine_clock.py has freshly built the private object directory.
The before variants differ only in the renderer changes under this engine wave.
PC CPU time is a host measurement; it is not proof of the target frame budget.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
from engine_private_build import ROOT, CHECKS, FLAGS, LIBRARIES


def command(args):
    result=subprocess.run(args,cwd=ROOT,capture_output=True,text=True,timeout=120)
    if result.returncode:
        raise RuntimeError(result.stdout+result.stderr)
    return result


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--repeat",type=int,default=3)
    parser.add_argument("--objects",type=Path,default=CHECKS / "integration/objects")
    parser.add_argument("--reference-only",action="store_true",help="Record/replay fixed decisions with the already-built profile executables")
    args=parser.parse_args()
    out=CHECKS / "profiles";out.mkdir(parents=True,exist_ok=True)
    if args.reference_only:
        reference={}
        for app in ("launcher","settings"):
            for rom in (0,1):
                for virtual in (0,1):
                    case=f"{app}_rom{rom}_virtual{virtual}"
                    script=out / (case+"_before.txt");tape=out / (case+".quality.txt")
                    hashes=[]
                    for variant in ("before","after"):
                        dest=out / (case+"_reference_"+variant);dest.mkdir(exist_ok=True)
                        (dest / "build").mkdir(exist_ok=True)
                        run=subprocess.run([str(out / (variant+".exe")),"--script",str(script),"--out",str(dest),
                            "--hashes",str(dest / "hashes.txt"),"--pixel-reference",
                            "--quality-record" if variant=="before" else "--quality-replay",str(tape),"--no-media","--quiet"],
                            cwd=dest,capture_output=True,text=True,timeout=120)
                        (dest / "run.log").write_text(run.stdout+run.stderr)
                        if run.returncode:raise RuntimeError(f"{case} {variant}: {run.stdout}{run.stderr}")
                        hashes.append((dest / "hashes.txt").read_bytes())
                    if hashes[0]!=hashes[1]:raise AssertionError(f"Frame pixel difference: {case}")
                    reference[case]={"same_frames":True,"frame_count":len(hashes[0].splitlines()),
                                     "sha256":hashlib.sha256(hashes[0]).hexdigest()}
                    print(case,reference[case],flush=True)
        (out / "pixel-reference.json").write_text(json.dumps(reference,indent=2));return
    objects=sorted(args.objects.glob("*.o"))
    if not objects:raise RuntimeError("First run the fresh private engine tests")
    main=out / "main.c"
    main.write_text("extern int sim_program_main(int,char **);int main(int n,char **v){return sim_program_main(n,v);}\n")
    before={}
    for source in ("nos/apps/sys_launcher.c","nos/apps/set_settings.c"):
        original=CHECKS / "sys_launcher.before.c" if source.endswith("sys_launcher.c") else ROOT / source
        obj=out / (Path(source).stem+".before.o")
        command(["gcc",*FLAGS,"-include",str(CHECKS / "native_tile.before.h"),"-c",str(original),"-o",str(obj)])
        before[source.replace("/","_").replace(".c",".o")]=obj
    result={"note":"Fresh real simulator paths. PC times are host CPU microseconds; no target FPS claim.",
            "before":"Same current source except native prepared spans and Apps row geometry precomputation reverted.",
            "repeat":args.repeat,"cases":{}}
    for variant in ("before","after"):
        exe=out / (variant+".exe")
        command(["gcc",*FLAGS,str(main),*(str(before.get(p.name,p)) if variant=="before" else str(p) for p in objects),
                 "-o",str(exe),*LIBRARIES])
        result[variant+"_sha256"]=hashlib.sha256(exe.read_bytes()).hexdigest()
        for app in ("launcher","settings"):
            for rom in (1,0):
                for virtual in (0,1):
                    case=f"{app}_rom{rom}_virtual{virtual}_{variant}"
                    script=out / (case+".txt")
                    script.write_text("\n".join(["skipboot",f"set native_rom {rom}",f"set history_virtual {virtual}",
                        "set mblur 2","motion on",f"open! {app}","wait 6000","mark forward",
                        "slide 233 420 233 45 450 0","wait 1200","mark reverse",
                        "slide 233 45 233 420 450 0","wait 1200","mark fling_forward",
                        "flick 233 420 233 45 120","wait 2000","mark fling_reverse",
                        "flick 233 45 233 420 120","wait 2000",f"set history_virtual {virtual}","mem"])+"\n")
                    rows=[];logs=[];hashes=[]
                    for repeat in range(args.repeat):
                        dest=out / f"{case}_{repeat}";dest.mkdir(exist_ok=True)
                        (dest / "build").mkdir(exist_ok=True)
                        run=subprocess.run([str(exe),"--script",str(script),"--out",str(dest),"--hashes",str(dest / "hashes.txt"),
                                            "--no-media","--quiet"],cwd=dest,capture_output=True,text=True,timeout=120)
                        (dest / "run.log").write_text(run.stdout+run.stderr)
                        if run.returncode:raise RuntimeError(f"{case}: {run.stdout}{run.stderr}")
                        with (dest / "frames.csv").open() as file:rows.extend(csv.DictReader(file))
                        logs.extend(line for line in run.stdout.splitlines() if line.startswith(("NOSHISTORYV","MEM","NOSNATIVE","SETTINGS_CACHE")))
                        hashes.append(hashlib.sha256((dest / "hashes.txt").read_bytes()).hexdigest())
                    selected=[r for r in rows if r["section"] in ("forward","reverse","fling_forward","fling_reverse") and int(r["dirty_px"])>0]
                    metrics={"sending_frames":len(selected),"frame_hashes":hashes,"diagnostics":logs}
                    for key in ("pc_record_us","pc_raster_us","pc_tick_us","pc_history_us"):
                        values=sorted(float(r[key]) for r in selected)
                        metrics[key]={"median":statistics.median(values),"p95":values[min(len(values)-1,int(len(values)*.95))],"max":max(values)}
                    result["cases"][case]=metrics
                    (out / "results.json").write_text(json.dumps(result,indent=2))
                    print(case,json.dumps({k:v for k,v in metrics.items() if k not in ("diagnostics","frame_hashes")}),flush=True)


if __name__=="__main__":main()
