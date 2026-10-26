"""Control this watch over native USB serial; one command at a time.

Install: python -m pip install pyserial
Examples: python tools/watchctl.py --list
          python tools/watchctl.py --port COM7 info
          python tools/watchctl.py --port COM7 --sync-time
          python tools/watchctl.py --port COM7 --smoke --log build/usb.jsonl
"""
import argparse
from datetime import datetime
import json
import os
import getpass
from pathlib import Path
import re
import sys
import time
if __package__:
    from .host_transport import HostTransport, SerialLines, DeviceError, CommandRejected, CommandTimeout, redact
else:
    from host_transport import HostTransport, SerialLines, DeviceError, CommandRejected, CommandTimeout, redact


def execute(port, command, timeout=10):
    transport = port if isinstance(port, HostTransport) else HostTransport(SerialLines(port))
    return transport.execute(command, timeout)

def result_record(transport, command, lines=None, error=None):
    state = transport.active or transport.last_result or {}
    if state.get('command') != command: state = {}
    record = {'time': datetime.now().astimezone().isoformat(), 'command': redact(command),
              'response': [redact(line) for line in (lines if lines is not None else state.get('lines', []))],
              'request_id': state.get('id'), 'correlated': transport.correlated, 'ok': error is None}
    if error is not None:
        record['error'] = {'type': type(error).__name__, 'message': redact(str(error))}
        if isinstance(error, DeviceError):
            record['error'].update(phase=error.phase, code=error.code, errno=error.errno)
    return record


SMOKE_APPS = (("Energy", "ENERGY"), ("Battery history", "BATTERY HISTORY"),
              ("Sports", "SPORTS"), ("Developer tools", "DEVELOPER TOOLS"))
READONLY = frozenset({'hostcaps', 'info', 'state', 'runtime_state', 'diag log', 'prefs',
                      'sd', 'imu', 'sports', 'nav_state', 'board', 'apps', 'time', 'help', 'touch_state', 'jobs'})
SMOKE_IDLE = ("ota_busy", "shutdown_pending", "prefs_loading", "prefs_dirty",
              "imu_pending", "imu_hz", "imu_queued", "imu_file_busy", "workout_dirty",
              "workout_busy", "workout_uncertain", "workout_corrupt", "workout_foreign",
              "track_active", "track_pending", "track_saving", "audio_active", "touch_down",
              "touch_pending", "button_down", "injected", "asset_busy", "radio_pending")
SMOKE_FLAGS = ("awake", "face", "perf", "trace", "mblur", "layers", "history", "uniform",
               "packed", "worker", "qz", "profile", "full", "light", "shade", "sun",
               "pipeline", "card_budget_us", "detail", "proxy", "pipeline_probe",
               "settings_shutter", "rotation", "damage_bands", "cache_packed")


def smoke_record(lines, prefix):
    matches = [line for line in lines if line.startswith(prefix + ' ')]
    if len(matches) != 1:
        raise ValueError(f'Smoke requires exactly one {prefix} record')
    result = {}
    for match in re.finditer(r'(?:^| )([A-Za-z_]\w*)=(.*?)(?= [A-Za-z_]\w*=|$)', matches[0][len(prefix)+1:]):
        key, value = match.groups()
        if key in result:
            raise ValueError('Duplicate smoke field: ' + key)
        result[key] = value
    return result


def smoke_int(record, key):
    text = record.get(key, '')
    if not re.fullmatch(r'-?\d+', text):
        raise ValueError('Smoke status is unknown: ' + key)
    return int(text)


def smoke_runtime(transport, info):
    nos = any(line.startswith('NOS ') for line in info)
    wos = [re.fullmatch(r'app=(\d+)/(\d+) name=(.+)', line) for line in info]
    wos = [match for match in wos if match and 0 <= int(match[1]) < int(match[2])]
    if nos and not wos:
        if not transport.correlated or transport.capabilities.get('v') != '1':
            raise ValueError('NOS smoke requires its negotiated request protocol')
        return 'NOS'
    if len(wos) == 1 and not nos and not transport.correlated and not transport.capabilities:
        return 'WOS'
    raise ValueError('Cannot identify a supported NOS or legacy WOS runtime; no smoke mutation sent')


def smoke_identity(info):
    build = smoke_record(info, 'NOSBUILD')
    for key in ('source_sha256', 'elf_sha256'):
        if not re.fullmatch(r'[0-9a-f]{64}', build.get(key, '')) or build[key] == '0'*64:
            raise ValueError('Smoke requires the full device build identity: ' + key)
    if build.get('source_build_id') != build['source_sha256'][:16]:
        raise ValueError('Device source build ID conflicts with its full source hash')
    return {key: build[key] for key in ('source_sha256', 'source_build_id', 'elf_sha256')}


