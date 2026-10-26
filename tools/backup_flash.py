"""Read and verify an ESP32-S3 flash backup in bounded chunks (esptool 4.x).

Run with the ESP-IDF Python environment. No flash contents are modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import struct
from esptool.targets.esp32s3 import ESP32S3ROM


def read_flash_bounded(esp, offset, length, max_inflight, block_size):
    # esptool's default is 64 in-flight packets. Native USB on this host can
    # overflow its receive buffering; one packet per acknowledgement is slower
    # but retains the same device-supplied end-to-end MD5 verification.
    esp.check_command("read flash", esp.ESP_READ_FLASH,
                      struct.pack("<IIII", offset, length, block_size, max_inflight))
    chunks = []
    received = 0
    while received < length:
        packet = esp.read()
        expected = min(block_size, length - received)
        if len(packet) != expected:
            raise RuntimeError(f"Short flash packet: expected {expected}, received {len(packet)}")
        chunks.append(packet); received += len(packet)
        esp.write(struct.pack("<I", received))
    data = b"".join(chunks)
    digest = esp.read()
    if len(digest) != 16 or digest != hashlib.md5(data).digest():
        raise RuntimeError("Device/host MD5 mismatch during flash backup")
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--hold-rom", action="store_true", help="Keep the verified device in ROM after reading, for an immediate checked migration")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=0x1000000)
    ap.add_argument("--max-inflight", type=int, default=1, help="Native USB receive window (1 is conservative)")
    ap.add_argument("--block-size", type=int, default=256, help="Stub read packet bytes (native USB-safe default)")
    ap.add_argument("--chunk-size", type=lambda x: int(x, 0), default=0x10000, help="Independently MD5-verified read size")
    args = ap.parse_args()
    if args.block_size <= 0 or args.chunk_size <= 0 or args.size % args.chunk_size or args.chunk_size % args.block_size:
        ap.error("--chunk-size must divide flash size and be a multiple of block size")
    if args.block_size not in (256, 512, 1024, 2048, 4096):
        ap.error("--block-size must be 256/512/1024/2048/4096")
    if not 1 <= args.max_inflight <= 64:
        ap.error("--max-inflight must be 1..64")
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
        for offset in range(0, args.size, args.chunk_size):
            for attempt in range(5):
                try:
                    if esp is None:
                        esp = ESP32S3ROM(args.port, baud=115200)
                        esp.connect()
                        # Windows USB serial defaults can be smaller than the
                        # stub stream. SetupComm is only a buffering hint.
                        try:
                            esp._port.set_buffer_size(rx_size=1024 * 1024, tx_size=65536)
                        except (AttributeError, OSError, ValueError):
                            pass
                        found = ":".join(f"{b:02x}" for b in esp.read_mac())
                        if mac is not None and found != mac:
                            raise RuntimeError("Different device connected during backup")
                        mac = found
                        esp = esp.run_stub()
                        esp.change_baud(460800)
                        esp.flash_spi_attach(0)
                        esp.flash_set_parameters(args.size)
                    esp._port.timeout = 3 # API constructor otherwise leaves unbounded read timeout
                    # read_flash validates each transfer's device-supplied MD5.
                    data = read_flash_bounded(esp, offset, args.chunk_size, args.max_inflight, args.block_size)
                    part = parts / f"{offset:08x}.bin"
                    part.write_bytes(data)
                    records.append({"offset": offset, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
                    if (offset + len(data)) % 0x100000 == 0:
                        print(f"Verified {(offset + len(data)) // 0x100000}/{args.size // 0x100000} MiB", flush=True)
                    break
                except Exception as exc:
                    if esp is not None:
                        esp._port.close()
                        esp = None
                    if attempt == 4:
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
            "chunks": records, "transport": "esptool stub MD5-verified reads", "max_inflight": args.max_inflight, "block_size": args.block_size, "chunk_size": args.chunk_size,
        }, indent=2), encoding="utf-8")
        print(f"Backup complete: {args.output}\nSHA256 {digest}", flush=True)
    finally:
        if esp is not None:
            try:
                if not args.hold_rom: esp.hard_reset()
            finally:
                esp._port.close()


if __name__ == "__main__":
    main()
