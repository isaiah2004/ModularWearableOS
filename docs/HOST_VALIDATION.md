# Current NOS setup and validation

Work on `NOS-v0.5-Alpha` in this recovered workspace. Preserve all dirty/untracked
sources and the recovery/alpha baseline archives. HEAD is historical provenance,
not the build input. Use the [workspace contract](../AGENTS.md).

Use a normal Python 3.12 host environment and GCC/WinLibs for simulator and
host/offline validation. Install `tools/requirements-host.txt` in that environment:
the IMU audit test runs in both profiles and produces plots with Matplotlib and
Pillow; the same file supplies pyserial for explicit host serial tools. The runner
checks Matplotlib/Pillow availability before GCC or tests, records the actual
Python executable and reports an install command if prerequisites are missing.
It does not import plotting modules or open a port during preflight.

Firmware builds, flashing and explicit hardware execution use the matching
ESP-IDF 5.5.x Python environment, including its SDK-managed pyserial and esptool.
An SDK Python environment is not assumed to contain the host plotting packages.
Keep host validation and SDK commands in separate activated environments; do not
replace SDK-managed esptool with a host package. Hardware execution checks these
two runtime modules first; a hardware plan needs neither. CI installs the host
requirements for both jobs and fixes Python 3.12.8 and Windows GCC 12.2.0.
Simulator/build manifests record the actual compiler hash, version and flags.
The full offline weather-parser case
also needs cJSON from ESP-IDF (or `--cjson-dir`); CI supplies cJSON v1.7.19 and
reports source/header hashes. Module availability checks do not certify binary
compatibility: imports and artifact generation still run in the required tests.

```powershell
# Host Python environment
python -m pip install -r tools/requirements-host.txt
.\sim\nos_build.ps1 -Strict -Verify
python tools/validate.py --profile host
python tools/validate.py --profile offline --sim build/nos_sim.exe
```

```powershell
# Matching ESP-IDF Python environment
.\tools\build.ps1
python tools/flash_watch.py --port COM3 --dry-run
.\tools\build.ps1 -Port COM3 -Flash
.\tools\package.ps1
```

The [machine-readable matrix](../tools/validation_matrix.json) is the inventory.
`host` exercises protocol/framing, SD tools, flash preflight, source/build identity,
import safety, exports and host report parsers. `offline` runs every
`tests/test_*.py` separately (zero tests fail), native cases, baked assets, script
failure handling, and the Sports simulator scenarios. It consumes an explicit
strict NOS executable whose source/header and binary hashes must still match.
The runner never builds firmware, flashes, or imports a hardware scenario to run
it. `tools/test.ps1` and `sim/build.ps1` remain archived core/apps coverage.

Each run creates `build/validation/validation-<profile>-<unique-id>/report.json`
plus logs and per-module test counts/skips. `--out` selects another parent inside
the workspace. Exit status is nonzero for a failed required command, missing
toolchain, stale executable, or empty suite. The reports distinguish a passed
portable profile from omitted simulator and hardware groups. Use the full
offline profile before accepting current NOS changes. Hardware evidence must
match the firmware image hashes; offline tests do not measure device behavior.

Heavy subprocess TEMP/TMP/TMPDIR is redirected to the workspace volume, normally
A:, under `.worktrees/_audit_tmp` or the unique validation run. `.worktrees/` is
ignored. Do not remove existing private worktrees or temporary directories to
make space. Named simulator outputs have independent locked caches; verification
uses fresh files and checks available disk space for both full RGB565 rasters.

## Build and package identity

