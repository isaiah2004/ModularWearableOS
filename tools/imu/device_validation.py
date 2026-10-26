"""Offline raw-IMU device evidence verifier. Never imports serial or opens a port.

Root/operator performs the explicit test described by --plan. Verification reads
CRC-checked SD downloads only. It never edits originals or deletes recordings.
"""
from pathlib import Path
import argparse
import csv
import hashlib
import io
import json
import math
import struct

PLAN="""RAW IMU DEVICE VALIDATION (operator/root only; this helper does not connect)

1. Keep existing recordings. Close all other COM3 clients between tools.
   With raw recording idle, capture `diag log`, `prefs`, and an SD baseline:
     python tools/sdsync.py --port COM3 --pull imu --out build/sports-reliability/imu-before
   Result directory contains imu-before/imu/*.csv, *.link, intent.*.bin.
   Do not press FILES / DELETE or run any --rm, erase, or flash operation.

2. Using the root-owned serial connection (or watchctl separately):
     open imu
   Wait 700ms for transition. tap 233 420 = RECORD tab.
   If previous recording review is visible, tap 233 342 once to dismiss review.
   When rate buttons are visible, tap 339 218 = 100Hz.
     tap 233 342
   Do not toggle the manual-awake setting. Allow display-off during this test;
   raw recording must continue independently. Root may add its explicit off test.
   Start only when idle. Confirm UI shows REC, increasing samples, and a new
   NNNN.csv. Record that ID. Wait at least 5 seconds, then capture `diag log`.
   `imu` is an instantaneous sensor query, NOT recorder status.
   Intent checkpoints should have valid active=1, hz=100 and current file ID.
   Do not download a growing CSV: transfer CRC/size can race append operations.

3. Intentional abrupt reset while this NEW test recording is active:
   a physical EN/RST press, or on the already root-owned pyserial link:
     from esptool.reset import HardReset
     HardReset(port, uses_usb=True)()
   This existing project reset method toggles EN via RTS; it does not invoke a
   bootloader, write firmware, or erase NVS. DTR starts false; do not use BOOT.
   Close/reconnect the root-owned port as needed and retain the boot log.
   Power-sheet Restart cannot test this: nos_host_power(1) rejects active IMU.
   Label this a MANUAL RESET test, not proof that a panic/watchdog occurred.

4. Wait 5 seconds after boot WITHOUT tapping START. `open imu`; wait 700ms;
   tap 233 420. Confirm REC and samples increasing in a DIFFERENT NNNN.csv.
   Capture `diag log`. It should persist IMU_RESUME (0x311), with a=new ID,
   b=old test ID,c=100. New NNNN.link must say reason=reset,previous=old ID,
   previous_bytes and previous_valid_bytes (last complete newline boundary).
   Original interrupted CSV must never be reopened for append or rewritten.

5. STOP only the resumed test recording: tap 233 342 once. Wait for SAVED /
   nonbusy UI (or visible error; do not claim success on an error).
   Capture `diag log`; checked newest intent must have active=0 and retain ID.
   Wait 2 seconds for bounded diagnostic persistence. Perform a SECOND manual
   reset. Wait 5 seconds; confirm recorder remains idle and no new CSV appears.
   Capture `diag log`. This establishes durable STOP, not merely an idle screen.

6. With recorder stopped, download immutable evidence to a NEW local directory:
     python tools/sdsync.py --port COM3 --pull imu --out build/sports-reliability/imu-after
     python tools/sdsync.py --port COM3 --pull watch/diagnostics --out build/sports-reliability/imu-after
     python tools/diag_export.py build/sports-reliability/imu-after/watch/diagnostics --json build/sports-reliability/imu-diag.json --strict
     python tools/imu/device_validation.py --before build/sports-reliability/imu-before --after build/sports-reliability/imu-after --diagnostics build/sports-reliability/imu-diag.json --require-reboot-after-stop --out build/sports-reliability/imu-validation.json
   Verifier checks baseline CSV/link hashes, checked inactive STOP, new files,
   reset linkage, complete CSV rows and recorded valid-tail boundaries. With
   diagnostics it also ties START/RESUME/STOP to the new file IDs and boot IDs.
   Retain all test files on SD; no delete/cleanup command is part of this plan.
"""

def card_dir(path):
    return path/'imu' if (path/'imu').is_dir() else path

def digest(path):
    with path.open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()

def checkpoint(path):
    data=path.read_bytes()
    if len(data)!=24:return None
    magic,seq,active,hz,file_id,checksum=struct.unpack('<6I',data)
    value=2166136261
    for byte in data[:20]:value=((value^byte)*16777619)&0xffffffff
    if magic!=0x494d5532 or checksum!=value or active>1 or hz not in (25,50,100) or file_id>10000:return None
    return dict(sequence=seq,active=active,hz=hz,file_id=file_id,path=str(path))

