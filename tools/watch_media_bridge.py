#!/usr/bin/env python3
"""
watch_media_bridge.py -- BLE music bridge between Windows and the watch (universal path;
works anywhere in range, no LAN/IP needed). Slower for art than WiFi, by design.

  PC -> watch  MEDIA_INFO (write)      : play state + position/duration + title/artist (~1 Hz)
  PC -> watch  MEDIA_COVER (write NR)  : album art in ~240-byte chunks: [offset:4 LE][data],
                                         then a [0xFFFFFFFF] commit. Art is iTunes high-res by
                                         title/artist, else the Windows SMTC thumbnail, RGB565.
  watch -> PC  MEDIA_CMD (notify)      : transport taps -> Windows System Media Transport Controls.

The watch's media chars are UNENCRYPTED / no-bond, so no pairing is needed and Windows won't
auto-reconnect in the background. Just have it advertising (reset it if a stale bond lingers).

Requires:  pip install bleak winsdk Pillow numpy
Run:       python -u watch_media_bridge.py
"""
import sys
sys.coinit_flags = 0        # COINIT_MULTITHREADED — must precede comtypes/pycaw so their COM
                            # apartment matches bleak + winsdk (WinRT needs MTA), else BLE scan dies.
import asyncio
import io
import json
import os
import struct
import time
import urllib.parse
import urllib.request

import numpy as np
from PIL import Image, ImageOps
from bleak import BleakScanner, BleakClient
from winsdk.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
)
from winsdk.windows.storage.streams import DataReader

try:                                            # host volume control (watch volume knob)
    from ctypes import cast, POINTER
    from comtypes import CLSCTX_ALL
    from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume
    _HAVE_PYCAW = True
except Exception:
    _HAVE_PYCAW = False

DEV_NAME   = "Watch"
TIME_UUID  = "e3f00002-1212-efde-1523-785feabcd123"
INFO_UUID  = "e3f00003-1212-efde-1523-785feabcd123"
COVER_UUID = "e3f00005-1212-efde-1523-785feabcd123"
CMD_UUID   = "e3f00004-1212-efde-1523-785feabcd123"
COVER_PX   = int(os.environ.get("COVER_PX", "220"))
CHUNK      = 240
# Pace the cover transfer so it doesn't hog the 2.4GHz radio and starve A2DP audio on other BT
# headphones. Send COVER_BURST chunks, then pause COVER_GAP_MS to hand the radio back.
#
# IMPORTANT: the watch negotiates an 80-120ms BLE connection interval, so the gap must be >= that
# (not 14ms!) or the packets just queue and go out at full connection-event throughput anyway — the
# pause never actually yields airtime to A2DP. 100ms leaves a real breather every burst.
#   COVER=0            -> skip album art entirely (pure text now-playing, near-zero airtime; use this
#                         if music on other BT headphones still stutters — it will be glitch-free).
#   COVER_MIN_S=<sec>  -> never re-send art more often than this (guards against players that jitter
#                         their title/artist string every poll and would otherwise re-blast art nonstop).
COVER_ENABLED = os.environ.get("COVER", "1") != "0"
COVER_BURST   = int(os.environ.get("COVER_BURST", "6"))
COVER_GAP_MS  = int(os.environ.get("COVER_GAP_MS", "100"))
COVER_MIN_S   = float(os.environ.get("COVER_MIN_S", "12"))
CMD_PLAYPAUSE, CMD_NEXT, CMD_PREV, CMD_REPLAY, CMD_SEEK, CMD_VOL, CMD_RESYNC = 0, 1, 2, 3, 4, 5, 6
CMD_FINDPHONE = 7          # watch -> here: ring THIS machine loudly so you can find it
LOCATE_MAGIC  = 0xF1       # here -> watch: 2-byte [0xF1][seconds] on the TIME char = ring the puck


