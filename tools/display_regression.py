"""Exercise app/face transitions after a display change; physical QA is separate."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import time

import serial
from serial.tools import list_ports
from display_perf import verify_identity, WATCH_SERIAL
from watchctl import execute


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    verify_identity(args.port, WATCH_SERIAL, list_ports.comports())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=.15, write_timeout=2)
    port.dtr = port.rts = False
    port.port = args.port
    port.open()
    original_face = None
    with args.output.open("a", encoding="utf-8") as output:
        def run(command, delay=.15):
            start = time.monotonic()
            lines = execute(port, command, 15)
            output.write(json.dumps({"time": datetime.now().astimezone().isoformat(),
                                     "command": command, "response": lines,
                                     "ack_ms": (time.monotonic()-start)*1000})+"\n")
            output.flush()
            if any(line.startswith(("ERR ", "? ")) for line in lines):
                raise RuntimeError((command, lines))
            time.sleep(delay)
            return "\n".join(lines)

        def wait_app(index):
            deadline = time.monotonic()+8
            while time.monotonic() < deadline:
                if re.search(r"\bapp="+str(index)+r"/", run("info")):
                    return
            raise RuntimeError(f"App {index} did not finish its transition")

        try:
            time.sleep(.25)
            port.reset_input_buffer()
            original_face = int(re.search(r"face=(\d+)", run("face")).group(1))
            run("awake 1")
            run("board")
            run("perf display info")
            app_ids = [int(x) for x in re.findall(r"^APP (\d+) ", run("apps"), re.M)]
            if not app_ids:
                raise RuntimeError("No app inventory returned")
            for index in app_ids:
                run(f"open {index}", .35)
                wait_app(index)
            for face in range(13):
                run(f"face {face}", .6)
                wait_app(0)
                if f"face={face}" not in run("face"):
                    raise RuntimeError(f"Face {face} selection failed")
            for edge in ("left", "right", "top", "bottom"):
                run(edge, .6)
                run("close", .6)
            run("home", .6)
            run("swipe 310 275 130 275", .6)
            run("home", .6)
            run("imu")
            run("bleinfo")
            run("sd")
            run("bri")
            run("screen")
            run("perf display info")
            print(f"PASS: {len(app_ids)} apps, 13 faces, four overlays and USB gesture; physical QA unverified")
        finally:
            try:
                if original_face is not None:
                    run(f"face {original_face}")
                run("home")
                run("awake 0")
            finally:
                port.close()


if __name__ == "__main__":
    main()
