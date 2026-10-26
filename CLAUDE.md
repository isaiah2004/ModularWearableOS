# Watch workspace and board records

## Active target

**Work on the Waveshare ESP32-S3-Touch-AMOLED-1.43-B watch by default.**
The user switched back to this board on **2026-10-23**. Keep the 1.43C as a
separate hardware target; do not carry its pin assignments into the B build.
This record supplements [AGENTS.md](AGENTS.md).

| Board | Purpose | Last known port | USB serial identifier |
|---|---|---|---|
| **1.43-B** | Active metal-case watch; same mapping as the plain 1.43 | **COM3** | `28:84:85:B0:8B:30` |
| **1.43C** | Separate AI/audio board | COM4 | `98:88:E0:0E:2F:54` |

Both use native ESP32-S3 USB, VID:PID `303A:1001`. Port numbers can change.
On 2026-10-23, COM3 enumerated with the B identifier above, but the firmware
`hostcaps` query timed out. Its running firmware identity was not confirmed.
Before flashing, enumerate ports, match the physical board/USB identifier, and
verify the responding firmware's `board` identity and intended build target.

## Build targets

Build from **`A:\makerspace\smartwatch2.0`**, preserving its recovered working
state. The source of truth is the current workspace, not its older branch HEAD.

| Setting | 1.43-B watch | 1.43C audio board |
|---|---|---|
| `WATCH_BOARD_NAME` | `ESP32-S3-Touch-AMOLED-1.43-B` | `ESP32-S3-Touch-AMOLED-1.43C` |
| `CONFIG_WATCH_BOARD_143C` | Unset | `y` |
| SDK config | `firmware/sdkconfig` | `firmware/sdkconfig.c143` |
| Defaults | `firmware/sdkconfig.defaults` | Common defaults plus `firmware/sdkconfig.defaults.c143` |
| Build output | `firmware/build` | `firmware/build_c143` |
| Flash / PSRAM | 16 MB / 8 MB | 8 MB / 8 MB |
| Partition table | `firmware/partitions.csv` | `firmware/partitions_c143.csv` |
| App layout | Two `0x780000`-byte OTA slots; first at `0x20000` | `0x7e0000`-byte factory app at `0x10000` |

PowerShell commands, from the workspace root:

```powershell
# Active B watch: build only
tools/build.ps1
# B: build and checked flash, after confirming COM3 is the B watch
tools/build.ps1 -Port COM3 -Flash

# C: build only
tools/build_c143.ps1
# C: supplying -Port builds AND flashes; confirm its port first
tools/build_c143.ps1 -Port COM4
```

ESP-IDF is at `A:\makerspace\esp-idf`. Use its Python environment:
`C:\Users\isaia\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe`.
The checked flash helper is `tools/flash_watch.py`; select `--build firmware/build`
for B or `--build firmware/build_c143` for C. Do not substitute another board's
binary or an old build from another worktree.

## Hardware differences

The authoritative firmware pin map is [firmware/main/pins.h](firmware/main/pins.h).
Both displays are 466 x 466 RGB565. The frequencies below are the current source
settings, not a claim about a panel's maximum supported speed.

| Function | 1.43-B watch | 1.43C audio board |
|---|---|---|
| LCD QSPI CS / CLK | 9 / 10 | 15 / 14 |
| LCD D0 / D1 / D2 / D3 | 11 / 12 / 13 / 14 | 9 / 10 / 11 / 12 |
| LCD reset / enable | 21 / 42 | 13 / no GPIO enable |
| QSPI clock | 80 MHz | 40 MHz |
| TE synchronization | Not routed; GPIO8 is IMU interrupt | GPIO8; Developer Tools toggle, default/saved off |
| Shared I2C SDA / SCL | 47 / 48 | 47 / 48 |
| Touch | FT3168, `0x38`; no reset/interrupt GPIO | CST820, `0x15`; reset16 / interrupt17 |
| IMU | QMI8658, `0x6b`; interrupt8 | Absent |
| RTC | PCF85063, `0x51`; interrupt15 | Absent |
| SD | One-bit SDMMC: CLK41 / CMD39 / D0=40; D3=38 unused | Absent |
| Built-in audio | Absent; external strap needs its own verified mapping | ES8311 speaker / ES7210 microphones |
| Battery ADC | GPIO4, ADC1 channel3; divider3 | GPIO4, ADC1 channel3; divider2 |
| Charge status / indicator | No corresponding definitions in `pins.h` | GPIO7 / GPIO5 active low |

- **GPIO42 has different roles:** B LCD enable versus C microphone data input.
  Never drive GPIO42 as LCD enable on C. B GPIO38..42 are occupied by SD/display
  hardware; do not reuse the C audio wiring on B.
- C audio uses I2S MCLK38, BCLK39, WS40, speaker DOUT41, microphone DIN42,
  audio LDO18 and amplifier46. Codec I2C addresses are `0x18` and `0x40`.
  See [firmware/main/voice_audio.c](firmware/main/voice_audio.c).
- B uses the working CO5300 initialization profile; this is not a confirmed
  controller identity. Forcing SH8601 previously produced a black screen.
- B display output rotates counterclockwise in software: UI `(x,y)` maps to
  native `(y,465-x)`; touch uses the inverse `(465-y,x)`. C has its own panel
  orientation/touch handling. Preserve the separate transforms.
- C Chat has tested built-in microphone/speaker support. B has no built-in audio;
  C's successful Chat audio checks do not establish B strap support.

## Persistence and flash safeguards

- Preserve all existing uncommitted/untracked source. Recovery provenance is in
  [docs/RECOVERY_20261020.json](docs/RECOVERY_20261020.json), with hashed archives
  under `build/recovery-20261020/`. Never reset to HEAD to recover this workspace.
- Before flashing either target, verify Solar/Orrery, Developer Tools, the
  integrated home pager, and sports apps are included. Use the checked build/flash
  scripts, which checkpoint preferences and verify flash writes exclude NVS.
  Do not silently erase NVS or format SD.
- B settings are authoritative on SD at `/watch/settings.0.bin` and
  `/watch/settings.1.bin`, loaded into RAM at boot. Keep the checksummed alternating
  checkpoint, fsync/readback verification, mount lease and SD lock. NVS is the
  migration/card-absent fallback; failed SD saves must remain dirty for retry.
- C physically has no SD and uses the checked NVS fallback. Preserve its saved
  credentials, preferences and compact Chat history when flashing.
- The recent C Chat/TE firmware was flashed to C. B was compile-checked during
  that work; do not assume those changes are already running on B.
- Keep API keys, Wi-Fi credentials and `.env` contents out of board records.

More context: [B hardware record](docs/HARDWARE.md),
[TE investigation](docs/te_investigation.md), and [Chat/audio record](docs/VOICE_CHAT.md).
For current flash layouts use the partition CSVs above: the older factory-only
layout paragraph in `docs/HARDWARE.md` predates B's current OTA partition table.