def ring_self(seconds=10):
    """Find-my-phone, PC edition: loud attention pattern on the default output device."""
    def _ring():
        try:
            import winsound
            end = time.monotonic() + seconds
            while time.monotonic() < end:                # klaxon: alternating two-tone
                for f in (988, 784):
                    winsound.Beep(f, 220)
        except Exception as e:
            print(f"[findphone] {e}")
    import threading
    threading.Thread(target=_ring, daemon=True).start()
    print(f"[watch->PC] FIND MY PHONE - ringing for {seconds}s")


def set_host_volume(pct):
    if not _HAVE_PYCAW:
        print(f"[vol] {pct}%  (pip install pycaw comtypes to actually set it)"); return
    try:
        dev = AudioUtilities.GetDeviceEnumerator().GetDefaultAudioEndpoint(0, 1)  # eRender, eMultimedia
        iface = dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
        vol = cast(iface, POINTER(IAudioEndpointVolume))
        vol.SetMasterVolumeLevelScalar(max(0.0, min(1.0, pct / 100.0)), None)
        print(f"[watch->PC] volume {pct}%")
    except Exception as e:
        print(f"[vol] error: {e}")


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def _lum(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def pick_palette(img):
    """Analyze the art -> [ring, artist_bg, song_bg, artist_txt, song_txt] RGB565."""
    try:
        small = img.convert("RGB").resize((64, 64))
        q = small.quantize(colors=8)
        pal = q.getpalette()
        cols = [(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]) for (_, i) in sorted(q.getcolors(), reverse=True)]
        if not cols:
            return list(DEFAULT_PAL)

        def sat(c):
            mx, mn = max(c), min(c)
            return (mx - mn) / (mx + 1)
        ring = max(cols, key=lambda c: sat(c) * (0.35 + _lum(c) / 255 * 0.65))   # vibrant
        artbg = cols[0]                                                          # dominant
        songbg = next((c for c in cols[1:] if abs(_lum(c) - _lum(artbg)) > 25),
                      cols[min(1, len(cols) - 1)])
        txt = lambda bg: (10, 10, 10) if _lum(bg) > 140 else (245, 245, 245)     # auto contrast
        return [rgb565(*ring), rgb565(*artbg), rgb565(*songbg), rgb565(*txt(artbg)), rgb565(*txt(songbg))]
    except Exception as e:
        print(f"[palette] {e}")
        return list(DEFAULT_PAL)


DEFAULT_PAL = [rgb565(60, 200, 120), rgb565(255, 255, 255), rgb565(255, 255, 255), 0, 0]
g_pal = list(DEFAULT_PAL)


async def current_session():
    return (await MediaManager.request_async()).get_current_session()


async def get_now_playing():
    try:
        s = await current_session()
        if s is None:
            return None
        props = await s.try_get_media_properties_async()
        pb = s.get_playback_info()
        tl = s.get_timeline_properties()
        playing = 1 if int(pb.playback_status) == 4 else 0
        pos = max(0, int(tl.position.total_seconds()))    # winsdk gives TimeSpan as timedelta
        dur = max(0, int(tl.end_time.total_seconds()))
        return (playing, props.title or "", props.artist or "", pos, dur)
    except Exception:
        return None


async def do_cmd(code, pos=0):
    try:
        s = await current_session()
        if s is None:
            print("[cmd] no active media session"); return
        if code == CMD_PLAYPAUSE:  await s.try_toggle_play_pause_async()
        elif code == CMD_NEXT:     await s.try_skip_next_async()
        elif code == CMD_PREV:     await s.try_skip_previous_async()
        elif code == CMD_REPLAY:   await s.try_change_playback_position_async(0)
        elif code == CMD_SEEK:     await s.try_change_playback_position_async(pos * 10_000_000)  # s -> 100ns ticks
        print(f"[watch->PC] cmd {code}" + (f" seek {pos}s" if code == CMD_SEEK else ""))
    except Exception as e:
        print(f"[cmd] error: {e}")