def inspect_csv(path):
    data=path.read_bytes();boundary=data.rfind(b'\n')+1
    rows=csv.reader(io.StringIO(data[:boundary].decode('ascii')))
    expected=['ms','utc_ms','ax_g','ay_g','az_g','gx_dps','gy_dps','gz_dps','a_g']
    assert next(rows,None)==expected,f'{path}: CSV header invalid'
    count=0;previous=-1
    for row in rows:
        assert len(row)==9,f'{path}: malformed complete row {count+1}'
        ms,utc=int(row[0]),int(row[1]);numbers=list(map(float,row[2:]))
        assert ms>previous and utc>=0 and all(math.isfinite(v) for v in numbers),f'{path}: invalid row {count+1}'
        assert abs(math.sqrt(sum(v*v for v in numbers[:3]))-numbers[-1])<.00003,f'{path}: bad magnitude'
        previous=ms;count+=1
    return dict(rows=count,bytes=len(data),valid_bytes=boundary,incomplete_tail_bytes=len(data)-boundary,sha256=digest(path))

def verify(args):
    before,after=card_dir(args.before),card_dir(args.after)
    assert before.is_dir() and after.is_dir(),'download directories do not exist'
    old={p.name:digest(p) for p in before.iterdir() if p.is_file() and p.suffix in ('.csv','.link')}
    for name,checksum in old.items():
        assert (after/name).is_file() and digest(after/name)==checksum,f'original recording changed/missing: {name}'
    new=sorted(p for p in after.glob('[0-9][0-9][0-9][0-9].csv') if p.name not in old)
    assert len(new)>=2,'need new initial and resumed CSV segments'
    files={int(p.stem):inspect_csv(p) for p in new}
    assert all(item['rows']>0 for item in files.values()),'new test segment has no samples'
    valid=[]
    for path in after.glob('intent.*.bin'):
        item=checkpoint(path)
        if item:valid.append(item)
    assert valid,'no valid durable checkpoint'
    latest=valid[0]
    for item in valid[1:]:
        delta=(item['sequence']-latest['sequence'])&0xffffffff
        if 0<delta<0x80000000:latest=item
    assert latest['active']==0,'STOP is not durably inactive'
    assert latest['file_id'] in files,'STOP references unexpected old file'
    links=[]
    for path in new:
        link=path.with_suffix('.link')
        assert link.is_file(),f'{path}: missing linkage'
        fields=dict(line.split('=',1) for line in link.read_text().splitlines())
        previous=int(fields['previous'])
        if previous<10000:
            prior=after/f'{previous:04}.csv';assert prior.is_file(),f'{link}: prior CSV missing'
            blob=prior.read_bytes()
            assert int(fields['previous_bytes'])==len(blob),f'{link}: original size differs'
            assert int(fields['previous_valid_bytes'])==blob.rfind(b'\n')+1,f'{link}: valid-tail boundary differs'
        links.append(dict(file_id=int(path.stem),**fields))
    reset_links=[item for item in links if item['reason']=='reset']
    assert reset_links,'no reset-resumed linkage'
    diagnostics={}
    if args.diagnostics:
        source=json.loads(args.diagnostics.read_text());assert not source.get('errors'),'diagnostic decoder reported corruption'
        events=source['events']
        resumes=[e for e in events if e['event']=='IMU_RESUME' and e['a'] in files]
        starts=[e for e in events if e['event']=='IMU_START' and e['a'] in files]
        stops=[e for e in events if e['event']=='IMU_STOP' and e['a']==latest['file_id']]
        assert starts and resumes and stops,'missing START/RESUME/STOP evidence for new test files'
        assert any(e['a']==item['file_id'] and e['b']==int(item['previous']) and e['c']==latest['hz'] for e in resumes for item in reset_links),'RESUME event and link disagree'
        assert any(a['boot']!=b['boot'] for a in starts for b in resumes),'no distinct pre/post-reset boot IDs'
        if args.require_reboot_after_stop:
            stop_boots={e['boot'] for e in stops}
            subsequent={e['boot'] for e in events if e['event']=='RESET_CONTEXT' and (e['a']&0xffffffff) in stop_boots}
            subsequent={boot for boot in subsequent if any(e['event']=='BOOT' and e['boot']==boot for e in events)}
            assert subsequent,'no boot after durable STOP found'
            assert any(not any(e['boot']==boot and e['event'] in ('IMU_START','IMU_RESUME') for e in events) for boot in subsequent),'recorder restarted after STOP'
        diagnostics=dict(starts=starts,resumes=resumes,stops=stops)
    else:
        assert not args.require_reboot_after_stop,'--diagnostics required to verify post-STOP boot'
    return dict(result='PASS',scope='Copied device files; verifier never connects to device',baseline_files_preserved=len(old),checkpoint=latest,new_files=files,links=links,diagnostics=diagnostics)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',action='store_true')
    parser.add_argument('--before',type=Path)
    parser.add_argument('--after',type=Path)
    parser.add_argument('--diagnostics',type=Path)
    parser.add_argument('--require-reboot-after-stop',action='store_true')
    parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    if args.plan:print(PLAN);return 0
    if not args.before or not args.after:parser.error('use --plan or provide --before and --after downloads')
    try:report=verify(args)
    except (AssertionError,OSError,ValueError,KeyError) as error:
        report=dict(result='FAIL',error=str(error))
    if args.out:
        args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2));return int(report['result']!='PASS')

if __name__=='__main__':raise SystemExit(main())
