# Device scheduling and measurement

The firmware follows `NOS_FPS` (40 Hz target, 25 ms budget). Every completed dirty frame is submitted to the display. If a frame overruns its target deadline, the next full frame starts immediately and the deadline is rebased; waiting for the next 50 ms grid point would itself violate the 30 Hz floor. Overruns are reported, not used to disable blur or drop queued frames. A 30 Hz floor requires consecutive moving-frame completions at most approximately 33.333 ms apart. This is a validation target, not a demonstrated hardware guarantee.

`frame_drain` now uses an ordered flush-queue fence. Previously a raster buffer was released before its DMA submission; observing both raster buffers and both DMA permits free could race the final submission. The flush task now acknowledges the fence only after all preceding submissions and DMA completions. Draw submission failures are checked instead of leaving an unmatched DMA permit and hanging indefinitely.

## Metrics

Existing NOSPERF fields remain compatible, including legacy `fps` (raw window count) and work-only `avg_ms`/`max_ms`/`late`. Added fields:

- `rate`: measured host-loop frequency from first/last starts within the report window, avoiding the initial count/window off-by-one.
- `painted`: iterations with actual dirty pixels; idle iterations are not display refreshes.
- `e2e_max`: maximum host-loop start through ordered DMA fence completion, including input, commands, wake checks, brightness sampling and Wi-Fi changes before rendering.
- `deadline`: completions later than the frame's target deadline.
- `target_slots`: target periods overrun at scheduling time, including trace/output and post-render housekeeping. A summary's own printing overhead is observed by the following report window.
- `floor`: consecutive painted completions more than 33,333 us apart (33,334 us fails in iteration 9 and the current parser). Iteration 8 and earlier firmware used more than 33,334 us. An intervening unchanged frame breaks the animation interval sequence, avoiding false failures for static screens.
- `present_max`: maximum consecutive painted completion interval in ms. These are transfer completions, not panel scanout/vblank timestamps.
- `present_p95`/`present_p99`: ceiling-ms interval histogram percentiles. Intervals >=63 ms share an overflow bin; a percentile falling there reports the window maximum rounded upward, a conservative bound rather than understating long stalls. Maximum timing retains full microsecond resolution internally.

NOSF appends `e2e` (us), signed `deadline_us` (completion minus deadline), device `begin_us`/`completed_us` and `frame_id`. It also exposes tick/history timings, history pixels/blends, layer record/raster times, rendered/reused bands, cache bytes and allocation failures. NOSCACHE reports aggregate retained-layer/history/UI-cache bytes, allocation failures and directional UI blur bytes/hits/misses/evictions/work pixels. Existing raster timing includes dirty-region calculation, raster work and stripe waits; rotation overlaps rendering and transport, so adding these stage durations double-counts overlap. Serial tracing affects the measured system; use trace-off summary runs for acceptance, with bounded trace runs for diagnosis.

From iteration 9, `history_us` sums preparation and worker elapsed time. It overlaps rasterization, rotation and DMA; adding it to those stages double-counts work. The exposure-worker fence completes before statistics are sampled, and the full DMA fence remains. NOSWORKER and per-frame flags identify asynchronous/packed/QZ modes. The reporter omits the 200 MHz extrapolation for asynchronous exposure because these overlapping elapsed counters do not establish a CPU-only critical path. A 240 MHz asynchronous run cannot certify a measured 200 MHz budget.

AOD/off reset the awake measurement window. DMA completion does not establish when a pixel becomes visible. The 1.43-B pins define no TE signal. If its panel scans at 60 Hz, 40 Hz content has a noninteger 3:2 scan ratio even with perfect host cadence. Equal optical frame durations and tear-free presentation cannot be promised by these counters. Validate the actual panel refresh and optical presentation separately; 1.43C's routed TE is currently unused and its firmware entrypoint remains separate.

## Persistence and power

Preferences and the board task's periodic pedometer NVS saves are held while awake and during AOD entry, then allowed when OFF or AOD is stable. Preference saves retain their pending state until `hal_store_save_checked` succeeds. Simulators keep the previous allowed-by-default behavior. There is no extra persistence worker, framebuffer or queue. Pending data may remain uncommitted indefinitely if the watch is kept awake. Abrupt power loss before an allowed window can lose those changes; deliberate Wi-Fi credentials and other direct store calls remain synchronous. A wake can meet a flash operation already in flight. This policy reduces known stalls but does not guarantee flash-free animation against every writer.

The existing four 466x16x2-byte internal raster/DMA buffers remain 59,648 bytes total. One small fence semaphore and a static timing histogram are added. Rotation stays unchanged until an ESP32 benchmark shows a useful critical-path reduction. The maintained brightness suppression, 1 Hz stable AOD rendering and 20 ms off sensor loop avoid unnecessary frame painting. No speculative higher SPI clock, unsupported 200 MHz CPU setting, DMA-in-PSRAM migration or automatic light sleep was introduced.

ESP32-S3 officially supports CPU maxima of 80, 160 and 240 MHz, not 200 MHz. For a requested 200 MHz equivalent budget, measure CPU-only work at 240 MHz and allow a conservative 1.2x scaling margin; do not scale the QSPI wire time as CPU work or call this a measured 200 MHz result. Real runs at 160 MHz provide a stricter supported-clock check. PSRAM cache, radio load and code-fetch contention can defeat simple scaling. The implemented 80/240 MHz DFS policy uses CPU_MAX locks during rendering; its correctness and measured energy savings are separate questions. Iteration 1 exercised the lock policy without reported errors, but did not measure board current.

## Evidence and validation

`build/nos-bench/blurdiag.jsonl` contains MBLUR instrumentation absent from the reviewed snapshot, so its roughly 28–32.5 ms full-frame samples cannot certify this implementation. `orrery.jsonl` records historical CPU-bound frames around 178–238 ms. `docs/display-data/transport.md` describes the older main.c benchmark path. Current target acceptance needs fresh logs tied to the firmware build and working-tree revision.

At 40 MHz QSPI, ideal payload time is 466*466*2/(40e6*4/8) = 21.7156 ms for full-screen RGB565, before command overhead. Historical practical figures around 23.5–25.6 ms leave almost no 25 ms target headroom. Sparse damage, cached raster layers and overlap are therefore essential; firmware scheduling cannot manufacture bus bandwidth.

Host unit tests cover full-frame deadline accounting, floor violations, report-boundary intervals, idle gaps, histogram overflow, preference deferral, retry after write failure, debounce and clock wraparound. Firmware build and hardware stress runs remain necessary. Test cold boot, all faces, fast lists, iris/shared-axis transitions, notification overlays, Wi-Fi/BLE traffic, preference edits, minute-boundary pedometer persistence, AOD entry/exit, screen wake and trace-on/off. Require no floor violations in motion windows and report target misses honestly. Capture panel output separately for tearing/scan cadence. The first guarded app-only hardware iteration was tested and failed its timing target; the evidence below is not a qualification of later source changes.

## Official references