def encode_info(playing, pos, dur, title, artist):
    t = title.encode("utf-8")[:60]
    a = artist.encode("utf-8")[:60]
    return (bytes([playing]) + struct.pack("<II", max(0, pos), max(0, dur))
            + struct.pack("<5H", *g_pal) + t + b"\x00" + a)


# ---- time + IP geolocation -----------------------------------------------------
def fetch_geo():
    """Public-IP geolocation (no GPS on the watch): city, lat, lon, UTC offset (seconds)."""
    try:
        url = "http://ip-api.com/json/?fields=status,city,lat,lon,offset"
        d = json.loads(urllib.request.urlopen(url, timeout=6).read().decode())
        if d.get("status") == "success":
            return d["city"], float(d["lat"]), float(d["lon"]), int(d["offset"])
    except Exception as e:
        print(f"[geo] {e}")
    return None


async def push_time_location(client):
    """Set the watch clock (local time as epoch) + push its IP-derived city/coords over the TIME char."""
    loop = asyncio.get_event_loop()
    geo = await loop.run_in_executor(None, fetch_geo)
    offset = geo[3] if geo else -time.timezone          # seconds east of UTC
    epoch_utc = int(time.time()) & 0xFFFFFFFF            # UTC — the watch applies tz for display
    tz_min = int(round(offset / 60.0))
    payload = struct.pack("<Ih", epoch_utc, tz_min)
    if geo:
        city, lat, lon, _ = geo
        payload += struct.pack("<ff", lat, lon) + city.encode("utf-8")[:22]
    try:
        await client.write_gatt_char(TIME_UUID, payload, response=True)
        print(f"[time] synced UTC, tz={tz_min}min" + (f", {geo[0]}" if geo else ""))
    except Exception as e:
        print(f"[time] write failed: {e}")


# ---- album art ----------------------------------------------------------------
def fetch_itunes(title, artist):
    try:
        q = urllib.parse.quote(f"{artist} {title}".strip())
        url = f"https://itunes.apple.com/search?term={q}&entity=song&limit=1"
        d = json.loads(urllib.request.urlopen(url, timeout=6).read().decode())
        if d.get("resultCount"):
            art = d["results"][0]["artworkUrl100"].replace("100x100", "600x600")
            print(f"[cover] iTunes art for {title!r}")
            return Image.open(io.BytesIO(urllib.request.urlopen(art, timeout=8).read()))
    except Exception as e:
        print(f"[cover] iTunes miss: {e}")
    return None


async def fetch_smtc_thumb(session):
    try:
        props = await session.try_get_media_properties_async()
        ref = props.thumbnail
        if ref is None:
            return None
        stream = await ref.open_read_async()
        reader = DataReader(stream)
        await reader.load_async(stream.size)
        data = bytes(reader.read_buffer(stream.size))
        print("[cover] Windows SMTC thumbnail")
        return Image.open(io.BytesIO(data))
    except Exception as e:
        print(f"[cover] SMTC thumb miss: {e}")
        return None


def to_rgb565(img):
    # center-crop to a square that FILLS the cover (no stretching), then to RGB565
    img = ImageOps.fit(img.convert("RGB"), (COVER_PX, COVER_PX), centering=(0.5, 0.5))
    a = np.asarray(img, dtype=np.uint16)
    v = ((a[:, :, 0] >> 3) << 11) | ((a[:, :, 1] >> 2) << 5) | (a[:, :, 2] >> 3)
    return v.astype("<u2").tobytes()


# Guards so album art can NEVER saturate the radio: at most one transfer in flight, and the same
# track's art is never re-sent (nor any art re-sent within COVER_MIN_S). Windows media players often
# nudge the title/artist string on every poll; without these, each nudge launched another overlapping
# 100 KB blast and the pile of them permanently starved A2DP -> constant music glitching.
g_cover_busy = False
g_cover_key  = None
g_cover_t    = 0.0

