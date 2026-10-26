"""Explicit hardware QA: changes watch UI/DND, joins its AP, and resets it.
Requires --port and the full expected NOSBUILD ELF SHA-256. --dry-run uses no
serial/network/reset dependencies or output directories. Use ESP-IDF Python.
"""
import json
import base64
import struct
import pathlib
import re
import subprocess
import time
import urllib.error
import urllib.request
from xml.sax.saxutils import escape
import argparse
import sys
import uuid
import zlib

if __package__:
    from .host_transport import HostTransport, SerialLines
else:
    from host_transport import HostTransport, SerialLines

ROOT = pathlib.Path(__file__).resolve().parents[1]

def decode_checkpoint(data):
    if len(data) != 276:
        raise ValueError('Unexpected SD preference checkpoint length')
    header = struct.unpack_from('<4I', data)
    checksum = 2166136261
    for value in data[:272]: checksum = ((checksum ^ value) * 16777619) & 0xffffffff
    if header[0] != 0x4e4f5350 or header[1] != 1 or not 12 <= header[3] <= 256 or checksum != struct.unpack_from('<I', data, 272)[0]:
        raise ValueError('Invalid SD preference header/checksum')
    return {'header': header, 'dnd': data[27]}

def newest_checkpoint(slots):
    if not slots: raise ValueError('No preference checkpoints')
    newest = slots[0]
    for slot in slots[1:]:
        delta = (slot['header'][2] - newest['header'][2]) & 0xffffffff
        if 0 < delta < 0x80000000: newest = slot
    return newest

