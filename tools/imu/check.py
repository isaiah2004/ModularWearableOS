"""Deterministic IMU app checks; no serial, flashing, or modifications to user SD data."""
from pathlib import Path
import csv
import math
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/imu'
OUT.mkdir(parents=True, exist_ok=True)

def run(name):
    card = Path(tempfile.mkdtemp(prefix=name+'_sd_', dir=OUT))
    if name == 'browser':
        (card/'imu').mkdir()
        for number in (0,7,9999):
            (card/f'imu/{number:04}.csv').write_bytes(b'ms,utc_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,a_g\n100,0,0,0,-1,0,0,0,1\n200,0,0,0,-1,0,0,0,1\n9999,0,partial')
    env = dict(os.environ, NOS_SD_ROOT=str(card))
    result = subprocess.run([str(ROOT/'build/nos_sim.exe'), '--script',
                             str(ROOT/f'tools/imu/{name}.txt'), '--out', str(OUT/name)],
                            cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT/(name+'.log')).write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError(f'{name} failed: {OUT/(name+".log")}')
    print(name, 'PASS', flush=True)
    return card

card = run('rates')
for number, hz in enumerate((25,50,100)):
    with (card/f'imu/{number:04}.csv').open(newline='') as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == hz*10
    times = [int(r['ms']) for r in rows]
    assert times == list(range(1000//hz,10001,1000//hz))
    utc = [int(r['utc_ms']) for r in rows]
    assert len(set(u-t for u,t in zip(utc,times))) == 1
    assert utc[0] > 1_700_000_000_000
    for r in rows:
        mag = math.sqrt(sum(float(r[k])**2 for k in ('ax_g','ay_g','az_g')))
        assert abs(mag-float(r['a_g'])) < 0.00002
    assert max(abs(float(r['gy_dps'])) for r in rows) > 10
print('CSV cadence / UTC / magnitude / gyro PASS')
run('faults')
run('allocation')
run('browser')
run('launch')
run('tour')
# Contact sheet from the final UI states, preserving full resolution source PNGs.
from PIL import Image, ImageDraw
names = ('live','ready','recording','files','delete_confirm','empty')
sheet = Image.new('RGB',(3*466,2*502),'#111111')
draw = ImageDraw.Draw(sheet)
for i,name in enumerate(names):
    x=(i%3)*466; y=(i//3)*502
    sheet.paste(Image.open(OUT/'tour'/f'{name}.png'),(x,y+30))
    draw.text((x+20,y+8),name.upper(),fill='white')
sheet.save(OUT/'contact_sheet.png')
print('Screenshots:', OUT/'contact_sheet.png')
