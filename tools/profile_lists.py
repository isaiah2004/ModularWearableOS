"""Identity-bound list cadence / scroll-position / GRAM-write profile.

Offline by default. --run --port uses temporary profiling/navigation changes and
exercises the six saved FPS caps, restoring the original cap and diagnostics.
Detailed frame data is fetched only after bench end, eight records per reply.
No optical/vblank timing is inferred from DMA completion.
"""
from pathlib import Path
import argparse
import json
import re
import sys

ROOT=Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:sys.path.insert(0,str(ROOT))
from tools import target_benchmark as B
from tools.benchmark_sharp import SharpBenchmark,usb_control
from tools.build_identity import verify,source_manifest
import hashlib


def jobs():
    # Quiet timings for every choice; detailed paired directions for 40 / 60.
    result=[]
    for cap in (60,48,40,30,24,0):
        for app in ('apps','settings'):
            result.append((cap,app,B.WORKLOADS[1],'up','quiet'))
    for cap in (60,40):
        for app in ('apps','settings'):
            for direction in ('up','down'):
                result.append((cap,app,B.WORKLOADS[1],direction,'detailed'))
    for app in ('apps','settings'):
        for direction in ('up','down'):
            result.append((60,app,B.WORKLOADS[0],direction,'detailed'))
    return result


def extra_command(command,mutation):
    if mutation:return command in {'list_profile 0','list_profile 1','prefs flush',
                                  *('fps_cap '+str(n) for n in (0,24,30,40,48,60))}
    if command in ('fps_cap','list_profile'):return True
    match=re.fullmatch(r'list_samples (\d+) ([1-8])',command)
    return bool(match and int(match[1])+int(match[2])<=512)


class ListSession(B.Session):
    def command(self,command,*,mutation=False,purpose=''):
        if not extra_command(command,mutation):
            return super().command(command,mutation=mutation,purpose=purpose)
        if self.tainted:raise B.ProtocolError('Complete recovery before another command')
        entry={'utc':B.utc_now(),'command':command,'mutation':mutation,
               'purpose':purpose,'status':'pending'}
        self.commands.append(entry);self.persist()
        try:
            lines=self.transport.execute(command,timeout=10,retries=0)
            table=B.records(lines)
            if command.startswith('list_samples '):
                start,count=map(int,command.split()[1:])
                frames=table.get('NOSLISTFRAME',[])
                if len(frames)!=count or [B.integer(f,'index') for f in frames]!=list(range(start,start+count)):
                    raise B.ProtocolError('Missing or misordered list frame records')
            else:
                record='NOSFPSCAP' if command.startswith('fps_cap') else 'NOSPREFS' if command=='prefs flush' else 'NOSLISTPROFILE'
                B.one(table,record)
            entry.update(lines=lines,status='complete',request_id=(self.transport.last_result or {}).get('id'))
            return lines
        except BaseException as exc:
            self.tainted=True;entry.update(status='unknown' if mutation else 'failed',error=str(exc));raise
        finally:self.persist()


