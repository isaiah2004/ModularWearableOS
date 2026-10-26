"""Repeatable directional menu workload. Host time and modeled cost are not device FPS."""
import argparse,csv,json,statistics,subprocess,time,hashlib,platform,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--exe',required=True);p.add_argument('--label',required=True);p.add_argument('--repeat',type=int,default=3);a=p.parse_args()
out=ROOT/'build/directional-benchmark'/a.label;out.mkdir(parents=True,exist_ok=True)
exe=(ROOT/a.exe).resolve(); result={'exe':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'platform':platform.platform(),'note':'Host process wall time includes startup and CSV output; estimated frame costs are the existing simulator model, not 200MHz device measurements.','cases':{}}
for app in ('launcher','settings'):
 for rom in (1,0):
  case=f'{app}_rom{rom}'
  lines=[f'set native_rom {rom}','set mblur 2','motion on',f'open! {app}','wait 6000','mark forward','slide 233 420 233 45 450 0','wait 1200','mark reverse','slide 233 45 233 420 450 0','wait 1200','mark fling_forward','flick 233 420 233 45 120','wait 2000','mark fling_reverse','flick 233 45 233 420 120','wait 2000','mem']
  script=out/f'{case}.txt';script.write_text('\n'.join(lines)+'\n')
  times=[];all_frames=[]
  for i in range(a.repeat):
   dest=out/f'{case}_{i}';t=time.perf_counter();run=subprocess.run([str(exe),'--script',str(script),'--out',str(dest),'--no-media','--quiet'],cwd=ROOT,capture_output=True,text=True,timeout=120);times.append(time.perf_counter()-t)
   (out/f'{case}_{i}.log').write_text(run.stdout+run.stderr)
   if run.returncode:raise RuntimeError(f'{case} failed {run.returncode}')
   all_frames.extend(csv.DictReader((dest/'frames.csv').open()))
  frames=list(csv.DictReader((dest/'frames.csv').open())); sections={}
  for section in ('forward','reverse','fling_forward','fling_reverse'):
   selected=[r for r in frames if r['section']==section and int(r['dirty_px'])>0]
   values=sorted(float(r['est_frame_ms']) for r in selected)
   sections[section]={'sending_frames':len(values),'model_ms_median':statistics.median(values) if values else 0,'model_ms_p95':values[min(len(values)-1,int(len(values)*.95))] if values else 0,'model_ms_max':max(values,default=0),'model_over_25ms':sum(v>25 for v in values),'model_over_33_333ms':sum(v>1000/30 for v in values)}
  for section,metrics in sections.items():
   selected=[r for r in all_frames if r['section']==section and int(r['dirty_px'])>0]
   for key in ('pc_record_us','pc_raster_us','pc_tick_us','pc_history_us'):
    values=sorted(float(r[key]) for r in selected)
    metrics[key]={'median':statistics.median(values) if values else 0,'p95':values[min(len(values)-1,int(len(values)*.95))] if values else 0,'max':max(values,default=0)}
  result['cases'][case]={'host_wall_seconds':times,'host_wall_seconds_median':statistics.median(times),'total_frames':len(frames),'sections':sections,'memory_lines':re.findall(r'^MEM.*$|^SETTINGS_CACHE.*$|^NOSNATIVE.*$',run.stdout,re.M)}
  print(case,json.dumps(result['cases'][case]),flush=True)
(out/'results.json').write_text(json.dumps(result,indent=2))
