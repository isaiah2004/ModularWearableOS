"""Real C recorder durability tests. Temporary cards only; never opens serial ports."""
from pathlib import Path
import csv
import argparse
import hashlib
import io
import math
import shutil
import struct
import subprocess
import tempfile
from recover import recover_segment

args=argparse.ArgumentParser(description=__doc__)
args.add_argument('--samples',type=int,default=720000,help='long-run sample count (default 2h at 100Hz)')
args=args.parse_args()
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build'/'imu_recovery'
OUT.mkdir(parents=True,exist_ok=True)
EXE=OUT/'recorder.exe'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Icore','-Inos',
                '-DIMU_SEGMENT_BYTES=16384','-DIMU_REC_TEST=1','tools/imu/recovery_harness.c','nos/imu_rec.c',
                '-lm','-Wl,--wrap=fwrite','-o',str(EXE)],cwd=ROOT,check=True)
class Recorder:
    def __init__(self,card):
        self.p=subprocess.Popen([str(EXE),str(card)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def cmd(self,cmd):
        self.p.stdin.write(cmd+'\n'); self.p.stdin.flush()
        line=self.p.stdout.readline().strip()
        assert line, (cmd,self.p.poll())
        return int(line.split()[0]),line
    def kill(self):
        self.p.kill(); self.p.wait(); self.p.stdin.close(); self.p.stdout.close()
    def close(self):
        self.p.stdin.close(); assert self.p.wait()==0; self.p.stdout.close()
def slots(card):
    result=[]
    for p in (card/'imu').glob('intent.*.bin'):
        if not p.is_file(): continue
        data=p.read_bytes()
        if len(data)!=24: continue
        words=struct.unpack('<6I',data)
        h=2166136261
        for b in data[:20]: h=((h^b)*16777619)&0xffffffff
        if h==words[-1]: result.append((words[1],words,p))
    return sorted(result)
def parse(path,partial=False):
    data=path.read_bytes()
    if partial: data=data[:data.rfind(b'\n')+1]
    else: assert data.endswith(b'\n'),path
    rows=list(csv.DictReader(io.StringIO(data.decode())))
    for r in rows:
        assert len(r)==9 and None not in r and all(v is not None for v in r.values()),(path,r)
        assert abs(math.sqrt(sum(float(r[k])**2 for k in ('ax_g','ay_g','az_g')))-float(r['a_g']))<0.00003
    times=[int(r['ms']) for r in rows]
    assert times==sorted(set(times)),path
    return len(rows)
with tempfile.TemporaryDirectory(prefix='imu_durable_',dir=OUT) as temp:
    card=Path(temp)
    r=Recorder(card)
    assert r.cmd('start 100')[0]==2
    assert r.cmd('ticks 1500')[0]==2
    assert slots(card)[-1][1][2]==1
    r.kill() # real abrupt process termination; no fclose/STOP
    files=list((card/'imu').glob('*.csv')); old={p:p.read_bytes() for p in files}
    latest=max(files)
    with latest.open('ab') as f: f.write(b'15001,1800000015001,0.1,broken')
    old[latest]=latest.read_bytes()
    r=Recorder(card)
    assert r.cmd('step')[0]==2
    new=max((card/'imu').glob('*.csv'))
    assert new not in old
    link=new.with_suffix('.link').read_text()
    assert 'reason=reset' in link and f'previous={int(latest.stem)}\n' in link
    assert f'previous_valid_bytes={old[latest].rfind(bytes([10]))+1}\n' in link
    assert all(p.read_bytes()==v for p,v in old.items())
    recovered=card/'recovered.csv'
    rows,tail=recover_segment(latest,recovered)
    assert tail==len(b'15001,1800000015001,0.1,broken')
    assert parse(recovered)==rows and latest.read_bytes()==old[latest]
    assert r.cmd('ticks 800')[0]==2
    assert r.cmd('stop')[0]==0
    assert slots(card)[-1][1][2]==0
    r.close()
    r=Recorder(card); assert r.cmd('step')[0]==0; r.close()
    for p in (card/'imu').glob('*.csv'): parse(p,p==latest)
    print('Abrupt kill, immutable torn CSV tail, linked new segment, STOP/no restart: PASS')
    # Corrupt the newest active slot: older checked slot must recover.
    r=Recorder(card); assert r.cmd('start 50')[0]==2; r.cmd('ticks 100'); r.kill()
    newest=slots(card)[-1][2]; data=newest.read_bytes(); newest.write_bytes(data[:11])
    r=Recorder(card); assert r.cmd('step')[0]==2; r.cmd('stop'); r.close()
    print('Torn newest checkpoint fallback: PASS')
    corrupt=card/'corrupt'; (corrupt/'imu').mkdir(parents=True)
    for i in range(2): (corrupt/'imu'/f'intent.{i}.bin').write_bytes(b'torn')
    r=Recorder(corrupt); state,text=r.cmd('step')
    assert state==4 and 'CHECKPOINT CORRUPT' in text
    assert r.cmd('start 100')[0]==2
    assert r.cmd('stop')[0]==0; r.close()
    print('Both invalid slots visible, explicit start recovers: PASS')
    # Block inactive checkpoint destination as a directory: STOP cannot acknowledge.
    r=Recorder(card); assert r.cmd('start 25')[0]==2; r.cmd('ticks 100')
    seq=slots(card)[-1][0]; target=card/'imu'/f'intent.{(seq+1)&1}.bin'
    target.unlink(missing_ok=True); target.mkdir()
    assert r.cmd('stop')[0]==4
    assert slots(card)[-1][1][2]==1
    target.rmdir(); assert r.cmd('ticks 1')[0]==0 # dirty STOP saved before confirming stopped
    assert slots(card)[-1][1][2]==0
    r.kill(); r=Recorder(card); assert r.cmd('step')[0]==0
    assert r.cmd('mounted 0')[0]==4
    r.cmd('start 100'); assert not r.cmd('ticks 20')[0]==2
    assert r.cmd('mounted 1')[0]==2
    assert r.cmd('mounted 0')[0]==4
    assert r.cmd('mounted 1')[0]==2
    r.cmd('stop');r.close()
    print('Checkpoint failure keeps dirty STOP, card absent and reinsert: PASS')
    failures=card/'failures'; failures.mkdir()
    r=Recorder(failures); assert r.cmd('start 100')[0]==2
    r.cmd('failwrite 1'); assert r.cmd('ticks 200')[0]==4
    assert slots(failures)[-1][1][2]==1
    damaged={p:p.read_bytes() for p in (failures/'imu').glob('*.csv')}
    r.cmd('failwrite 0'); assert r.cmd('ticks 300')[0]==2
    assert all(p.read_bytes()==v for p,v in damaged.items())
    r.cmd('failsync 1'); assert r.cmd('stop')[0]==4
    # Bytes may be visible despite fsync failure; STOP must remain unacknowledged.
    assert r.cmd('failsync 0')[0]==0
    assert slots(failures)[-1][1][2]==0
    r.close()
    for p in (failures/'imu').glob('*.csv'): parse(p,partial=p in damaged)
    print('ENOSPC short CSV writes, bounded retry, fsync failure and durable STOP retry: PASS')
    boundary=card/'boundary';boundary.mkdir()
    r=Recorder(boundary);assert r.cmd('start 100')[0]==2
    r.cmd('injectrollover 1');r.cmd('ticks 500');assert r.cmd('stop')[0]==0;r.close()
    assert sum(parse(p) for p in (boundary/'imu').glob('*.csv'))==501
    print('Producer push exactly between initial drain and rollover stop retained: PASS')
    # 2h synthetic 100Hz run through the real formatter/writer; small test segment limit
    # exercises hundreds of rollover/open/checkpoint/close cycles without big files.
    longcard=card/'long'; longcard.mkdir()
    r=Recorder(longcard); assert r.cmd('start 100')[0]==2
    assert r.cmd(f'ticks {args.samples}')[0]==2
    assert r.cmd('stop')[0]==0; r.close()
    recordings=sorted((longcard/'imu').glob('*.csv'))
    count=sum(parse(p) for p in recordings)
    assert count==args.samples,(count,len(recordings))
    assert len(recordings)>=(100 if args.samples>=720000 else 1)
    assert all(p.stat().st_size<23000 for p in recordings)
    print(f'{args.samples/360000:.2f}h / {count} rows, {len(recordings)} bounded segments, strict CSV parser: PASS')
print('All raw recorder recovery checks passed; no device accessed.')
