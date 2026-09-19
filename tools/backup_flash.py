"""Read and verify an ESP32-S3 flash backup in bounded chunks (esptool 4.x).

Run with the ESP-IDF Python environment. No flash contents are modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
from esptool.targets.esp32s3 import ESP32S3ROM


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--size", type=lambda x: int(x, 0), default=0x1000000)
    args = ap.parse_args()
    if args.size <= 0 or args.size % 0x100000:
        ap.error("--size must be a positive multiple of 1 MiB")
    if args.output.exists():
        ap.error("Output already exists; choose a new backup filename")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    parts = args.output.with_suffix(".parts")
    parts.mkdir(exist_ok=False)
    esp = None
    mac = None
    records = []
    try:
        for offset in range(0, args.size, 0x100000):
            for attempt in range(3):
                try:
                    if esp is None:
                        esp = ESP32S3ROM(args.port, baud=115200)
                        esp.connect()
                        found = ":".join(f"{b:02x}" for b in esp.read_mac())
                        if mac is not None and found != mac:
                            raise RuntimeError("Different device connected during backup")
                        mac = found
                        esp = esp.run_stub()
                        esp.change_baud(460800)
                        esp.flash_spi_attach(0)
                        esp.flash_set_parameters(args.size)
                    # read_flash validates each transfer's device-supplied MD5.
                    data = esp.read_flash(offset, 0x100000)
                    part = parts / f"{offset:08x}.bin"
                    part.write_bytes(data)
                    records.append({"offset": offset, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
                    print(f"Verified {(offset + len(data)) // 0x100000}/{args.size // 0x100000} MiB", flush=True)
                    break
                except Exception as exc:
                    if esp is not None:
                        esp._port.close()
                        esp = None
                    if attempt == 2:
                        raise
                    print(f"Retry at 0x{offset:x}: {exc}", flush=True)
                    time.sleep(0.25)
        with args.output.open("xb") as output:
            for item in records:
                data = (parts / f"{item['offset']:08x}.bin").read_bytes()
                if hashlib.sha256(data).hexdigest() != item["sha256"]:
                    raise RuntimeError("Backup chunk changed on disk")
                output.write(data)
        digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
        args.output.with_suffix(".json").write_text(json.dumps({
            "mac": mac, "bytes": args.size, "sha256": digest,
            "chunks": records, "transport": "esptool MD5-verified reads",
        }, indent=2), encoding="utf-8")
        print(f"Backup complete: {args.output}\nSHA256 {digest}", flush=True)
    finally:
        if esp is not None:
            try:
                esp.hard_reset()
            finally:
                esp._port.close()


if __name__ == "__main__":
    main()
