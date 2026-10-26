"""gb_rx_check.py - replay what the watch sent on NUS TX through Gadgetbridge's Bangle.js
receiver and fail on anything that would toast "Malformed JSON from Bangle.js".

  NOS_PHONE_TX=build/gb_tx.bin build/nos_sim.exe --script tools/gb_tx.txt
  python tools/gb_rx_check.py build/gb_tx.bin [--mtu 23] [--bare-lf]

Mirrors BangleJSDeviceSupport (codeberg.org/Freeyourgadget/Gadgetbridge):
  onCharacteristicChanged: drop XON(17)/XOFF(19), decode ISO-8859-1, receivedLine += chunk,
      while '\\n' in it: p = indexOf('\\n'); line = substring(0, p>0 ? p-1 : 0)   <- drops the
      byte BEFORE '\\n' (Espruino ends lines with \\r\\n)
  handleUartRxLine: "" ignored; '{' -> new JSONObject(line) + handleUartRxJSON, whose getInt /
      getBoolean / getString JSONExceptions land in the same "Malformed JSON" catch;
      anything else is logged and ignored.
--bare-lf re-frames every line with a bare "\\n" (the old firmware) to show the failure.
"""
import argparse
import json
import sys

MUSIC = {"PLAY", "PAUSE", "PLAYPAUSE", "NEXT", "PREVIOUS", "VOLUMEUP", "VOLUMEDOWN", "FORWARD", "REWIND"}
CALL = {"UNKNOWN", "ACCEPT", "END", "INCOMING", "OUTGOING", "REJECT", "START", "IGNORE"}
NOTIFY = {"DISMISS", "DISMISS_ALL", "OPEN", "MUTE", "REPLY"}


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def check_types(o):
    """The getters handleUartRxJSON uses; returns an error string or None."""
    t = o.get("t")
    if not isinstance(t, str):
        return 'missing "t"'
    if t in ("info", "warn", "error") and not isinstance(o.get("msg"), str):
        return "msg must be a string"
    if t == "ver":
        for k in ("fw", "hw"):
            if k in o and not isinstance(o[k], str):
                return k + " must be a string"
    if t == "status":
        for k in ("bat", "chg"):
            if k in o and not is_int(o[k]):
                return k + " must be an int (getInt)"
        if "volt" in o and (isinstance(o["volt"], bool) or not isinstance(o["volt"], (int, float))):
            return "volt must be a number"
    if t == "findPhone" and "n" in o and not isinstance(o["n"], bool):
        return "n must be a boolean (getBoolean)"
    if t == "music" and str(o.get("n", "")).upper() not in MUSIC:
        return "music n not a MusicControl event"
    if t == "call" and str(o.get("n", "")).upper() not in CALL:
        return "call n not a CallControl event"
    if t == "notify":
        if str(o.get("n", "")).upper() not in NOTIFY:
            return "notify n not a NotificationControl event"
        if "id" in o and not is_int(o["id"]):
            return "notify id must be an int"
    if t == "act":
        for k in ("hrm", "stp", "mov", "rt", "ts"):
            if k in o and not is_int(o[k]):
                return k + " must be an int"
    if t == "gps_power" and not isinstance(o.get("status"), bool):
        return "status must be a boolean"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--mtu", type=int, default=23, help="ATT MTU; notifications carry MTU-3 bytes")
    ap.add_argument("--bare-lf", action="store_true", help="re-frame lines with a bare \\n (old firmware)")
    a = ap.parse_args()
    raw = open(a.capture, "rb").read()
    if a.bare_lf:
        raw = raw.replace(b"\r\n", b"\n")
    bad = 0
    for i, b in enumerate(raw):
        if b == 0 or b > 0x7E or (b < 0x20 and b not in (0x0A, 0x0D)):
            print("byte %d: 0x%02x (non-printable / non-ASCII on the wire)" % (i, b))
            bad += 1
    chunk = max(20, min(180, a.mtu - 3))
    received, ok, types = "", 0, {}
    for off in range(0, len(raw), chunk):
        part = bytes(c for c in raw[off:off + chunk] if c not in (17, 19))
        received += part.decode("iso-8859-1")
        while "\n" in received:
            p = received.index("\n")
            line = received[:p - 1] if p > 0 else ""
            received = received[p + 1:]
            if not line:
                continue
            if line[0] != "{":
                print("ignored by GB (not JSON): %r" % line)
                continue
            try:
                o = json.loads(line)
                if not isinstance(o, dict):
                    raise ValueError("not an object")
                err = check_types(o)
                if err:
                    raise ValueError(err)
            except ValueError as e:
                print("MALFORMED: %r -> %s" % (line, e))
                bad += 1
                continue
            ok += 1
            types[o["t"]] = types.get(o["t"], 0) + 1
    if received:
        print("unterminated tail: %r" % received)
        bad += 1
    print("%d lines ok, %d bad; types %s" % (ok, bad, ", ".join("%s=%d" % kv for kv in sorted(types.items()))))
    return 1 if bad or not ok else 0


if __name__ == "__main__":
    sys.exit(main())