async def push_cover(client, title, artist):
    global g_cover_busy, g_cover_key, g_cover_t, g_pal
    if not COVER_ENABLED:
        return
    key = (title, artist)
    now = time.monotonic()
    if g_cover_busy:
        return                                          # one transfer at a time
    if key == g_cover_key:
        return                                          # already sent this exact art
    if g_cover_key is not None and now - g_cover_t < COVER_MIN_S:
        return                                          # rate-cap flip-flopping metadata
    g_cover_busy = True
    g_cover_key = key                                   # claim now so concurrent triggers bail
    try:
        await _push_cover(client, title, artist)
        g_cover_t = time.monotonic()
    except Exception as e:
        print(f"[cover] error: {e}")
        g_cover_key = None                              # let a genuine retry happen
    finally:
        g_cover_busy = False


async def _push_cover(client, title, artist):
    loop = asyncio.get_event_loop()
    img = await loop.run_in_executor(None, fetch_itunes, title, artist)
    if img is None:
        img = await fetch_smtc_thumb(await current_session())
    if img is None:
        print(f"[cover] no art for {title!r}"); return
    global g_pal
    g_pal = await loop.run_in_executor(None, pick_palette, img)
    print(f"[palette] ring={g_pal[0]:04x} artbg={g_pal[1]:04x} songbg={g_pal[2]:04x}")
    body = await loop.run_in_executor(None, to_rgb565, img)
    t0 = time.monotonic()
    try:
        for n, off in enumerate(range(0, len(body), CHUNK)):
            await client.write_gatt_char(COVER_UUID, struct.pack("<I", off) + body[off:off + CHUNK],
                                         response=False)
            if COVER_GAP_MS and (n + 1) % COVER_BURST == 0:
                await asyncio.sleep(COVER_GAP_MS / 1000.0)   # yield the radio to BT audio
        await client.write_gatt_char(COVER_UUID, struct.pack("<I", 0xFFFFFFFF), response=False)
        print(f"[cover] sent {len(body)} bytes over BLE in {time.monotonic()-t0:.1f}s "
              f"(paced {COVER_BURST}/{COVER_GAP_MS}ms)")
    except Exception as e:
        print(f"[cover] BLE send failed: {e}")


