"""Capture boot/runtime logs from the watch's native USB Serial/JTAG port.

python tools/usb_capture.py --port COM3 --reset --seconds 20 --output build/boot.log
The reset is optional and never erases or flashes the device.
"""
import argparse
from pathlib import Path
import sys
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.seconds
    port = None
    reset_done = False
    received = 0
    with args.output.open("wb") as log:
        try:
            while time.monotonic() < deadline:
                try:
                    if port is None:
                        port = serial.Serial(port=None, baudrate=115200, timeout=0.15)
                        port.dtr = False
                        port.rts = False
                        port.port = args.port
                        port.open()
                        if args.reset and not reset_done:
                            # Espressif's native USB HardReset sequence.
                            reset_done = True
                            port.rts = True
                            # usbser.sys needs a DTR update to transmit the new
                            # RTS state (same workaround as esptool ResetStrategy).
                            port.dtr = port.dtr
                            time.sleep(0.2)
                            port.rts = False
                            port.dtr = port.dtr
                            time.sleep(0.2)
                    data = port.read(4096)
                    if data:
                        received += len(data)
                        log.write(data)
                        log.flush()
                        print(data.decode("utf-8", errors="replace"), end="", flush=True)
                except (serial.SerialException, OSError):
                    if port is not None:
                        port.close()
                        port = None
                    time.sleep(0.25)  # Native USB can re-enumerate after a reset.
        finally:
            if port is not None:
                port.close()
    print(f"Captured {received} bytes to {args.output}", file=sys.stderr)
    return 0 if received else 1


if __name__ == "__main__":
    raise SystemExit(main())