def smoke_boot(lines):
    boot = smoke_record(lines, 'NOSLOG').get('boot', '')
    if not re.fullmatch(r'[0-9a-f]{8}', boot) or boot == '00000000':
        raise ValueError('Smoke requires a known hexadecimal boot identity')
    return boot


def smoke_idle(runtime):
    if smoke_int(runtime, 'v') != 1:
        raise ValueError('Unsupported NOS runtime status version')
    for key in SMOKE_IDLE:
        if smoke_int(runtime, key) != 0:
            raise ValueError('Smoke refused active or unresolved owner: ' + key)
    state, save = smoke_int(runtime, 'workout_state'), smoke_int(runtime, 'workout_save')
    if (smoke_int(runtime, 'imu_state') != 0 or smoke_int(runtime, 'workout_ready') != 1 or
            state not in (0, 3) or save not in (0, 2) or (state == 3 and save != 2) or
            smoke_int(runtime, 'audio_status') not in (0, 3) or
            smoke_int(runtime, 'capture_state') not in (0, 3, 4, 5)):
        raise ValueError('Smoke refused active, failed or unknown recording/capture state')


def run_smoke(transport, run, timeout, info):
    runtime = smoke_runtime(transport, info)
    if runtime == 'WOS':
        # Legacy WOS has no authoritative owner/awake query. Diagnostics remain
        # useful without navigating away from an unobservable recorder.
        for command in ('board', 'imu', 'sd', 'apps', 'info'):
            run(command)
        return {'runtime': 'WOS', 'mode': 'read-only diagnostics', 'ok': True}
    identity = smoke_identity(info)
    original = smoke_record(run('state'), 'NOSSTATE')
    awake = smoke_int(original, 'awake')
    if awake not in (0, 1):
        raise ValueError('Unknown awake flag; smoke will not change it')
    prefs = smoke_record(run('prefs'), 'NOSPREFS')
    if (any(prefs.get(key) != '1' for key in ('ok', 'stored', 'match')) or
            not re.fullmatch(r'[0-9a-f]{8}', prefs.get('hash', '')) or prefs.get('stored_hash') != prefs['hash']):
        raise ValueError('Smoke requires resolved, matching authoritative preferences')
    boot = smoke_boot(run('diag log'))
    flags = {key: original[key] for key in SMOKE_FLAGS if key in original}
    navigated = False

    def status(settled=True):
        state = smoke_record(run('state'), 'NOSSTATE')
        smoke_idle(smoke_record(run('runtime_state'), 'NOSRUNTIME'))
        if smoke_boot(run('diag log')) != boot:
            raise ValueError('Device rebooted during smoke; cleanup navigation refused')
        if smoke_int(state, 'pwr') != 0 or smoke_int(state, 'locked') != 0:
            raise ValueError('Smoke requires an already awake, unlocked watch')
        if not state.get('top'):
            raise ValueError('Smoke navigation state is unknown')
        if any(state.get(key) != value for key, value in flags.items()):
            raise ValueError('Awake/preferences/render flags changed; smoke will not overwrite them')
        if settled and smoke_int(state, 'tx') != 0:
            raise ValueError('Smoke requires settled navigation')
        return state

    def settled(name):
        deadline = time.monotonic() + min(timeout, 5)
        while True:
            state = status(settled=False)
            if state.get('top') == name and smoke_int(state, 'tx') == 0:
                return state
            if time.monotonic() >= deadline:
                raise TimeoutError('Smoke navigation did not arrive at ' + name)
            time.sleep(0.1)

    def navigate(command, name):
        nonlocal navigated
        status()
        navigated = True
        run(command)  # Mutations execute once; completion is acceptance only.
        settled(name)

    def preserved(settled=True):
        if smoke_identity(run('info')) != identity:
            raise ValueError('Device build changed during smoke')
        if smoke_record(run('prefs'), 'NOSPREFS') != prefs:
            raise ValueError('Preferences changed during smoke; original data was not rewritten')
        status(settled=settled)

    failure = None
    try:
        status()
        for command in ('sd', 'imu', 'sports', 'nav_state'):
            run(command)
        navigate('home', 'HOME')
        for name, top in SMOKE_APPS:
            navigate('open ' + name, top)
            navigate('home', 'HOME')
        preserved()
    except (OSError, TimeoutError, ValueError) as exc:
        failure = exc
        raise
    finally:
        if navigated:
            try:
                if failure is not None or transport.active is not None:
                    transport.resynchronize(timeout=min(timeout, 8))
                # A fresh build/boot/owner snapshot authorizes compensation;
                # an uncertain open/home is never sent again blindly.
                preserved(settled=False)
                state = status(settled=False)
                if state.get('top') != 'HOME' or smoke_int(state, 'tx'):
                    settled(state.get('top'))
                    state = status()
                    if state.get('top') != 'HOME':
                        run('home')
                        settled('HOME')
                preserved()
            except (OSError, TimeoutError, ValueError) as exc:
                raise ValueError('Smoke cleanup could not be verified: ' + str(exc)) from exc
    return {'runtime': 'NOS', 'ok': True, 'identity': identity, 'boot': boot,
            'awake_preserved': awake, 'preferences_preserved': prefs['hash'], 'home_verified': True}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--list", action="store_true", help="List available serial ports")
    ap.add_argument("--sync-time", action="store_true", help="Set local clock and timezone from this PC")
    ap.add_argument("--wifi-ssid", help="Join/save WPA Wi-Fi, including SSIDs with spaces")
    ap.add_argument("--smoke", action="store_true", help="Runtime-aware diagnostics and safe named-app smoke")
    ap.add_argument("--script", type=Path, help="Text file, one command per line; # lines skipped")
    ap.add_argument("--timeout", type=float, default=15)
    ap.add_argument("--log", type=Path, help="Append timestamped command/results as JSONL")
    ap.add_argument("--wire-log", type=Path, help="Append redacted transport diagnostics as JSONL")
    ap.add_argument("command", nargs="*")
    args = ap.parse_args(argv)
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        ap.error("Install pyserial: python -m pip install pyserial")
    if args.list:
        for item in list_ports.comports():
            print(f"{item.device}: {item.description}")
        return 0
    if not args.port:
        ap.error("Specify --port COMx (use --list to discover it)")
    if args.timeout <= 0:
        ap.error("--timeout must be positive")
    commands = []
    if args.script:
        commands += [line.strip() for line in args.script.read_text(encoding="utf-8").splitlines()
                     if line.strip() and not line.lstrip().startswith("#")]
    if args.command:
        commands.append(" ".join(args.command))
    if not commands and not (args.smoke or args.sync_time or args.wifi_ssid is not None):
        commands = ["help"]
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
    if args.wire_log:
        args.wire_log.parent.mkdir(parents=True, exist_ok=True)
    def trace(record):
        if args.wire_log:
            with args.wire_log.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(record) + "\n")
    # Set DTR/RTS before opening: keep native USB from intentionally resetting
    # the board. Another serial monitor must release the port first.
    port = serial.Serial(port=None, baudrate=115200, timeout=0.15, write_timeout=2)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        port.open()
        time.sleep(0.25)
        port.reset_input_buffer()
        transport = HostTransport(SerialLines(port), trace=trace)
        transport.negotiate()
        failed = False
        def run(command):
            nonlocal failed
            printable = redact(command)
            print(f"> {printable}", flush=True)
            for attempt in range(2):
                try:
                    lines = execute(transport, command, args.timeout)
                    break
                except (OSError, TimeoutError, ValueError) as exc:
                    if args.log:
                        with args.log.open('a', encoding='utf-8') as stream:
                            stream.write(json.dumps(result_record(transport, command, error=exc)) + '\n')
                    if attempt == 0 and command in READONLY and isinstance(exc, (CommandRejected, CommandTimeout)):
                        transport.resynchronize(timeout=min(args.timeout, 8))
                        continue
                    raise
            for line in lines:
                print(line)
            failed |= any(line.startswith(("ERR ", "? ")) for line in lines)
            if args.log:
                record = result_record(transport, command, lines)
                with args.log.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(record) + "\n")
            return lines
        info = None
        if args.smoke or args.sync_time or args.wifi_ssid is not None:
            info = run('info')
            runtime = smoke_runtime(transport, info)
            if runtime == 'NOS' and args.sync_time:
                raise ValueError('--sync-time uses legacy WOS commands; use NOS phone/Gadgetbridge time sync')
            if runtime == 'NOS' and args.wifi_ssid is not None:
                raise ValueError('--wifi-ssid uses legacy WOS commands; configure Wi-Fi through NOS Settings/setup')
        if args.wifi_ssid is not None:
            password = os.environ.get('WATCH_WIFI_PASSWORD')
            if password is None:
                password = getpass.getpass('Wi-Fi password: ')
            if not 1 <= len(args.wifi_ssid.encode('utf-8')) <= 32 or not 8 <= len(password.encode('utf-8')) <= 63:
                raise ValueError('SSID must be1..32 bytes and WPA password8..63 bytes')
            run('wifi ' + json.dumps({'ssid': args.wifi_ssid, 'password': password}, ensure_ascii=False, separators=(',', ':')))
        if args.sync_time:
            now = datetime.now().astimezone()
            for command in (f'tz {int(now.utcoffset().total_seconds()//60)}', now.strftime('settime %Y %m %d %H %M %S'), 'time'):
                run(command)
        if args.smoke:
            print(json.dumps({'smoke': run_smoke(transport, run, args.timeout, info)}), flush=True)
        for command in commands:
            run(command)
        return 1 if failed else 0
    except (serial.SerialException, OSError, TimeoutError, ValueError) as exc:
        print(f"watchctl: {exc}", file=sys.stderr)
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    raise SystemExit(main())