- [Espressif S3 power management](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/power_management.html): supported CPU frequencies, lock-based frequency control and DFS latency.
- [Espressif S3 external RAM](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/api-guides/external-ram.html): flash/cache coupling, external-memory limitations and DMA considerations. Later IDF features can change these constraints; use the project's actual SDK configuration.
- [Espressif S3 SPI master](https://docs.espressif.com/projects/esp-idf/en/v5.1.5/esp32s3/api-reference/peripherals/spi_master.html): DMA buffers, transaction overhead and bus behavior. Newer driver PSRAM flags are not assumed available through this LCD component.
- [Espressif screen-tearing guide](https://documentation.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/lcd_screen_tearing.html): GRAM display write/read timing and TE synchronization limitations.

## Recorded-run acceptance report

Run `python tools/nos_perf_report.py capture.jsonl --manifest run.json --output report.json`. The parser accepts raw serial, `{line,t}` records, and watchctl `{response,time}` records. The manifest must identify `source_revision`, a 64-digit `tree_sha256` for the dirty source snapshot, `firmware_elf_sha256` (matched against NOSBUILD when available), actual `cpu_mhz`, `instrumentation: "nos-device-v1"`, and `capture_scope: "animation"`. For a mixed NOSF trace, supply `animation_windows` with device `start_us`/`end_us` instead. NOSF's added `begin_us`, `completed_us` and `frame_id` establish completion intervals only between contiguous frame IDs; optional `trace_contiguous: true` asserts a gap-free trace when IDs are absent. Host receipt timestamps are retained as capture metadata and never substituted for hardware completion times.

The report distinguishes observed timing failures, estimated CPU-budget failures, incomplete evidence and a pass restricted to the recorded windows. A 240 MHz run can produce a 200 MHz estimate but cannot pass the measured CPU-budget check. `blur_coverage: "verified"` additionally requires `blur_coverage_evidence` pointing to a coverage audit; timing alone leaves coverage unmeasured. Legacy aggregate FPS and historical traces therefore cannot accidentally certify this working tree. Manifest declarations must be backed by the actual build/source capture; a supplied string is not independent proof of provenance.

The initial read-only COM3 capture `build/device-before.jsonl` reported older firmware `display-optimized-20260919-161-`. Before the later recovery backup, the device had independently changed to `display-optimized-20260919-162-`; the backed-up app descriptor and immediate preflash `info` both confirmed 162. These older MBLUR/PBLUR builds differ from the reviewed source and are not source-matched performance baselines. The new NOSBUILD reports ELF SHA256, CPU configuration, metric schema and largest free PSRAM block.

## Conservative idle DFS (implemented; device validation pending)

`firmware/main/nos_power.c` configures 240 MHz maximum / 80 MHz minimum with
light sleep disabled. `CONFIG_PM_ENABLE` is enabled, startup DFS is disabled,
and application initialization establishes fixed 240 MHz before creating locks
and enabling scaling. Failed initialization restores fixed 240 MHz and reports
the error. An unrecoverable failure to restore a known clock is fatal rather
than continuing an unqualified timing test.

The UI task acquires its own `ESP_PM_CPU_FREQ_MAX` lock before each iteration's
start timestamp. It retains the lock through input/wake policy, screen-on frames,
AOD entry/prepaint, core-1 raster rotation and the ordered DMA fence. Stable AOD
renders also retain it through completion. OFF polling, stable-AOD waiting and
normal frame pacing release the lock before blocking. Boot stays protected until
the first wait. The dot prewarmer owns a separate lock and releases it after its
finite bake, avoiding concurrent operations on one lock handle. `info` prints
`NOSPM` configuration, actual sampled frequency, held flags, balanced-call
counters and any error. A query executed inside a protected iteration naturally
samples 240 MHz; it does not measure idle-frequency residency.

Mock tests cover boot scope, duplicate begin/end calls, ON/OFF/AOD paths,
independent prewarming, setup failures and runtime lock failures. These establish
lock-state behavior, not hardware energy or timing acceptance. Run identical
cold/warm rendering, radios, wake and sensor scenarios after rebuilding; compare
DMA-completion deadlines, the 30 fps floor, input latency and measured board
current. Preserve the existing panel and bus clocks.

Espressif's [IDF 5.5 power-management guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/power_management.html)
documents the 80/160/240 MHz choices, recursive locks, increased interrupt
latency and separate SPI/I2C APB protection. The local implementation is in
`A:/makerspace/esp-idf/components/esp_pm/pm_impl.c`: RTOS locks retain maximum
CPU frequency while either core has runnable tasks, so actual 80 MHz residency
requires both cores to become idle. The [esp_timer guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/esp_timer.html)
documents timekeeping across frequency changes. Automatic light sleep stays out
of this change; USB, touch/IMU cadence, radio policy and wake latency need
separate validation before enabling it.

## Hardware iteration 1: measured failure, 2026-10-16

This iteration is frozen as local `build/artifacts/iteration1/` binaries, ELF and source-hash manifest. ELF SHA256 is `af9b52824464b00af7b9089da1275aefb0814de475d7def842ba5a5344a39391`; app binary SHA256 is `c6b0400d513bab1105db4e1bd5716784cf6031028178325f1e7962deedafb7ae`. Production file hashes were unchanged through its build. Later history/cache/launcher changes require a new build and measurement and are not covered by these results. The first artifact contains hashes rather than a full source archive; subsequent iterations archive the exact source/configuration before flashing.

Before writing, a complete 16 MiB recovery image was read in independently MD5-verified chunks and checked on disk with SHA256 `9ca81c66686918fbe4fac363fff60a605bf62c2c1aae76ce3acc71cd6122f519`. The private backup, chunks, manifest and logs stay in ignored local `build/recovery/`; no backup binary is committed. Native USB produced short packets with the original large-window reader; smaller packets, bounded 3-second reads, 64 KiB verified chunks and retries completed the backup. Short or corrupt packets were rejected rather than accepted as a backup.

The backed-up partition table matched the generated layout. OTA metadata selected valid `ota_0` at `0x20000`. Immediately before writing, the active app's full-slot MD5 still matched the recovery image. Only that app slot was written; the original bootloader, partition table, NVS, PHY and OTA metadata were retained and their MD5 hashes matched before/after the write. App readback matched the build. Captured cold boot reported the expected ELF fingerprint, enabled 80/240 MHz PM with no lock error, and no allocation failure. No 80 MHz panel trial was performed: QSPI remained 40 MHz.

Cold/warm home, launcher, settings, notifications and all eight faces were exercised. One native-USB command acknowledgement was missing while live 40 Hz idle telemetry continued; `info` still responded, and the remaining face cases were explicitly resumed in a separate capture. It was not reported as a firmware crash. After the complete tour, internal free RAM was 35,363 bytes, PSRAM free was 1,211,836 bytes and its largest block was 1,146,880 bytes. Reported retained caches used 4,461,976 bytes, including 2,662,400 UI blur bytes. Allocation failures remained zero.

| Motion case, trace enabled | Median end-to-end ms | Median history ms | Maximum end-to-end ms |
| --- | ---: | ---: | ---: |
| Launcher cold | 71.63 | 51.76 | 301.17 |
| Launcher warm | 71.89 | 51.98 | 147.55 |
| Settings warm | 69.08 | 48.87 | 157.13 |
| Notifications warm | 88.01 | 67.98 | 169.92 |

Trace-off summary runs confirmed that serial tracing did not explain the failure: launcher end-to-end maximum was 114.30 ms, settings reached 122.75 ms, and consecutive painted completions reached 122.81 ms. Motion-window loop rates fell around 22.5-32.4 Hz while static windows remained 40 Hz. Both the 40 Hz target and the hard 30 Hz completion floor **failed**. This is not a successful performance optimization result or a 200 MHz qualification. Trace-off notification content had already been cleared after the earlier card tests, so that window primarily measures transitions into an empty view.

Raw local evidence: `build/hardware-iteration1.jsonl`, `build/hardware-iteration1-remainder.jsonl`, `build/hardware-iteration1-traceoff.jsonl`, their summaries, and the `*-report.json` acceptance reports. The reporter identifies the artifact, confirms allocation success, fails timing checks and leaves blur coverage unmeasured. The dominant measured history cost motivated the subsequent aligned-load/bitmap fast-path work; those later changes need fresh hardware evidence.

## Runtime A/B diagnostics

The next build accepts `layers 0|1` and `history 0|1`, prints `NOSDIAG` state in `info`, and invalidates drawing when toggled. Inputs are clamped to booleans, values are not persisted, and both features default to enabled. These commands isolate costs during controlled diagnosis. Disabling history is not an acceptable way to meet the production motion-blur requirement. Always restore `layers 1` and `history 1` after an A/B run before collecting qualification data.

## Iteration 2 build and guarded hold

The next source was frozen and archived as `build/artifacts/iteration2/source.zip` with 265 source/configuration/dependency files, individual hashes, compile commands and project description. All archived input hashes stayed unchanged through the successful firmware build. ELF SHA256 is `3d017e4ef9936ee84ad992687837dcab9cda4806c692736bd2b8cb29cd150293`; app binary SHA256 is `4df5dae3f71b0d968c0aa931b151fd66750cc5685e3ab96ab0624bf6b1adc86f`. It includes the history fast paths, bounded launcher prewarming, mixed cached/native blur fixes and runtime diagnostic controls.

**Iteration 2 has not been flashed or hardware-qualified.** Immediately before its proposed app-only write, the expected-app guard found that the device had independently changed to build 163. Reading its descriptor and capturing a reset confirmed version `display-optimized-20260919-163-`, compiled at 23:43:52 on October 16, with ELF SHA256 `bb76b42caf44140d748d4ad36777ade917eb64dce9e911e56059aed7820d760d`. This differs from both retained test artifacts. The exact expected-prefix MD5 also differed; the guard stopped before any write. No padding or header-adjustment assumption was used to explain away the mismatch.

The observed 163 app was left intact and booted successfully. Local evidence is `build/recovery/active-after-preflight-stop.json`, `preflight-stop-info.jsonl`, and `test-app-iteration2-flash.log`. The source improvements have test/build evidence, but no iteration 2 hardware FPS, minimum-frame-rate or 200 MHz-equivalent result is claimed.

## Latest-face integration, 2026-10-17

At the user's request, the optimization delta was rebased onto watch-face commit `e4700d5`; the initial rebased integration is `63c65c5`. It preserves SD/BLE transfer, raw IMU recording, tracked PSRAM allocations, battery load allowances and panel sleep handling alongside the DMA fence, timing metrics and PM locks. Explicit restart/power-off now performs a checked preference flush; normal awake saves remain deferred. A failed shutdown checkpoint retains dirty data and keeps the device running for retry.

Before any rebased test write, a fresh read-only identity capture found build `display-optimized-20260919-164-`, with 26,919 internal bytes and 2,101,324 PSRAM bytes free. A new complete recovery read was started because the earlier build-162 backup cannot restore this newer firmware. This identity capture is local `build/recovery/rebased-prebackup-info.jsonl`. The first rebased build succeeded, but its input verifier rejected finalization because `nos/ui_blur.c` changed during the build. Its artifact is therefore unqualified and has not been flashed. A new source freeze, regression pass, source archive and build are required before testing.

The fresh 16 MiB recovery image completed with all 256 chunks independently MD5-verified and whole-file SHA256 `10cb6e2a2d9e77a7d9473717db414250be2c2d8c27132e53863df68395444570`. Its partition table matches the rebased build; CRC-valid OTA metadata selects `ota_0` at `0x20000`. The extracted complete active slot has SHA256 `023c2926a9196c3d047b79fd0f46ff04386cb5a3ad0ec68c9c6a3c4fff9e12ad`, and esptool validates both image checksum and validation hash. Its descriptor identifies build 164, compiled October 17 at 00:13:25, ELF SHA256 `a894ef48dddbcb111adca7350307957f79328e9f87585638f42c0812e887e514`. After backup the device again answered `info` as build 164. Recovery files remain private ignored artifacts under `build/recovery/pre-rebased-*`; no rebased test image has yet been written.

## Hardware iteration 5: rebased measurement, target failed

Iterations 3 and 4 were not flashed: iteration 3 changed during compilation; iteration 4 was reproducibly built but failed the simulator memory gate. Iteration 5 archived 301 production/configuration/dependency files under `build/artifacts/iteration5-rebased/`, tree SHA256 `aab7cd3b4e4d33c74aa40182c1eb309ec44232586377431a51d8aa224e8fc830`. Its finalized ELF SHA256 is `27c3c70f67bbee4bcd6ea821770891c0b75a04e5270c2b9b37217cc676e01ec7`, and app SHA256 is `34a0a7b6862ea56544b5b15bcd51f823a999eb29e1a0c856d1f5e662bd328198`. Archived source hashes stayed unchanged through compilation.

After simulator regression approval, the live build-164 whole-slot MD5 and boot/partition/OTA checks matched its fresh backup. Only the confirmed active app partition was written. App readback matched, and MD5 hashes of **every byte outside that app partition** matched before/after, including NVS, PHY, bootloader, partition table, OTA metadata, the other app slot and storage. Captured reset identified build 165 with the expected ELF, enabled PM with no error, both diagnostic features enabled and no reported allocation failure. These are integrity/boot checks, not timing qualification.

| Trace-enabled first/repeated entry | Median end-to-end ms | Median history ms | Maximum end-to-end ms |
| --- | ---: | ---: | ---: |
| Launcher first | 25.02 | 17.41 | 40.83 |
| Launcher repeated | 25.67 | 18.15 | 58.43 |
| Settings first | 28.44 | 17.75 | 742.24 |
| Settings repeated | 28.18 | 17.52 | 709.88 |
| Notifications first | 31.61 | 13.67 | 70.78 |
| Notifications repeated | 26.98 | 16.32 | 58.21 |

The settings entry stall occurs mostly before the work-only timer: one frame measured 742.235 ms end-to-end but 60.211 ms work, 1.735 ms tick and 27.055 ms history. Trace-off runs independently reproduced 730.21 ms settings end-to-end maximum. Other trace-off maxima were launcher 60.48 ms and populated notifications 70.50 ms; consecutive painted completions reached 60.54, 71.47 and 70.57 ms respectively. Both target 40 fps and floor 30 fps **failed**. History is substantially cheaper than iteration 1, but full-panel transition history still costs roughly 17-27 ms and remains a bottleneck. No 200 MHz-equivalent qualification or complete blur-coverage claim follows from this run.

All nine faces were visited. One missing native-USB acknowledgement stopped the first capture before face 6; the device continued 40 Hz idle telemetry, a separate `info` probe responded, and faces 6-8 were explicitly resumed. The full tour reported tracked peak 6,510,335 of 6,815,744 bytes, internal free 30,407 bytes, PSRAM free 1,661,552 bytes, and zero allocation failures. Orrery's first visit reached 206.50 ms end-to-end. A separate targeted gesture attempt reached one planet card during the case labeled `orrery_rings`: `info` confirms depth 2, top ORRERY CARD, tracked 6,052,898 bytes and no `orrery_card.c` allocation failure. Card frames 8072-8076 measured roughly 69-174 ms end-to-end, with 42-114 ms tick work. Later scripted card scrolling instead reached CONTROL/HOME and must not be treated as card-scroll coverage. All-card allocation coverage remains incomplete; gesture case names alone are not evidence that the target was reached.

Offline diagnosis found that the serial `btn` command sets the button raw state down, blocks the UI thread for 80 ms, then sets it up before the next engine button sampler runs. This cannot reliably dispatch a press. The following `home` closes the card without locking Orrery; a second long hold then toggles an already unlocked face back to locked. Subsequent vertical swipes legitimately open CONTROL. The initial unlock and card entry did work, so there is no evidence of a coordinate-rotation or hold-duration failure. Further scripted Orrery tests need observed lock state, must avoid assuming that this serial button command locks it, and must confirm top ORRERY CARD before scrolling.

Local raw evidence is `build/hardware-iteration5*.jsonl`, including separate boot, acknowledgement probes, resumed faces, trace-off and targeted attempts; write integrity is `build/recovery/iteration5-flash-verification.json`. A second missing acknowledgement stopped the first targeted attempt before interactions and was separately probed before retry. Final cleanup restored face 1, `layers 1`, `history 1`, trace/perf/awake off and removed the temporary local test notifications. COM3 was closed. Subsequent source changes require a new archive, build and hardware run.

## Hardware iteration 6: entry stall removed, timing still fails

Iteration 6's exact 302 source/configuration/dependency files are archived under `build/artifacts/iteration6-rebased/`. Tree SHA256 is `ef724f52253f9e46ffb5fddc85410c3e0378670fbc65fe2feace218cec5a63f7`; ELF SHA256 is `ed87d3295b7d00f9195ba508b200f63cad7474f55f850871872ab7292b22586d`; app SHA256 is `adaec6f4ee148a7dfc4e19ccf9b97c6113e02095c475e9afb98fd2d4f5d1153f`. All archived input hashes matched after the successful firmware build. The version string remains build 165 because CMake was not reconfigured; the ELF hash distinguishes the two measured builds.

Before this app-only write, both the live iteration-5 app prefix and its **entire slot** matched expectations. The complete expected slot was synthesized from the verified build-164 recovery slot, the exact iteration-5 app and its recorded padded write extent; its MD5 was `718cde081d014ae54298fb46dfe8db42`. The new app readback matched and every byte outside the selected app partition again retained its before/after MD5. Captured reset confirmed the expected ELF, PM enabled with zero error, both rendering diagnostics enabled and no allocation failure. The original build-164 full recovery image remains unchanged.

Trace-off Settings-first and repeated-entry tests no longer show the 700 ms entry stall. They still violate both frame requirements. The following maxima exclude summaries received during marked query phases; query boundaries and row `phase` are recorded so diagnostic printing is distinguishable from gesture work. One-second summaries can span nearby phases and are not exact per-gesture boundaries.

| Trace-off case | First/repeated maximum end-to-end ms | First/repeated maximum painted-completion interval ms |
| --- | ---: | ---: |
| Settings | 54.50 / 44.13 | 58.18 / 48.04 |
| Launcher | 37.17 / 37.07 | 41.99 / 43.24 |
| Populated notifications | 65.78 / 56.06 | 65.84 / 56.13 |

A separate reset preceded detailed tracing. Excluding query-phase rows, first/repeated entry median end-to-end times were launcher 23.66/22.32 ms, Settings 31.17/22.98 ms and notifications 27.24/22.91 ms. Corresponding history medians were 13.21/13.05, 12.27/6.43 and 8.62/10.32 ms. Settings frames processing roughly 151,000 history pixels still spent about 12 ms in history and 26-28 ms in total rasterization. These measured windows do not satisfy hard 40 fps or minimum 30 fps, and do not certify a 200 MHz-equivalent budget or complete blur coverage.

The full nine-face tour completed without a crash, allocation failure or missing acknowledgement. Its tracked peak was 6,193,456 of 6,815,744 bytes; internal free was 28,151 bytes and PSRAM free 1,983,420 bytes. The new read-only `orrery state` query observed lock/view/pan/zoom/rings/menu/time offset without allocation. The corrected targeted protocol confirmed unlock, panning, open rings, centered lock/unlock reset, and top ORRERY CARD before and after both scroll gestures. Card-case trace rows excluding queries had median end-to-end 46.00 ms and maximum 171.89 ms; tick work remains a major bottleneck. Targeted-run tracked peak was 6,214,257 bytes with no allocation failure. This covers one observed card and its scrolling, not every planet-card mode.

The serial button implementation now schedules a nonblocking pulse across UI samples and combines it with physical input. Its host state-machine tests cover normal timing, delayed first sampling, wraparound, overlapping requests and a held physical button. The corrected Orrery protocol uses observed lock state rather than assuming a button press; it does not constitute hardware validation of serial-button behavior.

Local evidence is `build/hardware-iteration6-boot.jsonl`, `hardware-iteration6-traceoff.jsonl`, `hardware-iteration6.jsonl`, `hardware-iteration6-orrery.jsonl` and `build/recovery/iteration6-flash-verification.json`. Query commands occur outside intended gesture intervals and carry explicit phase markers. Cleanup confirmed Orrery locked, restored face 1 and both rendering features, disabled trace/perf/awake, removed temporary notifications and closed COM3. Further source changes require a new artifact and measurement.

## Hardware iteration 7: partial measurements, touch-stack fault, restored

Iteration 7 archived 303 exact input files under `build/artifacts/iteration7-rebased/`: tree SHA256 `4f25f489f43fcee4f2faf2b932bbf1868de3940cc70ef29bdf7eb583c5db2518`, ELF SHA256 `97604707faadb973fd8224fbac0b2465b0798e6ed4e767e308c6da779a366899`, app SHA256 `9f6b850c4b8368b4a6495f53aa3058eefc2051c4f3785da99060165f5011d3be`. Input verification passed after compilation. Later working-tree edits are not part of this measured firmware.

The first preflight stopped before writing because its synthesized previous-slot model incorrectly treated the padded transport-write count as the erase extent. Read-only investigation proved the distinction: iteration 6 erased through slot offset `0x269000`, while the host transmitted padded blocks through `0x26c000`. The 12,278 differing bytes at flash `0x289000`-`0x28bfff` exactly retained the prior slot rather than assumed FF. Independently read-back 16 KiB tail bytes and the complete live slot MD5 both matched the corrected sector-erase model. No unknown difference was waived. The complete guard was repeated successfully; iteration 7 app readback matched, and every byte outside the app partition remained unchanged. Initial boot matched the expected ELF, PM/control settings and zero allocation failures.

Trace-off Settings motion still violated both timing requirements; one recorded second measured 29.40 Hz, maximum end-to-end 42.15 ms and 16 floor violations. Two acknowledgement gaps stopped separate trace-off captures. The commands had visible side effects: in one case HOME transitioned to SETTINGS despite missing both OPEN SETTINGS and DONE. These partial captures were retained and explicitly probed rather than silently continued or counted as complete tours.

Short uniform-history A/B runs retained `layers 1` and `history 1`; only the exact uniform shortcut changed. Both launcher orderings completed. History ns/pixel medians changed off-to-on 86.30 to 83.59 and on-to-off 75.15 to 85.98, but dirty-pixel distributions differed substantially. In the closest common 140,000-160,000-pixel range, on/off median history was 12.755/12.927 ms while end-to-end was 22.255/21.470 ms. This does not establish useful end-to-end improvement. Settings off-to-on completed at roughly 27,900 history pixels: median history 3.17/2.91 ms and end-to-end 9.87/9.22 ms. The reverse-order off segment did not reach measurement. Avoided-read counters report logical PSRAM accesses, not measured physical bus traffic; no avoided writes were reported in these samples. Local `build/hardware-iteration7-ab-summary.json` preserves the partial rollup.

**A real device fault ended iteration 7.** At 01:32:22 IST, during Settings A/B preconditioning with uniform shortcut off, the first failure was `***ERROR*** A stack overflow in task touch has been detected.` A corrupted backtrace followed. Offline decoding of the exact archived ELF identifies the FreeRTOS stack-overflow hook; the corrupted stack does not establish the originating call chain. The touch task had a 2,560-byte stack, priority 5, pinned to core 1. Multiple I2C transaction-timeout logs preceded the fault, starting around uptime 91 seconds; their causal relationship remains unproven. All further scenarios stopped. No iteration-7 full-tour, card or performance acceptance result is claimed.

The verified original build-164 **complete app slot was restored**, its full readback matched the recovery image, and MD5 of all outside-app bytes remained identical. Captured reset confirmed READY build 164, face 1 and zero tracked allocation failures. Recovery evidence is `build/recovery/iteration7-fault-restore164-verification.json` and `build/hardware-iteration7-restored164-boot.jsonl`. Raw fault evidence remains in `build/hardware-iteration7-ab-settings.jsonl`; both initial failed-model preflight and corrected write evidence remain under `build/recovery/iteration7-*`. COM3 is closed. Further test firmware requires explicit resolution of the touch-stack fault and a new source/build gate.

## Hardware iteration 8: touch-stack evidence and remaining timing misses

Iteration 8 archived source tree `6c00cfab3248436b42146f94e4cd5236c7860de6cffe709d325d06b596dd80cd`, ELF `606fc76fd9ad45bdf33939c2bb2e03aac07f85e928338cd5eebbb7b62a94c416` and app `657243ef19c93c2aab853b0f1f9e6bd3d929b23fb2a04a305848e41f577eebde` under `build/artifacts/iteration8-rebased/`. Source verification passed after compilation. Guarded app-only writing began from the verified restored build-164 slot; app readback and all outside-partition MD5 checks passed. No other flash region changed.

The touch task stack increased from 2,560 to 4,096 bytes. Zero-deadline injection initially returned errors without deep timeout logs, so the injected counter alone was explicitly rejected as proof. Later batches produced real driver timeout logs, and touch low-water reached 1,484 bytes without overflow. The injection run ended on an acknowledged-output gap after about 66 seconds; it is not a completed five-minute injection test. After stopping injection, only 13 of the next 1,220 transactions succeeded over 12 seconds. That interval alone did not establish the cause of poor communication.

A normal reset then preceded a separate **322.34-second ordinary UI/USB stress run without injection**: 1,370 painted diagnostic frames, no crash/corruption, no allocation failure, no acknowledgement gaps, and minimum touch-stack free 2,732 bytes. Reads advanced by 32,194, including 20,919 successful transactions (64.98%). Errors and subsequent recoveries occurred with no injection. This establishes observed stack headroom above the preferred 768-byte margin in these workloads, not physical-touch qualification. Serial gestures bypass the hardware controller. The [FT3168 datasheet, section 2.3](https://files.waveshare.com/wiki/common/FT3168.pdf) documents shared-bus communication loss in Monitor/Sleep after another slave access, periodic I2C state-machine clearing, and physical-touch wake to Active. Thus INVALID_STATE counts cannot independently prove timeout, bus corruption or failed physical-touch recovery. Baseline and injection evidence remain separately identified in `build/hardware-iteration8-touch-*.jsonl` and the baseline `.gate.json`.

The bounded diagnostic tour visited all nine faces; the final face-8 information response lost its acknowledgement and was not silently accepted. A separate verified Orrery protocol confirmed card entry and both scrolls. Card-case median end-to-end was 38.215 ms and tick was 1.187 ms, versus iteration 6's roughly 42 ms median tick; maximum end-to-end remained 167.625 ms. Tracked peak was 6,511,863 bytes with zero allocation failures; final internal free was 24,299 bytes and PSRAM free 2,139,064 bytes. A trace-off attempt stopped immediately on DONE without a fresh information response, so no iteration-8 trace-off acceptance window is claimed.

Excluding query-phase rows, first/repeated entry medians were launcher 22.96/22.70 ms, Settings 25.72/25.67 ms and notifications 27.21/23.77 ms. Their maxima were 39.21/37.40, 42.98/44.11 and 73.20/53.78 ms. These remain timing failures rather than a 40/30 fps qualification. First cold Activity entry rendered nine retained bands with 0.475 ms layer recording and 9.019 ms layer raster; its frame still took 42.566 ms end-to-end, including 15.067 ms history. The internal-output borrowing change therefore has measured stage evidence without implying a passed deadline.

The initial target basis export was rejected: a repeated row differed despite looking syntactically valid. Subsequent CRC-verified export conclusively showed that old row 3856 had acquired row 3857's last word through output loss/merging. No untrusted iteration-8 basis asset was imported. Diagnostic streams can lose bytes independently of whether the device processed a command. The IDF USB console uses a default 256-byte TX ring here and a 50 ms drain timeout followed by dropping; host disk flushing was removed from the per-line read path to reduce avoidable stalls, without treating that adjustment as a firmware performance gain.

Raw scope is `build/hardware-iteration8.jsonl`, `hardware-iteration8-orrery.jsonl`, the rejected initial trace-off capture, touch workload captures and `build/recovery/iteration8-flash-verification.json`. Face 1, normal rendering controls and nonpersistent diagnostic defaults were restored before the next image. The original 16 MiB build-164 recovery image remains unchanged.


## Iteration 9: asynchronous exposure controls and verified target export

The archived iteration-9 source tree is `1476dc175c40c2675a480f00490e40b946a245a9c9ef909b78d41a1e321d2d3a`, ELF `f23690874a9d8451fe51a4c138a48859fbdd361949297dbff435621e30601ad2`. Guarded app-only installation preserved all bytes outside the active slot. Target QZ validation reported 65,536 cases and zero mismatches; observed touch/flush stack minima were 2,828/1,848 bytes. No forced touch faults were injected.

Same-image Settings worker comparisons did not establish a benefit. With constant full-screen damage (179,188 history pixels), worker off/on median end-to-end was 24.568/27.892 ms; reversed on/off was 28.257/27.732 ms. These diagnostic traces include console overhead. Trace-off Settings summary rates remained about 36–39 painted fps and included missed target deadlines and 30 fps floor violations. Unqualified timing claims are therefore unchanged. Packed-history reversed Settings controls had closely matched median damage (~152,000 pixels): packed on/off median history was 13.463/14.713 ms and end-to-end 27.021/27.736 ms. This modest stage improvement does not establish minimum cadence.

Launcher reverse worker controls retained LAUNCHER: worker on/off had 86/92 samples, median end-to-end 22.493/20.919 ms and maxima 25.211/23.010 ms, with median damage 151,284/145,012 pixels. The forward worker-off segment navigated into NOTIFICATION during preconditioning and is explicitly excluded from launcher comparisons. Other paired scopes were not completed before the next build. Capture ACK gaps and query phases remain preserved and excluded from natural-animation evidence.

The target basis export completed all 3,999 rows of 13 float words with an IEEE CRC32 covering each little-endian index and its words. There were zero malformed rows and zero ACK gaps. Normalized word SHA256 is `279b33ac73e942d7002a0bd1ec3607b269d12980dffd9db877f95b7fb350ed42`; raw data and its pinned source/ELF manifest are in `build/orrery-basis-iteration9-target-crc.*`. This verified export supersedes the rejected iteration-8 stream. Cleanup restored face 1, full 0, worker/packed/uniform/QZ 1 and disabled awake/perf overrides.


## Iteration 10: verified basis activation and cold card stage profile

The guarded app-only `iteration10-final` image (not the earlier experimental artifact) has source tree `ad4ddfcd3e876a7fdb1157326e5b561edcab922d253e63cba05fa0f6c9fe8be6`, ELF `27e60d6b398b1362977b79310aa28cb8a1e352a3be4cbaf3f024551611c8be3c`, BIN `4e817a808058160ba3fc2f4f0d331dcae902ece5dd1c30c03111b0e215406312`. Exact prior whole-slot guard and outside-region preservation passed. Worker defaults off. Startup PREBAKE took 790 ms, reported zero basis mismatches, ready 1 and 2,152 bytes task stack minimum. Touch/flush minima were 2,828/1,848 bytes. QZ validated 65,536 cases with zero mismatches. The explicit dates validator ran once outside timing (362.293 ms, zero mismatches). After first Orrery initialization, compact state and check confirmed actual basis backend 1, ready 1 and zero mismatches.

Trace-off Sun card profiling exposed the remaining costs. Before cold entry the profiler was reset/enabled; allocation was 2.920 ms, lighting 1.738 ms, preparation 2.021 ms and fopen 1.654 ms. The loader eventually accumulated 32 fread calls totaling 1,371.268 ms, with a 44.031 ms maximum call. Initial LUT work was 102.957 ms over three calls (maximum 64.376 ms), and first globe generation took 50.286 ms. Later horizontal globe controls added eight globe calls totaling 439.134 ms. Their trace-off summaries had maximum end-to-end 66.01/65.76 ms and completion intervals 66.08/65.82 ms. The hard timing requirement remains failed. These are accumulated stage counters; allocation is inclusive and stages must not be blindly summed. Queries/reports run outside gesture windows.

Sun card entry and both scroll/globe cases were verified to remain ORRERY CARD. A subsequent bounded trace-off capture advanced through seven body transitions and confirmed Saturn from ring-stage `ob=7` telemetry. First Saturn ring generation took 29.038 ms; after two tilt controls rings accumulated nine calls/275.923 ms with maximum 50.664 ms. Globe maximum reached 77.945 ms on Saturn (stage 3). All selections remained ORRERY CARD with strict acknowledgements; the profiler is cumulative, so reported maxima retain the body associated with that maximum rather than necessarily the latest body. Final tracked peak was 6,432,482 bytes with zero allocation failures; touch/flush minima stayed 2,828/1,848 bytes. Raw extended scope is `build/hardware-iteration10-final-body-profile.jsonl`. No fault or allocation failure occurred. Cleanup restored face 1, profile 0, full 0, worker 0 and normal blur controls; awake/perf overrides were disabled. Raw scope is `build/hardware-iteration10-final-{boot,prebake,startup,card-profile}.jsonl` and `build/recovery/iteration10-final-flash-verification.json`.


## Panel clock rating and readback research (no device changes)

Keep QSPI at 40 MHz. The manufacturer CO5300 datasheet [V0.00 hosted by Espressif](https://dl.espressif.com/AE/esp-iot-solution/CO5300_Datasheet_V0.00.pdf#page=107) and [V0.01 linked by Waveshare](https://files.waveshare.com/wiki/common/Co5300_Datasheet.pdf#page=107), section 6.4, printed page 107, both specify:

| Timing | Limit |
| --- | --- |
| Write clock cycle | minimum 20 ns (50 MHz ceiling) |
| Read clock cycle | minimum 100 ns (10 MHz ceiling) |
| Write clock high / low | each minimum 6.5 ns |
| Read clock high / low | each minimum 45 ns |
| Clock rise / fall | each maximum 3.5 ns |
| Input setup / hold | each minimum 4 ns |
| CS setup / hold | each minimum 10 ns |

The table's note 3 explicitly caps quad RGB888/RGB666/RGB555 at 50 MHz; it provides no 80 MHz exception for RGB565. The general 20 ns write cycle also excludes 80 MHz (12.5 ns). This is controller timing evidence, not independent qualification of the installed module. The [official Demo V3](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-AMOLED-1.43-Demo-V3.zip) Arduino `07_LVGL_Test/esp_lcd_sh8601.h` lines 90/102 sets 40 MHz. Its factory example selects SH8601 only for DA=0x86 and otherwise the CO5300 initialization. Our `LCD_USE_SH8601=0` is a selected working profile; boot `id=0xff init=CO5300` is not positive controller identification. The probe's all-FF GPIO reads with pull-ups and GPIO-operation success cannot establish a responding panel. No 80 MHz trial is planned; the earlier owner-rejected trial remains documented separately.

Potential integrity diagnostics are documented, but none are validated on this device. CO5300 page 28 describes register reads using opcode03h, address `{00h, command, 00h}`, returning data on bidirectional SIO0. V0.01 RAMRD2Eh (page164) reads frame memory after CASET/RASET; RAMRDC3Eh (page179) continues reads, although its prose contains apparent write/read copy errors and the exact packing requires validation. An independently working register path and known-pattern readback at the rated slow read clock are prerequisites. The current all-FF identity probe does not meet them.

V0.01 RDFCS AAh (page212) and RDCCS AFh (page213) expose 8-bit checksums over user-command registers and frame memory, with at least150/300 ms after the last write respectively. The datasheet does not provide a host-computable checksum algorithm in those command descriptions; they could support carefully controlled comparison with a trusted baseline, not a collision-free pixel integrity certificate. RDDSDR0Fh (page141) reports a checksum-comparison flag. RDNUMED05h concerns DSI parity errors; the document's MIPI packet CRC is not evidence of QSPI payload CRC checking. Successful SPI submission or DMA completion proves host transport completion, not panel RAM contents or optical correctness. No reads, writes, resets or COM3 access occurred for this research.


CO5300 V0.01 MADCTL36h (printed page172) documents MY row increment, MX column increment and RGB/BGR order only; bit5 is marked don't-care, with no MV row/column exchange. RDDMADCTR0Bh agrees. The bundled Waveshare panel driver `panel_sh8601_swap_xy()` returns `ESP_ERR_NOT_SUPPORTED`. A supported 90-degree hardware axis swap is therefore not established; it cannot currently replace software rotation. This conclusion comes from the same [manufacturer datasheet](https://files.waveshare.com/wiki/common/Co5300_Datasheet.pdf#page=172) and local driver inspection, with no panel command changes.


## Iteration 11: asset worker and lighting coefficient cache

The archived `iteration11-async` source tree is `d1c40548919e99ce16fc1b74e9d9a1cdb40762c86ee191d52563c6f77bf0c47d`, ELF `6793f05a01630ed2d8a99ea969b16e9f4641c20110f024b9b37c4ea0a74553ca`, BIN `f4c9c6754d2da579ca6afd76033f277ca3344bfac1ec725a0c3bd9d9b14e7184`. Guarded app-only installation matched the entire prior iteration-10 slot and preserved all outside bytes. PREBAKE reported770 ms, zero basis mismatches, ready1 and2060-byte task stack minimum. QZ65536 and post-card coefficient512 checks both had zero mismatches. Basis backend1 was confirmed after initialization. Worker defaults remain exposure0, assetworker created on demand, lighting cache1.

Cold Sun loading observed assetstate2 with1,048,576 owned bytes, followed by idle0/owned0. Asset worker minimum stack was4388 bytes initially and4260 after later loads, with zero create failures or admission skips. UI fread profiling had zero calls; separate CARDASSET reported32 worker reads totaling1544.026 ms, maximum56.881 ms. Thus I/O moved off the UI thread; it did not become faster. Cold globe work remained83.339 ms across two calls, maximum42.952 ms. Assetidle alone does not prove successful texture contents: current diagnostics omit result code and card tex/night/ring success flags, so asset-success coverage remains limited.

AOD cleanup explicitly observed an active1 MiB load before entering pwr1, then idle/zero ownership afterward. Initial exit/reentry and OFF cases did not establish busy state before transition and are not accepted as active-load cancellation coverage. No allocation failure or crash occurred. Raw startup/lifecycle evidence is `build/hardware-iteration11-async-{startup,lifecycle}.jsonl`.

Settings baseline used normal layers/history/uniform/packed/light enabled and exposureworker0. Seven trace-off summary windows contained258 painted frames, rates38.61–39.60 fps, maximum end-to-end31.96 ms and completion interval34.85 ms, with141 target deadline misses and four30 fps floor violations. Summary boundaries include settling. A separate78-frame diagnostic window had median/max history10.552/14.863 ms, rotation5.276/5.661 ms and end-to-end24.298/33.114 ms; median history damage152,020 pixels. Final internalfree20,995 bytes, touch/flush stack minima2828/1848 bytes. These measurements still fail the hard timing requirement. Raw scope is `build/hardware-iteration11-async-settings-baseline.jsonl`; no optical correctness or200 MHz qualification is inferred.


With host builds stopped and PERF/trace disabled, same-image Sun lighting controls completed in both orders with eight globe calls per segment. Cache on/off mean stage time was44.039/54.644 ms; reversed off/on was54.598/44.042 ms (about19.4% less globe CPU time). Maximum on/off was44.238/55.109 ms in the first order and54.914/44.743 ms reversed. This is a repeatable stage improvement, still beyond the30 fps frame budget. A later read-only state ACK gap during Saturn navigation stopped that stream; the missing response was preserved, and a separate continuation validated actual Saturn via stage ob7. Earlier host rebuild saturation and persistent USB output loss are capture confounds, not evidence of a firmware fault.


The completed Saturn continuation confirmed ob7 in globe telemetry. Cache on/off mean globe time was51.533/59.250 ms; reversed off/on59.202/51.404 ms. On segments had eight calls and off segments six, because slower processing changes gesture sampling; this limits direct comparison, although both orders favor the cache. No30 fps claim follows. Tracked peak was6,440,674 bytes with zero allocation failures. Sources: `build/hardware-iteration11-async-light-ab-quiet.jsonl` and `hardware-iteration11-async-light-saturn-resume.jsonl`. Initial incomplete streams and ACK gaps are retained separately, not combined into successful pairs.

A later explicit-prerequisite lifecycle run supersedes the incomplete cancellation cases: active state2/owned1 MiB was observed before HOME, reentry again reached state2 and then idle/zero ownership; a separate active state2 load entered OFF and was observed pwr2 with idle/zero ownership afterward. Asset stack minimum stayed4260 bytes with zero create failures/admission skips. Final face1 and default controls were confirmed before iteration12. This establishes ownership/reaping behavior, while the iteration11 asset-success/result-code limitation remains. Raw scope: `build/hardware-iteration11-async-lifecycle-confirmed.jsonl`.


## Iteration 12: 64-byte cache-line comparison

This section is reconstructed from preserved raw captures and its previously authored append text; historical sections through iteration 11 were recovered by replaying the original successful transcript edits.

The `iteration12-cache64` artifact has tree `6a2cf27bd3652b890e30cf8331ac76a354a8d45e2074af535a4ecfe085a4b4c7`, ELF `7d65d03d27400105bf76b5be48706c03cee9960b0a355a3ffacb2b30e644830d`, BIN `1368af141b7662334daf9dd615e3b546d45cf5c24b7b8164e557cbecb53774c9`. Whole-slot prior-11 guard, app readback and all outside-region checks passed. Target NOSBUILD confirms32,768-byte data cache with64-byte lines; CPU, PSRAM and panel clocks are unchanged. PREBAKE791 ms/zero mismatches/ready1, QZ65536 and coefficient512 checks passed. Asset worker minimum4396 bytes; touch/flush2860/1840 bytes initially. Cold Sun completed with tex1, stage3, no pending request, resultcode0/adopted1/failed0. Cold globe was77.854 ms across two calls (maximum39.510 ms), versus83.339/42.952 ms for iteration11. Initial LUT maximum65.715 ms and async read total1529.949 ms remain expensive.

Identical Settings script: eight trace-off windows/285 painted frames, rates38.37–39.59 fps, maximum end-to-end31.99 ms/completion35.42 ms,113 deadline misses and one floor violation. Different one-second window alignment prevents direct aggregate-count comparison with11. Diagnostic median history improved10.552→9.461 ms at nearly equal median damage152,020→151,988 pixels; rotation stayed5.276→5.267 ms and end-to-end improved24.298→23.896 ms. This single cross-image pass supports a modest hot-path candidate, not a40/30 fps qualification or a proven general benefit.

Actual loaded flags were verified on target: Earth ob3/stage3/tex1/night1/pending0, Saturn ob7/stage3/tex1/ring1/pending0. UI-adopted results reported11 completed, zero failed/cancelled/stale, latestcode0/adopted1. Tracked peak6,440,674 bytes with zero allocation failures. Saturn tilt work still had ring/globe maxima79.062/98.335 ms; those are not matched32/64-byte controls, so the cache-line change is not established as their cause. Files are `build/hardware-iteration12-cache64-{startup,settings-baseline,bodies}.jsonl` and the guarded-flash report. Optical appearance remains unverified.


Matched quiet warm texture controls kept light1, exposureworker0 and PERF/trace0. Sun's two repeats had10 globe calls each, mean39.042/39.674 ms and maxima39.163/44.440 ms; iteration11 light1 had eight calls each at44.039/44.042 ms mean. Faster rendering changes gesture sampling, limiting a strict per-frame comparison. Saturn had eight calls per repeat in both images: iteration12 means48.013/47.413 ms and maxima53.387/47.848 ms, versus iteration11 means51.533/51.404 ms. These texture-heavy mean results do not show a regression from64-byte lines, but single cross-image sequences do not qualify all tails. The timing floor still fails. A read-only state ACK gap interrupted navigation; a separate continuation first verified loaded ob6, advanced once and verified loaded ob7. No gesture was silently retried.

Final cleanup confirmed HOME/face1, layers1/history1/uniform1/packed1, exposureworker0, light1/QZ1, full0/profile0/trace0 and awake/perf0. Asset state was idle/owned0, results completed21/failed0/cancelled0/stale0, latest code0/adopted1. Final internal free20,923 bytes, touch/flush/asset stack minima2828/1840/4396 bytes and tracked peak6,440,674 bytes with zero allocation failures. COM3 was closed. Warm evidence is `build/hardware-iteration12-cache64-warm-control.jsonl` and `hardware-iteration12-cache64-warm-saturn-resume.jsonl`. No later image was flashed as part of this comparison.


## Iteration 13: matched Sun specialization and Q16 shader controls

The authoritative archived `iteration13-final` source tree is `899714a946bdb90c9ee9bf4d8a9bdd918ea77caacc8210fb0910f2099ac0176a`, ELF `0b4d0b474b73345bf486084cf98e361c29cb7bcdb4419facb5ccc2b17b9abf57`, BIN `d4a9830134efe6119abe815f8e6ad8f1356a1af8f9d3967dcba9291906dff37b`. Production source changes for the next iteration do not describe this running image. The ignored guarded flasher now supports an immutable artifact directory and independently authorized binary SHA256; it verified the ZIP/ELF/tree hashes and all 317 archived source files. Exact prior-12 prefix and whole-slot guards, app readback and all outside-app hashes passed. Original build-164 recovery remains unchanged. No build or SD write was performed for this capture.

Target startup: PREBAKE 804 ms, basis/shade mismatches zero, shade ready 1/rejection 0 and bake-task stack minimum 1,932 bytes. After positive card/coefficient allocation, light check passed all 512 coefficients and card_shade_check reported zero original-formula mismatches, ready 1/rejection 0 (24.228 ms, outside timing). Sun texture completion/adoption was confirmed. Cold globe work totaled 46.059 ms across two renders, maximum 25.951 ms. Clocks remained actual 240 MHz CPU and 40 MHz panel, cache 32 KiB/64-byte lines. This does not qualify the 200 MHz-equivalent budget.

Shader isolation used PERF/trace off, light/QZ and normal blur controls enabled, exposure worker off, fully loaded textures and settled PHOTO pose. Mode cycling forced an actual mode-2-to-mode-0 render after warming each selected combination; toggling flags alone does not dirty a static globe. Accepted measured samples had exactly one globe call and matching observed spin 0, tilt 0, latitude 6, radius 106 for Sun. The original criterion requiring zero LUT invocations was amended after inspecting the archived wrapper: cached guard hits are also profiled. One guard invocation of at most 100 us was allowed with unchanged pose and no allocation, prepare, rings, preview or file-read work. Original raw rejection statuses are preserved; amended classifications are separate. These are shader-stage measurements, not whole-frame or optical guarantees.

| Sun specialization | Q16 shader | Matched samples | Mean globe ms | Range ms |
| --- | --- | ---: | ---: | ---: |
| off | off | 2 | 39.654 | 39.607-39.701 |
| off | on | 4 | 36.919 | 35.768-40.253 |
| on | off | 2 | 30.873 | 30.833-30.912 |
| on | on | 2 | 25.772 | 25.699-25.844 |

The forward factorial completed; only the generic/Q16 reverse cell completed. Two ACK gaps stopped later Sun cells, including an ambiguous tap that was not retried. Fresh state later established mode 0. A bounded Saturn sequence reached complete texture/ring flags and observed pose spin 0, tilt 0, latitude -8, radius 88. Q16-off had two matched renders at 47.177/47.403 ms (mean 47.290 ms). Its first Q16-on render was acknowledged, but the following read-only pose response was incomplete. Deferred recovery then found a second globe render and a 57.480 ms minute-refresh preparation, so that sample was excluded. No matched Saturn Q16-on benefit is claimed; no more static retries were run. CARDPOSE's three-decimal reporting and unreported prepared-body/time bits also limit any bit-identical dynamic-pose claim. Minute-refresh preparation remains a separate measured hotspot.

Final defaults were tested separately with tracing off and periodic performance summaries enabled. One-second windows may straddle settling and console overhead remains a caveat; engine rate is not an optical refresh measurement.

| Gesture case | Painted frames / completion intervals | Max end-to-end ms | Max completion interval ms | Target deadline misses | 30 fps floor violations |
| --- | ---: | ---: | ---: | ---: | ---: |
| Sun | 45 / 43 | 35.99 | 38.26 | 44 | 42 |
| Saturn | 10 / 8 | 56.53 | 56.58 | 8 | 6 |
| Settings | 141 / 140 | 31.88 | 36.20 | 72 | 3 |

All three scopes still fail the hard timing requirement. Faster kernels changed gesture sampling/inertia and render counts, so dynamic aggregates are not substituted for matched static shader controls. Three strict-ACK gaps are retained in raw static captures; the short final-default sequence completed without another gap. There was no crash, allocation failure or asset failure. Final tracked peak was 6,431,016 bytes, internal free 20,155 bytes and touch/flush/asset stack minima 2,828/1,852/4,176 bytes. Asset results were completed 12/failed 0 and state idle/owned 0. Cleanup confirmed HOME/face 1, layers/history/uniform/packed/light/QZ/shade/Sun enabled, exposure worker/full/profile/trace/perf/awake disabled; COM3 was closed.

Raw files are `build/hardware-iteration13-final-{startup,static-ab,static-resume,static-saturn,defaults-bounded}.jsonl`, `build/recovery/iteration13-final-flash-verification.json` and the pinned artifact manifest. Classification, ACK evidence and completion summaries are in `build/hardware-iteration13-final-analysis.json`; the explicit LUT-guard amendment is retained separately. Before this append, the complete device document was backed up with SHA256 `5f5e19201bff55c0eaf6e11208dc06f7cd2d9283f02ce2a774544ea3f2575b80`. This append used explicit UTF-8 and atomic replacement.


## Iteration 14: cooperative card work and quiet completion capture (2026-10-17)

The guarded app-only update used immutable `build/artifacts/iteration14-cooperative`, tree `ebea3985367c91352d700139187c1aecaffd5e66f805fa04b71562135a647088`, ELF `75e7bf95e462e3ac3654e26c6fe7d603b2654563a10988a3552641da6d931b02`, binary `7b6360406ff20009b4005707ac27dba7b322245a8dab9b28f9c751cfd9adde4b`. Exact iteration 13 image and whole-slot guards passed before writing; new-image readback and both complete outside-app regions passed afterward. Original build 164 recovery remains preserved. The local flash report is `build/recovery/iteration14-cooperative-app-only.json`.

Primary windows used PERF 0/trace 0 and in-memory `bench begin/end` completion accounting. The arm/report command frames were excluded; ordinary gesture ACK and peripheral costs remained. State, asset flags, content counters and expensive exact checks ran outside windows. State 3 reports were required; no ACK retries concealed missing mutations. Both captures completed with zero ACK gaps. Source and image remain pinned to the archived artifact despite later working-tree edits. Actual CPU was 240 MHz; these results do not qualify the 200 MHz-equivalent requirement.

PREBAKE completed in 802 ms: basis/shade mismatch 0, both ready 1, rejection 0, stack minimum 1932 B. Target Sun lighting and original-formula shade checks passed with mismatch 0. Sun and Saturn geometry checks were available with mismatch 0 before/after motion; these independent diagnostic checks took approximately 119–134 ms and were excluded from primary timing. Full detail 1 was observed after quiet with no preparation/backlog. All eight visited body owners were validated using actual texture flags, including Earth night and Saturn rings. No device crash, allocation failure or worker creation failure occurred; final touch/flush/asset stack minima were 2824/1844/4264 B.

| Quiet window | Frames / painted / intervals | Maximum end-to-end / present (ms) | 25 ms work misses / 33.333 ms interval violations |
| --- | --- | --- | --- |
| Cold Sun entry | 117 / 19 / 14 | 72.533 / 72.608 | 7 / 4 |
| Sun horizontal motion | 96 / 40 / 29 | 27.455 / 36.723 | 6 / 1 |
| Seven body previews, each | 84–85 / 29–31 / 27–29 | 20.732–23.390 / 25.932–29.640 | 0 / 0 |
| Saturn horizontal motion | 98 / 14 / 7 | 15.209 / 26.954 | 0 / 0 |
| Saturn tilt | 98 / 8 / 4 | 15.068 / 27.797 | 0 / 0 |
| Normal Settings swipes | 90 / 85 / 84 | 38.797 / 44.495 | 60 / 6 |
| Slow Settings swipes | 144 / 134 / 132 | 23.972 / 39.674 | 0 / 2 |
| Slow launcher swipes | 144 / 106 / 104 | 20.231 / 27.686 | 0 / 0 |

**Iteration 14 fails the required timing/freshness acceptance.** Sparse Saturn repainting cannot establish smooth 30 fps content. Sun motion added 13 publications, 23 distinct proxies, 93 moving observations and 67 suppressed observations; Saturn horizontal added 8 publications/3 proxies versus 94 moving/91 suppressed, while tilt added 4 publications/0 proxies versus 93 moving/93 suppressed. After tilt, proxy was inactive with source age 529 ms and spin/latitude error 9.634/10 degrees. Cumulative observed maxima reached error age 600 ms, stale run 93 and spin/latitude error 57.106/24 degrees, beyond the stated 200 ms and 5/3-degree bounds. Source-age maxima include idle/initial context and must not be interpreted as isolated gesture latency.

Restoration did return exact detail 1, but the reported full-resolution restore durations after motion were 775 ms for Sun and 1024/962 ms for Saturn horizontal/tilt, not completion within 125 ms. The 125 ms threshold is the quiet trigger, not proof of completed rendering. Bounded host waits also include residual spin settling (3.781/5.046/3.250 seconds), so those are not equivalent to shader execution times.

Raw captures are `build/hardware-iteration14-cooperative-quiet.jsonl` and `build/hardware-iteration14-slow-lists.jsonl`; `build/hardware-iteration14-analysis.json` pins their hashes and selected exact reports. Separate slow-scroll trace windows allow cache/blur correlation but cannot prove or disprove optical flicker, blur aesthetics, tearing or physical touch behavior. No further flash or runtime budget tuning was performed. Final verified state was HOME face 1, normal blur controls enabled, worker/full/profile/trace/perf/awake 0, budget 3000 us/detail 2/proxy 1; COM3 closed.


### Bounded same-image detail-4 tuning: stopped on system allocation error

No additional firmware was flashed. A first query-only ACK gap after Sun entry was preserved separately (`build/hardware-iteration14-detail4.jsonl`); explicit read-only recovery confirmed loaded Sun/default controls before a newly labeled run (`build/hardware-iteration14-detail4-resume.jsonl`). Repeated Wi-Fi warnings were present and are a possible environmental confound, not evidence that the UI command failed.

Sun detail 4/budget 3000 us: 96 iterations, 80 painted, 75 intervals; maximum end-to-end 24.767 ms/presentation 37.766 ms, zero 25 ms work misses but nine 30 fps interval violations. Budget 5000 us: 94/82/80, maxima 27.525/36.887 ms, 57 work misses/four floor violations. At 5000 us, motion added 43 publications/78 distinct proxies for 88 moving observations/nine suppressed; end source age was 55 ms and spin error 0.643 degrees. This improves sampled freshness relative to detail 2, but does not pass cadence. Exact full detail 1 restoration remained available; reported restore duration improved from 750 to 425 ms. Ordering, network activity and cumulative diagnostic maxima prevent claiming an isolated universal win.

The Saturn 3000 us window was interrupted at `bench end`: no report/ACK arrived, then the device printed `esp-aes: Failed to allocate memory`, TLS read failure and weather-fetch failure. This is an actual system allocation error even though renderer allocation counters did not fail. The interrupted window is excluded; Saturn 5000 us, Settings worker pilot and cold-entry profile were not run. No automatic retry or subsequent benchmark masked the fault.

Explicit cleanup captured memory and restored normal defaults with acknowledged commands. Final HOME internal free was 10,523 B; PSRAM largest 3,080,192 B/minimum 1,689,700 B. Asset worker was idle with no owned bytes/create failures. The app did not crash, but this network allocation fault requires investigation before further performance qualification. Raw cleanup is `build/hardware-iteration14-tuning-fault-cleanup.jsonl`; COM3 was closed. No recovery flash was performed at this point.


### System internal-memory follow-up: one reset, network preserved

The network allocation failure did not trigger a recovery flash. One authorized captured reset of the same archived iteration 14 performed only boot/idle HOME queries, with Wi-Fi/weather enabled and no card or A/B workload. `build/hardware-iteration14-memory-reset.jsonl` records internal free of 10,523 B before reset, 19,299 B at boot +12 seconds, 5,907 B at idle +32 seconds, and 11,083 B at idle +72 seconds. A weather connection failed at approximately 34.5 seconds (`ESP_ERR_HTTP_CONNECT`, HTTP 0). No AES allocation error or crash recurred in this bounded window. The 5,176 B rebound is consistent with transient network cleanup; remaining differences do not establish a leak, and successful weather recovery was not demonstrated. COM3 was closed after the single reset; no repeated reset loop was used.

Primary local ESP-IDF source explains why free PSRAM is insufficient evidence of AES headroom. `components/mbedtls/port/aes/dma/esp_aes_dma_core.c:1080–1105` routes external output not aligned to the cache line, or output lengths not divisible by that line, through a bounce path. On this S3 configuration its chunk is `min(len,1600)` bytes (:51–55, :244); input/output bounce requests use `MALLOC_CAP_DMA` (:239–240, :266, :275). The same error string also appears for DMA descriptor allocations (:1125 and :1298). Descriptor size on S3/AHB is 12 B. Consequently the existing log cannot identify the exact failed size or distinguish bounce versus descriptors; stating that a particular 1600 B request failed would exceed the evidence.

The archived configuration already sets mbedTLS allocations to PSRAM, RX content 16,384 B/TX 4096 B, internal-priority malloc threshold 4096 B and reserved internal memory 32,768 B. `components/mbedtls/port/esp_mem.c:19` explicitly selects SPIRAM for mbedTLS. Weather's 16 KiB response uses `nos_mem_alloc`, while its HTTP RX/TX buffers are 1024 B each using IDF default malloc, and its worker stack is 8192 B. Moving TLS wholesale to PSRAM is therefore not a new fix. Weather calls `esp_http_client_cleanup` synchronously after perform returns (`firmware/main/weather_service.c:120–122`); IDF cleanup destroys the transport/TLS and frees request/response buffers (`components/esp_http_client/esp_http_client.c:1029`). No app-level deferred TLS cleanup queue was found.

Recommended next evidence is a fixed, allocation-free failed-allocation record containing requested size/capabilities/function, plus current/largest/minimum free values for internal 8-bit and internal DMA heaps. Total internal free cannot establish the required contiguous DMA allocation. Potential changes should preserve DMA buffers in internal memory while moving eligible non-DMA application buffers away from it; reducing task stacks without low-water/call-chain evidence or disabling weather/Wi-Fi would hide the problem. A 64 B cache line can increase AES bounce eligibility, but this run does not prove it caused the failure versus the previous 32 B configuration.

The local capture classifier previously recognized crashes, stack low-water and renderer allocation counters but missed the AES driver's text. Six ignored capture helpers now also stop on actual `Failed to allocate memory`, malloc/calloc/allocation failure and worker-creation errors, with positive/negative examples checked. Original partial logs remain unchanged. The one reset inspection helper was prepared before that update; its complete raw stream was independently searched and contained no repeated allocation error.


### Next-image allocation diagnostics (implemented; not yet flashed)

`firmware/main/nos_heap_diag.c` registers the official `heap_caps_register_failed_alloc_callback` before application allocations. Local IDF `heap_caps.c:49–59` invokes this hook on failed API requests before any configured abort; it supplies the SDK allocator function, not the original AES callsite. `NOSALLOC` reports registration, total/dropped 32-bit counters, busy snapshot status, record sequence, requested size/capabilities and retained static function name. Counters wrap modulo 32 bits; boot allocations before app_main are outside scope. A busy snapshot leaves record fields unavailable rather than spinning or printing a torn record.

The fixed DRAM recorder and IRAM hook use a single native S3 CAS attempt; contention drops the detailed record while retaining a per-core dropped count. Per-core counters are updated with bounded local masking of allocator-capable interrupts. This avoids configured generic stdatomic PSRAM-workaround fallback locks. No heap query, string dereference, logging, blocking wait or allocation occurs inside the hook. `NOSHEAP`, queried only outside measurement windows, reports internal 8-bit free/largest/minimum and internal DMA free/largest. Those individually sampled heap values are diagnostic observations, not an atomic multi-heap snapshot.

Host tests forced contention, verified an inaccessible function-name pointer was retained without dereference, and checked record coherence over 80,000 concurrent failures and 60,000 snapshots. The target object compiled using current IDF flags; its IRAM callback disassembly contains native CAS, no function calls and no backward branches. Evidence is `build/heap-diag-target-compile.log` and `build/heap-diag-target-disassembly.txt`; `tests/test_nos_heap_diag.py` runs the host test. This is source/compile evidence only; no image containing the diagnostic has been flashed or used to identify the earlier AES request yet.


## Iteration 15: allocation recorder identifies internal-DMA starvation (2026-10-17)

One guarded app-only update used immutable `build/artifacts/iteration15-continuity`: tree `cd8de6d32457d1b8c931d702d2ee1803d2d5beac5224beac5a96bbe1e107335f`, ELF `b01940fb69ecb0d24100d957de79f7b3bc1de1ff9fc1f8ed86f57fc6319687ef`, binary `da5e3890d4153b4a6baf7d439fdf4606472c4f2f430983c81e11f54a62b57d95`. Expected iteration 14 whole-slot bytes were derived offline from the verified iteration 13 slot, overlaying the exact iteration 14 image and erasing padding only through the actual absolute 4 KiB edge `0x2c9000`. This expected slot's MD5 was `cc12e532463ee1f2e557ffe627085e83`, SHA256 `9b02d5eb372667ed306765bde9ac9ad06288d430e967763611a2e6a8d0cf1f4d`. Device prefix/whole-slot/boot/layout preflight passed; new image readback and both complete outside-app regions passed. Full build 164 recovery remains preserved. Flash report: `build/recovery/iteration15-continuity-app-only.json`.

Only a single captured boot and bounded HOME idle/network inspection followed. No heavy UI/card benchmark, Wi-Fi/weather toggle, repeated reset or additional write occurred. PREBAKE basis/shade mismatch 0, both ready 1, rejection 0; `NOSALLOC registered=1` verified the recorder was active. Boot +12 seconds: internal 8-bit free/largest/minimum 18,971/7680/12,952 B; internal DMA free/largest 11,239/5120 B; allocation total/dropped 0/0. A later idle snapshot remained at 19,147 B internal free and zero recorded failures.

**The memory gate then failed without card workload.** Wi-Fi initially retried authentication, eventually connected around 38 seconds; IP/web server/NTP followed around 42 seconds and certificate validation around 48.5 seconds. Subsequent Wi-Fi `m f null` and `mem fail` warnings preceded the next sampled record. At approximately 53 seconds, `NOSALLOC` reported total 14, dropped 0, busy 0, sequence 14, requested size **124 B**, capabilities **0x0000080c = INTERNAL | DMA | 8BIT**, SDK function **heap_caps_malloc**. This is the last captured failure, not necessarily the first or only request size. The stop snapshot showed internal 8-bit free/largest/minimum 5203/4608/5056 B, but internal DMA free/largest only **295/208 B**, with roughly 3 MB free PSRAM. All renderer allocation counters remained zero.

These observations establish failure in the internal DMA-capable pool, which aggregate internal free and abundant PSRAM concealed. The capability mask and request size are exact. The record does not contain the caller stack and cannot attribute that 124 B request specifically to AES versus Wi-Fi. A later 208 B largest block does not contradict a prior 124 B failure: sampling occurs after asynchronous allocations/frees. No successful weather response was captured before stopping. No timing or blur-quality qualification follows from this idle memory run.

On the first observed allocation count, the helper stopped its idle loop and made only explicit diagnostic snapshot queries. Raw evidence is `build/hardware-iteration15-memory.jsonl`; `build/hardware-iteration15-memory-analysis.json` pins its SHA and identity, and preserves exact heap/allocation reports. No ACK gaps, device crash or renderer allocation error occurred. HOME face 8 (actual persisted boot selection), normal blur controls, worker/full/profile/trace/perf/awake 0 and card detail 2/budget 3000/proxy 1 were unchanged; COM3 closed. Further performance tests remain held for internal-DMA headroom work; build 164 was not restored merely for this recovered network allocation fault.

Documentation correction: both iteration 15 NOSSTATE lines explicitly report HOME face 8. The earlier face 1 summary incorrectly carried forward the pre-reset cleanup selection; it did not describe the persisted boot selection. Original raw logs and their hashes are unchanged. Iteration 16 idle comparison will retain actual face 8 rather than silently change it.


### Unflashed candidate: retain 16-row rendering, split only rotated DMA

The attempted 8-row render geometry was rejected before flashing: cross-height host checks showed material differences in inherited streaming blur and would lose the retained-layer direct-output path. Iterations 16/16b were not flashed. The replacement keeps two canonical 16-row raw raster buffers and the 16-row PSRAM scratch while reducing only the two rotated DMA buffers to 8 rows. Internal display storage becomes 44,736 B, saving 14,912 B from the previous 59,648 B. Panel clock remains 40 MHz. No extra PSRAM copy is introduced; additional panel transactions may add overhead, which requires target measurement.

Each raw job receives exposure once, then flush_task rotates/transmits pieces of at most 8 rows. Source offset is row offset times the original stride, native panel x advances by that row offset, and native y retains the dirty x window. The raw buffer is released exactly once after the last rotation reads it; the final DMA uses independent rotated storage. Each rotated slot requires a DMA credit before reuse and toggles per piece. Ordered exposure/frame fences retain their previous completion semantics.

`tests/test_nos_dma_chunks.py` extracts the actual production flush_task body for a checked host model. It passes 21 row-count/width cases, including final 2-row jobs, dirty x offsets with full source stride, two queued raw jobs followed by a fence and another frame without resetting slot state. The test overwrites raw storage immediately upon release, delays FIFO DMA completions, verifies output canaries/windows, requires exposure once and rejects premature raw or rotated reuse. Target nos_main compiled cleanly using current IDF flags (`build/dma-chunks-target-compile.log`). This is an unflashed source/ownership candidate, not a measured memory or frame-rate pass.


## Iteration 16b: 16-row render / 8-row DMA, idle headroom improved; weather gate inconclusive

The final DMA-split artifact (distinct from the rejected 8-row-render candidate above) was guarded-flashed once: `build/artifacts/iteration16b-dma-headroom`, tree `a8bd20c003e7ceb87f59a980e7edc0bc0d450e5d029a08f77a6589c18f3abd6e`, ELF `50a83286d9bc0987da2e6c932307bb034ca782a3e5877a9367ae042c98ff0d3d`, binary `6d662a7a531947dca34dad642c1b0bbcad6e7b7574c865e2132a36508ff05135`. Exact iteration 15 prefix/whole-slot/boot/layout preflight, new-app readback and all outside-app byte-region checks passed. Original build 164 backup remains intact. Flash report: `build/recovery/iteration16b-dma-headroom-app-only.json`. Boot identity verified stripe_rows=16, dma_rows=8 and stripe_dma_bytes=44736.

One captured reset then performed only the bounded boot/idle network inspection, retaining actual persisted HOME face 8 and default controls. PREBAKE basis/shade mismatch 0, both ready 1, rejection 0; allocation recorder registered 1. No heavy UI/card test, network toggle or further reset occurred. Ten heap snapshots all reported allocation total/dropped 0/0 and busy 0. Internal 8-bit free ranged 34,007–34,451 B, largest stayed 20,480 B, minimum 27,908 B. Internal DMA free ranged 26,275–26,719 B and largest stayed 20,480 B. This is measured idle DMA headroom, not just the arithmetic 14,912 B buffer saving.

**The complete weather/network memory gate remains inconclusive.** The stream captured an initial Wi-Fi authentication disconnect (reason 15), but no subsequent IP acquisition, certificate validation or completed weather attempt in the bounded window. Zero allocation failures therefore cannot establish that the earlier TLS/Wi-Fi pressure has been solved. The helper ended at its time limit rather than changing network configuration or resetting until success. No further performance benchmarks were run. Transaction-overhead and cadence effects of smaller DMA pieces remain unmeasured.

Final state was HOME face 8 in normal AOD (pwr=1), awake/perf/trace 0, layers/history/uniform/packed 1, worker/full/profile 0, QZ/light/shade/Sun/proxy 1, card detail 2/budget 3000. No ACK gap, crash or renderer allocation failure occurred; COM3 closed. Raw `build/hardware-iteration16b-memory.jsonl` and hash-pinned `build/hardware-iteration16b-memory-analysis.json` preserve exact identity, every heap report and capture duration. A future guard model was derived offline from the verified iteration 15 slot plus exact 16b image with the absolute 4 KiB erase edge 0x2ca000: `build/recovery/iteration16b-dma-headroom-sector-expected-slot.bin`, MD5 `430101bb56e2d25655e2bfab4a0c2eda`, SHA256 `3e201b953a036d240432940ff2533f1422c660016d2f9b5a824c492b0e03dd3b`. No next flash is authorized by that preparation.


### Iteration 16b isolated list timing (same firmware; network still unqualified)

After separate authorization, a bounded no-reset/no-flash sweep exercised Settings and launcher with normal blur controls, worker/full/profile/perf 0 and trace 0 in primary windows. Firmware identity and render16/DMA8/44,736 B geometry were checked first. HOME face 8 was temporarily changed to face 1 to match the earlier iteration 14 slow-list setup, then restored to 8. The device was explicitly brought out of AOD and held awake during tests; no network settings changed.

Requested y velocities were generated by exact distance/time pairs: 270 px/1800 ms = 150 px/s, 290 px/1000 ms = 290 px/s, 270 px/300 ms = 900 px/s and 270 px/150 ms = 1800 px/s. Both signs were tested with 600 ms host settle after each gesture. Positive means increasing finger y. Firmware assigns the CLI swipe duration directly: the previously suspected 512 ms duration clamp does not exist here. Command timestamps/endpoints/durations are in `NOSINPUT` records; actual physical samples, inertial list velocity and optical presentation were not measured. Setup/warmup/state/info/arm/report frames were outside primary capture, while normal gesture ACK and background peripheral work stayed included. Warmup was bounded; cache histories/scroll positions were not asserted equal across every speed or image.

| Owner, commanded speed/sign (px/s) | Iterations / painted / intervals | Max work / presentation (ms) | 25 ms misses / 30 fps violations |
| --- | --- | --- | --- |
| settings 150 -1 | 101 / 94 / 93 | 23.795 / 27.915 | 0 / 0 |
| settings 150 1 | 102 / 102 / 101 | 27.029 / 42.020 | 4 / 1 |
| settings 290 -1 | 69 / 69 / 68 | 27.183 / 47.076 | 4 / 1 |
| settings 290 1 | 69 / 64 / 63 | 25.866 / 27.491 | 1 / 0 |
| settings 900 -1 | 39 / 35 / 34 | 29.398 / 29.478 | 25 / 0 |
| settings 900 1 | 37 / 37 / 36 | 30.186 / 35.092 | 24 / 1 |
| settings 1800 -1 | 32 / 32 / 31 | 28.853 / 31.506 | 28 / 0 |
| settings 1800 1 | 31 / 31 / 30 | 29.681 / 36.054 | 26 / 1 |
| settings prior slow pair | 144 / 133 / 130 | 24.348 / 37.767 | 0 / 1 |
| launcher 150 -1 | 100 / 83 / 81 | 27.481 / 27.557 | 16 / 0 |
| launcher 150 1 | 101 / 79 / 78 | 26.132 / 28.767 | 14 / 0 |
| launcher 290 -1 | 68 / 62 / 61 | 26.150 / 28.895 | 12 / 0 |
| launcher 290 1 | 69 / 64 / 62 | 26.021 / 28.245 | 10 / 0 |
| launcher 900 -1 | 41 / 36 / 35 | 26.773 / 29.047 | 13 / 0 |
| launcher 900 1 | 40 / 39 / 37 | 27.108 / 27.182 | 18 / 0 |
| launcher 1800 -1 | 35 / 34 / 32 | 26.713 / 26.868 | 22 / 0 |
| launcher 1800 1 | 33 / 32 / 30 | 26.854 / 27.084 | 24 / 0 |
| launcher prior slow pair | 145 / 112 / 109 | 25.263 / 26.815 | 3 / 0 |

Timing still fails the hard requirement. Settings had floor violations at positive 150/900/1800 and negative 290 px/s. The negative 290 window contains an incidental weather connection failure and its logging, retained as real system interference; it is not an isolated renderer regression. Launcher had zero floor violations across these eight short speed/sign windows, but each had 25 ms work misses. Sparse/settled frames and a short successful cell do not certify universal cadence.

The exact previous 140 px/1200 ms slow trajectory (116.667 px/s, two directions) gave Settings max work/presentation 24.348/37.767 ms, 0 target/1 floor miss (133 painted/130 intervals), versus iteration 14's 23.972/39.674 ms and 0/2 (134/132). Launcher gave 25.263/26.815 ms, 3/0 (112/109), versus 14's 20.231/27.686 ms, 0/0 (106/104). These are repeated commands, not a controlled attribution to DMA splitting: firmware/cache histories/network context and presentation counts differ. The small Settings tail change is not proof of a throughput win.

Separate slow-scroll diagnostic traces yielded 139 Settings and 98 launcher NOSF frames. Settings medians: end-to-end 18.737 ms, history 5.315 ms, rotation 3.898 ms, dirty/history area 100,224 px, blended history 8909 px. Launcher: 22.964 ms, 7.7405 ms, 5.073 ms, area 129,540 px, blended 13,538 px. These trace-on values are not primary acceptance samples. Worker was 0; the figures still should not be added to independently sampled whole-frame timings.

Settings pre/post generic blur-counter deltas reported sharp=0 and blurred=fast; at 150/290 px/s only 1–2 fast/blurred observations were counted per bracket, while 900/1800 reached 23–32. Those counters cover fast directional policy, not all moving-frame temporal coverage. Counter brackets include diagnostic/arm/report frames outside quiet windows and must not be equated one-to-one with painted counts. Generic launcher list counters stayed zero in this path, so they provide no launcher blur-coverage evidence. Temporal blended pixels were observed in the diagnostic traces; no optical quality/flicker guarantee follows.

Every sampled NOSALLOC remained total/dropped 0/0; there were no ACK gaps, crashes or renderer allocation failures. A weather attempt incidentally failed with `ESP_ERR_HTTP_CONNECT`, HTTP 0; no successful weather response/full TLS body-read was demonstrated. Final internal free/largest/minimum was 25,947/10,752/17,088 B and internal DMA free/largest 18,215/10,752 B. Original HOME face 8 and default controls were restored, awake/perf/trace/full/worker 0, then COM3 closed. No cards were tested.

Raw evidence is `build/hardware-iteration16b-isolated-lists.jsonl`; `build/hardware-iteration16b-isolated-lists-analysis.json` pins the raw hash, exact input metadata, primary windows, counter scope, trace medians and heap/state reports. No additional flash or production source edit occurred.


## Iteration 17: probe validation, shutter regression and partial rotation A/B

One guarded app-only flash used immutable `build/artifacts/iteration17-shutter-probe`: tree `708f1c03c03a27fd3fd05761beaeab2720221bdbbd9517e9f4dc33b9273d971a`, ELF `c64a9024cb90a5ecf62ba3b74619409e6febf35d136a2bfa079ef10613c6c1c3`, binary `04269f75bdc57441be1d3f162034fff7d5540c0ff764055130a21fcd45a3f76d`. Exact iteration 16b prefix/whole-slot/boot/layout guards passed, as did new-app readback and both full outside-app regions. Report: `build/recovery/iteration17-shutter-probe-app-only.json`. Rendering/DMA stayed 16/8 rows, 44,736 B and QSPI 40 MHz. Later navigation source edits do not describe this running artifact.

The separate single-reset startup captured a genuine successful weather update: IP around 5 seconds, certificate validation around 8.7 seconds and completed update around 13.7 seconds. NOSALLOC remained registered 1/total 0/dropped 0. First TLS-era snapshot: internal free/largest/minimum 19,811/9216/17,180 B; DMA free/largest 12,079/9216 B. After success: internal 25,183/8704/minimum 11,904 B; DMA 17,451/8704 B. This demonstrates one completed network transaction without allocation failure; it does not qualify all network conditions. Earlier iteration 16b's initial bounded weather gate remains correctly labeled inconclusive.

### Probe overhead and actual worst-presentation explanation

At default shutter/rotation 0, each 900 px/s control cell contained the same commanded up/down pair. Probe order was 0/1/1/0 for each owner; all quiet windows excluded setup/arm/report/query frames. Settings max end-to-end was 29.634/29.138/29.330/29.826 ms and max completion interval 41.094/42.017/41.610/41.388 ms; every cell had one floor violation, with 50/49/49/50 target misses. Painted counts were 72/75/75/73. Launcher maxima were 27.991/26.477/26.517/26.445 ms and intervals 29.176/29.353/28.938/28.706 ms; no floor failures, 42/27/26/23 target misses and 72/77/75/77 painted. Counts/cache histories differ, so these data do not imply that enabling instrumentation improves speed. Probe overhead is not isolated to sub-millisecond accuracy.

Every received enabled report matched NOSCAPTURE frame/painted counts, chunks equaled callbacks, mismatch was zero, and the two recorded worst-pair timestamps reproduced the quiet maximum exactly. A Settings 42.017 ms pair had frame starts 24.987 ms apart, but bytes changed 29,120 to 313,448 (14,560 to 156,724 pixels), and end-to-end changed 10.175 to 27.205 ms. First API entry was nearly unchanged at 2.999/2.856 ms; UI tick 1.717/1.783 ms. The current frame raster was 23.897 ms/history 8.907/rotation 6.153 and bitmap API elapsed 17.771 ms. The API duration includes prior DMA/window-command/bus waiting and overlaps other work: it is not pure CPU overhead and these values must not be added. Completion callbacks/fences are GRAM/DMA boundaries, not optical/vblank timing. This pair supports damage/workload variation as the observed gap mechanism, not universal bus-idle or scheduler attribution.

Raw overhead capture: `build/hardware-iteration17-probe-overhead.jsonl`.

### Settings experimental shutter: 32 cells completed; keep default off

Probe was 1, rotation 0, normal history/layers/uniform/packed remained 1. Shutter order was 0/1/1/0. Mode changes deliberately discard its row cache; index 1 is first-use after switching, index 2 is the warmed repeated mode 1, and index 3 switches back to a newly rebuilt default cache. No zero-work cold baseline is claimed because commands/arm/entry frames can already prewarm rows. Inputs were exactly the earlier 150/290/900/1800 px/s distance/time pairs, with separate signed windows and 600 ms settle. Raw commanded samples do not measure actual physical touch or optical freshness.

| Index / mode | Speed/sign px/s | Max e2e / completion interval ms | Painted / intervals | Target / floor misses |
| --- | --- | --- | --- | --- |
| 0 / 0 | -150 | 30.061 / 45.466 | 95 / 94 | 5 / 1 |
| 0 / 0 | +150 | 28.734 / 37.593 | 101 / 100 | 5 / 1 |
| 0 / 0 | -290 | 28.814 / 40.477 | 69 / 68 | 4 / 1 |
| 0 / 0 | +290 | 29.174 / 43.919 | 67 / 66 | 3 / 1 |
| 0 / 0 | -900 | 30.083 / 30.185 | 33 / 32 | 24 / 0 |
| 0 / 0 | +900 | 30.560 / 30.662 | 37 / 36 | 25 / 0 |
| 0 / 0 | -1800 | 29.946 / 48.165 | 32 / 31 | 27 / 1 |
| 0 / 0 | +1800 | 29.539 / 36.277 | 31 / 30 | 27 / 1 |
| 1 / 1 | -150 | 52.229 / 52.333 | 68 / 66 | 67 / 48 |
| 1 / 1 | +150 | 55.435 / 69.755 | 54 / 52 | 44 / 42 |
| 1 / 1 | -290 | 58.702 / 58.808 | 21 / 20 | 21 / 19 |
| 1 / 1 | +290 | 60.236 / 60.339 | 22 / 21 | 21 / 19 |
| 1 / 1 | -900 | 62.631 / 62.740 | 7 / 6 | 7 / 5 |
| 1 / 1 | +900 | 64.162 / 64.264 | 8 / 6 | 6 / 4 |
| 1 / 1 | -1800 | 67.159 / 67.255 | 5 / 3 | 4 / 3 |
| 1 / 1 | +1800 | 66.565 / 55.329 | 18 / 17 | 12 / 3 |
| 2 / 1 | -150 | 45.567 / 45.673 | 46 / 45 | 46 / 43 |
| 2 / 1 | +150 | 53.171 / 53.265 | 54 / 53 | 47 / 41 |
| 2 / 1 | -290 | 53.297 / 53.415 | 23 / 21 | 22 / 20 |
| 2 / 1 | +290 | 53.292 / 53.398 | 36 / 35 | 26 / 20 |
| 2 / 1 | -900 | 62.910 / 63.014 | 8 / 7 | 8 / 6 |
| 2 / 1 | +900 | 59.328 / 58.896 | 7 / 6 | 6 / 5 |
| 2 / 1 | -1800 | 72.359 / 72.454 | 5 / 3 | 4 / 3 |
| 2 / 1 | +1800 | 66.879 / 57.206 | 18 / 17 | 12 / 3 |
| 3 / 0 | -150 | 27.990 / 28.084 | 91 / 90 | 55 / 0 |
| 3 / 0 | +150 | 30.184 / 43.663 | 99 / 98 | 8 / 1 |
| 3 / 0 | -290 | 27.918 / 47.344 | 71 / 70 | 5 / 1 |
| 3 / 0 | +290 | 26.142 / 28.497 | 67 / 65 | 3 / 0 |
| 3 / 0 | -900 | 29.146 / 29.249 | 34 / 33 | 24 / 0 |
| 3 / 0 | +900 | 28.938 / 33.025 | 38 / 37 | 26 / 0 |
| 3 / 0 | -1800 | 29.276 / 46.902 | 31 / 30 | 27 / 1 |
| 3 / 0 | +1800 | 30.232 / 35.078 | 32 / 31 | 28 / 1 |

Both experimental mode 1 passes failed materially, including warmed cells with zero additional fallback calls. Aggregate painted/intervals/floor failures were 465/457/6 for initial off, 203/191/143 first-use on, 197/187/141 warm on, and 463/454/4 switched-back off. Actual NOSSHUTTER filtered-pixel counters grew in every warmed on cell, with fallback delta zero there; this is real backend execution, not inferred from legacy NOS_BS fast thresholds. Counter brackets include arm/report/state frames outside primary quiet windows and are not exact per-frame coverage certification. Exposure sign was commanded both directions; settled post-window exposure usually returned to zero, so no unobserved in-motion signed numeric value is invented.

The warmed +150 px/s worst frame took 53.171 ms with 49.943 ms raster, 8.706 ms history, 6.175 ms rotation and 30.147 ms post-first raw queue waiting (maximum 3.009 ms), while bitmap API elapsed totaled 13.952 ms and first entry stayed 2.538 ms. That worker-visible lack of published raster work is consistent with expensive producer-side filtering; it is not a measured physical bus-idle duration. Values overlap. Warm prefix filtering therefore cannot be promoted as a timing optimization from these results.

Initial/final default passes were not identical traffic: bytes 105,628,248/104,116,168, chunks 22,403/25,297, painted 465/463 and target misses 120/176. Cache publication/damage fragmentation/frame sampling differ, so no exact paired default regression claim follows. All 32 cells, exact probe pairs and actual shutter deltas are in `build/hardware-iteration17-shutter-analysis.json`, raw `build/hardware-iteration17-shutter-ab.jsonl`. No allocation, crash or ACK gap occurred during completed overhead/shutter campaigns.

### Rotation: stage improves in first pair; reverse/launcher comparison incomplete

The first rotation stage attempt stopped before mutations because its read-only info had DONE/build identity but omitted fresh heap/allocation lines. That incomplete record is `build/hardware-iteration17-rotation-ab.jsonl`. Explicit read-only recovery verified exact identity/default flags/zero allocations before a separately named resumed capture. Successfully completed earlier campaigns were not rerun.

With probe 1/shutter 0, paired up/down Settings inputs produced the following first 0-to-1 rotation comparison. Traffic is similar but not equal, so normalized stage costs are useful evidence without establishing a whole-frame causal win:

| Speed px/s | Mode 0 bytes/chunks; rotation us; ns/pixel | Mode 1 bytes/chunks; rotation us; ns/pixel | Max e2e / interval ms, mode 0 to 1 |
| --- | --- | --- | --- |
| 150 | 37,775,840/7982; 747,371; 39.57 | 38,154,376/8079; 595,975; 31.24 | 28.654/42.088 to 27.119/46.866 |
| 290 | 28,687,656/6193; 564,878; 39.38 | 28,521,288/6143; 444,746; 31.19 | 28.204/47.295 to 27.415/41.467 |
| 900 | 19,843,264/4092; 388,373; 39.14 | 19,536,040/4037; 303,291; 31.05 | 29.690/32.028 to 29.418/32.415 |
| 1800 | 16,844,336/3542; 332,170; 39.44 | 16,902,384/3552; 263,805; 31.22 | 29.696/29.803 to 29.502/29.604 |

The candidate reduced normalized rotation stage cost by approximately 21% in these first four pairs, but whole-frame/interval changes were small or inconsistent. Slow 150/290 pairs still had one floor violation in each mode; faster pairs had no floor failure but unchanged target misses (48 at 900 and 54 at 1800). Default mode remains 0; no full reverse-order or launcher result was completed.

At the next repeated mode 1/150 cell, the quiet report was complete and internally validated, but its required post-state query lost ACK. That final cell is retained as diagnostic-only and excluded from matched acceptance. The matrix stopped immediately; there was no automatic full restart. Raw resumed capture: `build/hardware-iteration17-rotation-ab-resume.jsonl`; its missing post-state ACK is preserved. Explicit read-only recovery confirmed Settings/probe1/rotation1/shutter0 with no allocations, then acknowledged cleanup restored probe/shutter/rotation/worker/full/profile/perf/trace/awake 0 and HOME face 8. Final internal free/largest/minimum was 24,959/8704/11,904 B, DMA free/largest 17,227/8704 B, NOSALLOC total/dropped 0/0. COM3 closed; no cards or additional flash.

`build/hardware-iteration17-analysis.json` pins all raw hashes, accepted/diagnostic-only cell scope, aggregate/pair records and final defaults. The verified-current 17 whole-slot guard model is `build/recovery/iteration17-shutter-probe-sector-expected-slot.bin`, image 2,799,824 B, absolute 4 KiB erase edge 0x2cc000, MD5 `c636e2e5dabb47f62a074953acf2b0ae`, SHA256 `282588dea3693f60a6275563ecf4135a57ada72cf726154138d648e242d512b3`. Original build 164 recovery and all protected partitions remain preserved. Actual measurements were at 240 MHz; no 200 MHz-equivalent, optical blur or universal no-drop qualification is claimed.

## Iteration 19b: navigation target verification and bounded shutter rejection

The sole guarded update from iteration17 used immutable `build/artifacts/iteration19b-navigation-shutter`: source tree `47e8c5b4cc345d2bb0b88b30bbb63e46caf2e9ca7279b3a6275b30688934d629`, ELF `768483e19307cf26ee2f224e465d6b8ef873d7219e0f2fdc06f2300544b2515b`, BIN `4c0388dae4982206a5b939768be239ed2413f3fa8870bf7c8557f4a4a6834be1`. Iteration18 and preliminary19 were not flashed. All prefix/whole-slot, boot/layout/OTA, outside-slot and app readback checks passed; recovery164 remains unchanged. The next expected whole-slot model is `build/recovery/iteration19b-navigation-shutter-sector-expected-slot.bin`, SHA256 `b7c1c66b4aa518bb48ad23767ecbaba01e732969b8409ec814a009edda14aaf2`, MD5 `75f4b1323c539c07f2ec179832836505`, actual erase edge `0x2cc000`.

Startup confirmed exact ELF, HOME face8, canonical16-row render/8-row DMA and default-off probe/shutter/rotation. NOSALLOC stayed registered with total0; internal free20,087 to25,671B, DMA12,355 to17,939B, final largest11,264B and minimum internal17,180B. Weather hit a select/connect timeout; this is not network qualification.

Fresh NOSNAV assertions verified actual rightward locked-face swipe to embedded Apps, leftward return, Activity tap/open/home return with remembered row, vertical scrolling owned by Apps, button Apps-to-face and face-to-Apps, and double-button last-app opening. AOD forced logicalface0. Separate AOD proof captured pwr1, nav_state page0, then fresh pwr1 with identical frame counter, proving the diagnostic did not wake the device. Its later cleanup state query lost its ACK; this remains in the raw log. A separately named authorized read-only recovery established exact identity/default controls/fresh HOMEface8/page0/clean heap, then completed explicit cleanup. No successful stage was replayed.

Embedded Apps primary quiet captures used probe0/shutter0/rotation0, blur/history/uniform/packed1, worker0/full0 and face8. Negative means decreasing commanded y; 150 used270px/1800ms,290 used290px/1000ms,900 used270px/300ms,1800 used270px/150ms. Arm/report/transition frames are excluded; normal gesture ACK cost remains. The eight cells are a bounded post-warmup trajectory, not cold-cache or optical certification.

| px/s, sign | painted / intervals | max e2e / present ms | 40fps deadlines / 30fps floor |
|---|---:|---:|---:|
| 150, -1 | 75 / 74 | 32.586 / 32.668 | 63 / 0 |
| 150, +1 | 75 / 74 | 33.208 / 33.293 | 62 / 0 |
| 290, -1 | 59 / 58 | 29.696 / 30.814 | 54 / 0 |
| 290, +1 | 60 / 59 | 30.348 / 30.429 | 54 / 0 |
| 900, -1 | 33 / 32 | 29.862 / 30.044 | 29 / 0 |
| 900, +1 | 35 / 33 | 34.681 / 34.793 | 35 / 1 |
| 1800, -1 | 28 / 26 | 35.627 / 35.709 | 28 / 2 |
| 1800, +1 | 29 / 27 | 34.529 / 34.608 | 29 / 4 |

Fast Apps gestures still violate the hard30fps floor; every cell misses40fps slots. No causal comparison against standalone17 is justified because ownership, damage and trajectories differ.

The optimized Settings experiment used probe1/rotation0 and planned ABBA0/1/1/0 at150/290, both signs. It stopped after the first warm repeated-candidate floor failure: nine of sixteen planned cells were collected, with no final reverse A. Setting the mode releases caches, so first candidate use is not a warm result. Warm repeated150-negative:79frames/72painted/71intervals, max e2e37.311ms/present37.453ms, deadline72/floor25. Initial default150-negative:101frames/94painted/93intervals,27.637/27.733ms,deadline5/floor0. Initial default also had occasional presentation gaps (150-positive42.908ms and290-negative40.562ms), so it is not globally qualified either. Candidate first-use290-negative/positive reached46.177/43.054ms presentation with22/16floor misses. Candidate remains OFF.

The warm candidate probe validated3,226chunks/completions with zero mismatch over15,880,512bytes. Worst pair carried205,608bytes and51chunks in each frame: previous e2e36.611ms, current37.311ms; first API entry3.451/3.418ms, current raster34.703ms/history5.272ms/rotation4.235ms, raw-queue wait18.187ms, API elapsed11.181ms. These overlap and must not be added. Across the capture, API maximum643us, total768.392ms over3,226calls (238.19us/call including previous-DMA/command waiting), DMA-credit wait7.418ms, maximum116us. This is not pure per-command CPU cost or a general small-transfer intercept; bytes/window geometry differ and API includes prior DMA. Current first/last completion3.802/37.283ms bounds the pipeline tail, not optical output.

Final acknowledged cleanup restored probe/shutter/rotation0, worker0/full0, HOMEface8, perf0/trace0/awake0 and closed COM3. Final internal25,287B/largest11,264B/min12,140B; DMA17,555B/largest11,264B; NOSALLOC total0. There were no observed allocation faults or crashes. Actual240MHz timing does not qualify200MHz-equivalent performance or motion-blur image quality.

Raw windows, selected counters/worst-pair snapshots and provenance are pinned in ignored `build/hardware-iteration19b-analysis.json`; raw SHA256 values:

- `build/hardware-iteration19b-aod-proof.jsonl`: `318bce1a32202814cb2658a9f55b32d502b79721a180b5c5fd6cabc5cc592593`.
- `build/hardware-iteration19b-embedded.jsonl`: `e493bf528fe2e5b8ff0bcd5f60fc7ce3f302104b0a2ff28caa284ab2b5f464de`.
- `build/hardware-iteration19b-navigation.jsonl`: `374fa47d5bae11e22eb298cd10c6115faa6bf5d6eb8bfcbf178372b83094907f`.
- `build/hardware-iteration19b-readonly-recovery.jsonl`: `16a3106667f4d9e3d7b81919b7ab42951cbec9472ba81d460b6856835fd759b1`.
- `build/hardware-iteration19b-shutter-ab.jsonl`: `aa1c708e72822f14327983510ebdb9e8c8cb1ea493d2fcf756738a99b28c7996`.
- `build/hardware-iteration19b-startup.jsonl`: `59801eeeef6a7e07e765f5578f3add1d91b194770152353b5d237b635760687a`.

## Iteration 20: damage bands remain unmeasured; console integrity blocks A/B

Immutable `build/artifacts/iteration20-damage-bands` source tree `9e5715e84a186b3c453150347b35f64cc80c7669bfbcb9ffc663f85a7881669f`, ELF `f1bb8218557ddfaad7d3cd188e1948c9e6061b07be95334e6388b86988bf1f07`, BIN `adb6b7286a30615880b7f065394cce5893d5555886b468256d575d4ac3da026a` passed the sole guarded19b-to20 app-only update. Expected old prefix/whole-slot, boot/layout/OTA, all outside-slot bytes and new image readback passed. Current complete-slot model `build/recovery/iteration20-damage-bands-sector-expected-slot.bin` SHA256 `cd9eb746c7aa7cb35c1ae6c3404e7bc94a27768b29bc0a24da4368f2d1f70dc3`, MD5 `b1c420172330d1b387a2006eadb18cfa`, absolute4KiB erase edge `0x2cd000`. Original164 recovery remains unchanged.

The single captured reset confirmed exact20 identity, default damage_bands0, HOMEface8 and blur controls enabled, but startup stopped at a missing nav_state ACK before its planned idle phase. Its partial heap snapshot was internal18,127B/largest9,216/min17,492, DMA10,395B/largest9,216, NOSALLOC total0. This is an incomplete startup sequence, not a successful network gate. Authorized read-only recovery later supplied complete identity/state/nav and internal23,471B/largest11,264/min17,492, DMA15,739B/largest11,264, NOSALLOC0. Its inherited cleanup assertion incorrectly required awake1 while the actual normal startup state was awake0; that host assertion failure is preserved and is not a firmware fault.

Two separately named Settings campaigns were attempted and stopped. Neither enabled damage_bands1, neither reached embedded Apps, and no performance or target activation conclusion about the candidate is available. The initial synchronous campaign logged a pre-window info ACK gap and used its inherited explicit read-only query retry; this violated the newly stricter stop-on-any-gap instruction. Its only mode0 window is therefore diagnostic-only, not an accepted paired campaign:69frames/64painted/63intervals, max e2e27.014ms/present27.910ms, deadline5/floor0. Its post-window state ACK was also missing. The exact used helper is preserved as `build/bands20-used-first-partial.py`; subsequent helpers removed all automatic retries. A later broad-info cleanup attempt received expected identity/DONE but lost its leading heap/allocator lines and stopped without mutations. Separately authorized compact cleanup succeeded; unavailable fresh heap counters were explicitly retained as a limitation.

Read-only host investigation found PySerial Windows flush() polls out_waiting with time.sleep(0.05), and the old synchronous helper performed flush before reading. Local IDF USB-JTAG VFS also has a50ms silent-drop policy; therefore host read starvation was a plausible mechanism, not established cause. A new dedicated background reader starts before writes/flush, queues timestamped raw chunks for single-threaded parsing/logging, records write/flush/first-read timings, rejects missing/duplicate timestamp-correlated response prefixes/DONE, preserves raw bytes/partial tail, and checks terminal errors/thread shutdown. A host test of its actual reader/poll/cmd functions drained a fake response during a75ms blocked flush; independent review hardened duplicate-ACK and shutdown checks. No production console change was made for this test.

The new explicitly independent threaded preflight completed compact state/nav then exactly one full info with correct identity and NOSALLOC0. Internal22,999B/largest8,704/min12,140; DMA15,267B/largest8,704. Its new290px/s Settings campaign used probe1/shutter0/rotation0,290px over1000ms, actual repeated endpoint poses0 and748,288q8 (2923px), and explicit conditioning bounded to12logged swipes per endpoint. Settings source has36entries/104units and max2923px; first-repeat endpoint confirmation can need more than four flings. Warmup traversed endpoint regions, but was not proof every directional-cache level was ready. Cache/blur brackets include outside-capture query frames.

| New threaded mode0 cell | frames / painted / intervals | max e2e / present ms | deadlines / floor |
|---|---:|---:|---:|
| 290, negative y, top start | 91 / 72 / 71 | 26.794 / 27.594 | 5 / 0 |
| 290, positive y, bottom start | 90 / 72 / 71 | 23.848 / 26.904 | 0 / 0 |

Both quiet payloads had state3, matching pipeline completion counts and fresh NOSDAMAGE mode0/painted72/band0/fallback0. The first cell had complete postconditions. The second retained valid quiet payloads but its post-state response was truncated/merged, so its accepted paired-cell postcondition is incomplete. These cells are not combined with the earlier diagnostic campaign; host pacing and observed trajectories differ.

The failure is conclusive evidence that a dedicated reader alone did not solve console integrity. Received bytes contained `...worker=0 qzNOSSHUTTER enabled=0...` with the rest of NOSSTATE missing, then DONE. Exact-prefix checks rejected it without retry. All255received bytes arrived13.622 to15.938ms after command-write start; that command took1.608ms to write and0.0243ms to flush. Session maximum flush was0.191ms, maximum write3.211ms; reader terminal errors0, queued/unparsed chunks0, unterminated tail empty and thread stopped normally. Thus this specific corruption cannot be attributed to a measured50ms PySerial flush sleep. Raw transport chunks prove the missing middle was already absent on receipt. The available evidence does not distinguish device stdio/VFS/USB causes; an allocator or rendering fault is not inferred from transport loss. IDF usb_serial_jtag_write_bytes can return0 on tx-mutex contention as well as a full ring. Archived20 already used a1024B TX ring, so increasing from the SDK256B default was not sufficient either.

Further performance attempts stopped. Authorized final compact threaded cleanup needed no broad-info prerequisite: fresh state/nav succeeded, each idempotent damage0/probe0/shutter0/rotation0 had a clear fresh zero report plus DONE, trace0/home were acknowledged, standalone FACE8/navpage0 confirmed, and only then fresh state guided awake/perf toggles. Final fresh HOMEface8/page0/awake0/perf0/trace0/allcandidateflags0 was confirmed and COM3 closed. Fresh allocator/heap counters were unavailable during that last cleanup; the last complete22,999/15,267B snapshot predates the final baseline cells. No claim of clean final allocator counters, completed A/B,200MHz qualification, power qualification or optical quality is made.

`build/hardware-iteration20-analysis.json` pins selected state/heap/quiet/probe/damage/conditioning data, exact helper hashes and transport metadata. Original gaps, protocol exception, failed host assertion and partial sequences remain unchanged. Raw SHA256 values:

- `build/hardware-iteration20-compact-cleanup.jsonl`: `49df8a9b40671063c0ac1f2ea764b231351db027b5b27a8b62b2138f35ac3d0f`.
- `build/hardware-iteration20-readonly-recovery.jsonl`: `bb81ad5002bccf093137e1acebc8ee7ef60c8c753e874ec5d2c5a619407dcea4`.
- `build/hardware-iteration20-settings-ab.jsonl`: `d0ec7352cf01b89e550382155a8b7ded43b180c77db4704fa50f87becd1a6363`.
- `build/hardware-iteration20-startup.jsonl`: `9fcfa969032ec0c5267ba68e805687ed28cf7116077b6b044a74e018858a0dc7`.
- `build/hardware-iteration20-strict-cleanup.jsonl`: `d5f54a0eae0d0fd9785b765effd033cfe0014c6e273980f1670241fbfda22364`.
- `build/hardware-iteration20-threaded-cleanup.jsonl`: `d67097c856db9a094b4b1cb95dbd0692f2d0585c8b3dc2fe8c0812d68c27d94b`.
- `build/hardware-iteration20-threaded-settings-ab.jsonl`: `2a576ad6901afd0e78df4bca21b32f5523ac4c0f225ba067aed078c1ace29293`.
- `build/hardware-iteration20-threaded-cleanup.transport.json`: `f86f3a08cd5337d907013af844eb96ddd6abd4760a75730b1c0b988006c0d0fc`.
- `build/hardware-iteration20-threaded-settings-ab.transport.json`: `dadbac09d023d749ccb69ad8fd1cc579155cbd31550d507a977331c4c3bb111f`.

## Iteration 21: bounded console diagnostics completed without reproducing loss

Diagnostics-only artifact `build/artifacts/iteration21-usb-diagnostics`: tree `da7d0186693915a375cb3bf2362ca873d433c5150339c9b80d81fa9d2d813409`, ELF `b4f0bc0899b295d14b7646f04a901d41f86fbce48e010e7ca8b24e7c5a00e8cf`, BIN `982d73518c27189db2289821d8877501936d75885d8ce001633cc2c4ff2d1ab4`. The authorized app-only update from exact20 passed current prefix/whole-slot, boot/layout/OTA, outside-slot and new image readback checks. Current complete-slot expected model `build/recovery/iteration21-usb-diagnostics-sector-expected-slot.bin` SHA256 `97577ca48fa8c056e53732e8ccf376639c9c780f9950cd7660301b73983de9d1`, MD5 `b57303b3f85dfa15f79f88f4d38f81db`, absolute erase edge `0x2cd000`. Renderer/default runtime controls were unchanged; this is not an optimization performance run.

The reviewed host parser checks the exact21-field frozen C format, raw payload length, IEEE/zlib CRC, version/valid flags and fresh random uint32 nonce. Eight parser tests cover malformed fields, overflow, stale nonce, invalid CRC, duplicate frames, orphan-prefix preservation and every byte-fragment cutoff. The helper now raises a dedicated DeviceFault from the actual inherited device-health emit chain;14 fault-class fixtures (including renderer allocation, WiFi, stacks, assets and target prewarm gates) stop without the planned follow-up query. Transport-only assertions remain separate. Exact host helpers/tests are retained in ignored `build/console21-host-snapshot`, hashed in `build/hardware-iteration21-analysis.json`.

After the single captured reset, PREBAKE reported basis/shade mismatch0 and ready1, rejection0, elapsed824ms, minimum task stack1928B. The finite console-only sequence then completed with no retries: direct diagnostic nonce300071901, compact state, nav_state, asset_state, exactly one broad info, direct diagnostic nonce300071902. No ordinary failure occurred, so the separately planned post-failure diagnostic was not used. Both direct frames passed exact nonce/length/CRC checks (153 and159bytes); direct replies omit generic DONE. No UI workload, input, renderer A/B, network toggles or cleanup mutations were performed. Normal default HOME face8/page0/awake0/perf0/trace0, worker0/full0, probe0/shutter0/rotation0/damage_bands0 were observed; read-only commands were quiet. COM3 closed after the bounded sequence.

| Device counter | Baseline | Final | Interpretation |
|---|---:|---:|---|
| completed USB lines / queue accepted / dispatched | 1 / 1 / 1 | 6 / 6 / 6 | +5: four ordinary queries plus current final diagnostic |
| queue failures | 0 | 0 | no observed queue rejection |
| connection checks / false | 31 / 0 | 91 / 0 | no false-connection event in60 additional checks |
| zero-wait driver calls / failures | 1157 / 0 | 4188 / 0 | no enqueue failure in3031 additional calls |
| nonzero-wait driver calls / failures | 0 / 0 | 1 / 0 | includes the prior direct diagnostic write |
| previous direct replies / incomplete | 0 / 0 | 1 / 0 | final snapshot excludes its own transmit |
| last direct requested / accepted bytes | 0 / 0 | 153 / 153 | driver acceptance plus independent hostCRC for baseline |

The wrapper times increased by28,065us across zero-wait calls; final maximum684us. The prior direct driver call took32us. These are instrumented diagnostics, not pure USB wire time or renderer cost. The direct driver timeout argument is50ms per potential wait, with two possible waits, so there is no50ms total-call guarantee. Accepted/enqueued bytes alone do not establish host delivery; the matching host CRC does establish receipt of these two frames.

The one complete info snapshot reported internal free18,115B/largest9,216/min17,492; DMA10,383B/largest9,216; NOSALLOC registered/total0. Touch minimum stack2856B and flush1832B were observed; asset worker was inactive with no owned bytes/create failures. There is no target main-task high-water measurement. This short window did not establish a weather/network success, long-duration heap qualification or safety under heavy UI.

Host maximum write0.5123ms/flush0.0512ms; all six commands were single attempts. Reader terminal errors0, unparsed queue0, empty unterminated tail and stopped thread were recorded. This run did not reproduce20 corruption and therefore identifies no drop cause. The previous merged/truncated response remains valid evidence; no claim that wrappers, background reading or ring size solved it is supported. No200MHz-equivalent, frame-rate, power or optical qualification follows.

Raw/source/helper provenance and counters are pinned in `build/hardware-iteration21-analysis.json`. Raw capture artifacts:

- `build/hardware-iteration21-console.jsonl`: `c5528c2057f114cecd5c37e2f1751c81527514f83fd1f1882a47be30aaac4db9`.
- `build/hardware-iteration21-console.diagnostics.json`: `26fd5e93879431395abaccb1132774e4b31cf9081cb90206b76e9a66ec4a82ad`.
- `build/hardware-iteration21-console.transport.json`: `866c6ecea4dc32ec0e3c8d3f791e5fc5dbb2f005c126e7bed46ae60ce7335f88`.

### Iteration 21: additional finite idle/AOD console stress

A separately authorized run on the same21 image performed no reset or UI mutation: one fresh direct baseline,64planned alternating state/nav_state queries, exactly one broad info after all64succeeded, and one final direct diagnostic. Each sample index and write/flush/read timing is logged. All30NOSSTATE fields were checked against the archived firmware format, and all5NOSNAV fields were required; prefix-only responses could not pass. No automatic retry or post-failure diagnostic occurred.

All67commands completed, including both valid nonce/length/CRC direct frames. Counter delta RX/queue-accepted/dispatched was66each (64shortqueries, oneinfo and the current finaldiagnostic). Queue failures0; falseconnection0 over303additional checks; zero-wait enqueue failures0 over18,934calls; nonzero-wait failures0; prior direct reply incomplete0. Wrapper elapsed zero-wait delta165,186us is instrumentation evidence, not renderer or wire performance.

Fresh states explicitly showed pwr1/AOD, awake0 and HOMEface8/page0 throughout. This is idle/AOD console evidence, not the active Settings context where20 produced corruption. Internal free22,935B/largest8,704/min12,140; DMA15,203B/largest8,704; NOSALLOC total0. Touch minimum2824B/flush1832B; no reader errors/backlog/unterminated tail, and reader stopped before closure. Maximum host write2.158ms/flush0.1325ms. Normal power/default controls were unchanged and COM3 closed.

The console failure again did not reproduce. Neither a cause nor an integrity repair is proven; the diagnostic wrappers time and lock each call and may perturb transient behavior. No additional rendering benchmark, power,200MHz or optical qualification follows. The earlier complete short sequence and its analysis remain unchanged; this run is separately pinned in `build/hardware-iteration21-console-stress-analysis.json`. Raw/helper hashes:

- `build/hardware-iteration21-console-stress.jsonl`: `e63f9fb9a9dd412a1f75c53cebf671491431704ac9c16e8361f2e15ac592ff5e`.
- `build/hardware-iteration21-console-stress.diagnostics.json`: `a7dd73f7d9160fd0a577d4f2aa1ce285c26dae3705cb2d30f5692911558363b8`.
- `build/hardware-iteration21-console-stress.transport.json`: `c18cbbaf14fa3099ec0befec1db71ca65037bb8c82ffde326bd2618db684095c`.
- `build/console21-host-snapshot/console21_stress.py`: `9cb09c2d612289e5a8fa935fddd5c9b174bcd1ba814b98be5bc883bb04126f14`.

### Iteration 21: ON-state preparation reproduced loss with a false-connection event

The separately authorized active-load Settings A/B attempt used current21, no flash/reset, with direct USB diagnostics bracketing its ordinary commands. It passed complete identity/state/nav/heap preflight, then explicitly enabled awake1/pwr0. During HOME preparation, before opening Settings or collecting any measured window, a nav_state command received no bytes or DONE within5s. The attempt stopped; the one separately planned fresh direct diagnostic succeeded, then the port closed. Settings entry, warmup, candidate-on activation, measured cells and embedded Apps were all NOT reached. This is ON/HOME preparation evidence, not a completed heavy Settings-load test or renderer comparison.

Direct snapshots (nonce835996617 baseline,835996619 post-failure) both passed exact length/CRC. Across this bounded interval, RX/queue-accepted/dispatched each increased10: nine ordinary commands including the failed nav_state plus the current post-failure diagnostic. Queue failures remained0. False connection checks increased2to3 over80checks, while zero-wait enqueue failures remained0 over3493calls, nonzero-wait failures0 and previous direct reply incomplete0; prior requested/accepted direct bytes166/166. Thus the failed ordinary command was received, queued and dispatched, with one false connection observation and no reported driver enqueue failure somewhere in the interval. The valid direct reply proves response-path liveness at that later time. Aggregate snapshots do not assign the false check to an exact individual stdout write, so correlation is strong evidence for the VFS gate, not complete per-write proof or a repair.

Primary local IDF `components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c:185` returns-1 before enqueue if usb_serial_jtag_is_connected() is false. Its connection monitor (`usb_serial_jtag_connection_monitor.c:44`) samples/clears the SOF interrupt flag at each tick. Archived CONFIG_FREERTOS_HZ=1000 gives3tolerance ticks. The source decrements missing-SOF credits but replenishes them only upon reconnect, not on a good-SOF tick while already connected; therefore separated missing-SOF observations can exhaust credits cumulatively (code inference). No SDK/source fix or extra experiment was made by this capture.

A separately authorized bounded compact cleanup succeeded with no retry and no broad-info prerequisite. Fresh full HOMEface8/default-state/nav were observed, control-specific zero acknowledgements and FACE8/page0 confirmed, then known state guided awake/perf toggles. Final full HOMEface8/page0/awake0/perf0/trace0/probe0/shutter0/rotation0/damage_bands0 was confirmed and COM3 closed. The preflight info snapshot was internal22,935B/largest8,704/min12,140; DMA15,203B/largest8,704 and NOSALLOC0. Cleanup did not obtain newer heap/allocator counters; that limitation remains explicit.

The earlier two successful console sequences remain positive limited evidence. This failure disproves generalizing them into an active-console integrity fix. No damage-band benefit, frame-rate qualification,200MHz budget, power or optical claim follows. `build/hardware-iteration21-active-analysis.json` separately pins raw commands, two diagnostics, zero quiet windows, cleanup, and frozen helper hashes. Raw evidence:

- `build/hardware-iteration21-active-settings-ab.jsonl`: `78b1042b537cd149e89eaf3b7b2f6d94a90d0b4772a72cd27365ee5854c699ca`.
- `build/hardware-iteration21-active-settings-ab.diagnostics.json`: `ec930788c5f225e8917687b5ea51be83efda3f0208296d971c9fb5cc7d9d46a6`.
- `build/hardware-iteration21-active-compact-cleanup.jsonl`: `f9cd2e284ff66b8cf73f3070ab7d467b619bef45a274dc482944868985945d00`.

## Iteration 22: scoped guard64 active console run passed, override unexercised

Immutable `build/artifacts/iteration22-usb-reply-guard`: tree `0d575126bdc7db1a8df5709d8309884288f8072ed45996824ecefd4caa69fd05`, ELF `ad73e1165793ec3c2d38deba18b421df110c042d3277fc5fdb586067fc700686`, BIN `97e7d7b5a8b52a03405eb72b0e9f5c7a9bb254e91cf67525c2cc1125fad5e6fd`. The authorized app-only update from exact21 passed prefix/whole-slot, boot/layout/OTA, all outside-slot bytes and image readback checks. Current expected-slot model `build/recovery/iteration22-usb-reply-guard-sector-expected-slot.bin` SHA256 `c3907cd6ea81048f1197a3a1f92a1de57249572d0af2b55efd11e7156411ed86`, MD5 `4c5275a5a80972aff456e208c6af7f9d`, absolute erase edge `0x2cd000`. No renderer/config change was tested here.

The default-OFF reply guard can promote a raw false connection predicate only while the queued command runs synchronously on the UI task; other tasks/ISR paths remain unguarded. v2direct frames retain the exact length/IEEE CRC/nonce domain and add guard mode and override count, while cf remains raw false checks. The23-field host parser matched the archived C format; nine parser tests passed. Full inherited fault-chain tests and four actual main/shutdown-path tests passed. Response-failure handling uses exactly one planned direct usb_guard0 snapshot, emits incomplete and exits nonzero; real DeviceFault/unterminated fatal fragments send no follow-up. Cancellation errors cannot skip raw evidence and port closure. These are host/static validations, not proof a target false event was corrected.

After the single authorized captured post-flash reset, PREBAKE reported813ms, basis/shade mismatches0/ready1, rejection0, task minimum1928B. A directCRC baseline confirmed guard0, another confirmed explicit guard1, then complete identity/health checks preceded known awake1/pwr0. All64planned alternating full30-field state/5-field nav queries completed under active HOMEface8/page0. This is ON/HOME console activity, not Settings/list rendering or motion-blur evaluation. No retries or renderer cells occurred.

Between the enabled snapshot and post-query diagnostic, RX/queue-accepted/dispatched each increased73 (64queries plus setup/currentdiagnostic), queue failures0. Raw connection false0 over332additional checks; zero-wait enqueue failures0 over20,665calls; nonzero-wait failures0 and previous direct reply incomplete0. Four direct frames (baseline, enable, counters, disable) passed exact nonce/CRC/length. The critical limitation: cf0 and overrides0 throughout, including cleanup, so the protective branch was NOT exercised. Successful replies do not prove the guard fixed21 loss or future active-load integrity. Source behavior and the previous positive false-connection observation remain separate evidence.

Initial info snapshot: internal free18,103B/largest9,216/min17,492; DMA10,371B/largest9,216; NOSALLOC registered/total0. Touch minimum2860B/flush1836B; target main-task highwater remains unavailable. Incidental weather failed with ESP_ERR_HTTP_CONNECT; this is not network qualification. Final cleanup obtained fresh controls/state, not a new heap/allocator snapshot.

Ordinary cleanup ran while guard1: idempotent renderer controls0 and HOMEface8/page0 were acknowledged, known state guided awake0/perf0/trace0. Final direct usb_guard0 CRC confirmed guard0 as the last device command, then COM3 closed. No ordinary command was sent after disabling the guard. All87device commands were single attempts; maximum host write2.1037ms/flush0.1162ms, reader/shutdown errors0, queue/partial tail empty, reader stopped before closure. No power,200MHz, optical or rendering timing qualification follows.

Exact helpers/tests are frozen under `build/console22-host-snapshot`; `build/hardware-iteration22-analysis.json` pins provenance, counters, states and raw/source/helper hashes. Raw capture artifacts:

- `build/hardware-iteration22-console-guard.jsonl`: `df36ba9a58988868df5392b51f4b38042c4280de9dbd9f5311fb7608d06a0b33`.
- `build/hardware-iteration22-console-guard.diagnostics.json`: `bd03870740d15d49952531e16476be53f8b13249834043573f1c339419e8ec24`.
- `build/hardware-iteration22-console-guard.transport.json`: `b15bf7449764fb325307bd0f2c98d09e71977bd180f6d479970f97735650be55`.

### Iteration 22: Settings guarded damage-band ABBA completed

On the unchanged22 image, the separately authorized Settings-only sequence collected exactly eight new quiet cells: damage_bands0/1/1/0, each negative/positive y at290px over1000ms. It used probe1, guard1, worker0/full0, shutter0/rotation0 and blur/history/uniform/packed1. Fresh full source/health/state/nav conditions and matching start offsets0/748,288q8 (2923px) were checked. Bounded endpoint conditioning and warmup were logged outside each quiet window; arm/report/transition frames were excluded. These cells are not combined with20/21 partial attempts.

Every window passed fresh NOSPIPE chunks/completions/mismatch and NOSDAMAGE mode/painted validation; all289candidate-on painted frames used bands, fallback0. Default-off band/fallback counts stayed0. No ordinary reply failed and no retries occurred. Guard counters now exercised the intended path: enabled-to-final raw false+4 and scoped overrides+4, while tested replies remained complete, driver enqueue/queue failures0 and direct reply incomplete0. This is positive bounded target evidence that raw false predicates were promoted within queued-command replies; it does not qualify arbitrary disconnects, other tasks or future workloads. Final directCRC confirmed guard0 after ordinary default cleanup.

| ABBA index / mode / sign | painted / intervals | max e2e / present ms | deadlines / floor | RGB565 bytes / painted | chunks / painted |
|---|---:|---:|---:|---:|---:|
| 0 / 0 / -1 | 71 / 70 | 27.585 / 27.743 | 5 / 0 | 222318.0 | 47.296 |
| 0 / 0 / +1 | 72 / 71 | 23.701 / 26.789 | 0 / 0 | 170309.7 | 46.569 |
| 1 / 1 / -1 | 73 / 72 | 24.856 / 27.646 | 1 / 0 | 206333.0 | 44.890 |
| 1 / 1 / +1 | 72 / 71 | 21.611 / 27.242 | 0 / 0 | 172890.1 | 39.181 |
| 2 / 1 / -1 | 72 / 71 | 25.982 / 27.042 | 1 / 0 | 206914.6 | 45.069 |
| 2 / 1 / +1 | 72 / 71 | 21.428 / 27.776 | 0 / 0 | 172914.1 | 39.208 |
| 3 / 0 / -1 | 71 / 70 | 26.967 / 27.551 | 5 / 0 | 222939.0 | 47.211 |
| 3 / 0 / +1 | 72 / 71 | 25.090 / 28.241 | 1 / 0 | 170388.8 | 46.597 |

Normalized across the four cells per mode, bytes/painted fell196,306to189,820 (3.3%), chunks/painted46.916to42.097 (10.3%) and rotation3.855to3.757ms/painted (2.5%). Direction matters: negative-y bytes/painted fell about7.2%, while positive-y increased about1.5%. The host12–15%Settings pixel estimate is therefore not a universal measured hardware byte saving. Painted counts/poses sampled differ slightly despite equal commanded input/endpoints; totals without normalization would overstate attribution.

Max e2e improved in both on-mode repeats relative to neighboring off-mode cells, but max presentation intervals did not consistently improve: off26.789–28.241ms, on27.042–27.776ms. All eight had floor0, while target40fps work deadlines were still missed in some cells and presentation maxima exceeded25ms. This is limited290px/s evidence; it does not establish all-speed or global30/40fps qualification. Stage timings overlap and must not be added.

Settings blur-counter brackets showed1–2eligible fast frames/cell, all counted blurred with sharp0, alongside75–76moving counts. Brackets include outside-capture query frames; they are not per-frame proof of temporal/history coverage or optical shutter quality. The experiment retained temporal blur enabled and made no blur-quality shortcut.

Cleanup confirmed damage/probe/shutter/rotation0, HOMEface8/page0, awake0/perf0/trace0, followed by finalCRC guard0 as the last command; COM3 closed before any next stage. Final complete info snapshot internal22,871B/largest8,704/min12,100; DMA15,139B/largest8,704; NOSALLOC0. No allocation/crash evidence was observed. `build/hardware-iteration22-settings-analysis.json` and `build/hardware-iteration22-settings-cells.json` retain normalized traffic, cache brackets and all worst-presentation pairs; exact helper snapshot SHA `6a0d974471ae13fb3a0112ad13cfe7351b0812c11e7f47f151c3eb62006f776b`. Raw capture SHA256:

- `build/hardware-iteration22-settings-ab.jsonl`: `cea0a4848d85cae6d3f830670c75da5796d3a5478ed68fa2dc7711e843bb0b06`.
- `build/hardware-iteration22-settings-ab.diagnostics.json`: `33e45bdeeb182bf5ff95d1770d1ce12061814b6dafd4913f9ea0f0dfbbd3eb91`.
- `build/hardware-iteration22-settings-ab.transport.json`: `d1f9b8aebfd78d37d85426d4f7beef5cb08bcd62301375d3aef27e0331bff4b6`.
- `build/hardware-iteration22-settings-ab.quiet-windows.json`: `eb23d15180987f50dc2a6b7a50ee1685c43ea3bcb81dfac842ace52ad30f3e53`.

### Iteration 22: embedded Apps damage-band ABBA completed

The separately authorized embedded Apps stage used the same immutable 22 firmware without flash/reset or Settings replay. Frozen `build/bands22_apps.py` SHA256 `992c3c3a867c6603e93226a3d67942670b9d9c0c561c852da168c067f7767e32` entered Apps by a real rightward swipe from locked HOME face 8, verified HOME page 1 and exact remembered list endpoints, and collected eight new ABBA 0/1/1/0 windows at 290 px over 1000 ms, both y signs. Probe 1, guard 1, blur/history/uniform/packed 1, worker/full/shutter/rotation 0 were retained. No standalone launcher substitution or old cells were used.

All cells passed complete state/nav/source/heap, pipeline completion and damage-backend checks. All 284 on-mode painted frames used damage bands with fallback 0. No query retries, faults or allocation evidence occurred. Raw false connection checks increased 6 and scoped overrides increased 6 while required replies remained complete; queue/driver enqueue/direct-reply failures stayed 0. This adds bounded exercised-guard evidence, without qualifying other tasks/disconnections.

| ABBA index / mode / sign | painted / intervals | max e2e / present ms | deadlines / floor | RGB565 bytes / painted | chunks / painted |
|---|---:|---:|---:|---:|---:|
| 0 / 0 / -1 | 70 / 69 | 27.625 / 28.474 | 34 / 0 | 260228.7 | 55.786 |
| 0 / 0 / +1 | 70 / 69 | 27.401 / 28.599 | 35 / 0 | 267573.8 | 54.729 |
| 1 / 1 / -1 | 71 / 70 | 22.412 / 26.756 | 0 / 0 | 169464.5 | 56.000 |
| 1 / 1 / +1 | 71 / 70 | 23.625 / 26.863 | 0 / 0 | 178759.2 | 54.958 |
| 2 / 1 / -1 | 71 / 70 | 22.513 / 26.671 | 0 / 0 | 169464.5 | 56.000 |
| 2 / 1 / +1 | 71 / 70 | 22.470 / 26.337 | 0 / 0 | 178759.2 | 54.958 |
| 3 / 0 / -1 | 69 / 68 | 27.555 / 28.964 | 37 / 0 | 260107.6 | 55.739 |
| 3 / 0 / +1 | 69 / 68 | 28.087 / 28.851 | 32 / 0 | 268757.9 | 54.652 |

Across each mode's four windows, normalized bytes/painted fell 264,165 to 174,112 (34.09%); chunks/painted rose 55.227 to 55.479 (0.46%); rotation fell 5.170 to 3.481 ms/painted (32.68%). The on-mode warm repeat had identical bytes/chunks to its first on-mode pair, while off reverse results remained close after normalization. Commanded input and starting endpoint poses matched; painted frame/pose samples differed slightly (278 off versus 284 on), so normalized figures are reported instead of raw totals or an exact same-frame causal percentage.

Max e2e fell from 27.401–28.087 ms off to 22.412–23.625 ms on. Max presentation interval fell from 28.474–28.964 ms off to 26.337–26.863 ms on. All eight windows had floor 0; on-mode work deadline counts were 0 versus 32–37 off. Nevertheless, presentation maxima still exceeded the 25 ms target: this is a repeatable bounded 290 px/s improvement, not all-speed/global 40 fps or 30 fps certification. API/history/raster/rotation overlap; their elapsed counters must not be summed. No 200 MHz extrapolation or power result is claimed.

Generic BLUR list counter brackets were all zero during these launcher windows. Thus that diagnostic does not establish launcher motion-blur coverage; enabled blur/history flags alone are not a per-frame or optical proof. Endpoint warmup is not proof every directional cache level was ready. The stage made no blur-disable shortcut, but the image-quality requirement remains separately unqualified.

Cleanup explicitly returned HOME face 8/page 0, damage/probe/shutter/rotation 0, worker/full 0, awake/perf/trace 0, then direct CRC confirmed guard 0 as the final device command. COM3 closed. Final complete heap: internal 22,871 B/largest 8,704/min 12,100; DMA 15,139 B/largest 8,704; NOSALLOC total 0. All 180 commands were single attempts; max host write 2.5016 ms/flush 0.2033 ms; reader/shutdown errors 0, no pending/unterminated bytes.

`build/hardware-iteration22-embedded-analysis.json` and `build/hardware-iteration22-embedded-cells.json` pin traffic, exact counters, cache brackets, worst completion pairs and helper snapshot. Raw evidence SHA256:

- `build/hardware-iteration22-embedded-ab.jsonl`: `4b3c34e10b5c22c0e1ec8461d15d60e076a65a7cf3e4be04ed4ee69dd514f846`.
- `build/hardware-iteration22-embedded-ab.diagnostics.json`: `ebc6bb4500b54ddd1e939558c8c25eb4ed0d4edb3e56553c7752754b06a7245a`.
- `build/hardware-iteration22-embedded-ab.transport.json`: `0e644588971d33f5d6c339527ecc85beb3531eda8ea7367a8707204d681d03c2`.
- `build/hardware-iteration22-embedded-ab.quiet-windows.json`: `cbf4a269fe94d4d547f5a6161cdd95f295c843997f131684085b5a23d973ad3d`.


## Iteration 23a: exact app-only update and isolated vertical-kernel measurement

Immutable `build/artifacts/iteration23a-vertical-blur-diagnostics`: tree `a656e8b793f7273ad02210039dec373587708a85eaca6214599a6af8876bd512`, ELF `9f770cfb71772873c679d8504ce39f732b0885290048955385bed3bcb876e477`, BIN `a6583bf1dc630671b6d52e66c849e0bf535462219cbf675cd69e71d6f43b5fea` (2,810,352 bytes). The single authorized device flash started from exact22 prefix and complete-slot MD5 `4c5275a5a80972aff456e208c6af7f9d`. It passed MAC, boot/layout/OTA, image readback, additive complete-slot readback and all outside-slot before/after checks. Complete23a slot `build/recovery/iteration23a-vertical-blur-diagnostics-sector-expected-slot.bin` has SHA256 `6d042202a301ad9aa4d4bec3346e9fc7a9912f0a977706b6e3e6a5ccc7a31fe7`, MD5 `6a2eec9efa195bed82b9e5d0e7d9346e`, absolute sector erase edge `0x2cf000`. The first outside-slot region's baseline was `7f57c51e00677962f88e5c6c1ed06351`, different from the earlier22 record; it stayed unchanged during this write. The second region remained `d39d7a06af70d1b1e1d903f52cee17bc`. No cause is assigned to that pre-existing baseline change.

`build/hardware_flash23a.py` retained the reviewed original checks and added whole-slot MD5 verification while the same flasher stub remained active. An initial system-Python launch failed importing esptool before opening COM3; its log is separately preserved. The established ESP-IDF Python then made the only actual device flash attempt. One captured reset followed, with PREBAKE830ms, basis/shade mismatches0, ready1/rejection0 and startup main-task minimum1976B.

Only explicit `--stage kernel` ran. Four complete fresh info snapshots confirmed the exact23a ELF and240MHz. Direct nonce/length/CRC frames established guard0, confirmed guard1, sampled counters, and finally confirmed guard0 after ordinary cleanup. The isolated command used a temporary3,728B four-row internal RGB565 stripe and2,796B PSRAM column state, reset once per full pass and carried across all117 stripes (last stripe two rows). For each k1–4 and four input patterns, two full466×466 passes timed only actual `mb_v_columns` calls. Input fill, checksumming, printing, allocation and release were outside the timers. Buffers were reported released; no renderer cost seed or UI blur counter changed. Runtime admitted/skipped/reduced H/V counters stayed0 and seeds remained full100/150ns per pixel, reduced35/100ns per pixel.

Values below are cell mean / slower complete-pass ns per processed pixel. Means alone are insufficient for a conservative seed decision.

| Input | k1 | k2 | k3 | k4 |
|---|---:|---:|---:|---:|
| All black | 155.277 / 155.478 | 155.891 / 156.298 | 154.844 / 155.473 | 156.895 / 158.913 |
| Sparse white dots | 157.867 / 158.674 | 157.473 / 157.845 | 157.246 / 157.440 | 159.570 / 160.207 |
| Dense white | 168.852 / 169.404 | 168.184 / 168.515 | 168.812 / 168.856 | 167.812 / 168.432 |
| Dense RGB | 168.703 / 169.344 | 168.562 / 168.856 | 170.254 / 171.448 | 168.332 / 168.409 |

The slowest complete pass was dense RGB/k3:37,231us for217,156pixels,171.448176ns/pixel; its two-pass mean was170.253906ns/pixel. Full-pass observed times ranged33,489–37,231us. Thus this full-area kernel alone exceeded the26ms live-filter CPU budget, before app drawing/history/rotation/DMA. These are actual240MHz microbenchmark results with four-row internal stripes, PSRAM state and possible scheduler/interrupt interference; they are not full app/frame timing,30/40fps,200MHz, power or optical qualification. No navigation workload, standalone app workload or quiet rendering window was run, and no cost seed was changed.

All16 target checksums, both passes, matched the immutable23a host-C wrapper and an independent scalar R/G/B integer recurrence oracle. `build/hardware-iteration23a-kernel-oracle.json` records the full table; the host wrapper's fake timer is explicitly excluded from performance evidence. `build/hardware-iteration23a-kernel-cells.json` preserves pass0/pass1 times, maximum stripe times, normalized means, checksums and CPU frequency.

Fresh heap before and after the kernel was internal22,547B/largest8,704/min11,932; DMA14,815B/largest8,704. NOSALLOC registered1/total0 across all four info snapshots; touch-task minimum2,848B and flush minimum1,828B. Touch transport health was not clean: preflight reads1,399/errors95, pre-kernel1,737/432, post-kernel2,170/864, cleanup2,841/1,213, last_error259 and injected/pending0. This error growth was reported immediately and is not attributed to the kernel by these aggregate brackets; no real-touch health or input-path qualification follows. No fatal/allocator line, USB reader error, command response failure or retry occurred.

All33 commands were single attempts:29 ordinary commands each had exactly one timestamp-correlated fresh DONE, plus four valid direct CRC frames. Enabled-to-final raw false checks0, scoped overrides0, queue/driver enqueue/direct-reply failures0; the protective false-predicate branch was not exercised. Max host write1.0845ms/flush0.074ms. Cleanup confirmed HOMEface8/page0, awake/perf/trace0, worker/full/probe/shutter/rotation/damage0 and blur/history/uniform/packed1. The final device command was direct CRC guard0; no ordinary command followed. Reader stopped with no errors, pending queue or unterminated bytes; COM3 closed. Navigation/apps remain held for the measured-cost decision.

`build/hardware-iteration23a-analysis.json` pins the exact archive, host snapshot, flash helper/report, raw captures, counters and cleanup. Raw SHA256:

- `build/hardware-iteration23a-kernel.jsonl`: `cd8622c1e21e6300054f3729034b96081af0842c67fe64321a9848902f51febf`.
- `build/hardware-iteration23a-kernel.diagnostics.json`: `a51f940aaa89c5b9cfff483443ea872764dc8eb32491498b37efe1f88a4ea43c`.
- `build/hardware-iteration23a-kernel.transport.json`: `4bdb39bd3caa5a829ba42b726aec4a01a220f4b1bc23050169558ef1f264dbc6`.
- `build/hardware-iteration23a-kernel-oracle.json`: `aedf6d64f6055fada55a1c6e4046a8aa5cfbc83189339786f1ee108347b02f5a`.


## Iteration 24: pixel-identical span kernel reduced isolated vertical-filter time

Immutable `build/artifacts/iteration24-vertical-span-kernel`: tree `bda0d4fcf93ba776d2ee5fcb1cacfc256602761d233fdfd7bd89aac53c66a56d`, ELF `95ecd2a6d3dd901bdb0bfa332b17f02d52d068c583425fe3f26d8def9e6f2d95`, BIN `21f71c9201aa4b1486d4e4c67393c9de9e994e847b198024fbd9647a9fbe930c` (2,810,688 bytes). One authorized app-only write started from the exact23a prefix and complete-slot MD5 `6a2eec9efa195bed82b9e5d0e7d9346e`; MAC, boot/layout/OTA, image and complete-slot post-readback, and all outside-region before/after checks passed. Current complete expected slot `build/recovery/iteration24-vertical-span-kernel-sector-expected-slot.bin` SHA256 `97541e14f4fece28cbf65f9280bac3234375d2e57ff11053845d4b7ab9de5fd5`, MD5 `36b6dab00b0a7b31feedb78b5d046a82`, absolute sector erase edge `0x2cf000`. The same additive host flash helper and ESP-IDF Python were used; no reset beyond the one captured post-flash reset occurred.

Only explicit kernel stage ran with the unchanged frozen host protocol and exact24 identity. Four fresh info snapshots confirmed that ELF/240MHz. PREBAKE823ms reported basis/shade mismatches0/ready1, rejection0 and startup main-task minimum1,928B. The benchmark kept the same four patterns, k1–4, two passes, full466×466 area, temporary3,728B internal four-row stripe and2,796B PSRAM state as23a. Allocation/reset/input fill/checksum/printing remained outside timed kernel calls; buffers were released. It changed no runtime seed or blur-control state. All16 target checksum pairs also matched23a, archived24 host C, and the independent scalar R/G/B recurrence oracle.

Cell mean / slower complete-pass ns per processed pixel:

| Input | k1 | k2 | k3 | k4 |
|---|---:|---:|---:|---:|
| All black | 104.559 / 104.948 | 106.867 / 109.626 | 104.840 / 107.098 | 108.211 / 109.124 |
| Sparse white dots | 104.414 / 104.708 | 105.355 / 107.305 | 106.598 / 109.055 | 104.410 / 104.952 |
| Dense white | 105.289 / 106.338 | 106.660 / 106.803 | 104.496 / 105.385 | 107.684 / 111.284 |
| Dense RGB | 104.219 / 104.639 | 106.344 / 107.863 | 104.047 / 105.086 | 106.770 / 108.015 |

The slowest complete pass was dense white/k4:24,166us for217,156pixels,111.284054ns/pixel; its two-pass mean was107.683594ns/pixel. Full-pass times ranged22,276–24,166us. Matched-cell two-pass mean times fell31.03–38.89% relative to23a, with bit-identical output. This is a measured isolated kernel improvement, not full app/frame qualification. The worst kernel sample leaves only1.834ms of a26ms CPU budget for other work; app drawing/history and display transfer were not timed here. A seed decision must account for the slower complete pass and interference, rather than selecting the best or mean sample. No app/navigation workload, quiet rendering window,30/40fps, optical,200MHz or power result follows. Runtime H/V admitted/skipped/reduced counts stayed0 and existing full100/150/reduced35/100ns-per-pixel seeds stayed unchanged.

Final fresh heap: internal22,851B/largest9,216/min12,296; DMA15,119B/largest9,216. All four NOSALLOC reports registered1/total0. Touch-task minimum2,848B and flush minimum1,828B were observed. Touch errors again grew (reads1,396/errors93 at first info to2,795/809 at cleanup, last_error259, injected/pending0), so touch health remains unqualified and is not assigned to the kernel by these aggregate observations. No fatal/allocator evidence or USB response/reader failure was observed.

All33 device commands were single attempts,29 ordinary replies each with exactly one fresh timestamp-correlated DONE plus four valid direct nonce/length/CRC frames. Raw false/override, queue/driver enqueue and prior direct-incomplete deltas stayed0; the reply guard's override branch was not exercised. Maximum host write0.754ms/flush0.1287ms. Ordinary cleanup confirmed HOMEface8/page0, awake/perf/trace0, candidate/worker/full flags0 and normal blur/history/uniform/packed1. Direct CRC guard0 was the last device command, then the reader stopped and COM3 closed with empty tail/queue and no reader/shutdown errors.

`build/hardware-iteration24-analysis.json` pins exact source/identity, complete flash report, per-pass metrics, comparison/oracle, health counters, raw bytes, host snapshot and cleanup. Raw SHA256:

- `build/hardware-iteration24-kernel.jsonl`: `d72d1f955678aba9da2b9d05ed1bdfab701da3ee22e0768d35dfc7a43b5cd203`.
- `build/hardware-iteration24-kernel.diagnostics.json`: `d47feafb27fb5b6972e25ac0246f11ac2352ea48d3ccd6033e292dcb2e569e61`.
- `build/hardware-iteration24-kernel.transport.json`: `889580dcdd2f3ce0e82428fd961489c7fe350f2111e840ce691bef7ca3a2e58f`.
- `build/hardware-iteration24-kernel-oracle.json`: `5067d688b417d8f821173be8a650a87ffe73eb8b7db15a26d084925fbe831d5c`.


### Iteration 24: bounded navigation direction and signed-slot checks passed

On the unchanged exact24 ELF `95ecd2a6d3dd901bdb0bfa332b17f02d52d068c583425fe3f26d8def9e6f2d95`, the separately authorized navigation-only stage attached normally with no flash/reset. Initial compact state observed HOMEface8/page0 inAOD/pwr1. Guard1 was confirmed by direct nonce/CRC, then known awake1 plus explicit aod0 established activepwr0; three fresh full info snapshots throughout the stage confirmed exact identity and allocation/heap state. Ordinary rendering flags remained normal, with experimental controls0.

All29 fresh settled navigation checks passed. Logical IDs remain Face0/Apps1/Activity2/Heart3/Weather4/Music5/Timer6. Their physical left-to-right order and offsets were observed as:

| Page | Logical ID | Physical slot | Offset q8 |
|---|---:|---:|---:|
| Activity | 2 | -5 | -596480 |
| Heart | 3 | -4 | -477184 |
| Weather | 4 | -3 | -357888 |
| Music | 5 | -2 | -238592 |
| Timer | 6 | -1 | -119296 |
| Face | 0 | 0 | 0 |
| Apps | 1 | +1 | 119296 |

A leftward finger swipe from Face entered embedded Apps; a rightward swipe returned to Face, then subsequent rightward swipes traversed Timer/Music/Weather/Heart/Activity. The reverse walk and both outer limits passed. Button Apps→Face and Face→Apps behavior passed, as did a vertical embedded-list gesture that kept topHOME/page1. Its remembered list offset ended460px; cleanup returned face/page0 while retaining that list position. These were bounded serial-injected gestures through the normal input path and fresh state/pose observations; there were no standalone app windows, quiet timing cells, optical observations or frame-rate claims.

The98 single-attempt commands comprised94 ordinary replies with exactly one timestamp-correlated fresh DONE and four valid direct CRC frames; no query/cell retry occurred. Raw false/guard-override, queue/enqueue and prior direct-incomplete deltas remained0. Maximum host write2.7061ms/flush0.0971ms. Final complete heap was internal22,627B/largest9,216/min12,296; DMA14,895B/largest9,216 and NOSALLOC0. Touch minimum fell from2,848to2,816B; flush minimum remained1,828B. Touch errors continued growing (reads8,055/errors3,069 to14,221/6,552,last_error259,injected/pending0), so this does not qualify the physical touch transport or attribute those errors to navigation.

Cleanup confirmed HOMEface8/page0, awake/perf/trace0 and candidate/worker/full controls0; the final command was direct guard0 with valid CRC, then COM3 closed. Reader/shutdown errors0 and no pending/unterminated bytes were retained. `build/hardware-iteration24-navigation-analysis.json` pins all cases, commands, identity/health, raw hashes and frozen helper. No app device calls followed.

- `build/hardware-iteration24-navigation.jsonl`: `5bd4d21c4795c51bfe088cb3afeb0bea6df94a3e90ece8c9f78e9298fa9860e0`.
- `build/hardware-iteration24-navigation.diagnostics.json`: `caf6b70557c2d3d4370238acb5bc3f6c2cbb1270e41c1bee1eb492791e7f7a35`.
- `build/hardware-iteration24-navigation.transport.json`: `6e383946920aaa6095a446fd834aa3dbc13d6f89af0fc1ecfb78dda717e7f1f2`.


## Iteration 25: guarded kernel-only capture; runtime cadence candidate not yet app-qualified

Immutable `build/artifacts/iteration25-cadence-exposure`: tree `ed3d77ec61c5354829000ebd352dfef7195f4263ac316373f6bae4308451999d`, ELF `c6bea2f085f18874bf73fc8320d0b34121fba7c4f6f14e74df2ede1d2a1e8d57`, BIN `048074102f7545cb6ddb4fdaed62d3e4b9178cc9dc46315b63a55c1bf389926c` (2,812,448bytes). One authorized app-only flash from exact24 prefix and whole-slot MD5 `36b6dab00b0a7b31feedb78b5d046a82` passed MAC, boot/layout/OTA, image and complete-slot post-readback and all outside-region checks. Complete current slot `build/recovery/iteration25-cadence-exposure-sector-expected-slot.bin` SHA256 `4f36881d01851c1da54505aa60d765c86c3bac2879673a1278695625cce641b9`, MD5 `1795e919eb8d87c32e6c9f41d17a5e3e`, absolute sector erase edge `0x2cf000`. The reviewed additive flash helper and established ESP-IDF Python were reused; one captured reset followed.

Only the frozen kernel-stage host protocol ran; no app sentinel, app/navigation workload or quiet rendering window ran. Four complete fresh info snapshots matched exact25/240MHz. Each retained new readonly `NOSHISTORY v=1 cadence=1 interval_us=25000 previous_q5=11 denominator=32`. There was no history-mode command or toggle: the authorized kernel stage left the default candidate at1. These snapshots verify reported mode/weight at their query boundaries, not temporal image quality, all-frame weight distribution, toggle correctness or app performance. The candidate remains unaccepted pending the separately reviewed runtime AB sentinel. Production intervals are logical quantized/clamped motion time, not an invariant wall-clock shutter.

The same isolated four-pattern/k1–4/two-pass kernel benchmark used the temporary four-row3,728B internal stripe and2,796B PSRAM state, with only actual kernel calls timed. Allocation/setup/reset/fill/checksum/printing and DMA were excluded; buffers were released. All16 target checksum pairs matched24, archived25 host C and independent scalar RGB math. Cell mean / slower complete-pass ns per processed pixel:

| Input | k1 | k2 | k3 | k4 |
|---|---:|---:|---:|---:|
| All black | 90.008 / 90.221 | 92.848 / 93.081 | 92.949 / 95.236 | 93.539 / 95.705 |
| Sparse white dots | 91.355 / 92.496 | 95.672 / 99.076 | 93.023 / 96.065 | 91.660 / 92.703 |
| Dense white | 95.301 / 99.141 | 92.574 / 94.674 | 90.832 / 91.308 | 91.633 / 91.948 |
| Dense RGB | 91.430 / 92.109 | 91.281 / 92.252 | 93.305 / 94.241 | 89.723 / 89.742 |

The slowest complete pass was dense white/k1:21,529us for217,156pixels,99.140710ns/pixel; full-pass samples ranged19,480–21,529us. Means were89.723–95.672ns/pixel, lower than the prior24 run even though the kernel source is unchanged. No causal speedup is assigned to the new temporal candidate: pure-kernel wall timing can change with code/data placement, cache state, interrupts and scheduling, and this run does not isolate those effects. It is not complete app rendering/history/transfer cost, frame-rate, directional/optical,200MHz or power qualification; the next app stage remains held for review.

PREBAKE834ms reported basis/shade mismatches0/ready1, rejection0 and startup main-task minimum1,928B. Final fresh heap: internal22,459B/largest8,704/min11,868; DMA14,727B/largest8,704. All four NOSALLOC reports registered1/total0. Touch-task minimum2,856B and flush minimum1,836B were observed. Touch errors grew from reads1,401/errors97 to2,775/1,213,last_error259,injected/pending0; touch transport remains unqualified, with no cause assigned by these aggregate brackets.

All33 commands were single attempts:29 ordinary replies each had one timestamp-correlated fresh DONE, plus four valid direct nonce/length/CRC frames. Raw false/override, queue/driver enqueue and prior direct-incomplete deltas remained0, so the override branch was unexercised. Maximum host write0.7074ms/flush0.1401ms. Cleanup confirmed HOMEface8/page0, awake/perf/trace0 and candidate/worker/full controls0 except the explicitly retained history_cadence1; normal blur/history/uniform/packed flags stayed1. Final command direct CRC guard0 confirmed mode0, then COM3 closed with reader/shutdown errors0 and no pending/unterminated bytes. No app command followed.

`build/hardware-iteration25-kernel-analysis.json` pins exact source/identity, flash/recovery, all per-pass data, oracle, raw history metadata, health, frozen helper and cleanup. Raw SHA256:

- `build/hardware-iteration25-kernel.jsonl`: `9f044d9b2d1f368372254cfa30dd3f8eeb1d7a74c60edb2fb6a43b4c533df6aa`.
- `build/hardware-iteration25-kernel.diagnostics.json`: `422f01f9dfed8a9286a63563b3d93502b6f4814a1b5647f335d2604170e42ebc`.
- `build/hardware-iteration25-kernel.transport.json`: `71c3c298ad5a238cdacb268b559eb0dac7f27a2cea002294beaddae6cc6aa103`.
- `build/hardware-iteration25-kernel-oracle.json`: `42096a6c84e5f7d43835d888f9ce393535982dddffdc52027e53c90bb2eec992`.


### Iteration 25: Activity runtime AB sentinel INCOMPLETE and candidate rejected

A separately authorized normal attachment on exact25 ran only the eight planned Activity windows:290px/s cadence0→1,900px/s cadence1→0, each negative/positive y. No reset/flash, cell retry, full matrix, Heart or Weather workload occurred. The frozen helper retained strict identity/heap/owner/control/schema/actual-pose assertions while recording V0 denial and temporal blending as diagnostics. Its cadence selection ACK validated the new mode only; fresh info after>=0.3s validated the subsequent generation's logical interval/Q5 weight (off8, on25/50/75/100ms→11/5/4/3). Both25ms/off8 and25ms/on11 were observed. Those query brackets do not measure every captured frame's weight or prove optical quality.

All eight hardware quiet captures completed, but only seven passed the endpoint checks. The final baseline900px/s positive-y window started at119,296q8 (466px), reached a minimum61,184q8 (239px), then ended again at119,296 instead of the required0. The helper stopped on this validation failure, preserved the final raw window and sent only its one planned direct CRC guard0 diagnostic before closing. It did not retry the gesture, query a new pose, or perform ordinary cleanup in that failed capture. The original quiet-windows JSON therefore contains seven validated cells; `build/hardware-iteration25-activity-sentinel-windows.json` separately preserves all eight raw captures with the last clearly marked pose-invalid. This failed endpoint cannot be used as a matched return traversal.

| Cadence / speed px/s / y sign | max e2e / present ms | deadline / floor misses | actual V-filter frames / denied calls | history blended pixels | Pose |
|---|---:|---:|---:|---:|---|
| 0 / 290 / -1 | 54.515 / 56.371 | 34 / 32 | 0 / 23 | 555763 | valid |
| 0 / 290 / +1 | 48.548 / 50.879 | 36 / 37 | 0 / 26 | 546121 | valid |
| 1 / 290 / -1 | 50.376 / 59.926 | 33 / 35 | 0 / 25 | 539766 | valid |
| 1 / 290 / +1 | 52.294 / 52.672 | 35 / 36 | 0 / 26 | 509336 | valid |
| 1 / 900 / -1 | 52.187 / 61.644 | 18 / 21 | 0 / 11 | 322188 | valid |
| 1 / 900 / +1 | 50.755 / 55.300 | 19 / 20 | 0 / 11 | 291848 | valid |
| 0 / 900 / -1 | 50.087 / 60.166 | 20 / 19 | 0 / 11 | 324010 | valid |
| 0 / 900 / +1 | 44.217 / 51.990 | 19 / 20 | 0 / 11 | 338812 | FAILED endpoint |

The candidate's four pose-valid windows all failed the declared floor0/e2e<=33,333us/presentation<=33,333us criteria; baseline misses do not excuse those failures. The balanced sentinel is incomplete and cadence1 is NOT accepted. Every window had actual live-V filter frames/pixels0, positive denied calls and positive actual history-blended pixels. This proves observed temporal processing and denial of live directional blur, not restored directional quality. No numerical or optical directional requirement is marked passed.

Worst-presentation pairs are retained with exact frame IDs/timestamps in the windows JSON. For candidate900px/s negative-y, the previous/current e2e were15.615/52.187ms. The current frame reported tick0.482ms, history13.243ms, raster49.696ms, UI wait0.068ms, API13.930ms, DMA wait0.152ms, queue wait26.265ms, rotation7.065ms and340,864RGB565 bytes. Its baseline900px/s negative-y current frame reported e2e50.087/history11.831/raster47.641/API13.730/queue24.307/rotation7.080ms for the same reported byte count. These stage counters overlap and must not be summed; a worst-presentation frame is not necessarily the maximum-e2e frame. Record and layer-specific stage times are unavailable in the current quiet NOSPIPEFRAME schema; no trace or extra profiling was enabled, and those missing stages are not inferred by subtraction. The slow frames already occur with zero executed live directional filter, so the isolated kernel benchmark alone does not establish acceptable app work/presentation time.

The failed sentinel used101 single-attempt commands (98 ordinary unique-DONE replies and3 valid direct CRC frames). No USB/reader, allocator or fatal fault caused this stop. Raw false checks and scoped overrides each increased3 while tested replies remained complete; queue/driver enqueue/direct-incomplete counters stayed0. This is bounded exercised-guard evidence; it does not assign each override to an exact stdout write. The last confirmed history mode was already0 before the failed final window, but ACTIVITY/awake1/probe1 ordinary cleanup remained incomplete.

Root separately authorized a bounded cleanup with fresh normal attachment, exact identity/health, explicit history_cadence0 and no gesture/cell replay. That separate capture completed23 single-attempt commands (20 ordinary unique-DONE replies and three valid CRC frames), confirmed history0/previous8, restored HOMEface8/page0, worker/full/probe/shutter/rotation/damage0 and awake/perf/trace0. Final power wasAOD/pwr1 after normal autosleep was restored, not an active-display claim. Final complete heap: internal22,235B/largest8,704/min11,868; DMA14,503B/largest8,704, NOSALLOC0; touch-task minimum2,824B and flush minimum1,836B. Touch reads/errors reached27,319/8,889,last_error259,injected/pending0, so physical touch transport remains unqualified. Final direct CRC guard0 was the last command, reader/shutdown errors0, no pending/unterminated bytes, COM3 closed. The separate cleanup does not retroactively complete or accept the sentinel.

Exact paths: failed raw `build/hardware-iteration25-activity-sentinel.jsonl`, all-eight-window/stage data `build/hardware-iteration25-activity-sentinel-windows.json`, separately authorized cleanup `build/hardware-iteration25-sentinel-cleanup.jsonl`, combined provenance/health/pose/miss record `build/hardware-iteration25-activity-sentinel-analysis.json`. Raw SHA256:

- `build/hardware-iteration25-activity-sentinel.jsonl`: `6f0b57f5cd3e7eddf94c82f15e70c1d967971d08acea01ac9230b24a81c437a1`.
- `build/hardware-iteration25-activity-sentinel.transport.json`: `e0ef098552dbef5ac4d64281b0d1ffbded2323673023b61674299b717b6b74d2`.
- `build/hardware-iteration25-activity-sentinel-windows.json`: `71c82e81137ea5082171e0d83d6e3c08ea1f52d9b03a88e92261a4105a64733f`.
- `build/hardware-iteration25-sentinel-cleanup.jsonl`: `b18957f50a10249dab655666f9138d943c55d63997005823e0eb14ee6f9b2fe8`.
- `build/hardware-iteration25-sentinel-cleanup.transport.json`: `b2d447cc6856caa6ee6a069f6d1aed7c5e1e3d1f175e135c79636f4f271b965f`.


## Iteration26 exact sharp-drawing candidate: incomplete Activity geometry sentinel (2026-10-17)

Authorized bounded scope: one guarded25-to26 app-only write, one captured reset, Activity290 geometry0/1/1/0 ABBA both signs (eight requested windows), then stop for review. Broader Activity/Heart/Weather290/900 matrix was not authorized or run. Exact drawing changes are ring angle/extent metadata, solid polygon spans, and grouped sharp-history scans. History cadence remains0 following iteration25 rejection. Optical quality, actual directional coverage, and the absolute frame floor were not waived.

Frozen artifact `build/artifacts/iteration26-sharp-raster`: tree `09ac56be27236f92066189048df2d825a32d098cd6b6b0a2b345087504af9557`; ELF `3fd451d871e77abfab46ea597bf10a83f9af486f9266b8ab862610e29ec12faf`; BIN `e9dc2fb268d91be237a8e3d23616e558c57a6123ccc9f28c4024f03833b199fc`, 2,814,640bytes. Root reported firmware/sim/link,59 NOS tests,16,419 damage/full comparisons, lifecycle/memory/IO gates passed, with unchanged seven motion-shape failures and timing excluded. Device fresh identity independently matched the ELF at actual240MHz. Frozen host `build/validate26-host-snapshot/validate26.py` SHA256 `a954ba125d3b4c958edb9c3b977eb3235489a7b91371e66c830bbed4204df19c`; six exact-schema tests plus16 actual helper geometry/pose/fault/lifecycle tests passed offline; six inherited history-parser tests passed. Frozen manifest pins all host dependencies. No production source changes were made by the validation owner in this run.

Guarded flash used unchanged reviewed `build/hardware_flash23a.py`: exact25 full-slot SHA256 `4f36881d01851c1da54505aa60d765c86c3bac2879673a1278695625cce641b9` / MD5 `1795e919eb8d87c32e6c9f41d17a5e3e` matched before write. Verified full26 slot SHA256 model `9a0243ffe27b4a980557a09019b2581e6f0796f975ff5a8d2698ee48d71bda51` / on-target MD5 `e02de724bf0766501b204b4cf3e78ac1`, absolute sector erase edge0x2d0000, 0x780000bytes at0x20000. Boot/OTA/layout/MAC and both outside-partition regions remained guarded; outside MD5s before/after `7f57c51e00677962f88e5c6c1ed06351` and `d39d7a06af70d1b1e1d903f52cee17bc`. `build/recovery/iteration26-sharp-raster-sector-expected-slot.bin/json` preserve the complete next-write verifier. Original full16MiB recovery remains unchanged.

Host trajectories changed explicitly from iteration25: center x233, y423-to43 negative and43-to423 positive,380px over1310ms (290.076px/s). Every AB block had a predetermined fresh HOME/open; every window checked fresh settled start and end poses, exact owner, schema, heap, controls and immutable identity. The negative and positive windows required page0-to1 and1-to0 respectively. No conditioning gesture, command retry or cell replay was used. This longer traversal avoids relying on release velocity when slow frames discard the injector's final sample; it does not guarantee success at arbitrary frame duration. Results must not be pooled with iteration25's shorter trajectory as identical workloads.

**Sentinel INCOMPLETE: six of eight requested windows completed and passed exact pose/owner/protocol checks.** Before sending the seventh `bench begin`, the inherited pre-command boundary check found partial unsolicited background text `I (90505) weather: Updated Bengaluru: 27 C, feels 32 `. No seventh capture command or gesture was sent. The host stopped immediately, and its one planned direct CRC guard0 diagnostic drained the now-complete benign weather line and confirmed guard0. COM3 closed. This was a host validation boundary failure, with no observed device fault, fatal allocation event, missing/duplicate DONE, driver failure or retry. The incomplete ABBA cannot establish complete relative acceptance. Successful post-failure cleanup is separate evidence and does not complete this sentinel.

| Geometry/order/sign | Work max ms | Presentation max ms | Deadline misses | Floor misses | Actual V frames | History blended pixels |
|---|---:|---:|---:|---:|---:|---:|
| 0/0/-1 | 48.741 | 53.979 | 41 | 42 | 0 | 675302 |
| 0/0/+1 | 49.449 | 50.083 | 42 | 43 | 0 | 614870 |
| 1/1/-1 | 45.628 | 54.495 | 51 | 45 | 0 | 817842 |
| 1/1/+1 | 43.441 | 43.717 | 48 | 45 | 0 | 769472 |
| 1/2/-1 | 46.052 | 55.406 | 50 | 42 | 0 | 820698 |
| 1/2/+1 | 48.988 | 49.148 | 47 | 46 | 0 | 730889 |

Both baseline windows had geometry bytes0 throughout; all four candidate windows had enabled1/bytes2088 in fresh pre- and post-window info. The initial mode1 acknowledgment/home info bytes0 preceded standalone Activity opening; actual measured windows did allocate the optional metadata. This is immutable layout metadata, not app-image caching. Baseline p95 presentation bins49/49ms; candidate42/42/46/44ms. Candidate four-window worst work48.988ms versus baseline two-window49.449ms; candidate worst presentation55.406ms versus baseline53.979ms. Counts differ with frame cadence: baseline painted52/51, intervals50/49; candidate painted62/57/61/56, intervals60/55/59/54. These incomplete, unequal sample sets show observations only. Every candidate window still failed the absolute30fps requirements; no performance acceptance or complete ABBA claim is made.

All six windows executed zero live vertical kernel frames and zero actual V pixels, with V denied requests27/27/38/35/38/34 respectively. Actual history blends were nonzero as tabulated; history cadence remained0/Q5=8 before/after every cell. Admissions, kernel processed area, and history blended pixels are distinct counters. No directional blur quality requirement passed. Weather's cached directional pixels remain separately uninstrumented; Weather windows were not run here.

New quiet `NOSPIPEFRAME` exact fields record/compare/layer-record/layer-raster/layer-bands/layer-reused/producer-submit-sum/max were accepted with strict field order/count/range, fencing and worst-pair timestamp checks. `NOSPIPE queuewait_is_worker_idle=1` explicitly identifies flush-worker `xQueueReceive` idle time, excluding its first raw job. Producer-submit times measure UI `xQueueSend` separately. These timings overlap raster, history, DMA and API work: do not add them or subtract them to invent unobserved stages. API timing includes prior DMA credit waits; reported completion is not an optical fence.

At the worst candidate presentation interval55.406ms (second negative candidate), previous/current e2e13.239/43.642ms; current tick0.488, record0.983, compare0.131, raster40.844, history10.600, layer-record0.385, layer-raster0, UI buffer-wait0.061ms. Current API13.583, DMA-wait0.157, rotation6.889, worker receive-idle18.928, producer-submit sum0.147/max0.007ms,335168bytes. Baseline worst presentation53.979ms previous/current e2e18.941/47.918ms; current tick0.448, record1.407, compare0.129, raster44.872, history10.070, layer-record0.524, layer-raster0, UI-wait0.060, API13.288, DMA-wait0.154, rotation6.887, worker-idle23.071, producer-submit sum0.154/max0.006ms,335168bytes. Full prior/current stages for every window are preserved in the analysis/window JSON. These pairs support CPU drawing starvation as an inference; they do not indicate producer queue backpressure.

The sentinel transported115 commands (112 ordinary each with one fresh DONE, three direct CRC frames). SDK false-connected checks and UI overrides both increased2 with full ordinary responses; driver/queue/direct-incomplete counters remained0. Final reader stopped with no terminal/shutdown error, no pending bytes/chunks and guard0 confirmed. Physical touchscreen error counts were not qualified by these synthetic injected gestures.

Root separately authorized bounded26 cleanup only. `build/cleanup26.py` used the frozen26 transport and normal attachment with no flash/reset, no replay and no recovery followup on identity/health/transport failure. It completed27 commands (24 ordinary plus three direct), confirmed exact fresh ELF/heap, restored HOME face8/page0/offset0, geometry1/2088bytes, history0/Q5=8, pipeline_probe/shutter/rotation/damage/worker/full/trace/perf/awake0 and normal autosleep/AOD (last pwr1). CRC guard0 was the final command and COM3 closed. Last internal22119/largest8192/min11536; DMA14387/largest8192; NOSALLOC total0; touch stack minimum2824 and flush1832bytes. Last physical touch errors4051/16403reads,last_error259,injected/pending0 remain unqualified. Broader24-window app validation remains held pending root review.

Evidence hashes (all SHA256; the analysis file also embeds per-window and final health provenance):

- `build/hardware-iteration26-geometry-sentinel.jsonl`: `b0504b266a4925a6e3780e4a8bdf6f3ee614a93819ebab8a276b26f6198304ef`.
- `build/hardware-iteration26-geometry-sentinel.transport.json`: `23620d91c93ce0fc8721056de4ed81fbd6b354bb72045e23ccfa47d748ee7c9d`.
- `build/hardware-iteration26-geometry-sentinel.quiet-windows.json`: `1b6a2d712c9eaceb1b04cbfde422554af75394ee85589d7f79c8acc704acede6`.
- `build/hardware-iteration26-geometry-sentinel-analysis.json`: `7681df3b6ac5211892a29024d52e984135239288fef904d970ceb2b984c7bd98`.
- `build/hardware-iteration26-geometry-sentinel-windows.json`: `b666f96e5e493af975804cece1658011af35ff51c3999b7a9f946a6cbcd917cf`.
- `build/hardware-iteration26-sentinel-cleanup.jsonl`: `16fe83287a910edeed51e45ec1d0be5fc0d72fb70b608a1a16187a14f77faabe`.
- `build/hardware-iteration26-sentinel-cleanup.transport.json`: `4017538960f092222949e2f07394c2173a83d323d2b0aab510da8a49cd769e90`.
- `build/recovery/iteration26-sharp-raster.json`: `6ed4d0a2c16ecde90813467cf3d2b251dafc9c79b9830dcede8fefb465d34016`.
- `build/validate26-host-snapshot/manifest.json`: `15cdac249b1d2206ce9ce5b18219beb93f0508440a17b64e62b7d98add21bdcb`.
- `build/cleanup26.py`: `82c374052439bb836d01591ddf9a7d97fd292cbb4608254b14cb650d57d0a3fa`.


Iteration26 directional-count correction: the preceding prose V denied-request sequence was manually transcribed incorrectly. The exact per-window values in table order are **30/27/36/37/38/32**, as preserved in the immutable raw/analysis/window JSON. All six actual V frame/pixel counts remain0; timing, endpoint and cleanup conclusions are unchanged.


## Iteration26b host-only bounded app capture: Weather endpoint assumption rejected (2026-10-17)

Firmware unchanged from26: tree `09ac56be27236f92066189048df2d825a32d098cd6b6b0a2b345087504af9557`, ELF `3fd451d871e77abfab46ea597bf10a83f9af486f9266b8ab862610e29ec12faf`, BIN `e9dc2fb268d91be237a8e3d23616e558c57a6123ccc9f28c4024f03833b199fc`. No flash or reset occurred. Root authorized normal attachment and12candidate-only Activity/Heart/Weather windows at290/900 both signs, geometry1/history0/worker0 fixed. Frozen host `build/validate26b-host-snapshot/validate26b.py` SHA256 `dc70f925e73ec3bc505fd48a1572d8b12cf5917109080e2fdaa2674f29cafa25`. Target fresh identity matched exact26 at240MHz independently of evolving27 working sources.

The host-only pre-write boundary fix waits at most1s only for an already recognizable ASCII ESP I/W/E timestamped `weather:` background fragment to terminate. It discards no bytes, synthesizes no newline, validates every completed line as background, and still rejects unknown/short/command/ACK fragments, foreign lines or timeout before writing. All firmware diagnostic parsers are byte-identical to26; strict schema tests read immutable26 source.zip. Eight boundary tests (including actual cmd execution proving no command sent for unsafe/timeout fragments),14actual candidate/pose/fault/lifecycle tests, and6archive-schema tests passed. No bounded-fragment completion event occurred during this actual run, so split-line tolerance was verified offline, not exercised by observed target data. Original26 six-window incomplete evidence remains unchanged.

**Capture INCOMPLETE: ten fully validated windows plus one hardware-complete window that failed the planned endpoint assertion.** Activity4/4 and Heart4/4 met exact fresh0-to1-to0 pose/owner/control/heap/schema checks, and Weather290 both signs did likewise. Weather900 negative started0 and finished238592q8=page2, while the helper required119296q8=page1. The host stopped immediately after its complete quiet report, without querying the endpoint again, replaying the gesture, or sending Weather900positive. Its one planned direct CRC guard0 succeeded, then COM3 closed. No fatal allocator/stack/device fault or missing/duplicate ordinary DONE was observed. Normal cleanup was separately authorized afterward.

The failed Weather900 endpoint is a script-contract mismatch, not evidence that reaching page2 is incorrect UI behavior. Frozen26 Weather uses mscroll initialized with466px page spacing and releases using `msc_release(&W.sc,-in->vy)`; sampled release velocity can fling multiple pages. Its exposed page is the rounded current scroll offset, not a forced adjacent-page destination. Future Weather work must derive its expected endpoint from that policy instead of applying Activity's adjacent-page assumption. The existing invalid-pose label and raw record are preserved; this run is not retroactively accepted. Weather is excluded from the initial27 worker experiment.

All trajectories were the explicitly new380px x233 y423<->43 traversal, duration1310ms at290.076px/s or422ms at900.474px/s, with a predetermined fresh open at each app/speed and exact fresh pose checks per window. No cell or ordinary command was retried. Geometry1 metadata2088bytes was confirmed before/after every fully validated cell; history0/Q5=8 and worker0 remained confirmed.

| App speed sign | Pose accepted | Work max ms | Presentation max ms | Deadline misses | Floor misses | Actual V frames | V denied | History blended pixels |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| activity 290 -1 | yes | 44.478 | 54.593 | 50 | 40 | 0 | 37 | 833896 |
| activity 290 +1 | yes | 42.524 | 45.842 | 48 | 45 | 0 | 35 | 756014 |
| activity 900 -1 | yes | 44.715 | 56.761 | 25 | 18 | 0 | 16 | 439419 |
| activity 900 +1 | yes | 43.295 | 44.594 | 22 | 22 | 0 | 14 | 361021 |
| heart 290 -1 | yes | 37.331 | 41.994 | 56 | 20 | 0 | 46 | 957105 |
| heart 290 +1 | yes | 35.978 | 44.368 | 53 | 15 | 0 | 47 | 909361 |
| heart 900 -1 | yes | 40.171 | 43.819 | 28 | 14 | 0 | 18 | 518647 |
| heart 900 +1 | yes | 38.642 | 45.471 | 21 | 11 | 0 | 18 | 444507 |
| weather 290 -1 | yes | 33.717 | 43.582 | 39 | 2 | 0 | 39 | 538448 |
| weather 290 +1 | yes | 34.560 | 42.389 | 55 | 7 | 0 | 2 | 392308 |
| weather 900 -1 | no: page2 | 34.653 | 40.198 | 22 | 5 | 0 | 0 | 315964 |

Every captured window had nonzero floor misses; the candidate does not meet the absolute30fps requirement. Actual live V filter frames/pixels and pblur V command hits/misses were0 across these windows. Actual history blending occurred in all eleven hardware-complete windows. Weather's warm ui_fblur cached directional coverage is not measured by these V/pblur counters: V0 does not prove no cached filtering, and no directional quality requirement passed. The invalid-pose Weather900 timing is retained as observation, not a valid comparable cell. No baseline was rerun here; comparison against25 is limited by changed trajectory and26's exact primitive changes.

Worst-presentation pairs by app, previous/current stages in microseconds (overlapping timings; do not sum). These are each app's worst presentation pair, not necessarily its maximum-work frame:

- `activity_geometry1_fixed_900_sign-1`, presentation56761us: e2e_us=12955/44715; tick_us=421/437; record_us=870/888; compare_us=98/127; raster_us=11008/41809; history_us=1590/11212; layer_record_us=437/460; layer_raster_us=0/0; layer_bands=0/0; layer_reused=0/0; ui_wait_us=41/57; api_sum_us=5816/13937; dmawait_sum_us=101/154; rotation_sum_us=699/6986; queuewait_sum_us=3641/19551; producer_submit_sum_us=112/155; producer_submit_max_us=15/8; bytes=29120/340864.
- `heart_geometry1_fixed_900_sign1`, presentation45471us: e2e_us=10777/31229; tick_us=432/483; record_us=482/744; compare_us=80/114; raster_us=9158/28390; history_us=1994/9085; layer_record_us=116/234; layer_raster_us=0/0; layer_bands=0/0; layer_reused=0/0; ui_wait_us=57/298; api_sum_us=5616/11460; dmawait_sum_us=92/121; rotation_sum_us=707/5493; queuewait_sum_us=2323/11348; producer_submit_sum_us=119/109; producer_submit_max_us=17/7; bytes=29120/266688.
- `weather_geometry1_fixed_290_sign-1`, presentation43582us: e2e_us=9812/28379; tick_us=599/2763; record_us=500/662; compare_us=37/106; raster_us=7945/23729; history_us=934/9000; layer_record_us=125/0; layer_raster_us=6309/0; layer_bands=9/0; layer_reused=0/0; ui_wait_us=20/805; api_sum_us=3260/15615; dmawait_sum_us=53/119; rotation_sum_us=777/6240; queuewait_sum_us=3356/1638; producer_submit_sum_us=50/92; producer_submit_max_us=6/7; bytes=37440/314816.

Worker receive queuewait remains worker idle waiting for producer, not producer backpressure. Producer-submit xQueueSend time is reported separately. Source/measurement inference continues to point at CPU drawing work, with small measured producer submission waits in the Activity/Heart worst pairs; API/DMA/rotation/history/raster overlap. Completion timing is not an optical fence. Full per-window pairs and aggregate DMA counts remain in the window/analysis JSON.

After root separately authorized bounded cleanup, `build/cleanup26b.py` normal-attached exact26 with the same26b boundary helper and the previously reviewed bounded cleanup body. No flash/reset, cell replay or retry occurred. It completed27commands (24ordinary+3direct), restored HOMEface8/page0/offset0, geometry1/2088bytes, history0/Q5=8, worker/probe/shutter/rotation/damage/full/awake/perf/trace0, normalAOD/autosleep (lastpwr1), and sent CRCguard0 LAST before COM3 closure. Last NOSALLOC total0; internal22119/largest8192/min11536, DMA14387/largest8192; touch stack minimum2824, flush1832. Physical touch errors14569/43599reads,last_error259,injected/pending0 remain unqualified. Cleanup does not complete the12-window campaign.

Evidence hashes (SHA256):

- `build/hardware-iteration26b-candidate-apps12.jsonl`: `274e0e6bb576f01924658a48b22d701e243f58a477a5e2460144dd5263d8ad55`.
- `build/hardware-iteration26b-candidate-apps12.transport.json`: `5f561ead42c24926b970c6009301c84d41715818a37a7616b9ef2fb232c27ce3`.
- `build/hardware-iteration26b-candidate-apps12.quiet-windows.json`: `04048c48304d4709a3babaa340753e3b41179cbc5654a402bc1e944af0fab1e3`.
- `build/hardware-iteration26b-candidate-apps12-analysis.json`: `34e1091193442d0ca680be4b9b331535a28fa6fa6181143beb095ef2491379f4`.
- `build/hardware-iteration26b-candidate-apps12-windows.json`: `0518883ecd6515fcef358f1cdaee02c67329082dce26be1cf885d483ec84f2bb`.
- `build/hardware-iteration26b-apps-cleanup.jsonl`: `0c3d45bfa194b790be86b48ee3137811ebe32dcd82cfedfc6f74165ac363fde0`.
- `build/hardware-iteration26b-apps-cleanup.transport.json`: `1c10b07710ea2b8bf7e553d7996417ee60d03b0a17c34361e5ab72838ba3eccb`.
- `build/validate26b-host-snapshot/manifest.json`: `f39e410498f6bb10d1a5f6db25c9c7e1cedae21e3e86ed2b9fa6d3f06137ed84`.
- `build/cleanup26b.py`: `5c2d9df09c208029857ead8da3c67f53d1a73a6eeb552bce84a7dfb19fb673ce`.


## Iteration27 shared exact primitives: complete Activity exposure-worker ABBA (2026-10-17)

Root authorized one guarded26-to27 app-only write and one captured reset into eight Activity290 windows, exposure_worker0/1/1/0 both signs, then cleanup/close. No Heart/Weather window, worker policy change, extra kernel benchmark or automatic extension was run. Geometry1 and history_cadence0 remained fixed; the same380px x233 y423<->43 /1310ms trajectory was used. Each AB block had a predetermined fresh HOME/open; each window had exact fresh settled0-to1/1-to0 pose, owner, identity, controls, schema, stack/heap/allocation and completed-DMA assertions. No command/cell was retried.

Frozen artifact `build/artifacts/iteration27-shared-text-history`: tree `9a78530ab66920c3919d26f9f7b8f63885906cb54b5cd906c1026dbd9c6edbc1`; ELF `ff940e86ad637512177aae8b94ca376cfd9b5607fdb1cafa532c7ed8f9192660`; BIN `75cbb7818c7164dd915e5738160b1636538d6f28a52797784bb2baa9a409a876`,2,814,720bytes. Exact shared text/dot/history primitive changes retain default history0. Root's archived `host-validation.json` records63NOS Python tests plus native/power/asset suites,16,419damage/full frames, six forced497-frame paths exact25, memory/lifecycle/IO and USB link PASS, app_main960/8192bytes; existing unrelated motion-shape failures/timing exclusions remain documented separately. Fresh target ELF matched at actual240MHz; working-tree changes were not used to identify the target.

Reviewed guarded write checked exact26 complete slot SHA256 `9a0243ffe27b4a980557a09019b2581e6f0796f975ff5a8d2698ee48d71bda51` / MD5 `e02de724bf0766501b204b4cf3e78ac1` before writing. Complete27 expected slot SHA256 `e9e10589d40dd7e615d1f2438153293343dd1ea95b41b7ce5a07e7994e7777cf` / on-target full-slot MD5 `9c952e46f88dccb09bd8caad2498a254` verified after,0x780000bytes at0x20000, absolute erase edge0x2d0000. Boot/layout/OTA/MAC remained guarded; both outside-partition MD5s stayed `7f57c51e00677962f88e5c6c1ed06351` and `d39d7a06af70d1b1e1d903f52cee17bc`. Complete next-write model `build/recovery/iteration27-shared-text-history-sector-expected-slot.bin/json` is preserved; original full16MiB recovery remains unchanged.

Frozen host `build/validate27-host-snapshot/validate27.py` SHA256 `98fc2d1dcd1b752bf94f011746ea2534c6af784a42cbe8272abc827df0795955`;31offline tests passed (17worker configuration/dynamic state/ABBA/pose/fault/lifecycle, eight bounded background-boundary/actual command no-write tests, six strict pipeline tests against immutable27 source archive). Existing diagnostic schemas remained unchanged. Every window measured fresh exact six-field NOSWORKER pre/post selection, actual history pixels/blends, quiet frame counts and fenced DMA counts. **Per-job asynchronous execution and history-fence ownership are inferred from archived prepare/queue/fence code and the existing queued-job pixel oracle, not independently counted on the target.** `worker27-ownership-inference.json` pins relevant source hashes and this limitation. Selected worker1 is not reported as measured deferred/applied/fenced job counts.

**All eight requested windows completed and passed the capture protocol, pose, owner, mode and health checks. This is not a30fps or directional-quality acceptance.** Geometry metadata2088bytes/history0/Q5=8 were confirmed pre/post every cell. All actual liveV frames/pixels were0, with denied requests and nonzero actual history blends below. No pblurV command hit/miss was observed. Optical blur quality was not measured.

| Worker/order/sign | Work max ms | Presentation max ms | p95 bin ms | Deadline misses | Floor misses / intervals | V denied | History blended pixels |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0/0/-1 | 43.763 | 55.320 | 42 | 49 | 42 / 60 | 36 | 833118 |
| 0/0/+1 | 48.440 | 48.601 | 47 | 49 | 43 / 56 | 38 | 784903 |
| 1/1/-1 | 36.487 | 49.989 | 38 | 54 | 29 / 63 | 44 | 865555 |
| 1/1/+1 | 37.187 | 45.282 | 38 | 55 | 41 / 63 | 44 | 844493 |
| 1/2/-1 | 38.374 | 49.565 | 37 | 53 | 32 / 63 | 43 | 857397 |
| 1/2/+1 | 36.432 | 46.232 | 38 | 56 | 39 / 63 | 44 | 845980 |
| 0/3/-1 | 44.093 | 54.006 | 44 | 48 | 42 / 57 | 31 | 774107 |
| 0/3/+1 | 46.523 | 46.683 | 45 | 48 | 45 / 55 | 31 | 762589 |

Worker0 worst work48.440ms/presentation55.320ms; worker138.374/49.989ms. Worker0 floor172/228intervals, worker1141/252; painted counts236/260. Worker0/1 deadline counts194/218; frame counts/cadence differ, so raw miss totals are not an equal-frame rate comparison. Worker0/1 history blended totals3154717/3413425, V denied136/175, actual V0both. Worker-enabled observations improve bounded Activity timings, while every worker1 cell still has floor misses and exceeds the absolute30fps maxima. Global/selective worker policy and broader qualification remain subject to root review; default cleanup is worker0.

The mode0/1 worst-presentation current frames both transfer335168bytes using27rawjobs/54completed chunks. Mode0 current work43.763ms/raster40.813/history10.512/record1.058/compare0.127/layer-record0.385/layer-raster0/tick0.504/UIbuffer-wait0.055; API13.198/DMAwait0.157/rotation6.911/worker-receive-idle19.506/producer-submit sum0.146/max0.007ms. Mode1 current work36.306/raster34.506/history11.943/record0.973/compare0.127/layer-record0.366/layer-raster0/tick0.436/UIbuffer-wait2.091; API12.997/DMAwait0.144/rotation6.851/worker-idle0.941/producer-submit sum0.082/max0.007ms. Previous frames work13.442/11.317ms respectively. These are worst-presentation pairs, not necessarily maximum-work frames. History CPU time can overlap raster/wall time; worker1's larger summed history time does not negate lower measured end-to-end time. The large worker0 receive-idle value is not producer queue backpressure. All stage timings overlap: do not sum/subtract them to invent unseen stages. Completion is not an optical fence.

Full per-window prior/current stage provenance (microseconds) is retained in JSON; selected summary here:

- `activity_worker0_order0_290_sign-1` worst-pair current: tick_us=504, record_us=1058, compare_us=127, raster_us=40813, history_us=10512, layer_record_us=385, layer_raster_us=0, layer_bands=0, layer_reused=0, ui_wait_us=55, api_sum_us=13198, dmawait_sum_us=157, rotation_sum_us=6911, queuewait_sum_us=19506, producer_submit_sum_us=146, producer_submit_max_us=7, bytes=335168.
- `activity_worker0_order0_290_sign1` worst-pair current: tick_us=425, record_us=919, compare_us=81, raster_us=46016, history_us=10328, layer_record_us=386, layer_raster_us=0, layer_bands=0, layer_reused=0, ui_wait_us=60, api_sum_us=14076, dmawait_sum_us=157, rotation_sum_us=7071, queuewait_sum_us=24039, producer_submit_sum_us=151, producer_submit_max_us=7, bytes=331712.
- `activity_worker1_order1_290_sign-1` worst-pair current: tick_us=436, record_us=973, compare_us=127, raster_us=34506, history_us=11943, layer_record_us=366, layer_raster_us=0, layer_bands=0, layer_reused=0, ui_wait_us=2091, api_sum_us=12997, dmawait_sum_us=144, rotation_sum_us=6851, queuewait_sum_us=941, producer_submit_sum_us=82, producer_submit_max_us=7, bytes=335168.
- `activity_worker1_order1_290_sign1` worst-pair current: tick_us=446, record_us=998, compare_us=120, raster_us=29058, history_us=10644, layer_record_us=381, layer_raster_us=0, layer_bands=0, layer_reused=0, ui_wait_us=5608, api_sum_us=11410, dmawait_sum_us=121, rotation_sum_us=5838, queuewait_sum_us=247, producer_submit_sum_us=67, producer_submit_max_us=6, bytes=285760.
- `activity_worker1_order2_290_sign-1` worst-pair current: tick_us=364, record_us=896, compare_us=130, raster_us=34457, history_us=11970, layer_record_us=345, layer_raster_us=0, layer_bands=0, layer_reused=0, ui_wait_us=1892, api_sum_us=13285, dmawait_sum_us=140, rotation_sum_us=6813, queuewait_sum_us=1010, producer_submit_sum_us=82, producer_submit_max_us=7, bytes=335168.
- `activity_worker1_order2_290_sign1` worst-pair current: tick_us=443, record_us=878, compare_us=118, raster_us=29938, history_us=10966, layer_record_us=346, layer_raster_us=0, layer_bands=0, layer_reused=0, ui_wait_us=5056, api_sum_us=11663, dmawait_sum_us=128, rotation_sum_us=5727, queuewait_sum_us=643, producer_submit_sum_us=74, producer_submit_max_us=10, bytes=285760.
- `activity_worker0_order3_290_sign-1` worst-pair current: tick_us=438, record_us=915, compare_us=129, raster_us=39563, history_us=10412, layer_record_us=480, layer_raster_us=0, layer_bands=0, layer_reused=0, ui_wait_us=58, api_sum_us=13328, dmawait_sum_us=132, rotation_sum_us=6846, queuewait_sum_us=17929, producer_submit_sum_us=144, producer_submit_max_us=6, bytes=335168.
- `activity_worker0_order3_290_sign1` worst-pair current: tick_us=434, record_us=808, compare_us=78, raster_us=44206, history_us=10003, layer_record_us=559, layer_raster_us=0, layer_bands=0, layer_reused=0, ui_wait_us=55, api_sum_us=13247, dmawait_sum_us=149, rotation_sum_us=6843, queuewait_sum_us=22967, producer_submit_sum_us=157, producer_submit_max_us=8, bytes=331712.

Transport completed163commands (159ordinary each with a unique fresh DONE, four direct CRC frames). SDK false-connected checks and UI-scoped overrides both increased12 with complete responses; driver errors, command queue failures and incomplete CRC replies remained0. No background fragment-completion event was observed in this target run. Reader closed with no terminal/shutdown errors, no pending chunks/unterminated bytes. No fault or ordinary cleanup recovery was needed.

Normal cleanup restored worker0 regardless floor failure, HOMEface8/page0/offset0, geometry1/2088bytes, history0/Q5=8, pipeline_probe/shutter/rotation/damage/full/trace/perf/awake0; CRCguard0 was the final command and COM3 closed. Last reported pwr0 with normal autosleep allowed; an eventual AOD transition was not re-queried after closing. No post-guard command was sent. Final NOSALLOC total0; internal22163/largest8704/min11908, DMA14431/largest8704; touch stack minimum2824, flush1832. Physical touch errors2027/11634reads,last_error259,injected/pending0 remain unqualified. No Heart/Weather worker qualification or per-app enablement is implied.

Evidence hashes (SHA256):

- `build/hardware-iteration27-worker-sentinel.jsonl`: `b0d65ed9ba2767d00a2458c4e8a05ca19e876f40474d8787689dfad487b4302b`.
- `build/hardware-iteration27-worker-sentinel.transport.json`: `7b2c32b53c02b1b53800a75a7fe19045b140c5c3328850d43f37707c3b17f8be`.
- `build/hardware-iteration27-worker-sentinel.quiet-windows.json`: `c054edf9a204a00136b63b24d48db89696abe267498cfa5d1863f2640f7c04ee`.
- `build/hardware-iteration27-worker-sentinel-analysis.json`: `567abe90486664408a562e625181a7e3c7b76e1273e27dd68ff5af62d19dc2d6`.
- `build/hardware-iteration27-worker-sentinel-windows.json`: `6562f51d22923c0a2e555ef98375fb22c41701fec236a884921b9115954587ab`.
- `build/recovery/iteration27-shared-text-history.json`: `835bbd324ffae5673c2652b2b5b75a9faa584280dd07f85c1c2ee1403002d2be`.
- `build/validate27-host-snapshot/manifest.json`: `3ddfadb132773e39e3d92bd15716092f380f348827fe410add44fba8e77ad073`.
- `build/worker27-ownership-inference.json`: `bda5c243a9899f73b6200f5b08241649e656e47f29807e3a95990bdea74ce3b4`.
- `build/artifacts/iteration27-shared-text-history/host-validation.json`: `7cfae0e498458ebc1228f0fb688b51f074d008b9e25f347dbdbcf918978a1252`.

## Iteration28a guarded flash; app diagnostic preflight incomplete (2026-10-17)

The authorized27→28a app-only flash passed. The first guarded identity/health info query then exposed a host contract error before any app was opened or any gesture, awake override, or diagnostic mode command ran. This is incomplete validation with0/12 motion windows; no app timing, blur-quality, or frame-floor conclusion is available. The single planned direct CRC guard0 command succeeded, then COM3 closed. No ordinary retry, replay, extra reset, or recovery commands followed.

Exact28a archived tree `acc5ea791bdaecb7423ac8e1d7e6584dbd64a49db578da95ea9630a8ce6180eb`; ELF `dfe1d508e890ef28ef8dec5fd7146d4672ff9fbb9077dc50a966ca34989cae73`; BIN `f1372605d67877bd67d6adae25e8486631ec319bb21b6f37e9005c4bf095858d` (2816272B); source.zip `9cccb7c3a29d2d3dd053168084a58757260a9d3f8f6d29dcb89490abf3009a79`. Flash helper verified current27 entire-slot MD5 `9c952e46f88dccb09bd8caad2498a254`, exact current image, MAC/flash/boot/OTA guards, new whole-slot MD5 `b50bdf228d71f273a1f37eee6469d398` against offline expected-slot SHA `e8cf910c9d4aa12bbc93d096ccaaa49c8e32db65fef8a752813c13790cefa37b`, and both outside regions unchanged (`7f57c51e00677962f88e5c6c1ed06351`, `d39d7a06af70d1b1e1d903f52cee17bc`). Erase edge0x2d0000. Exactly one subsequent reset was captured with the background reader already running.

Frozen host SHA `52d850df759be475486c656328230b36ded5598f3df8f2ea3e9acafa4634c629`;32 named offline methods had passed, but their printf-schema check failed to verify command dispatch. The host wrongly required `MEM_CACHE directional` in `info`. Archived source shows this line is emitted by `ui_fblur_mem()` through PSRAM memory reporting; `info` emits `NOSCACHE` alone. The saved info response was complete with fresh unique DONE and exact target identity; the missing MEM_CACHE line was expected under the actual command contract. Original failed evidence is retained unchanged.

Observed preflight: HOME face8/page0/offset0, pwr0 awake/perf/trace/probe/settings-shutter/rotation/damage/full0; selected worker0/history_cadence0/Q5weight8 at25000us, geometry1 bytes2088; NOSALLOC total0, NOSCACHE allocfail0/blur_bytes0. Internal free30827/largest19456/min24428, DMAfree23095/largest19456; CPU240MHz, PSRAMlargest3932160, flush stack free1828. Direct diagnostic counters showed no false-connected, failed-write, queue-failure, or direct-reply failure deltas. Final guard0 was CRC-confirmed; background reader stopped without terminal/shutdown errors, pending chunks, or unterminated bytes. No extra query was sent after guard0.

Evidence and SHA256:

- `build\hardware-iteration28a-candidate-apps12.jsonl` (46255B): `b6c556867e1d079d723563e312620e8d3ae9cf7e41854eefb8bded0a3741b2b6`
- `build\hardware-iteration28a-candidate-apps12.diagnostics.json` (3435B): `3c97bfab73525ad0505bc5662616263bc3d631d4a71729dc16db738728076c39`
- `build\hardware-iteration28a-candidate-apps12.transport.json` (89933B): `c1f0abeb2783858b2050f7b6aa4148f3cf2e9abcd7a888cc85771cf4b004492a`
- `build\hardware-iteration28a-candidate-apps12.quiet-windows.json` (2B): `4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945`
- `build\hardware-iteration28a-candidate-apps12-host.log` (1202B): `c161634b9b4f47f06db03e8fe448225bb658ff6e03542bb5345bfcb3f6d836cc`
- `build\iteration28a-flash-host.log` (7354B): `f2c8ec05130db39d90757c83e3ca4ec089baffc776874ed40a7683c48f037ee8`
- `build\recovery\iteration28a-scroll-cache.json` (1459B): `b702357192198fd39827dbdb239c5941396d6ba74ee2295a97ea76d04f1f15f3`
- `build\validate28a-host-snapshot\manifest.json` (5588B): `12af0af257cc42ceb154f6d176324aba014094951762ad53d7163ac4230cf3a4`
- `build\validate28a-host-snapshot\validate28a.py` (54759B): `52d850df759be475486c656328230b36ded5598f3df8f2ea3e9acafa4634c629`

- `build\hardware-iteration28a-candidate-apps12-analysis.json`: `8d9e848ff3570e9dd8eb747b63f9d983b74cbf4624ec854a1c4693f3f9ea5455`

Append provenance: exact prior191494B SHA `807aa95ef141164cf1a72d87c6c1d2261323de78c94fae5b94c277fc9a00a0fc` backed up as `build\doc-backups\DEVICE-before28a-preflight-20261017.md` before append; unchanged byte prefix verified.

## Iteration28b host-only normal attach: preflight power-transition stop (2026-10-17)

The separately authorized28b host-only helper attached to the already flashed exact28a without reset/reflash. It stopped before any app open, gesture, mode change, or awake toggle: paired cache-state validation observed pwr1→0. All remaining state fields matched except advancing frame (2867→2904). Both fresh info responses carried exact ELF `dfe1d508e890ef28ef8dec5fd7146d4672ff9fbb9077dc50a966ca34989cae73`, unique DONE, healthy NOSALLOC0 and identical heap reports. Fresh mem reported directional bytes0/slots0/published0/failures0, matching both NOSCACHE blur_bytes0/allocfail0. The run is INCOMPLETE0/12 with no app timing, blur coverage, quality, or performance result.

Offline archived-source review identifies the host ordering error: `firmware/main/nos_main.c:1121` defines the UI quiet-command allowlist; info/state/nav are included but mem is excluded. Line1123 consequently calls `go_state(PWR_ON)` before executing mem when initially in AOD. The new test proved command dispatch into memory reporting but had not covered its earlier power dispatch. The strict state guard correctly rejected this wake transition; it was not silently accepted or retried. Future paired mem measurements require the planned explicit awake1/aod0 setup after quiet identity/heap preflight, with unchanged strict state equality thereafter.

The failure path sent only its preplanned direct CRC guard0 command, confirmed, then closed COM3. No replay, ordinary cleanup, reset, or extra query followed. Default awake/perf/trace/probe/full/damage/worker0/history0/geometry1 and HOME face8/page0 were observed; after mem, pwr0 with normal autosleep still enabled. CPU240MHz, internal free30655/largest19456/min24428, DMAfree22923/largest19456, PSRAMlargest2883584, NOSALLOC total0. Direct deltas since guard1 showed cf/zf/wf/qf/ri0; preexisting cumulative disconnected-driver counters were retained, not attributed to this guarded attempt. Reader closed without errors, pending chunks, or partial bytes.

Frozen helper SHA `8736faf8ceb5bcce11e5304c3188fa0d3b18803c2194e950de2945432388c1b6`;40 explicit named offline methods passed before this attempt. Earlier aborted28a evidence is preserved. Evidence SHA256:

- `build\hardware-iteration28b-for28a-candidate-apps12.jsonl` (45921B): `0a2fd0960e224e70a4bd89c96a804438689c591c2e0b324efa3186085cb29afd`
- `build\hardware-iteration28b-for28a-candidate-apps12.transport.json` (177373B): `b34878421dc41fc75f6eab57d13658712911169f323cc7ecc050f8709a7134cc`
- `build\hardware-iteration28b-for28a-candidate-apps12.diagnostics.json` (4085B): `c989701d8506845a0caea38c5970e0da6adf6a665d4e27f24ee52c8dd8dd9bf7`
- `build\hardware-iteration28b-for28a-candidate-apps12.quiet-windows.json` (2B): `4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945`
- `build\hardware-iteration28b-for28a-candidate-apps12-host.log` (1275B): `7125149c9c6e936d07f6a35b956f9a211b2a71bed440cb3f1b7351e90f924bcd`
- `build\validate28b-host-for28a-snapshot\manifest.json` (5119B): `f4fa4834c18f178a57a38d0b57ba255c4c548de61979926a8f3812421fd7a206`
- `build\validate28b-host-for28a-snapshot\validate28b.py` (56977B): `8736faf8ceb5bcce11e5304c3188fa0d3b18803c2194e950de2945432388c1b6`

- `build\hardware-iteration28b-for28a-candidate-apps12-analysis.json`: `2f6157fe0a5c2328559acfa208e9ff331e4b80000d552e09e84d5c130a24a16a`

Append provenance: exact prior195689B SHA `49ab833d27abb8410212f8f4c0d0b406846a12e3bb253f2b45f07a741ddc455c` backed up before append as `build\doc-backups\DEVICE-before28b-preflight-20261017.md`; unchanged prefix verified.

## Iteration28a target with28c host:8valid cells; Weather motion absent (2026-10-17)

The authorized normal attachment used exact flashed28a ELF `dfe1d508e890ef28ef8dec5fd7146d4672ff9fbb9077dc50a966ca34989cae73` and source tree `acc5ea791bdaecb7423ac8e1d7e6584dbd64a49db578da95ea9630a8ce6180eb`; no reset/reflash occurred. Host28c SHA `aa286dd750908a06554095b3f18539a849af2909600595bcbca003df0b0ae7ab`, with46 named offline methods passed and independently reviewed by root. Quiet initial identity/heap/mode preflight, once-only explicit awake1/aod0 setup, and subsequent strict separately fresh state/info/mem/info/state pairs passed. Fixed worker0/history_cadence0/geometry1 and all original endpoint/freshness/health/transport gates were retained.

Campaign is INCOMPLETE:8 valid Activity/Heart cells,1 hardware-completed but invalid Weather290negative sample,3 unattempted Weather cells. All commands/windows were single attempt. The eight valid cells contain449 painted frames/433 presentation intervals,267 deadline misses/60 floor misses. Max frame work41.721ms, max completed-frame presentation gap48.915ms. Absolute frame-floor and directional-quality requirements are not passed. Measured live vertical filter frames/pixels0 throughout; denial counter283 and history-blended pixels5,790,200. History coverage is measured; generic cached visible directional coverage is not.

| App | px/s | Sign | Pose | Painted / intervals | Max work ms | Max present ms | Deadline / floor | V denied | History blended |
|---|---:|---:|---|---:|---:|---:|---:|---:|---:|
| ACTIVITY | 290 | -1 | 0→1 | 69/67 | 36.716 | 48.407 | 51/8 | 48 | 924441 |
| ACTIVITY | 290 | +1 | 1→0 | 71/69 | 41.721 | 41.882 | 61/16 | 50 | 970438 |
| ACTIVITY | 900 | -1 | 0→1 | 39/37 | 37.094 | 48.915 | 23/8 | 18 | 477653 |
| ACTIVITY | 900 | +1 | 1→0 | 40/38 | 41.617 | 42.630 | 31/16 | 19 | 422269 |
| HEART RATE | 290 | -1 | 0→1 | 75/73 | 30.228 | 39.768 | 33/5 | 54 | 1021519 |
| HEART RATE | 290 | +1 | 1→0 | 75/73 | 30.139 | 41.635 | 32/1 | 54 | 944004 |
| HEART RATE | 900 | -1 | 0→1 | 40/38 | 34.302 | 40.409 | 19/5 | 20 | 554679 |
| HEART RATE | 900 | +1 | 1→0 | 40/38 | 32.285 | 41.702 | 17/1 | 20 | 475197 |

All eight cells reported generic cache bytes32768 (header only), slots0/published0 at both boundaries, with bracketed preparation hit/miss/work-pixel deltas0. These data establish no published-cache benefit on the target. Publication is current resident count, preparation hits are not raster hits, and separate mem/info queries are not atomic or exact quiet-window raster coverage. Geometry metadata remained2088B. Complete worst previous/current frame records and stage timings are retained in the paired-stages artifact; record/raster/history/layer/tick/UI-wait, worker idle, rotation, submission, API and DMA timing overlap and must not be summed. Completion gaps are independently checked from frame begin+completed offsets, not inferred from FPS.

Weather owner/page0/drag0, pwr0/awake1/probe1 were freshly confirmed before the first290negative request. Bench arm, exact380px1310ms swipe and bench end each received one fresh unique DONE. The actual window contained143 UI frames over3.574s with app3 throughout/foreign0, but pose_changed0/drag_frames0, no painted frames/history/transfer work, and no DMA jobs. The helper stopped on its motion gate;1.181ms idle work is not an app-motion performance result. No endpoint was accepted or gesture replayed. Archived weather_input:792 rejects all input when view state is not WS_OK, and weather_tick:734 similarly returns; a Wi-Fi reason201 disconnect was logged before the window. Existing telemetry does not expose runtime weather view state or input-consumption count, so empty/unavailable data is a leading explanation, not independently proven. The capture remained correctly marked invalid.

Failure handling sent only the preplanned CRC guard0 command, then closed. Root separately authorized bounded cleanup. That attachment verified exact identity, fresh heap/alloc and last WEATHERpage0/probe1/awake1 state before restoring HOME face8/page0, worker0/history0/geometry1, probe/shutter/rotation/damage/full/trace0, perf0/awake0 and normal AOD/autosleep. Final fresh state pwr1/awake0 in HOME; CRC guard0 was LAST command, then COM3 closed. No reset/replay/performance window was added. Cleanup helper SHA `c8e59a7c5c1ee1f5181c8b972d7eb7b1c176b322ddcebdccc4be48a590761064`, derived from frozen28c command/transport helpers with two bounded-cleanup offline tests passed.

Capture258 commands (255 ordinary+3direct), cleanup47 (44ordinary+3direct), each ordinary command with unique fresh DONE; no retries. Capture cf+4/guard-overrides+4, cleanup cf+5/overrides+4; driver-write failure, zero-write failure, queue failure and direct-reply failure deltas0 in each. Counter differences are retained verbatim. Both background readers stopped without terminal/shutdown errors, unparsed chunks or unterminated bytes. Health reports remained NOSALLOC0 and allocation/cache failures0, CPU240MHz; final internal free30655/largest19456/min24428, DMAfree22923/largest19456, flush stack free1828/touch stack free2788. Physical touch errors34356/75675reads,last_error259 at final info, injected/pending0; these accumulated physical errors remain unqualified and are not erased or treated as validated input health.

Evidence SHA256:

- `build\hardware-iteration28c-for28a-candidate-apps12.jsonl` (1159922B): `30a6ce8a4387021bfd4dfb4d0d790cf035c22ad8436f785ef8679e229a946753`
- `build\hardware-iteration28c-for28a-candidate-apps12.diagnostics.json` (3630B): `69d98176be9746502a9125ffa5acd7d2b34eb9f3e8bbff4c47d2684177cb26fb`
- `build\hardware-iteration28c-for28a-candidate-apps12.transport.json` (4117084B): `608ccffd852576c9f3fec7a4594f6ec1abb95a9db9b01c90a2f5b4b0da4fa8cc`
- `build\hardware-iteration28c-for28a-candidate-apps12.quiet-windows.json` (122193B): `7bc5ce39f72c7ab97f80310d9e971df06e9143d8d25bb64a9b20f9e695330ca3`
- `build\hardware-iteration28c-for28a-candidate-apps12-host.log` (1246B): `4c40990957c9e9f958a735f569330b641840559f3a92e682c6d1bb4681cf4ead`
- `build\hardware-iteration28c-for28a-candidate-apps12-analysis.json` (309136B): `8532d2e7e1fc1572b00d13b8c98ada392a38a689ab44b0fe5ef75aa74ed8ba40`
- `build\hardware-iteration28c-for28a-candidate-apps12-weather-rejection.json` (17430B): `3bace5770949a3dae375b93cbf0c344e19be0c0d34f39db158f6ef452f65187f`
- `build\hardware-iteration28c-for28a-candidate-apps12-paired-stages.json` (20749B): `b7d008ab4fb18256af252d83bbb965d9b8f69b8964fa90c167bd1f2857796cb4`
- `build\hardware-iteration28c-cleanup.jsonl` (196371B): `29faedff531b19517ba9b0a2857af608eb92a2400df49fdb87b3fe27f8b21ef3`
- `build\hardware-iteration28c-cleanup.diagnostics.json` (3644B): `7dcc27260a5c4bcf84cab8f90e480f7f86f4d97bf3c9d67107604ddb9be72906`
- `build\hardware-iteration28c-cleanup.transport.json` (687927B): `4f5afe8bbb99fb654cb8253ab466d78e5d79c57db4fc82c8193508498aff821b`
- `build\hardware-iteration28c-cleanup.quiet-windows.json` (2B): `4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945`
- `build\hardware-iteration28c-cleanup-host.log` (0B): `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
- `build\validate28c-host-for28a-snapshot\manifest.json` (4855B): `e1bdf9bf8a3292684265f0f9b117c4c27e96c7a76068790afaf37545e8cda84f`
- `build\validate28c-host-for28a-snapshot\validate28c.py` (58256B): `aa286dd750908a06554095b3f18539a849af2909600595bcbca003df0b0ae7ab`
- `build\cleanup28c-host-snapshot\manifest.json` (1244B): `a1b5bbafe32fa18c6b6a4c7b71c8bf8d636b3f6182edecd5f19ec7d1df1613e1`
- `build\cleanup28c-host-snapshot\cleanup28c.py` (54905B): `c8e59a7c5c1ee1f5181c8b972d7eb7b1c176b322ddcebdccc4be48a590761064`

- `build\hardware-iteration28c-for28a-candidate-apps12-final-record.json`: `f95cb2ecbaa038c863a21230b34f1c94bce04a2cb35f12a8856d23093d3e6bfd`

Append provenance: exact prior199338B SHA `d1f742b449ebdcf6e968cbd222c94d6bec94da7d98484659d86ac67d2b51745c` backed up before append as `build\doc-backups\DEVICE-before28c-apps-20261017.md`; unchanged byte prefix verified. Earlier28a/28b aborted evidence and frozen snapshots remain preserved.


## 2026-10-17: iteration29 packed cache observer; one guarded flash, host-rejected first About window

Root authorized exactly one28a->29 app-only flash, one captured reset and ONLY About ABBA8 (scalar/packed0/1/1/0,290 px/s, both signs). Firmware source stayed frozen. Exact source tree `a7b2b390efe9566f9bc4d0cbc6e50fe088208b536bcf1f4b2dd10da794371bfc`, ELF `43fca3e2d573f50b74fc8698487c19201a3a54dc2180437ed40352b6e2be58b7`, BIN `53c845b80dfb266e9432cf3baf32a80d07acd25e6bcd4c34e957d919cb0676a4` (0x2b0410 bytes), source.zip SHA256 `0b59365e8c611a236d504aae7a0239b2da72e913d67ed9cf8a8cae85c4140bb4`. Root host bundle passed with the documented7 motion-shape failures/14 excluded timing assertions; it did not establish target timing or optical acceptance.

Flash PASS: original16MiB backup, partition table, same MAC/flash size, immutable boot/layout/OTA, current28a exact prefix and entire slot, new exact prefix/entire slot and all outside-slot regions were checked by the unchanged guarded helper. Current slot SHA256 model `e8cf910c9d4aa12bbc93d096ccaaa49c8e32db65fef8a752813c13790cefa37b` matched target MD5 `b50bdf228d71f273a1f37eee6469d398`. New expected7.5MiB model SHA256 `5b349df2d1aeed4acee4fbec3f992ee909f1cca7799bc8c994642bf087390f14` matched whole target slot MD5 `fbf38ebd5ecf1714c65e70cbe736bb2b`. Erase edge0x2d1000; suffix beyond that retained. Outside-region MD5 values remained `f4a9b680c18538c06402bd170801fda4` / `d39d7a06af70d1b1e1d903f52cee17bc`. One flash attempt; helper retained stub for one captured reset, without a retry.

The ABBA campaign is **INCOMPLETE:0 accepted cells;1 raw host-rejected scalar0 negative window preserved**. The stage checked actual last geometry1/history0, but omitted the checked setters that initialize `expected_prog_mode` / `expected_history_mode`; both globals remained None. The actual cell assertion compared geometry1==1==None and raised “Geometry changed during window.” Fresh actual pre/post geometry1 bytes2088 and history0 were unchanged. This was a host setup/test-coverage bug, not firmware control drift. Original stage tests mocked setters and the window, missing their lifecycle interaction. The frozen helper SHA256 is `c8eefd079b374a1446a9b9ea113fa3fbcfbf725df17ef1fb87ba98001452ff64`; its failed evidence is not replaced or promoted to an accepted A/B result.

Raw first-window observations (scalar mode0 selected):428 UI frames,221 painted,213 presentation intervals,135 deadline misses and45 floor misses; maximum work37.390ms and presentation36.641ms; p95/p99 interval35/36ms. About app6/axis2 continuous limit661 reported offset0->661 after the two planned380px negative gestures, drag0 at both fresh endpoints; within-window overshoot reached757.223px before settling. No foreign frames, allocation failures, stale observer epochs or unknown-owner requests. CPU reported240MHz.

Separate NOSCACHECAP v1 measured21,090,560 requested processed area,0 cached-sharp and13,582,940 cached-blurred area across138 cache-request/blur frames; known requested-but-uncopied area7,507,620. These are clipped draw-call processed areas, include overlap/black/dimmed writes and later covered pixels, and are **not unique/final-visible coverage or optical quality**. Live V executed0 frames/0 pixels, live V gate requests0; this app used its generic directional cache path. History processed24,300,036 pixels, blended691,154 across209 frames, uniform13,599,200. Publication/get-reuse counts remain distinct: both separate cache boundaries showed1,548,288 bytes,7 slots/7 published, failures0. Preparation work counter726,384->2,294,592 (+1,568,208); those non-atomic boundary observations include command/query separation and are not exact-window kernel timing. No packed1 window ran, so there is no measured packed speedup here.

The worst presentation pair had equal354,728 transfer bytes and completed DMA counts. Previous/current frame1551/1552 work37.390/36.475ms, record1.123/0.395ms, compare0.104/0.051ms, raster32.962/31.583ms, history9.632/9.620ms, layer record/raster0/0, UI wait2.259/2.128ms, tick2.367/3.559ms, API17.718/17.060ms, DMA wait0.127/0.124ms, worker receive-idle7.553/6.704ms and producer submit0.209/0.119ms. Stages overlap and **must not be summed**; history is included in raster accounting. Both frames were expensive; the36.641ms completion gap was not preceded by a cheap frame. Aggregate48,600,072 bytes/9,119 chunks all completed, mismatch0, optical_fence0. This is an app workload, not an isolated cache-kernel microbenchmark or30fps/optical acceptance.

Failure policy: no cell/command replay, no automatic ordinary cleanup; one CRC guard0 confirmation and COM close. First run transport56 commands, no bad ordinary ACKs, pending bytes/chunks, reader terminal/shutdown errors. Root then separately authorized bounded normal-attach cleanup; no reset/reflash/gesture. Cleanup PASS54 commands, exact29 identity and documented ABOUT661/cache0 state checked, HOME face8/page0 restored, packed1/worker0/history0/geometry1, probe/shutter/rotation/damage/full/perf/trace/awake0, normalAOD pwr1, CRC guard0 LAST and COM closed. Last fresh internal free21,871/largest8,192/min11,436; DMA free14,139/largest8,192; allocator total0, flush stack low-water1,828, touch2,820. Physical touch reads26,657/errors9,294/last259 are preserved as observed and not attributed to the host setup failure.

Host-only successor29b changes only the ABBA setup line to call actual checked worker0/history0/geometry1 setters before any cell. Actual stage+actual setters+actual window regression tests reproduce the old rejection and pass from expected modesNone through8 mocked-transport windows and packed1 cleanup;68 named tests pass. New helper SHA256 `206748f768b180e7f6f0a3f1a3ed3aba7925a4ade4b178579b480334eac4ebd9`; manifest `1adf00a4001cbcd697062eecf6333ea47ad966d49e1c1d9e95d3ddf796af36af`; diff `fc6242b4695ef8ffaa071c28b077b2cf28f6f89836ccd679890a230839e09582`. Unchanged29 firmware; no new capture authorized or run at this entry.

Evidence: `build/hardware-iteration29-about-cache-abba8.{jsonl,transport.json,diagnostics.json,quiet-windows.json}`, separate `build/hardware-iteration29-cleanup.*`, `build/hardware-iteration29-flash.log`, `build/recovery/iteration29-packed-cache-observer.json`; analysis SHA256 `1ecd55587d8a125724f040f911de8fefb9c00ae40ffff3cb436f895c55726e6a`. Exact raw/source/helper hashes, complete pair and cleanup state are in `build/hardware-iteration29-device-final-record.json` SHA256 `9f7833a8857663d65b5239480683c4853dd47b0f88c352294f62caafd2607e50`. This append preserves the complete207,518-byte prior prefix and its backup `build/doc-backups/DEVICE-before29-abba-20261017.md`.


## 2026-10-17:29b About ABBA complete on unchanged29, normal attach only

This independent run used the reviewed host-only29b correction: explicit checked worker0/history0/geometry1 setters initialize expected globals before ABBA. Root authorized ONLY About8 windows, scalar/packed0/1/1/0,290 px/s, both signs per fresh entry. No flash/reset, gesture replay or extra stage; one attempt. Helper SHA256 `206748f768b180e7f6f0a3f1a3ed3aba7925a4ade4b178579b480334eac4ebd9`, frozen manifest `1adf00a4001cbcd697062eecf6333ea47ad966d49e1c1d9e95d3ddf796af36af`,68 named offline methods PASS. Installed source tree `a7b2b390efe9566f9bc4d0cbc6e50fe088208b536bcf1f4b2dd10da794371bfc`, ELF `43fca3e2d573f50b74fc8698487c19201a3a54dc2180437ed40352b6e2be58b7`, BIN `53c845b80dfb266e9432cf3baf32a80d07acd25e6bcd4c34e957d919cb0676a4` unchanged; fresh exact identity/heap/state/modes checked before explicit wake and later strict state/info/mem/info/state pairs. The earlier29 host-rejected scalar window is excluded from every result here.

**Protocol COMPLETE8/8; frame-floor and optical acceptance NOT established.** About app6/axis2 remained continuous limit661/page0/pages1, two planned380px gestures/window with4s settling each: exact0->661 negative and661->0 positive, drag0 at all fresh endpoints, intended direction, no foreign owners, stale/unknown cache observations or allocation failures. Worker0/history cadence0/geometry1 fixed; selected cache mode checked in every compact state and separately fresh info pair and before/after every cell. The pipeline probe was enabled only for this stage. CPU240MHz; actual DMA chunks completed/matched, optical fence absent.

| Block | Packed | Sign | Painted/intervals | Max work/present ms | Deadline/floor misses | Cached blurred/requested processed area | Cache bytes/published slots |
|---|---|---|---|---|---|---|---|
| 0 | 0 | -1 | 230/222 | 36.867/37.045 | 122/42 | 12,691,778/20,698,910 (61.32%) | 1,220,608/5 |
| 0 | 0 | +1 | 258/252 | 38.322/38.499 | 139/41 | 12,379,808/21,314,982 (58.08%) | 1,220,608/5 |
| 1 | 1 | -1 | 231/221 | 36.880/37.061 | 137/38 | 17,309,860/20,921,458 (82.74%) | 1,515,520/6 |
| 1 | 1 | +1 | 259/253 | 37.948/53.076 | 143/53 | 17,600,156/21,667,722 (81.23%) | 1,515,520/6 |
| 2 | 1 | -1 | 231/223 | 37.445/37.613 | 134/39 | 17,439,820/21,085,316 (82.71%) | 1,482,752/6 |
| 2 | 1 | +1 | 262/256 | 38.460/38.634 | 144/48 | 18,577,056/21,796,912 (85.23%) | 1,482,752/6 |
| 3 | 0 | -1 | 233/224 | 37.128/37.302 | 137/42 | 15,753,162/20,878,122 (75.45%) | 1,454,080/6 |
| 3 | 0 | +1 | 260/254 | 37.232/37.407 | 141/41 | 14,353,524/21,634,948 (66.34%) | 1,454,080/6 |

Combined3,393 frames/1,964 painted/1,905 intervals,1,097 deadline misses/344 floor misses; maximum work38.460ms/presentation53.076ms. Scalar4 windows:539 deadline/166 floor, max38.322/38.499ms,55,178,272 cached-blurred of84,526,962 requested processed area (**65.28%**),2,903,934 history-blended pixels. Packed4 windows:558 deadline/178 floor, max38.460/53.076ms,70,926,892 of85,471,408 (**82.98%**),1,560,114 history-blended pixels. Cached-sharp0, stale0/unknown0 all windows. Live V filter frames/pixels and V gate requests0: generic cache draws provided the measured directional path; no live-filter denial or execution is invented. All misses remain included. More measured cached-blurred area does not establish30fps or optical quality.

Attribution is limited by **unequal cache capacity and preparation work**. Actual retained allocation was stable within each block but1,220,608/1,515,520/1,482,752/1,454,080 bytes across blocks0/1/2/3, published5/6/6/6. Fresh entry resets/invalidate the About owner and uses the same planned sequence; it does not make allocation cold or equal. Scalar negative fractions61.32/75.45%, positive58.08/66.34%; packed negative82.74/82.71%, positive81.23/85.23%. Non-atomic preparation counter deltas sum4,975,464 scalar vs6,725,712 packed processed pixels, including boundary query/command time outside the exact quiet window; this is not an isolated kernel benchmark. Same endpoints do not imply equal raster bytes/content work. No isolated packed-kernel speedup is claimed from these app windows.

The53.076ms maximum occurred in packed block1 positive. Previous frame14806 did4.948ms work and3,200 transfer bytes; current14807 began24.997ms later and did33.027ms work/354,728 bytes. Completion gap=24.997+33.027-4.948=53.076ms. Previous/current record0.506/0.691ms, compare0.042/0.132ms, raster3.250/28.637ms, history0.285/10.684ms, layer record/raster0, UI wait0.005/1.669ms, tick0.558/2.756ms, API1.531/17.637ms, DMA wait0.015/0.127ms, worker receive-idle0.896/3.338ms, producer submit0.017/0.116ms. This directly shows a cheap-to-expensive transition under fixed25ms pacing; it is not an equal-work pair or evidence of large producer queue backpressure. Stage timings overlap, history is included in raster, worker receive wait is idle, and stages must not be summed. Complete worst-pair stages for all8 cells are retained in JSON.

Processed areas count actual clipped raster cache draw-call writes/requests, including overdraw, black/dimmed pixels and later overlays; they are not unique/final-visible pixels or image-quality evidence. Publication slots and cache-get reuse are separate counters. Numerically higher cached use/fewer temporal blends is an observation, not optical acceptance.

Normal guarded cleanup PASS: HOME face8/page0, cache_packed1/worker0/history0/geometry1, probe/shutter/rotation/damage/full/perf/trace/awake0 and normal autosleep; pwr0 at final quiet check. CRC guard0 was LAST and COM closed.325 commands, unique ordinary ACKs, no pending bytes/chunks, reader terminal/shutdown errors or post-close reader. Last internal free21,871/largest8,192/min11,436; DMA14,139/largest8,192; allocator total0, flush stack1,828/touch2,820. Physical touch reads56,945/errors23,213/last259 are retained as observed, not declared resolved or caused by this capture. Broader apps12 was not run; all COM work stops here pending separate authorization.

Independent raw evidence: `build/hardware-iteration29b-for29-about-cache-abba8.{jsonl,quiet-windows.json,transport.json,diagnostics.json}`, host/attempt logs, editable `...-cells.csv`. Analysis SHA256 `718578c23675dc5279526a2b64ece413e9cf0a8421a989f3887db54dff3e332c`; complete report/provenance `build/hardware-iteration29b-for29-about-cache-abba8-final-record.json` SHA256 `6463019f513e530f59b71e96bdfc038eeaca1fe082eb0b97cbf61c38c1a513ae`. Prior214,212-byte document prefix preserved in `build/doc-backups/DEVICE-before29b-abba-20261017.md`.

## 2026-10-19: native-menu baseline and iterations 30b–32

The user authorized implementation and watch validation. COM3 has one owner for
this run. Each new image was built from an unchanged source snapshot, archived,
and written through the existing guarded app-only helper. Exact image and full
0x780000-byte app-slot readback passed; both regions outside that slot remained
unchanged. Each flash had one captured reset. Firmware manifests and source
archives are under `build/artifacts/iteration{30b,31,32}-native-tiles`.

All times below are completed-transfer measurements at the reported 240 MHz.
These are different workloads/trajectories and must not be treated as isolated
kernel A/B speedups. Every missed interval remains included.

| Capture | Cells | Painted / intervals | Max work ms | Max completion interval ms | Intervals >33.333 ms |
|---|---:|---:|---:|---:|---:|
| Installed 29 native baseline | 20 | 1891 / 1865 | 29.886 | 44.384 | 3 |
| 30b Settings pacing 0/1 | 12 | 758 / 701 | 61.918 | 62.105 | 318 |
| 30b Settings history-uniform 0/1 | 4 | 218 / 194 | 46.858 | 47.043 | 89 |
| 31 Settings early cells | 2 | 190 / 176 | 40.561 | 40.778 | 13 |
| 31 Settings exposure-worker 0/1 | 4 | 332 / 304 | 47.811 | 48.018 | 46 |
| 31 Launcher early cells | 2 | 226 / 224 | 30.879 | 31.057 | 0 |
| 32 Settings early cells | 2 | 217 / 205 | 37.227 | 37.418 | 14 |

The baseline covers Launcher and Settings at 150/290/900/1800 px/s, both
directions and bounded alternating reversals. The early pairs cover 150 px/s
negative motion and 1800 px/s alternating reversals. Host acknowledgement gaps
separate the alternating gestures; these are not zero-gap physical reversals.
Launcher preserves its remembered scroll on re-entry, so its early reversal
began at 368 px while the older comparison began at zero. Settings trajectories
also differ with frame cadence. None of these entry sequences forces all rows
cold, and the planned broader cold/warm matrix has not yet been accepted.

Iteration 30b exposed a substantial native-render regression. Iteration 31
hoisted pixel-loop bounds and fixed native background history ownership;
iteration 32 removed additional zero-source reads and repeated row layout work.
The 32 slow cell had 120 intervals, zero floor misses, maximum completion
29.108 ms and maximum work 29.865 ms. Its reversal had 85 intervals and 14 floor
misses. The worst reversal current frame had 31.802 ms raster, including
10.399 ms history, 3.720 ms tick, 0.361 ms record and 0.274 ms UI wait. Its
323,624 bytes/59 DMA chunks all completed. Stages overlap; do not sum them.

Neither pacing changes, disabling uniform history scans, nor worker overlap
established the required floor for these Settings workloads. Worker and uniform
defaults remain 0 and 1 respectively. The newest observed UI stack low-water
is 1,660 of 8,192 bytes after the larger inlined Settings draw; internal free
20,295 bytes/largest 7,680 and DMA free 12,563/largest 6,656. Allocation failures
remained zero. Every completed run restored HOME face 8, normal awake/perf/probe
controls, disabled the diagnostic guard last, stopped its reader, and closed
COM3. The port remains reserved for the next candidate.

Raw bytes, frame pairs, mode checks, stack/heap, transport and cleanup records
are in `build/iteration30-watch/`. Aggregations are `native-baseline-analysis.json`,
`iteration30b-settings-pacing-analysis.json`,
`iteration30b-history-uniform-ab-analysis.json`,
`iteration31-settings-early-analysis.json`, `iteration31-worker-ab-analysis.json`,
`iteration31-launcher-early-analysis.json`, and
`iteration32-settings-early-analysis.json`. Per-image final records pin their
immutable manifests and all evidence. The initial 30 build whose source changed
during compilation was rejected and never flashed.

Host validation includes the 86-method NOS suite before the later focused menu
changes, independent native pixel/stripe/lifecycle tests after each change, the
3,195-case Orrery edge-gesture regression, and actual elapsed-clock/pacer checks.
Legacy test fixtures that changed app state after tick were corrected to publish
that state in the next draw generation; their pixel assertions were retained.
The Settings integration tool still records seven modeled late invalidation
frames and seven modeled late warm-fling frames. These failures are not waived
or converted into target passes. No optical, power, physical-touch fault, general
app, or 200 MHz-equivalent qualification is claimed. The newer static-ROM menu
candidate is still being prepared at this entry.
## 2026-10-19: iteration 33 immutable menu sources

Iteration 33 was built from 350 frozen files and flashed application-only to
COM3. Prefix, whole application slot, and preserved regions verified; the
captured boot reported the expected ELF and canonical artwork identity. The
5,041,776-byte image fits the 7.5 MiB slot. The 2,053,700-byte pixel blob is in
flash rodata; `CONFIG_SPIRAM_RODATA` remains off. The source archive, map, actual
asset object, linker audit, and root host review are in
`build/artifacts/iteration33-native-tiles`.

Initial measured cells, CPU 240 MHz, pacing 1, uniform shortcut 1, worker 0:

| Cell | Work max ms | Completion interval max ms | Intervals over 33.333 ms |
| --- | ---: | ---: | ---: |
| Settings 150 px/s negative | 27.250 | 27.879 | 0 / 120 |
| Settings 1800 px/s alternating reversals | 38.008 | 38.206 | 20 / 87 |
| Launcher 150 px/s negative | 36.633 | 36.833 | 7 |
| Launcher 1800 px/s alternating reversals | 33.538 | 33.725 | 1 |

Across the four cells: 684 frames, 437 painted frames, 423 completion intervals,
185 target-deadline misses and 28 floor misses. ROM identity, owner/pose, DMA
completion, and cleanup checks passed. Settings's worst reversal frame includes
1.446 ms tick and 34.816 ms raster, of which 11.311 ms is history. Flush rotation
and API work overlap those stages and must not be added as serial costs.
UI stack minimum free was 1,660 / 8,192 bytes; allocation failures were zero.
These results fail the required frame floor. Immediate static availability does
not by itself make the complete renderer fast enough for every tested motion.

A balanced six-cell Settings reversal comparison starts and ends at exactly
zero: baseline, uniform shortcut off, history worker on, then reverse order.
Baseline completion maxima were 36.647 / 35.714 ms, shortcut-off 33.965 / 34.571
ms, and worker-on 35.515 / 35.882 ms. Every arm still misses the floor. Motion
maxima differ despite equal starts, so these are workload comparisons, not an
isolated kernel benchmark. Controls were restored to uniform 1 / worker 0 and
COM3 closed. Evidence is `build/iteration30-watch/iteration33-native-early*.json`
and `iteration33-settings-controls*.json`; the sealed first capture record is
`iteration33-device-final-record.json`.

The host suite now covers 98 unique Python methods, plus the compiled checks.
One extracted raw-baker test initially lacked stubs for the new ROM branch;
only that fixture was repaired and its three methods rerun successfully before
the remaining suite ran. Settings integration keeps its model timing failures
fatal: 7 invalidation, 9 immediate-entry, and 7 warm-fling misses remain recorded.
Pixels, ownership, both theme palettes, font changes, allocation recovery,
native/temporal coverage, damage, and clean-source export reproduction pass.

A fixed boot mirror was rejected by host memory tests: reserving the whole
2,053,700-byte blob adds ten Orrery allocation failures and prevents full card
residency; another lifecycle loses recovered menu caches. The tour alone passes
and does not override those failures. `build/iteration33-mirror-memory/report.json`
retains both baseline and reservation results. This tests allocation pressure,
not device bandwidth or physical heap fragmentation.

Neither these 240 MHz captures nor a 1.2x timing projection qualifies a 200 MHz
CPU budget. Local IDF accepts 80/160/240 MHz, not 200. A separately identified
160 MHz build with consistent power limits and observed clocks is the smaller
supported qualification experiment once frame timing at 240 MHz is acceptable.
Physical touch, optical presentation, all-app blur, and power consumption remain
outside these native-menu captures.

A subsequent bounded source comparison used exact zero starts and at least
30 seconds of preparation before each capture. Settings raw mode additionally
reported all 36 rows resident. The Settings reversal completion maxima were
38.947 ms for ROM and 38.043 ms for raw RAM, both with 12 floor misses. Launcher
150 px/s measured 29.751 ms for ROM and 29.941 ms for raw RAM, both with zero
floor misses. Launcher residency was not directly observable, so its raw result
is described only as warmed. This comparison does not establish a decisive
flash-source penalty. It also shows that the initial Launcher 36.833 ms result
is not a stable source-placement effect. All DMA chunks completed and cleanup
restored the defaults and closed COM3. Evidence is
`build/iteration30-watch/iteration33-source-ab*.json`.

## 2026-10-19: iteration 34 grouped exposure and native copy

The 5,043,168-byte candidate was built from 350 frozen source files. Root verified
every current/archive source, the tree and firmware hashes, embedded ELF identity,
flash asset object, and application slot fit. Art pixels, palettes, descriptors,
and offsets are unchanged from 33; only the artwork dependency fingerprint changed
for the new coverage API. The sealed artifacts are under
`build/artifacts/iteration34-native-tiles`.

The synchronous and worker exposure paths group matching consecutive rows.
Native-only initialized history takes a direct copy without uniform scanning;
generic temporal blending is unchanged. Launcher variant interpolation now moves
source bounds and stride calculations outside the pixel loop. Exact host tests
cover 6,000 worker and 6,000 synchronous exposure cases, 600 two-pass native-copy
frames, and the extended 5,818,951-pixel prepared-source oracle. A fresh strict
103-source simulator build and 26 targeted Python methods pass. Actual Settings
integration passes pixel, lifecycle, fault recovery, blur coverage, and damage
checks but still fails the same three model timing assertions: 7 invalidation,
9 immediate-ROM-entry, and 7 warm-fling late frames. No performance waiver is
implied by the passing correctness checks.

The single guarded application write passed full-slot and preserved-region
verification. Four initial cells ran after the captured reset, with fresh ROM
and native-copy identity checks at each capture boundary. CPU remained 240 MHz.

| Cell | Work max ms | Completion interval max ms | Intervals over 33.333 ms |
| --- | ---: | ---: | ---: |
| Settings 150 px/s negative | 27.890 | 30.106 | 0 / 121 |
| Settings 1800 px/s alternating reversals | 40.454 | 40.703 | 6 / 88 |
| Launcher 150 px/s negative | 34.941 | 35.140 | 3 / 79 |
| Launcher 1800 px/s alternating reversals | 28.880 | 29.076 | 0 / 134 |

These are 685 frames, 437 painted frames, 422 completion intervals, 188 target
deadline misses, and 9 floor misses. The fast Launcher cell starts at the prior
368-pixel offset and returns to zero; Settings starts and ends at zero. Changed
frame cadence also changes intermediate positions, so fewer misses than 33 do
not establish a fixed per-frame speedup. The Settings reversal maximum worsened
and the required floor still fails. Its worst current frame has 37.004 ms raster,
including 10.790 ms history, 1.547 ms tick, and 0.639 ms UI wait. The Launcher
slow maximum has 33.287 ms raster, including 11.103 ms history.

All measured DMA jobs completed. Allocation failures remained zero; UI stack
minimum free was 1,628 bytes, PSRAM minimum 3,685,928 bytes, and internal minimum
9,680 bytes. Cleanup returned HOME, restored normal autosleep and controls,
disabled the guard last, and closed COM3 without pending input. The immutable
record and raw evidence are `build/iteration30-watch/iteration34-device-final-record.json`
and `iteration34-native-early*.json`. No broad frame-floor qualification follows
from these four cells.

A separate normal-attach Settings comparison used native-copy modes 0/1/1/0,
at least 30 seconds of warming after reentry, and verified zero start/end offsets.
Completion maxima were 37.327 / 34.421 / 34.833 / 37.686 ms, with 17 / 7 / 9 / 15
floor misses respectively. The result favors keeping direct copy enabled for
this workload, but every arm still fails. Trajectory maxima differ (797 / 652 /
657 / 672 pixels), and control capture counts differ (20 / 10 / 12 / 12); this
is not an equal-work kernel benchmark. The first arm spent 33.891 ms cumulative
in control preparation, versus 10.447 / 13.475 / 12.692 ms in later arms.
All DMA, identity, ownership, allocation, stack and cleanup gates passed. Mode 1
was restored; the port is closed. Evidence is `iteration34-copy-ab*.json` beside
the initial capture files.

Two additional compact captures disabled the detailed pipeline probe while
retaining the independent completion counter and its post-DMA-fence timestamp.
They measured 34.683 / 34.377 ms maximum completion intervals and 3 / 6 floor
misses across 93 / 88 intervals. Thus this workload still fails without the rich
probe. These captures deliberately have no per-chunk timing, DMA pairing, or
stage attribution. Native identity, pose, blur/cache and cleanup checks pass;
raw records are `iteration34-compact*.json` in the same directory.

## 2026-10-19: iteration 35 rolling-sum reuse and scheduler experiment

The next candidate retains exact rolling-control column sums between compatible
display stripes. Its caller-owned 4,024-byte pool lives inside the existing
Settings PSRAM allocation. Continuations copy sums to the existing stack for
the hot loop, then save them for a following stripe. Full pools, changed source
generations, clipping changes, noncontiguous draws, recording, and cache resets
fall back to the original calculation. The pixel operation and rounding are
unchanged. Native tests cover 19.2 million cached/uncached output positions and
3.41 million independent fractional-box pixels, plus actual Settings lifecycle,
source mutation, allocation failures, and pool exhaustion. The target rolling
call chain grows 48 stack bytes; it adds no canvas or internal heap allocation.

`settings_carry 0|1` and the separate `NOSCARRY` report allow a controlled device
comparison. The measurement candidate defaults to carry 1. `frame_priority 0|3`
is a separate experiment, default 0: it preserves the inherited main-task
priority except during an explicitly enabled ON-frame input/render/DMA window.
It restores the saved priority before bookkeeping, serial output, housekeeping,
AOD/off waits, and pacing. `NOSSCHED` records actual sampled in-frame priority,
cumulative mismatches, and sample count; outside-paint queries also verify
restoration. Priority changes do not establish a 200 MHz CPU budget or prove
background-service latency.

The strict firmware build contains 351 frozen source files; root verified the
archive, current sources, ELF/BIN identity, flash artwork object, and slot fit.
Artifacts are `build/artifacts/iteration35-native-tiles`. The fresh 103-source
simulator and 12 targeted test methods pass. Settings integration correctness
passes, while the same 7 / 9 / 7 modeled timing failures remain fatal and recorded.
The application-only flash and captured boot passed identity and whole-slot
verification. The reported 5,062,656-byte host transfer count includes 16 KiB
transport padding; it is not evidence that all those bytes changed. The verified
full-slot image preserves the non-erased historical tail after the modeled
5,050,368-byte sector edge. `iteration35-write-length-note.json` records this
distinction without inferring an extra erase.

Four warmed Settings fast-reversal cells compared carry 0/1/1/0 while priority
mode stayed 0. Maximum completion intervals were 35.984 / 33.663 / 38.008 /
34.356 ms, with 11 / 3 / 7 / 11 floor misses. Carry-on windows recorded 712 / 708
actual continuation hits, avoided 1,291,075 / 1,267,736 initialization byte reads,
and copied 995,872 / 968,880 bytes of state. This proves reuse, not a consistent
worst-frame speedup. Every cell still fails the floor. Actual sampled paint and
query priority were 1, mismatch counters stayed zero, and sample counts advanced.
Query CPU frequency was 240 MHz. Allocation failures were zero; UI stack minimum
free was 1,604 / 8,192 bytes and flush minimum 1,828 bytes. The run restored carry
1, priority mode 0, HOME, autosleep and probe 0, and closed COM3. Raw evidence is
`build/iteration30-watch/iteration35-carry-ab*.json`.

The separate warmed priority comparison used modes 0/3/3/0 with carry 1 fixed.
Maximum completion intervals were 35.003 / 32.248 / 32.589 / 34.797 ms, with
3 / 0 / 0 / 5 floor misses across 86 / 92 / 91 / 88 intervals. Actual sampled
paint priorities were 1 / 3 / 3 / 1, outside-paint priority remained 1, mismatch
counters stayed zero, and sample counts advanced. Both boosted cells meet the
floor in this bounded workload but still miss the 25 ms target. The current
frames in their worst pairs spend 28.828 / 28.095 ms in raster, including
8.264 / 8.343 ms in history. Rotation is concurrent, not an additional serial
6.457 / 6.836 ms. Trajectories, painted counts and control preparation differ;
this is not an equal-work kernel benchmark.

Touch read errors increased in both modes: 706 / 1,209 / 1,483 / 186 errors
over 1,448 / 1,458 / 1,487 / 1,458 reads between the health snapshots, with
last error 259 and no injected failures. Those snapshots extend beyond the
quiet captures. The code alone does not identify an electrical cause or show
that priority changes caused the errors. Physical-touch and background-service
latency remain unqualified. Priority mode 0 was restored along with the other
defaults, HOME and normal autosleep; the reader stopped and COM3 closed.
Evidence is `iteration35-priority-ab*.json` and
`iteration35-priority-detailed-review.json` in `build/iteration30-watch`.

The scheduler boost remains opt-in. These menu-only captures do not qualify
all-app blur, power consumption, optical presentation, or the 200 MHz budget.

A subsequent rotation 0/1/1/0 comparison held priority mode 3 and carry 1 fixed.
Normalized rotation time was 20.08 / 16.09 / 16.05 / 20.11 ns per output byte,
about a 20% reduction in that concurrent stage. Maximum completion intervals
were 33.623 / 32.657 / 32.645 / 32.779 ms, with 1 / 0 / 0 / 0 floor misses.
The first baseline cell also shows that the scheduler boost's earlier zero-miss
result does not generalize even to every repeat. Every arm misses the 25 ms
target. Painted counts and trajectories differ; rotation savings cannot be
subtracted directly from whole-frame time or equated with measured power.

Actual sampled paint priority was 3 in all four cells, query priority was 1,
and mismatch counters remained zero. Allocation failures were zero. Touch
errors again occurred in both rotation modes. Cleanup restored rotation 0,
priority 0, carry 1, HOME and normal autosleep, then closed COM3. The immutable
records are `iteration35-rotation-*` in `build/iteration30-watch`.

Source review found an independent input defect: `board_touch_read` maps an
I2C read failure to the same zero result as a real release. The NOS sampler
therefore ends a physical drag on a failed read. Error 259 is the local SDK's
`ESP_ERR_INVALID_STATE`; the I2C driver also returns it for failed transactions,
so it does not alone identify concurrent API misuse. A correct recovery needs
an explicit unavailable/cancel path, bounded stale-input handling, and review
of press/hold consumers. Holding or synthesizing ordinary UP events can still
activate actions. This is documented, not fixed or attributed to the rendering
changes; design evidence is `build/touch-recovery-review/REVIEW.md`.

## 2026-10-19: iteration 36 virtual-black history experiment

Iteration 36 promotes paired-store rotation to default 1 based on iteration 35's
completed rotation-stage comparison and correctness tests. Priority mode remains
0. Virtual-black history is an opt-in exact representation, default 0:
`history_virtual 0|1` reports a separate `NOSHISTORYV` line. Disabling it
materializes virtual blocks before acknowledging the change.

The strict firmware build archived 351 unchanged source hashes in
`build/artifacts/iteration36-native-tiles`. Independent source/archive, ELF/BIN,
flash artwork placement and slot checks passed. The fresh 103-source simulator
and 28 targeted methods passed, including actual command guards. Settings
correctness passed; modeled timing failures of 7 / 9 / 7 frames remain recorded
in `root-host-validation.json`. A single guarded application-only write verified
the complete expected slot and unchanged outside regions, followed by one
captured reset and identity check. Transport padding is distinguished from
verified contents in `iteration36-write-length-clarification.json`.

Four normal-attach Settings fast-reversal cells used virtual modes 0/1/1/0,
exact zero starts and at least 30 seconds of warming. Rotation 1, priority mode
0, carry 1, native copy 1, ROM 1, uniform 1, worker 0 and pacing 1 stayed fixed.

| Virtual mode | Maximum completion interval | Floor misses / intervals |
| --- | --- | --- |
| 0 | 36.505 ms | 10 / 90 |
| 1 | 35.864 ms | 8 / 87 |
| 1 | 35.528 ms | 11 / 87 |
| 0 | 35.433 ms | 9 / 89 |

All four cells fail the frame floor. Enabled cells reported 9,193,536 /
9,196,800 avoided bytes, 653,760 / 672,192 materialized bytes, and 5,796,960 /
5,793,312 classified pixels. These cumulative-counter deltas are logical
estimates, not measured physical bus traffic. Work and trajectories differ;
the comparison establishes no consistent frame-floor benefit. Worst-pair
history times were 9.246 / 8.161 / 8.636 / 8.560 ms, included in raster time.

Fresh identity, mode, owner/pose, history/cache, DMA totals and recorded
worst-pair checks passed. Actual sampled paint priority was 1, outside-paint
priority was 1, and mismatch counters stayed zero. Query CPU frequency was
240 MHz; this is neither continuous in-paint clock evidence nor 200 MHz
qualification. Allocation failures stayed zero; minimum free UI/flush stack
was 1,700 / 1,836 bytes and PSRAM minimum free was 3,685,940 bytes.

Touch snapshot deltas were 137 / 1 / 319 / 1,076 errors over 1,455 / 1,449 /
1,455 / 1,455 reads, without injected failures. These observations do not
qualify physical touch or radio health. Cleanup restored virtual 0, rotation 1,
priority mode 0, carry 1, HOME, normal autosleep and probe 0, released the guard
last, and closed the reader and COM3 with no pending input. Raw and analyzed
evidence is `build/iteration30-watch/iteration36-history-ab*.json`; the sealed
summary is `iteration36-history-final-record.json` in that directory.