`tools/build.ps1` configures first, runs `build_identity.py prepare`, then builds
and finalizes only after successful native exits. The archive contains actual
dirty/untracked sources, generated source assets, sdkconfig and dependency lock.
Finalization refuses source changes during the build, checks the ESP application
descriptor's ELF hash, and requires Solar/Orrery, Developer tools, Sports/Energy/
Sensor logs and the integrated home pager in both compile commands and ELF
symbols. The flash plan is parsed, checked against its partition table and the
real 16 MB target, and reports exact application-slot headroom in bytes. App
overflow is fatal; headroom is not an invented warning threshold.
The generated `build/generated/nos_build_identity.h` supplies the full tree hash
and short ID. NOSBUILD must emit the full hash; finalization rejects an ELF that
does not contain it. Include the generated directory unconditionally in the
firmware target so the header written after configure is used during compilation.

Packaging requires that finalized identity still match the current workspace.
It creates a new bundle directory and includes ELF/map, flash segments/plan,
compile commands, config/dependency lock, compiler identity, source manifest/ZIP,
checked updater and hardware-evidence match record. Existing bundles are never
overwritten. Device `NOSBUILD elf_sha256` joins a capture to the bundled ELF.
Generated Solar artwork from `build/sd/sky` is hashed into the source identity,
archived, and copied separately to a bundle's `sd/sky` for SD provisioning.
`sd_assets.missing` explicitly lists absent textures; missing artwork uses the
firmware fallback. User logs/recordings and other copied card contents are excluded.
Symbolicate with the matching ESP-IDF toolchain, for example:

```text
xtensa-esp32s3-elf-addr2line -pfiaC -e watchfw.elf 0xADDRESS
```

Both workspace and packaged flashing validate all required hashes/ranges before
opening serial. They require a completed `prefs flush` with `ok=1 stored=1
match=1`, revalidate artifacts, and only then invoke esptool. A missing/failed
acknowledgment stops the operation; preference mutations are never replayed.
There is no erase-NVS or checkpoint-bypass switch. The actual partition-table
offsets govern flashing, including OTA initialization. A merged image's gap
bytes would overwrite NVS and must never be used for an update. The 1.43C's
legacy build/runtime is separate and is not supported by this updater.

## Command transport and restore

`host_transport.py` owns USB framing, command validation, typed errors and
redacted wire diagnostics. `hostcaps` negotiates v1 envelopes:

```text
hostcaps
NOSHOST v=1 req=1 max_command=191 typed_errors=1
DONE
req 12ab34cd prefs flush
BEGIN 12ab34cd
NOSPREFS ... ok=1 stored=1 match=1
DONE 12ab34cd ok=1
```

IDs must match; a delayed DONE cannot complete a new request. Legacy firmware is
accepted only after an explicit unknown-command rejection plus DONE. Legacy
completion cannot provide v1 correlation, so tools serialize requests. Typed SD
responses such as `SDCRC ERR open 16` raise a `DeviceError` with phase/errno;
only `MISSING` means absent. Input rejection raises `CommandRejected` promptly.
Only explicit read-only commands may retry. Resynchronization invalidates a
partial command with NUL/newline and requires a fresh read-only capability probe
before releasing the barrier. Upload/delete/reset/navigation/recording and
preference saves never replay after an ambiguous timeout.

`sdsync` negotiates paced GET and `typed_errors`, `safe_put`, `restore_put`.
`--restore-protected` opts into `sdput!` only if the device advertises it. The
firmware still validates protected checkpoint paths/structures and rejects
active recordings with EBUSY. Ordinary uploads retain their legacy wire form.
Use `watchctl --wire-log PATH` for redacted transport diagnostics.

`watchctl --smoke` identifies NOS from `hostcaps` and `info`, pins its complete
source/ELF identity and boot ID, and checks resolved idle recording owners before
each navigation. It opens the registered Energy, Battery history, Sports and
Developer tools screens, preserves awake/preferences/render flags, and verifies
HOME at completion. It requires an already unlocked, active watch. A rejected or
incomplete read-only query may retry once after resynchronization; an uncertain
navigation is observed before any cleanup compensation. It never replays the
uncertain mutation. Explicit legacy WOS gets read-only smoke diagnostics; NOS
refuses the legacy `--sync-time` and `--wifi-ssid` options before prompting or
changing state. Use NOS Settings, phone/Gadgetbridge or Wi-Fi/NTP for time and
Settings/setup for Wi-Fi.