def run(port_name, expected_elf_sha256, output):
    import serial
    from esptool.reset import HardReset
    OUT = pathlib.Path(output)
    OUT.mkdir(parents=True, exist_ok=True)
    port = serial.Serial()
    port.port = port_name
    port.baudrate = 115200
    port.timeout = .1
    port.dtr = port.rts = False
    port.open()
    transport = HostTransport(SerialLines(port))
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


    def read(seconds):
        end = time.monotonic() + seconds
        data = bytearray()
        while time.monotonic() < end:
            data.extend(port.read(8192))
        return data.decode("utf-8", "replace")


    def command(text, settle=.1):
        lines = transport.execute(text, timeout=10)
        time.sleep(settle)
        return "\n".join(lines)


    def prefs():
        line = next(x for x in command("prefs").splitlines() if x.startswith("NOSPREFS "))
        return dict(x.split("=", 1) for x in line.split()[1:])

    def slots(stage):
        result=[]
        for slot in range(2):
            raw=command(f"sdget /watch/settings.{slot}.bin")
            (OUT/f"{stage}-slot{slot}.log").write_text(raw,encoding="utf-8")
            data=bytearray()
            for line in raw.splitlines():
                if line.startswith("SDG "): data.extend(base64.b64decode(line[4:], validate=True))
            header = next(line.split() for line in raw.splitlines() if line.startswith('SDGET ') and line != 'SDGET END')
            if int(header[1]) != len(data) or int(header[2],16) != zlib.crc32(data) & 0xffffffff or 'SDGET END' not in raw.splitlines():
                raise ValueError('Unverified SD checkpoint download')
            (OUT/f"{stage}-settings.{slot}.bin").write_bytes(data)
            result.append(decode_checkpoint(data))
        return result

    def wlan(*args):
        return subprocess.run(["netsh", "wlan", *args], capture_output=True, text=True, check=True).stdout


    def request(path, body=None):
        req = urllib.request.Request("http://192.168.4.1" + path,
                                     data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Content-Type": "application/json"} if body is not None else {})
        with opener.open(req, timeout=5) as reply:
            return reply.read()


    mutated = False
    original = None
    original_profile = None
    profile_added = False
    temporary = "NOS-Setup-QA-" + uuid.uuid4().hex[:10]
    xml_path = OUT / "temporary-wlan.xml"
    report = {}
    try:
        transport.negotiate()
        info = command("info")
        identities = re.findall(r"elf_sha256=([0-9a-fA-F]{64})(?:\s|$)", info)
        if not identities or identities[-1].lower() != expected_elf_sha256.lower():
            raise RuntimeError("Device ELF identity mismatch; no QA mutations were started")
        report["verified_elf_sha256"] = identities[-1].lower()
        original = prefs()
        assert original["match"] == "1"
        report["initial_preferences"] = original
        interfaces = wlan("show", "interfaces")
        profile_match = re.search(r"^\s*Profile\s*:\s*(.+)$", interfaces, re.M)
        original_profile = profile_match.group(1).strip() if profile_match else None
        mutated = True
        command("network wifi", .8)
        command("tap 233 225", .8)
        command("tap 233 380", 1)
        info = command("network setup")
        match = re.search(r"SETUP active=1 ssid=(\S+) password=(\S+)", info)
        assert match, "Setup hotspot did not start"
        ssid, password = match.groups()
        xml_path.write_text(
            '<?xml version="1.0"?><WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">'
            f"<name>{temporary}</name><SSIDConfig><SSID><name>{escape(ssid)}</name></SSID></SSIDConfig>"
            "<connectionType>ESS</connectionType><connectionMode>manual</connectionMode>"
            "<MSM><security><authEncryption><authentication>WPA2PSK</authentication>"
            "<encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
            "<sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
            f"<keyMaterial>{escape(password)}</keyMaterial></sharedKey></security></MSM></WLANProfile>",
            encoding="utf-8")
        wlan("add", "profile", f"filename={xml_path}", "user=current")
        profile_added = True
        wlan("connect", f"name={temporary}", f"ssid={ssid}")
        for _ in range(12):
            time.sleep(1)
            if re.search(r"^\s*SSID\s*:\s*" + re.escape(ssid) + r"\s*$", wlan("show", "interfaces"), re.M):
                break
        else:
            raise RuntimeError("PC could not join the watch hotspot")
        page = request("/").decode()
        assert "Your watch." in page
        (OUT / "served-page.html").write_text(page, encoding="utf-8")
        before = json.loads(request("/api/status"))
        report["portal_status_before"] = before
        assert before["brightness"] == int(original["brightness"])
        scan = json.loads(request("/api/networks"))
        time.sleep(4)
        scan = json.loads(request("/api/networks"))
        report["wifi_network_count"] = len(scan["networks"])
        assert scan["networks"], "Watch scan has no visible APs"
        for invalid in ({"brightness": 101, "auto_bright": False, "dnd": False, "aod": False},
                        {"brightness": 30.5, "auto_bright": False, "dnd": False, "aod": False}):
            try:
                request("/api/settings", invalid)
                raise AssertionError("Invalid settings accepted")
            except urllib.error.HTTPError as exc:
                assert exc.code == 400
        patch = {key: before[key] for key in ("brightness", "auto_bright", "dnd", "aod")}
        for key in ("auto_bright", "dnd", "aod"):
            patch[key] = bool(patch[key])
        patch["dnd"] = not patch["dnd"]
        assert json.loads(request("/api/settings", patch))["queued"]
        for _ in range(15):
            time.sleep(1)
            status=json.loads(request("/api/status"))
            if not status["settings_pending"] and status["dnd"]==patch["dnd"]: break
        else: raise AssertionError("Remote setting never reached SD checkpoint")
        changed = prefs()
        report["changed_preferences"] = changed
        assert changed["dnd"] != original["dnd"] and changed["match"] == "1"
        report["remote_setting_saved"] = True
        try:
            request("/wifi", {"ssid": "NOS test", "pass": "short"})
            raise AssertionError("Short Wi-Fi password accepted")
        except urllib.error.HTTPError as exc:
            assert exc.code == 400
        # A nonexistent test SSID cannot overwrite the last successful saved network.
        assert json.loads(request("/wifi", {"ssid": "NOS QA + %", "pass": 'space + & % " test'}))["accepted"]
        time.sleep(3)
        assert json.loads(request("/api/status"))["dnd"] == patch["dnd"]
        report["portal_survives_sta_join"] = True
        report["pre_reboot_preferences"] = prefs()
        report["slots_before_reboot"] = slots("before")
        (OUT / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("PASS hotspot association, HTTP page, real scan, input validation and remote SD save", flush=True)
    finally:
        primary_error = sys.exc_info()[1]
        cleanup_errors = []
        if profile_added:
            for arguments in (("connect", f"name={original_profile}") if original_profile else ("disconnect",),
                              ("delete", "profile", f"name={temporary}")):
                try: wlan(*arguments)
                except Exception as exc: cleanup_errors.append('WLAN restoration: ' + str(exc))
        if xml_path.exists():
            try: xml_path.unlink()
            except OSError as exc: cleanup_errors.append('Temporary WLAN file cleanup: ' + str(exc))
        if original and mutated:
            try:
                HardReset(port, uses_usb=True)()
                boot = read(14)
                (OUT / "reboot.log").write_text(boot, encoding="utf-8")
                rebooted = prefs()
                report["slots_after_reboot"] = slots("after")
                report["rebooted_preferences"] = rebooted
                if report.get("remote_setting_saved"):
                    report["remote_setting_survived_reboot"] = rebooted["dnd"] != original["dnd"] and rebooted["match"] == "1"
                if rebooted["dnd"] != original["dnd"]:
                    command("open control", 1)
                    command("tap 329 124", 3)
                command("home")
                command("prefs flush")
                restored = prefs()
                assert restored["hash"] == original["hash"] and restored["match"] == "1"
                report["original_preferences_restored"] = True
                print("PASS original watch settings restored" +
                      ("; remote setting survived reboot" if report.get("remote_setting_survived_reboot") else ""), flush=True)
            except Exception as exc:
                cleanup_errors.append('Watch restoration: ' + str(exc))
            finally:
                port.close()
        else:
            port.close()
        report['cleanup_errors'] = cleanup_errors
        (OUT / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if cleanup_errors:
            if primary_error: primary_error.add_note('; '.join(cleanup_errors))
            else: raise RuntimeError('; '.join(cleanup_errors))
    assert report.get("remote_setting_survived_reboot"), "Remote setting did not survive reboot"
    for name in ("slots_before_reboot","slots_after_reboot"):
        assert all("header" in slot for slot in report[name]), "SD read failed during setup/reboot"
        newest=newest_checkpoint(report[name])
        assert newest["dnd"]==int(report["changed_preferences"]["dnd"]), "SD checkpoint did not retain remote change"
    print("PASS both SD slots readable; newest SD checkpoint retains remote setting",flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-elf-sha256", required=True,
                        help="Full 64-digit ELF hash from the intended device/build")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "build/connectivity-validation")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true", help="Describe mutations without device or filesystem I/O")
    mode.add_argument("--run", action="store_true", help="Run UI/settings/WLAN mutations, reboot, and restoration")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.expected_elf_sha256):
        parser.error("--expected-elf-sha256 must be the full 64-digit hash")
    if args.dry_run:
        print(json.dumps({"profile": "hardware-only", "port": args.port,
                          "expected_elf_sha256": args.expected_elf_sha256,
                          "mutations": ["watch setup UI", "DND toggle and SD checkpoint", "temporary WLAN profile",
                                        "WLAN association", "reboot", "restore original watch preferences/PC network"],
                          "output": str(args.out), "device_io": False}))
        return 0
    run(args.port, args.expected_elf_sha256, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
