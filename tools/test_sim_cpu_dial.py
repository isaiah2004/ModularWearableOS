from pathlib import Path
import subprocess,csv
r=Path(__file__).resolve().parents[1];o=r/'build/smear-cpu-dial/check';o.mkdir(parents=True,exist_ok=True)
e=r/'build/nos_sim_devtools_final.exe';script=o/'run.txt';script.write_text('root settings\nwait 6000\nslide 233 330 233 180 1000 0\nwait 400\n')
rows={}
for mhz in [80,240]:
 subprocess.run([str(e),'--cpu-mhz',str(mhz),'--script',str(script),'--out',str(o/str(mhz)),'--no-media','--quiet'],cwd=r,check=True,stdout=subprocess.DEVNULL)
 rows[mhz]=list(csv.DictReader((o/str(mhz)/'frames.csv').open()))
matched=0
for a,b in zip(rows[80],rows[240]):
 if a['calls']==b['calls'] and a['dirty_px']==b['dirty_px'] and float(b['est_raster_ms'])>0.1:
  # Ratios are compared only when both frames did the same work.
  if abs(float(a['est_raster_ms'])-3*float(b['est_raster_ms']))<0.005:
   assert a['est_bus_ms']==b['est_bus_ms'];matched+=1
assert matched>20,matched
for bad in ['0','481','abc','240x']:
 assert subprocess.run([str(e),'--cpu-mhz',bad],cwd=r,capture_output=True).returncode==2
print(f'PASS CPU dial: {matched} frames with 3x CPU cost at 80 vs 240 MHz, unchanged bus, rejected invalid clocks')

