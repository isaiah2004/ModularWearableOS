"""Capture display performance records from the watch over native USB serial.

Examples:
  python tools/display_perf.py --port COM3 --output build/display.jsonl --command "perf display info"
  python tools/display_perf.py --port COM3 --output build/display.jsonl --script perf.txt
  python tools/display_perf.py --port COM3 --output build/legacy.jsonl --legacy-apps --seconds 60

The output is JSONL with one timestamped record for every transmitted command
and every raw line received.  PERF_* JSON payloads are also decoded into a
``perf`` member, without discarding the original line.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import sys
import time


WATCH_SERIAL = "28:84:85:b0:8b:30"
PERF_KINDS = {
    "PERF_INFO", "PERF_RUN", "PERF_APP", "PERF_STATUS",
    "PERF_CONFIG", "PERF_STARTED", "PERF_SWEEP", "PERF_META",
    "PANEL_PROBE",
}


def parse_perf_line(line):
    """Return (record kind, JSON object) for a PERF_* line, else None."""
    match = re.fullmatch(r"\s*([A-Z][A-Z0-9_]*)(?:\s+|\s*,\s*)(.+?)\s*", line)
    if not match or match.group(1) not in PERF_KINDS:
        return None
    try:
        payload = json.loads(match.group(2))
    except json.JSONDecodeError as exc:
        raise ValueError(f"Malformed {match.group(1)} JSON: {exc.msg}") from exc
    if not isinstance(payload, dict):
        raise ValueError(f"{match.group(1)} payload must be a JSON object")
    return match.group(1), payload


def expected_record(command):
    """Map a performance command to the asynchronous record it promises."""
    words = command.lower().split()
    if len(words) < 3 or words[:2] != ["perf", "display"]:
        return None
    operation = words[2]
    if operation in {"fill", "rect", "sweep"}:
        return "PERF_RUN"
    return {"info": "PERF_INFO", "app": "PERF_APP",
            "status": "PERF_STATUS"}.get(operation)


def read_script(path):
    return [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")]


def app_seconds(command, fallback):
    """Best-effort extraction used only to size the bounded PERF_APP wait."""
    if expected_record(command) != "PERF_APP":
        return 0.0
    numbers = []
    for word in command.split()[3:]:
        try:
            numbers.append(float(word))
        except ValueError:
            pass
    return max(numbers[-1], 0.0) if numbers else fallback


class Capture:
    def __init__(self, port, output, monotonic=time.monotonic):
        self.port = port
        self.output = output
        self.monotonic = monotonic

    def _write(self, direction, line, **extra):
        record = {"time": datetime.now().astimezone().isoformat(),
                  "direction": direction, "line": line}
        record.update(extra)
        self.output.write(json.dumps(record, separators=(",", ":")) + "\n")
        self.output.flush()

    def receive(self):
        raw = self.port.readline()
        if not raw:
            return None, None
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        extra = {}
        parsed = parse_perf_line(line)
        if parsed:
            extra = {"record": parsed[0], "perf": parsed[1]}
        self._write("rx", line, **extra)
        print(line, flush=True)
        return line, parsed

    def command(self, command, timeout, async_timeout=0.0):
        encoded = command.encode("utf-8")
        if not encoded or len(encoded) > 191 or b"\r" in encoded or b"\n" in encoded:
            raise ValueError("Use one command of 1..191 UTF-8 bytes")
        self._write("tx", command)
        print("> " + command, flush=True)
        self.port.write(encoded + b"\n")
        self.port.flush()
        expected = expected_record(command)
        seen = set()
        errors = []
        done = False
        deadline = self.monotonic() + timeout
        # A sweep consists of multiple complete runs. Progress extends the
        # per-run timeout; a stalled device still expires after `timeout`.
        sweep = command.lower().split() == ["perf", "display", "sweep"]
        while self.monotonic() < deadline:
            line, parsed = self.receive()
            if line is None:
                continue
            if line == "DONE":
                done = True
                break
            if line.startswith(("ERR ", "? ")):
                errors.append(line)
            if parsed:
                seen.add(parsed[0])
                if sweep and parsed[0] == "PERF_RUN":
                    deadline = self.monotonic() + timeout
        if not done:
            raise TimeoutError(f"No DONE for {command!r} within {timeout:g}s")
        if errors:
            raise RuntimeError(f"{command!r} failed: {'; '.join(errors)}")

        # PERF_APP acknowledges immediately, then reports after its sampling
        # interval. Other perf commands normally emit their record before DONE,
        # but tolerate a short scheduling delay as well.
        if expected and expected not in seen:
            wait = async_timeout if expected == "PERF_APP" else min(timeout, 2.0)
            record_deadline = self.monotonic() + wait
            while self.monotonic() < record_deadline:
                line, parsed = self.receive()
                if line and line.startswith(("ERR ", "? ")):
                    raise RuntimeError(f"{command!r} failed asynchronously: {line}")
                if parsed:
                    seen.add(parsed[0])
                    if expected in seen:
                        break
        if expected and expected not in seen:
            raise TimeoutError(f"No {expected} record for {command!r}")

    def observe(self, seconds):
        deadline = self.monotonic() + seconds
        while self.monotonic() < deadline:
            self.receive()


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--command", action="append", default=[], help="Command to run; repeat as needed")
    parser.add_argument("--script", type=Path, help="One command per line; blank and # lines ignored")
    parser.add_argument("--timeout", type=float, default=120, help="Maximum seconds to wait for each DONE")
    parser.add_argument("--seconds", type=float, default=60, help="PERF_APP fallback or each legacy capture duration")
    parser.add_argument("--legacy-apps", action="store_true", help="Profile face 0, level (app 17), and breathe (app 8)")
    identity = parser.add_mutually_exclusive_group()
    identity.add_argument("--device-serial", default=WATCH_SERIAL, help="Required USB serial number")
    identity.add_argument("--allow-any-device", action="store_true", help="Explicitly bypass USB identity verification")
    return parser


def verify_identity(port_name, expected, ports):
    matching = [item for item in ports if item.device.lower() == port_name.lower()]
    if not matching:
        raise RuntimeError(f"Serial port {port_name} was not found")
    actual = (matching[0].serial_number or "").lower()
    if actual != expected.lower():
        raise RuntimeError(f"Refusing {port_name}: USB serial {actual or '<missing>'} does not match {expected}")


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.timeout <= 0 or args.seconds <= 0:
        parser.error("--timeout and --seconds must be positive")
    commands = list(args.command)
    if args.script:
        commands.extend(read_script(args.script))
    if not commands and not args.legacy_apps:
        parser.error("provide --command, --script, or --legacy-apps")
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        parser.error("Install pyserial: python -m pip install pyserial")
    try:
        if not args.allow_any_device:
            verify_identity(args.port, args.device_serial, list_ports.comports())
        port = serial.Serial(port=None, baudrate=115200, timeout=0.15, write_timeout=2)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
    except (serial.SerialException, OSError, RuntimeError) as exc:
        print(f"display_perf: {exc}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    failed = None
    with args.output.open("a", encoding="utf-8") as output:
        capture = Capture(port, output)
        try:
            time.sleep(0.25)
            port.reset_input_buffer()
            if args.legacy_apps:
                capture.command("awake 1", args.timeout)
                capture.command("prof 1", args.timeout)
                for setup in ("face 0", "open 17", "open 8"):
                    capture.command(setup, args.timeout)
                    capture.observe(args.seconds)
            for command in commands:
                async_wait = app_seconds(command, args.seconds) + 5.0
                capture.command(command, args.timeout, async_wait)
        except (OSError, serial.SerialException, TimeoutError, ValueError, RuntimeError) as exc:
            failed = exc
            print(f"display_perf: {exc}", file=sys.stderr)
        finally:
            # These are runtime-only controls. Attempt every restoration even if
            # the workload failed; record all responses in the same raw log.
            for command in ("prof 0", "awake 0", "resume", "home"):
                try:
                    capture.command(command, min(args.timeout, 15.0))
                except Exception as exc:
                    if failed is None:
                        failed = exc
                    print(f"display_perf: cleanup {command!r}: {exc}", file=sys.stderr)
            port.close()
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