class ListBenchmark(SharpBenchmark):
    original_cap=None
    original_profile=None

    def position(self,app,direction,travel):
        if app!='apps':return super().position(app,direction,travel)
        previous=None
        for _ in range(12):
            pose=self.settled_pose(app)
            offset=B.integer(pose,'offset_q8')/256
            limit=B.integer(pose,'limit_q8')/256
            if limit<2*travel+60:raise B.AdmissionError('List too short for measured travel')
            # Apps snaps to 92 px rows. Half-error corrections can stop short
            # of half a row and snap back forever, especially at 24 FPS.
            target=round(limit*(.4 if direction=='up' else .6)/92)*92
            if abs(offset-target)<=46:return pose
            if previous==offset:raise B.AdmissionError('Acknowledged positioning did not cross a row; stop before repetition')
            previous=offset
            distance=max(64,min(300,round(abs(target-offset)*.75)))
            duration=max(500,round(distance/150*1000))
            y0,y1=233+distance//2,233-(distance-distance//2)
            if target<offset:y0,y1=y1,y0
            self.mutate(f'swipe 233 {y0} 233 {y1} {duration}','row-aware positioning outside measured capture')
            self.session.sleep(duration/1000+.75)
        raise B.AdmissionError('Bounded Apps positioning did not settle')

    def set_cap(self,cap):
        current=B.one(B.records(self.session.read('fps_cap')),'NOSFPSCAP')
        if B.integer(current,'fps')!=cap:
            self.mutate(f'fps_cap {cap}','exercise requested saved frame-rate cap')
            self.session.sleep(.15)
            saved=B.one(B.records(self.mutate('prefs flush','verify checkpoint before timing')),'NOSPREFS')
            B.prefs_fingerprint(saved)
        B.require_equal(B.one(B.records(self.session.read('fps_cap')),'NOSFPSCAP'),'fps',cap)

    def execute(self):
        self.preflight()
        self.original_cap=B.integer(B.one(B.records(self.session.read('fps_cap')),'NOSFPSCAP'),'fps')
        self.original_profile=B.integer(B.one(B.records(self.session.read('list_profile')),'NOSLISTPROFILE'),'enabled')
        if self.original_profile:raise B.AdmissionError('A list trace is already owned by another session')
        if not B.integer(self.original['state'],'awake'):
            self.possible_flags.add('awake');self.mutate('awake','inhibit sleep during requested list profiling')
        previous_route=None
        for cap,app,work,direction,profile in jobs()[self.configuration.get('skip_windows',0):]:
            self.set_cap(cap)
            self.diagnostic('cost_probe',int(profile=='detailed'))
            self.set_flag('pipeline_probe',int(profile=='detailed'))
            enabled=B.integer(B.one(B.records(self.session.read('list_profile')),'NOSLISTPROFILE'),'enabled')
            if enabled!=int(profile=='detailed'):
                self.mutate(f'list_profile {int(profile=="detailed")}','bounded PSRAM frame trace; output only after capture')
            if previous_route!=app:self.route(app);previous_route=app
            self.window(app,work,direction,'cap-'+str(cap),profile,0)
            entry=self.report['windows'][-1];entry['fps_cap']=cap
            if profile=='detailed':
                status=B.one(B.records(entry['raw_report']),'NOSLISTPROFILE')
                B.require_equal(status,'dropped',0)
                count=B.integer(status,'samples');frames=[]
                if count!=entry['statistics']['capture']['frames']:raise B.AdmissionError('Frame trace/capture mismatch')
                for offset in range(0,count,8):
                    lines=self.session.read(f'list_samples {offset} {min(8,count-offset)}')
                    frames.extend(B.records(lines)['NOSLISTFRAME'])
                if any(B.integer(f,'app')!=(7 if app=='apps' else 8) for f in frames):
                    raise B.AdmissionError('Foreign app in list frame trace')
                entry['frames']=[{k:int(v) for k,v in f.items()} for f in frames]
            self.persist()
            print(json.dumps({'window':len(self.report['windows'])+self.configuration.get('skip_windows',0),'total':len(jobs()),'app':app,
                'cap':cap,'profile':profile,'direction':direction,
                'p50_ms':entry['statistics']['p50_ms'],'max_ms':entry['statistics']['max_ms']}),flush=True)
        self.report['status']='measured'

    def cleanup(self):
        custom=[]
        try:
            if self.original is not None and self.original_profile==0:
                if self.session.tainted or self.session.transport.active is not None:self.session.recover('barrier before list-profile cleanup')
                self.check_same_device()
                runtime=B.one(B.records(self.session.read('runtime_state')),'NOSRUNTIME')
                if B.integer(runtime,'capture_state') in (1,2) and self.possible_capture:
                    self.mutate('bench end','stop owned trace before restoring cap');self.possible_capture=False
                if B.integer(B.one(B.records(self.session.read('list_profile')),'NOSLISTPROFILE'),'enabled'):
                    self.mutate('list_profile 0','release owned PSRAM trace')
                self.set_cap(self.original_cap)
        except BaseException as exc:custom.append(str(exc))
        super().cleanup()
        if custom:
            self.report['status']='failed';self.report['cleanup'].update(status='failed',list_errors=custom);self.persist()


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run',action='store_true');parser.add_argument('--port')
    parser.add_argument('--out',type=Path,default=ROOT/'build/list-profile-20261022/caps')
    parser.add_argument('--resume-from',type=Path,help='Resume only the unattempted suffix after verified cleanup')
    args=parser.parse_args(argv)
    configuration=B.plan(1,pipeline_profile=True)
    configuration.update(sharp=True,window_count=len(jobs()),scope=__doc__)
    configuration['jobs']=[{'cap':cap,'app':app,'workload':work['name'],'direction':direction,'profile':profile}
                           for cap,app,work,direction,profile in jobs()]
    configuration['safety']=[
        'Require finalized current workspace, exact installed ELF/source/boot and idle owner state',
        'No flash/erase/OTA/unlock; exercise only the requested FPS preference and restore its original value',
        'Checkpoint cap changes outside capture; restore and verify the complete original preference checksum',
        'Save/restore diagnostics, sleep inhibition, HOME page and USB reply guard',
        'Send each mutation once; recover the request barrier before cleanup after an uncertain result',
        'Store frames in bounded PSRAM and fetch them after capture; DMA/GRAM timing is not optical scanout']
    if not args.run:print(json.dumps(configuration,indent=2));return 0
    if not args.port:parser.error('--run requires --port')
    output=args.out.resolve()
    if not output.is_relative_to((ROOT/'build').resolve()):parser.error('--out must be under workspace build/')
    identity=verify(ROOT/'firmware/build')
    archived=json.loads((ROOT/'firmware/build'/identity['source_manifest']).read_text())
    old={f['path']:f['sha256'] for f in archived['files']}
    current=source_manifest(ROOT);new={f['path']:f['sha256'] for f in current['files']}
    changed={p for p in old.keys()|new.keys() if old.get(p)!=new.get(p)}
    if not changed<={'tools/profile_lists.py','tests/test_list_profile.py'}:
        raise B.AdmissionError('Unbuilt firmware/source changed: '+','.join(sorted(changed)))
    configuration['host_update']={'firmware_source_unchanged':True,'host_only_changes':sorted(changed),
                                  'host_tree_sha256':current['tree_sha256']}
    previous=None
    if args.resume_from:
        previous=json.loads(args.resume_from.read_text())
        if previous['cleanup']['status']!='verified' or any(w['status']!='valid' for w in previous['windows']):
            raise B.AdmissionError('Resume needs a valid prefix with verified cleanup')
        if previous['device_identity']['elf_sha256']!=identity['firmware_elf_sha256']:
            raise B.AdmissionError('Resume firmware differs')
        for w,job in zip(previous['windows'],jobs()):
            cap,app,work,direction,profile=job
            if (w['fps_cap'],w['app'],w['workload'],w['direction'],w['profile'])!=(cap,app,work,direction,profile):
                raise B.AdmissionError('Resume windows differ from planned prefix')
        skip=len(previous['windows'])
        if not 0<skip<len(jobs()):raise B.AdmissionError('No unattempted suffix')
        configuration.update(skip_windows=skip,window_count=len(jobs())-skip,
            prior_evidence=str(args.resume_from.resolve()),prior_sha256=hashlib.sha256(args.resume_from.read_bytes()).hexdigest())
    output.mkdir(parents=True,exist_ok=False)
    session=ListSession(args.port,output,usb_paced_writes=True,diagnostic_commands=('cost_probe 0','cost_probe 1','rotation 1','rotation 2'))
    original_guard=None;changed=False;result=None;boot=None
    try:
        session.connect()
        # Identity is checked before waking; a sleeping watch correctly reports
        # 80 MHz. The active 240 MHz check belongs to Benchmark.preflight.
        build=B.one(B.records(session.read('info')),'NOSBUILD')
        if (build.get('elf_sha256')!=identity['firmware_elf_sha256'] or
            build.get('source_sha256')!=identity['source_tree_sha256'] or
            build.get('source_build_id')!=identity['source_tree_sha256'][:16]):
            raise B.AdmissionError('Prelude installed build differs')
        runtime=B.one(B.records(session.read('runtime_state')),'NOSRUNTIME');B.check_runtime(runtime)
        state=B.one(B.records(session.read('state')),'NOSSTATE')
        if state['top']!='HOME' or B.integer(state,'locked') or B.integer(state,'tx'):raise B.AdmissionError('Need settled unlocked HOME')
        B.prefs_fingerprint(B.one(B.records(session.read('prefs')),'NOSPREFS'))
        boot=B.one(B.records(session.read('diag log')),'NOSLOG')['boot']
        if previous and (boot!=previous['before']['log']['boot'] or
            B.prefs_fingerprint(B.one(B.records(session.read('prefs')),'NOSPREFS'))!=B.prefs_fingerprint(previous['before']['prefs'])):
            raise B.AdmissionError('Boot or original preferences changed before resume')
        original_guard=int(usb_control(session)['guard'])
        if not original_guard:changed=True;usb_control(session,1)
        if B.integer(state,'pwr'):
            session.command('home',mutation=True,purpose='wake unlocked HOME for requested profiling');session.sleep(.8)
        result=ListBenchmark(session,identity,configuration).run()
        print(json.dumps({'status':result['status'],'error':result.get('error'),'cleanup':result['cleanup']}))
        return 0 if result['status']=='measured' and result['cleanup']['status']=='verified' else 1
    finally:
        try:
            if changed:
                if session.tainted or session.transport.active is not None:session.recover('barrier before reply guard restoration')
                B.check_identity(session.read('info'),identity)
                if B.one(B.records(session.read('diag log')),'NOSLOG')['boot']!=boot:raise B.AdmissionError('Boot changed before reply guard restore')
                if int(usb_control(session)['guard'])!=original_guard:usb_control(session,original_guard)
                if int(usb_control(session)['guard'])!=original_guard:raise B.AdmissionError('Reply guard restore unconfirmed')
        finally:session.close()

if __name__=='__main__':raise SystemExit(main())
