"""Control this watch over native USB serial; one command at a time.

Install: python -m pip install pyserial
Examples: python tools/watchctl.py --list
          python tools/watchctl.py --port COM7 board
          python tools/watchctl.py --port COM7 --sync-time
          python tools/watchctl.py --port COM7 --smoke --log build/usb.jsonl
"""
import argparse
from datetime import datetime
import json
import os
import getpass
from pathlib import Path
import sys
import time


def execute(port, command, timeout=10):
    encoded = command.encode("utf-8")
    if not encoded or len(encoded) > 191 or b"\n" in encoded or b"\r" in encoded:
        raise ValueError("Use one command of 1..191 UTF-8 bytes")
    port.write(encoded + b"\n")
    port.flush()
    deadline = time.monotonic() + timeout
    lines = []
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line == "DONE":
            return lines
        lines.append(line)
    raise TimeoutError(f"No DONE response within {timeout:g}s; check board firmware/USB connection")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--list", action="store_true", help="List available serial ports")
    ap.add_argument("--sync-time", action="store_true", help="Set local clock and timezone from this PC")
    ap.add_argument("--wifi-ssid", help="Join/save WPA Wi-Fi, including SSIDs with spaces")
    ap.add_argument("--smoke", action="store_true", help="Exercise board diagnostics and all four overlays")
    ap.add_argument("--script", type=Path, help="Text file, one command per line; # lines skipped")
    ap.add_argument("--timeout", type=float, default=15)
    ap.add_argument("--log", type=Path, help="Append timestamped command/results as JSONL")
    ap.add_argument("command", nargs="*")
    args = ap.parse_args()
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
    if args.wifi_ssid is not None:
        password = os.environ.get("WATCH_WIFI_PASSWORD")
        if password is None:
            password = getpass.getpass("Wi-Fi password: ")
        if not 1 <= len(args.wifi_ssid.encode("utf-8")) <= 32 or not 8 <= len(password.encode("utf-8")) <= 63:
            ap.error("SSID must be 1..32 bytes and WPA password 8..63 bytes")
        commands.append("wifi " + json.dumps({"ssid": args.wifi_ssid, "password": password}, ensure_ascii=False, separators=(",", ":")))
    if args.sync_time:
        now = datetime.now().astimezone()
        commands += [f"tz {int(now.utcoffset().total_seconds() // 60)}", now.strftime("settime %Y %m %d %H %M %S"), "time"]
    if args.smoke:
        commands += ["awake 1", "board", "imu", "sd", "apps", "home",
                     "left", "close", "right", "close", "top", "close", "bottom", "close",
                     "open 16", "open 17", "open 24", "home", "info", "awake 0"]
    if args.script:
        commands += [line.strip() for line in args.script.read_text(encoding="utf-8").splitlines()
                     if line.strip() and not line.lstrip().startswith("#")]
    if args.command:
        commands.append(" ".join(args.command))
    if not commands:
        commands = ["help"]
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
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
        failed = False
        for command in commands:
            printable = "wifi <redacted>" if command.startswith("wifi ") else command
            print(f"> {printable}", flush=True)
            lines = execute(port, command, args.timeout)
            for line in lines:
                print(line)
            failed |= any(line.startswith(("ERR ", "? ")) for line in lines)
            if args.log:
                record = {"time": datetime.now().astimezone().isoformat(), "command": printable, "response": lines}
                with args.log.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(record) + "\n")
            if args.smoke:
                time.sleep(0.4)  # allow UI spring transitions to settle
        return 1 if failed else 0
    except (serial.SerialException, OSError, TimeoutError, ValueError) as exc:
        print(f"watchctl: {exc}", file=sys.stderr)
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    raise SystemExit(main())