# ---- main loop ----------------------------------------------------------------
async def run():
    print(f"scanning for {DEV_NAME!r} ...")
    dev = await BleakScanner.find_device_by_name(DEV_NAME, timeout=15.0)
    if not dev:
        print("  not found -- reset the watch so it advertises cleanly."); return
    print(f"  found {dev.address}; connecting ...")
    loop = asyncio.get_event_loop()
    async with BleakClient(dev) as client:
        print("connected (unencrypted, no pairing).")

        batt = {}   # accumulates a battery-log dump (header + deltas)

        def save_battlog():
            if not batt.get("hdr"):
                return
            epoch, iv, cnt, base = batt["hdr"]
            mv, v = [base], base
            for d in batt["deltas"][:cnt - 1]:
                v += d - 256 if d > 127 else d
                mv.append(v)
            path = "battlog.csv"
            with open(path, "w") as f:
                f.write("iso,epoch,mv,pct\n")
                for i, m in enumerate(mv):
                    ts = epoch - (cnt - 1 - i) * iv
                    pct = max(0, min(100, round((m - 3300) / (4200 - 3300) * 100)))
                    f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(ts))},{ts},{m},{pct}\n")
            print(f"[battlog] saved {len(mv)} samples -> {path}")
            batt.clear()

        def on_cmd(_s, data):
            if not data: return
            if data[0] == 0xB1 and len(data) >= 11:           # battery-log header
                epoch = int.from_bytes(data[1:5], "little")
                iv    = int.from_bytes(data[5:7], "little")
                cnt   = int.from_bytes(data[7:9], "little")
                base  = int.from_bytes(data[9:11], "little")
                batt.clear(); batt["hdr"] = (epoch, iv, cnt, base); batt["deltas"] = []
                return
            if data[0] == 0xB2:                                # battery-log deltas
                batt.setdefault("deltas", []).extend(data[1:]); return
            if data[0] == 0xB3:                                # battery-log end
                save_battlog(); return
            if data[0] == CMD_FINDPHONE:
                ring_self()
            elif data[0] == CMD_RESYNC:
                print("[watch->PC] resync requested")
                loop.create_task(push_time_location(client))
            elif data[0] == CMD_VOL and len(data) >= 5:
                set_host_volume(int.from_bytes(data[1:5], "little"))
            elif data[0] == CMD_SEEK and len(data) >= 5:
                loop.create_task(do_cmd(CMD_SEEK, int.from_bytes(data[1:5], "little")))
            else:
                loop.create_task(do_cmd(data[0]))
        await client.start_notify(CMD_UUID, on_cmd)
        if COVER_ENABLED:
            print(f"bridge live over BLE: now-playing + position + art "
                  f"(paced {COVER_BURST}/{COVER_GAP_MS}ms, {COVER_PX}px, >= {COVER_MIN_S:g}s apart).")
            print("  if music on other BT headphones still stutters, run with COVER=0 for glitch-free text-only.")
        else:
            print("bridge live over BLE: now-playing + position ONLY (album art disabled, COVER=0).")
        print("  listening for taps.  Ctrl-C to quit.\n")

        await push_time_location(client)      # sync clock + IP location right after connect

        async def console():
            """Type 'find' + Enter to make the puck ring (find-my-puck). 'q' quits."""
            while client.is_connected:
                line = await loop.run_in_executor(None, sys.stdin.readline)
                if not line:
                    return
                cmd = line.strip().lower()
                if cmd.startswith("find"):
                    secs = 20
                    parts = cmd.split()
                    if len(parts) > 1 and parts[1].isdigit():
                        secs = max(1, min(120, int(parts[1])))
                    try:
                        await client.write_gatt_char(TIME_UUID, bytes([LOCATE_MAGIC, secs]), response=True)
                        print(f"[PC->watch] FIND MY PUCK - ringing it for {secs}s")
                    except Exception as e:
                        print(f"[findpuck] failed: {e}")
                elif cmd == "sync":
                    await push_time_location(client)
        asyncio.create_task(console())
        print("  type 'find' + Enter to ring the puck ('find 30' for 30s), 'sync' to re-sync time.\n")

        last_meta, last_pos, last_play = None, -99, None
        tick = 0
        while client.is_connected:
            tick += 1
            if tick % 300 == 0:               # re-sync time + location every ~5 min
                await push_time_location(client)
            np_ = await get_now_playing()
            if np_:
                playing, title, artist, pos, dur = np_
                meta = (title, artist)
                # The watch interpolates position locally, so only correct drift every few seconds
                # (metadata + play/pause still go out immediately). Fewer BLE writes = less airtime
                # stolen from A2DP audio on other headphones.
                if meta != last_meta or playing != last_play or abs(pos - last_pos) >= 4:
                    try:
                        await client.write_gatt_char(INFO_UUID, encode_info(playing, pos, dur, title, artist),
                                                     response=False)
                        print(f"[PC->watch] {'play ' if playing else 'pause'} {pos}/{dur}s  {title} - {artist}")
                        last_pos, last_play = pos, playing
                    except Exception as e:
                        print(f"[info] write failed: {e}")
                if meta != last_meta:
                    last_meta = meta
                    asyncio.create_task(push_cover(client, title, artist))
            await asyncio.sleep(1.0)
    print("disconnected.")


if __name__ == "__main__":
    while True:
        try:
            asyncio.run(run())
        except KeyboardInterrupt:
            print("\nbye."); break
        except Exception as e:
            print(f"error: {e}")
        print("retrying in 3s ...")
        time.sleep(3)
