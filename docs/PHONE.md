# Phone link: Gadgetbridge (Android)

NOS talks to your phone through **Gadgetbridge** using the **Bangle.js** protocol. The
watch looks like a Bangle.js to Gadgetbridge: it offers a Nordic UART Service (NUS) and
advertises as **`Bangle.js NOS`**.

| | |
|---|---|
| BLE name | `Bangle.js NOS` (Gadgetbridge recognises names matching `Bangle\.js.*`) |
| Service | NUS `6e400001-b5a3-f393-e0a9-e50e24dcca9e`, its UUID is in the scan response |
| RX (phone writes) | `6e400002-...` write / write-without-response |
| TX (watch notifies) | `6e400003-...` notify |
| Address | fixed static-random `D7:B5:93:71:59:37` |
| Security | no bonding, no encryption (same as the companion characteristics) |

## Pairing

1. Install Gadgetbridge from F-Droid, or **"Bangle.js Gadgetbridge"** from the Play Store.
   They behave the same here. The Play Store build also has internet access, which NOS
   does not use yet.
2. Disconnect the PC companion first (`tools/watch_media_bridge.py`). The watch allows
   **one** BLE connection at a time (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`).
3. In Gadgetbridge tap **+** (Connect new device). Wait for **Bangle.js NOS** to appear,
   then tap it.
   - If it doesn't appear, open Gadgetbridge *Settings -> Discovery and pairing options*,
     turn on **Discover unsupported devices**, scan again, long-press the watch and pick
     **Bangle.js** as the device type.
4. Gadgetbridge asks whether to pair (bond). Choose **not** to bond ("Don't pair" / "No"):
   the watch has bonding switched off. If you bonded by mistake and it keeps failing,
   remove the device in Gadgetbridge and in Android Bluetooth settings, then add it again
   without bonding.
5. Give Gadgetbridge **Notification access** when Android asks, or later under Android
   *Settings -> Notifications -> Device & app notifications*. Use Gadgetbridge's
   *Notification settings* to choose which apps reach the watch.
6. Optional settings:
   - **Weather**: Gadgetbridge needs a weather app that sends it data, e.g. Breezy
     Weather with "Send Gadgetbridge data" turned on. Without one, NOS uses the Wi-Fi
     (Open-Meteo) weather when the watch is online.
   - **Text as bitmaps** off. Transliteration is optional: the watch folds accents and
     smart punctuation to ASCII itself.
   - **Allow high MTU** on is fine. The watch sends in chunks of MTU-3 bytes either way.

On connect the watch log shows `NUS subscribed (Gadgetbridge)`. Within a second
Gadgetbridge sets the clock and timezone, and the watch reports its firmware and battery
(`ver`, `status`).

## What works

Phone -> watch (`\x10GB({...})` lines, plus the `setTime(...)` time line):

| Message | NOS behaviour |
|---|---|
| `setTime(utc);E.setTimeZone(h)` | sets the clock (`hal_set_datetime`) and display timezone; saved to `nos_prefs.tz_min` |
| `notify` | stored (20 kept, newest first). Banner unless Do Not Disturb is on; tapping it opens the detail. Icon by app: messengers and mail -> message, Calendar -> calendar, Phone/dialer -> phone, otherwise bell |
| `notify~` / `notify-` | updates or removes the notification by id |
| `call` incoming / start / end / reject | incoming opens the call screen and rings; it closes when the call ends |
| `musicinfo`, `musicstate` | Music app; the position is interpolated between updates |
| `weather` | temperatures converted from Kelvin, OWM code mapped to NOS conditions; the hourly/daily forecast comes from the Wi-Fi weather when available |
| `find` n:true/false | opens the Find watch screen and rings until stopped (2 min safety stop) |
| `alarm` | **replaces** the watch alarms with the phone's list (days converted from Gadgetbridge's Monday-first bitmask), saved as "nosalarm" |
| `act` | replies right away with steps (delta) and heart rate, then every `int` seconds while requested |
| `vibrate` | one buzz |
| `is_gps_active` | replies `gps_power` false |

Watch -> phone (one JSON object per line):

| Sent | When |
|---|---|
| `{"t":"ver","fw":"NOS 1.0","hw":"ESP32-S3 AMOLED 1.43"}` | 0.5 s after connect |
| `{"t":"status","bat":78,"volt":3.95,"chg":0}` | after connect, every 10 min, on a 5 % or charger change |
| `{"t":"notify","id":N,"n":"DISMISS"}` | a phone notification is dismissed or cleared on the watch |
| `{"t":"notify","id":N,"n":"OPEN"}` | "Open on phone" |
| `{"t":"music","n":"play"}` (pause/next/previous/volumeup/volumedown) | music controls |
| `{"t":"call","n":"ACCEPT"}` / `"REJECT"` / `"END"` | call screen buttons |
| `{"t":"findPhone","n":true}` / `false` | Find phone |
| `{"t":"act","stp":N,"hrm":N,"rt":1}` | realtime activity, when Gadgetbridge asks for it |

Text: Gadgetbridge sends non-ASCII text as raw Latin-1 bytes, `\xNN` escapes, `\uNNNN`
escapes or UTF-8. NOS decodes all of them and then folds to ASCII for the dot fonts:
"Zoë" -> "Zoe", smart quotes -> `'`/`"`, dashes -> `-`, "…" -> "...". Emoji are dropped and
other scripts become `?`.

Not handled yet: `calendar`, `nav`, `gps`, `http`, quick replies.

## The PC companion still works

The companion GATT service (`e3f00001-...`: time, media info/cover/command,
notifications) is unchanged, and the NUS is registered after it, so its handles don't
move. **One catch:** `tools/watch_media_bridge.py` finds the watch by the name `"Watch"`
(`DEV_NAME`). Since the watch now advertises as `Bangle.js NOS`, set `DEV_NAME` to match.
Only one of the two links can be connected at a time.

## Testing without a phone

- **USB console** (firmware/main/nos_main.c): `gb <line>` feeds one line to the services,
  e.g. `gb GB({"t":"notify","id":5,"src":"WhatsApp","title":"Maya","body":"hi"})`.
- **Simulator**: `sim/nos_phone_sim.c` is a fake phone. 3 s after start it connects and
  sends setTime, a weather report and a playing track, then sends notifications at 14 s,
  33 s and 55 s. Scripted runs (`--script`) hold those notifications back so their banners
  don't end up in other screenshots. Script commands:
  `phone sim notify` releases the held notifications; `phone sim call [NAME]` and
  `phone sim callend` start and end a call; `phone sim find` and `phone sim findstop` start
  and stop Find watch; `phone sim on`, `off` and `auto` control the link;
  `phone <any GB line>` injects that line. The fake phone answers music commands the way
  Gadgetbridge does.
- **Parser tests**: `gcc -std=c99 -Wall -Wextra -Inos tools/gb_test.c nos/gb.c -o build/gb_test.exe`,
  then run `build/gb_test.exe`. It feeds the Espruino documentation samples and
  encoding edge cases through the parser and exits with the number of failures.