## Apps and Settings target benchmark

The source-archived `tools/target_benchmark.py` prints its plan without importing
serial or opening a port. Its offline tests construct protocol and capture
fixtures from source; they do not require a preexisting ignored benchmark script,
device log or build identity.

```powershell
python tools/target_benchmark.py
python -m unittest tests.test_target_benchmark_plan tests.test_watchctl -v
python tools/target_benchmark.py --run --port COM3 --build firmware/build --repeat 3 --out build/alpha05/checks/target-benchmark/final-quiet
```

Only the last command opens the explicitly selected port. It verifies the
finalized current source/build and complete target ELF/source hashes before
mutation. The output directory must be new and remain below
`build/alpha05/checks/target-benchmark/`. Start unlocked on settled HOME page0
or embedded Apps page1, with an acknowledged lit panel, clean saved preferences,
and no active, failed or unknown recording/OTA/shutdown owners. Use exclusive
device custody. The tool never unlocks, flashes, erases, stops a recorder or
writes preferences. It observes and restores the original awake flag and any
optional diagnostic flags it owns; failed cleanup is reported as incomplete.

Default coverage has three repeats in both directions of slow 75 px/s,
typical 333.33 px/s and fling 2500 px/s motion. Apps uses the actual embedded
HOME page1 through a horizontal pager swipe; Settings opens the registered app.
Quiet captures report p50/p95/p99 from ceil-millisecond histogram bins, exact
maximum microseconds and counts above 33,333 microseconds for consecutive
painted presentations after the DMA fence. Idle iterations break the chain.
Per-window percentiles are retained; they are not averaged into pooled
percentiles. Optional `--history-ab` and `--pipeline-profile` results are labeled
diagnostics and remain separate from the quiet acceptance pass.

For USB delivery diagnosis, `--usb-paced-writes` sends the same command and
barrier bytes one at a time, flushes each byte, then waits 2 ms. This option is
off by default and recorded in `invocation.json`; other host tools are unchanged.
It stops immediately on a failed or short write and never replays a mutation.
Pacing is an optional diagnostic mitigation. It does not fix USB reply gaps or
establish benchmark acceptance; identity, payload, owner and cleanup checks still
apply to every run.

With native rendering pinned on, app7/app8 captures require positive
`NOSCACHECAP native_blurred_frames` and `native_blurred_processed_area`, with
consistent nonnegative counts/areas. Generic blend counters alone do not qualify
the native path. These counters count prepared native calls and processed raster
area, including black pixels and overlap; direct sharp fast paths are excluded.
The blur flag records caller exposure intent, which can quantize to an identity
kernel at tiny exposure. The counters therefore establish retained native blur
work/intent during the workload, not unique blurred pixels, every eligible frame,
visible blur, or optical panel timing. Offline checks and a printed plan do not
establish a physical 30 fps floor; that requires matching target evidence.

## Explicit hardware profile

Use the matching ESP-IDF Python environment for execution; dry-run remains
dependency-free and performs no serial/network/reset operation.

```powershell
python tools/validate.py --profile hardware --port COM3 --expected-elf-sha256 <64-hex-digits>
python tools/validate.py --profile hardware --port COM3 --expected-elf-sha256 <64-hex-digits> --run-hardware
```

The first command only describes the run. The second checks the target's
NOSBUILD ELF identity before changing UI/DND, checkpointing settings, temporarily
joining the setup AP, testing HTTP settings/scan behavior, rebooting and restoring
the prior PC network/watch preferences. Only root/integration owns the device
during the alpha swarm. Importing `check_connectivity_device.py` creates no
directories, serial connection, WLAN profile or reset. A failed identity check
never enters the mutation/reset path. Hardware-only radio/display/touch/power,
reboot durability and sensor checks remain explicit target work.
