"""Identity-bound sharp-firmware benchmark; offline unless --run and --port.

Measures 32 moving windows, a matched-pixel rotation ABBA, and nested raster
costs. Changes only temporary diagnostic flags/navigation; restores those flags,
HOME page and sleep inhibition, and verifies preferences and boot identity.
"""
import argparse
from pathlib import Path
import json
import sys
import hashlib
import re
import secrets
import time
import zlib

ROOT=Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:sys.path.insert(0,str(ROOT))
from tools import target_benchmark as B
from tools.build_identity import verify,source_manifest
DIAGNOSTIC_COMMANDS=('cost_calibrate','rotation_bench','cost_probe 0','cost_probe 1','rotation 1','rotation 2')

def verified_build():
    identity=verify(ROOT/'firmware/build')
    archived=json.loads((ROOT/'firmware/build'/identity['source_manifest']).read_text())
    current=source_manifest(ROOT)
    before={x['path']:x['sha256'] for x in archived['files']}
    after={x['path']:x['sha256'] for x in current['files']}
    changed={x for x in before.keys()|after.keys() if before.get(x)!=after.get(x)}
    # A corrected host harness does not require reflashing unchanged firmware.
    # All other source, config, generated pixels and artifacts must still match.
    allowed={'tools/benchmark_sharp.py','tests/test_sharp_benchmark.py'}
    if not changed<=allowed:raise B.AdmissionError('Unbuilt source changed: '+','.join(sorted(changed)))
    return identity,{'firmware_source_unchanged':True,'host_only_changes':sorted(changed),
                     'host_tree_sha256':current['tree_sha256'],'script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}

USB_FIELDS=set('v nonce valid guard overrides cc cf zc zf zu zm wc wf wu wm rx qa qf ui rc ri rq ra'.split())
def parse_usb_frame(line,nonce):
    match=re.fullmatch(r'USBDIAG (\d+):(.*) crc=([0-9a-f]{8})',line)
    if not match:raise B.ProtocolError('Invalid direct USB diagnostic frame')
    body=match[2].encode('ascii')
    if len(body)!=int(match[1]) or zlib.crc32(body)!=int(match[3],16):raise B.ProtocolError('USB diagnostic length/CRC mismatch')
    pairs=[p.split('=') for p in match[2].split()]
    if any(len(p)!=2 for p in pairs) or len(pairs)!=len(USB_FIELDS):raise B.ProtocolError('USB diagnostic fields malformed')
    values=dict(pairs)
    if set(values)!=USB_FIELDS or any(not re.fullmatch(r'\d+',v) or int(v)>0xffffffff for v in values.values()):raise B.ProtocolError('USB diagnostic fields invalid')
    if values['v']!='2' or values['valid']!='1' or int(values['nonce'])!=nonce or values['guard'] not in ('0','1'):raise B.ProtocolError('USB diagnostic nonce/version mismatch')
    return values

def usb_control(session,setting=None):
    if setting not in (None,0,1) or session.tainted or session.transport.active is not None:raise B.ProtocolError('USB diagnostic requires a completed capability barrier')
    nonce=secrets.randbelow(0xffffffff)+1
    command=('usb_diag' if setting is None else f'usb_guard {setting}')+f' {nonce}'
    entry={'utc':B.utc_now(),'command':command,'mutation':setting is not None,'purpose':'one nonce/CRC checked reply-guard observation or setting; never replayed','status':'pending','raw_frames':[]}
    session.commands.append(entry);session.persist()
    try:
        session.transport._record('tx',command);session.link.send(command);deadline=time.monotonic()+3
        while time.monotonic()<deadline:
            line=session.link.readline(.1)
            if not line:continue
            session.transport._record('rx',line);entry['raw_frames'].append(line)
            if line.startswith('USBDIAG '):
                values=parse_usb_frame(line,nonce)
                if setting is not None and values['guard']!=str(setting):raise B.ProtocolError('USB guard setting did not apply')
                entry.update(status='complete',values=values);return values
        raise B.ProtocolError('Direct USB diagnostic outcome unknown; command not replayed')
    except BaseException as exc:
        session.tainted=True;entry.update(status='unknown',error=f'{type(exc).__name__}: {exc}');raise
    finally:session.persist()

def jobs():
    result=[];typical=B.WORKLOADS[1]
    for arm,mode in enumerate((1,2,2,1)):
        for app in ('apps','settings'):
            for direction in ('up','down'):result.append((mode,app,typical,direction,f'sharp-rotation{mode}-arm{arm}','quiet',arm))
    for app in ('apps','settings'):
        for work in (B.WORKLOADS[0],B.WORKLOADS[2]):
            for direction in ('up','down'):result.append((2,app,work,direction,'sharp-rotation2','quiet',0))
    for mode in (1,2):
        for app in ('apps','settings'):
            for direction in ('up','down'):result.append((mode,app,typical,direction,f'sharp-rotation{mode}','instrumented',0))
    return result

class SharpBenchmark(B.Benchmark):
    def diagnostic(self,name,value):
        state=B.records(self.session.read('state'))
        current=B.integer(B.one(state,'NOSRENDER' if name=='cost_probe' else 'NOSSTATE'),name)
        if current!=value:
            self.mutate(f'{name} {value}','temporary measured sharp-rendering variant')
            state=B.records(self.session.read('state'))
            B.require_equal(B.one(state,'NOSRENDER' if name=='cost_probe' else 'NOSSTATE'),name,value)

    def execute(self):
        self.preflight()
        if not B.integer(self.original['state'],'awake'):
            self.possible_flags.add('awake');self.mutate('awake','temporarily inhibit sleep during the authorized benchmark')
            B.require_equal(B.one(B.records(self.session.read('state')),'NOSSTATE'),'awake',1)
        self.report['cost_calibration']=B.records(self.mutate('cost_calibrate','calibrate empty profiling scope overhead'))
        rotation=B.records(self.mutate('rotation_bench','matched SRAM pixels, paired/columns ABBA; restores rotation mode'))
        rows=rotation.get('NOSROTBENCH',[])
        if len(rows)!=16 or any(B.integer(row,'mismatches') for row in rows):
            raise B.AdmissionError('Rotation kernel pixel comparison incomplete or incorrect')
        self.report['rotation_microbenchmark']=rotation;self.persist()
        previous_route=None
        for mode,app,work,direction,variant,profile,repetition in jobs()[self.configuration.get('skip_windows',0):]:
            self.diagnostic('rotation',mode);self.diagnostic('cost_probe',int(profile=='instrumented'))
            self.set_flag('pipeline_probe',int(profile=='instrumented'))
            route=(app,mode,profile)
            if route!=previous_route:self.route(app);previous_route=route
            self.window(app,work,direction,variant,profile,repetition)
            if profile=='instrumented':
                entry=self.report['windows'][-1]
                cost=B.one({'NOSCOST':entry['statistics']['cost']},'NOSCOST')
                B.require_equal(cost,'enabled',1);B.require_equal(cost,'errors',0)
                if len(entry['statistics']['cost_buckets'])!=6:raise B.AdmissionError('Missing raster cost categories')
        self.report['status']='measured'

    def cleanup(self):
        custom_errors=[]
        try:
            if self.original is not None:
                if self.session.tainted or self.session.transport.active is not None:
                    self.session.recover('mandatory capability barrier before custom diagnostic cleanup')
                self.check_same_device()
                runtime=B.one(B.records(self.session.read('runtime_state')),'NOSRUNTIME')
                if B.integer(runtime,'capture_state') in (1,2) and self.possible_capture:
                    self.mutate('bench end','stop only this benchmark owned capture before restoring diagnostic flags')
                    self.possible_capture=False
                self.diagnostic('rotation',B.integer(self.original['state'],'rotation'))
                self.diagnostic('cost_probe',B.integer(B.one(self.original['sections'],'NOSRENDER'),'cost_probe'))
        except BaseException as exc:
            custom_errors.append(f'{type(exc).__name__}: {exc}')
        super().cleanup()
        if custom_errors:
            self.report['cleanup'].update(status='failed',custom_diagnostic_errors=custom_errors)
            self.persist()

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--port')
    parser.add_argument('--out',type=Path,default=ROOT/'build/sharp-20261022/measurements')
    parser.add_argument('--resume-from',type=Path,help='Complete only the unattempted suffix of a verified-cleanup run')
    args=parser.parse_args(argv)
    configuration=B.plan(1,pipeline_profile=True)
    configuration.update(sharp=True,window_count=32,scope='Sharp rendering experiment; separate quiet and instrumented windows; rotation kernel ABBA uses identical pixels')
    configuration['additional_diagnostic_commands']=DIAGNOSTIC_COMMANDS
    configuration['usb_paced_writes']=True
    if not args.run:
        print(json.dumps(configuration,indent=2));return 0
    if not args.port:parser.error('--run requires an explicit --port')
    output=args.out.resolve()
    if not output.is_relative_to((ROOT/'build').resolve()):parser.error('--out must be inside workspace build/')
    output.mkdir(parents=True,exist_ok=False)
    identity,host_update=verified_build()
    previous=None
    if args.resume_from:
        previous=json.loads(args.resume_from.read_text())
        if previous['cleanup']['status']!='verified' or any(w['status']!='valid' for w in previous['windows']):raise B.AdmissionError('Resume requires only valid completed windows and verified cleanup')
        if previous['expected_identity']!=identity:raise B.AdmissionError('Resume firmware differs')
        for old,job in zip(previous['windows'],jobs()):
            if (old['app'],old['workload'],old['direction'],old['variant'],old['profile'],old['repetition'])!=(job[1],job[2],job[3],job[4],job[5],job[6]):raise B.AdmissionError('Resume windows do not match the planned prefix')
        configuration.update(skip_windows=len(previous['windows']),window_count=32-len(previous['windows']),
                             prior_evidence=str(args.resume_from.resolve()),prior_sha256=hashlib.sha256(args.resume_from.read_bytes()).hexdigest())
        if configuration['window_count']<=0:raise B.AdmissionError('No unattempted windows remain')
    B.write_json(output/'plan.json',configuration)
    B.write_json(output/'host-update.json',host_update)
    session=B.Session(args.port,output,usb_paced_writes=True,diagnostic_commands=DIAGNOSTIC_COMMANDS)
    guard_original=None;guard_may_change=False;guard_receipt={};result=None;boot=None
    try:
        session.connect()
        # The panel may be asleep. Verify exact image, clean settings, and all
        # active owners before waking the user's unlocked HOME for measurement.
        info=B.one(B.records(session.read('info')),'NOSBUILD')
        for field,key in (('source_sha256','source_tree_sha256'),('elf_sha256','firmware_elf_sha256')):
            if info[field]!=identity[key]:raise B.AdmissionError('Prelude build mismatch: '+field)
        state=B.one(B.records(session.read('state')),'NOSSTATE')
        runtime=B.one(B.records(session.read('runtime_state')),'NOSRUNTIME');B.check_runtime(runtime)
        prefs=B.one(B.records(session.read('prefs')),'NOSPREFS');B.prefs_fingerprint(prefs)
        boot=B.one(B.records(session.read('diag log')),'NOSLOG')['boot']
        if previous and (boot!=previous['before']['log']['boot'] or B.prefs_fingerprint(prefs)!=B.prefs_fingerprint(previous['before']['prefs'])):raise B.AdmissionError('Resume boot or preferences changed')
        if state['top']!='HOME' or B.integer(state,'locked') or B.integer(state,'tx'):
            raise B.AdmissionError('Prelude needs unlocked, settled HOME')
        B.write_json(output/'prelude.json',{'info':info,'state':state,'runtime':runtime,'prefs':prefs})
        observed=usb_control(session);guard_original=int(observed['guard']);guard_receipt['before']=observed
        if not guard_original:
            guard_may_change=True;guard_receipt['enabled']=usb_control(session,1)
        if B.integer(state,'pwr'):
            session.command('home',mutation=True,purpose='wake unlocked HOME for the explicitly requested performance test')
            session.sleep(.8)
        result=SharpBenchmark(session,identity,configuration).run()
        result['host_update']=host_update
        print(json.dumps({'status':result['status'],'error':result.get('error'),'cleanup':result['cleanup'],'summary':result.get('summary')}))
        return 0 if result['status']=='measured' and result['cleanup']['status']=='verified' else 1
    finally:
        try:
            if guard_may_change:
                if session.tainted or session.transport.active is not None:session.recover('capability barrier before observing reply-guard restoration')
                fresh=B.one(B.records(session.read('info')),'NOSBUILD')
                log=B.one(B.records(session.read('diag log')),'NOSLOG')
                if fresh['elf_sha256']!=identity['firmware_elf_sha256'] or fresh['source_sha256']!=identity['source_tree_sha256'] or log['boot']!=boot:raise B.AdmissionError('Build/boot changed before guard restoration')
                observed=usb_control(session)
                if int(observed['guard'])!=guard_original:guard_receipt['restored']=usb_control(session,guard_original)
                final=usb_control(session)
                if int(final['guard'])!=guard_original:raise B.AdmissionError('Reply guard restoration unconfirmed')
                guard_receipt.update(status='verified',after=final)
            else:guard_receipt['status']='unchanged'
        except BaseException as exc:
            guard_receipt.update(status='failed',error=f'{type(exc).__name__}: {exc}')
            if result is not None:
                result['status']='failed';result['cleanup'].update(status='failed',usb_guard_error=guard_receipt['error'])
            raise
        finally:
            B.write_json(output/'usb-guard.json',guard_receipt)
            if result is not None:
                result['usb_guard']=guard_receipt;B.write_json(output/'results.json',result)
            session.close()

if __name__=='__main__':raise SystemExit(main())
