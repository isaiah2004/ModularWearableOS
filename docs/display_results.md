# Display optimization results — 2026-09-19

**Subsequent owner-requested SH8601 trial:** firmware now explicitly selects
the existing Waveshare SH8601 initialization table and its zero-pixel native
X offset, including synthetic benchmark addressing. The former all-FF probe
fallback selected CO5300 with a six-pixel offset. Raw probe evidence remains
available; selecting SH8601 does not establish controller identity. QSPI stays
at 40 MHz. The benchmark results below describe the earlier fallback firmware,
not a fresh performance measurement of this trial. Visual acceptance is pending.
The application-only flash was hash-verified on COM3. USB confirms SH8601,
owner override, gap 0 and 40 MHz; ID reads still return all FF. Both 20-frame
generator checks (100x100 and 466x466) completed with ESP_OK, no pending or
cancelled transfers, unmatched completions or overlapping submissions. The
watch was returned home. Evidence: [SH8601 validation](display-data/sh8601-validation.jsonl).
The previous working application remains at
`build/display-optimization/optimized-image/watchfw.bin`; the trial image is
at `build/display-optimization/sh8601-image/watchfw.bin`.

**Owner visual review: open defect.** During the attended rerun, the owner
reported uneven left/right gradient fades: vertically stacked square-like
sections appeared to update at different times. Navigation and DMA accounting
passes do not establish visual correctness. This transition issue remains
unfixed and its precise cause is not yet confirmed.

Source inspection found automatic transitions advance `0.16 * 466`, about
75 pixels per rendered frame, while the fade front is only 34 pixels wide.
This establishes coarse animation stepping, but does not by itself establish
the cause of the reported patchwork appearance. The composed frame is also
sent as separate rotated 16-row strips without panel scan synchronization;
"atomic" in the current code means coherent CPU composition, not an atomic
or tear-free panel refresh. Firmware timestamps measure CPU work and DMA
completion, not the timing/location of a visible tear.

The prototype reduces Breathe's unnecessary progress-ring damage while retaining
the existing display clock, buffers, panel initialization and smartwatch apps.
Firmware change: `a75a68e` on `perf/display-optimization`. Original OS rollback:
`dc1999c` / `display-baseline-20260919`. Binary hashes are in
[images.json](display-data/images.json); local rollback images and exact build
configuration are described in [display_baseline.md](display_baseline.md).

## What caused the slowdown

There is no 70,000-pixel limiter in this checkout. A 350×200 internal-SRAM
transfer delivered **120.32 region presentations/s**, 16.85 MB/s, P95 9 ms,
without transfer errors. 466×432 and 466×440 transfers delivered 42.27 and
41.39 presentations/s respectively. Full-screen SRAM reached 39.02/s,
16.95 MB/s. These are host writes, not physical panel refresh rates.

Breathe was invalidating the entire bounding box of its growing progress arc
every frame. Damage grew above 100,000 pixels/frame on average. A single damage
rectangle at least 233 rows high triggers PSRAM composition followed by a
separate presentation pass. Baseline mean raster time was 23.29 ms; mean
frame-end presentation/drain was 10.26 ms. The 60-second trace averaged 26.81
presentations/s with P50/P95/P99 work times of 32/64/72 ms. This explains the
measured app slowdown without a fixed pixel cap. The historical sub-1-fps
failure was not reproduced in these representative apps.

The optimization records a settled arc plus its moving tip for damage tracking,
while rasterizing the original single arc to preserve anti-aliasing. The final
10% of a turn conservatively records the full arc because the existing fast
sector math makes smaller bounds unreliable near a full turn. That portion
retains the older large-redraw cost. Corrected arc recorder bounds include all
rows/columns that the rasterizer can touch. A separate fix prevents explicit
overlay damage from replacing a required full repaint.

## Before and after on COM3

Each trace lasts 60 seconds; units below are milliseconds. Percentiles are
conservative histogram upper bounds. Level responds to live IMU data, so its
present counts are not a deterministic comparison. Both runs used the same
clock/buffers and retained background Wi-Fi retries and BLE advertising.

| App | Presentations/s before → after | P50 before → after | P95 before → after | P99 before → after |
|---|---:|---:|---:|---:|
| Face 0 | 59.41 → 59.39 | 11 → 11 | 15 → 15 | 17 → 17 |
| Level | 46.07 → 42.88 | 5 → 5 | 7 → 8 | 10 → 10 |
| Breathe | **26.81 → 44.12** | **32 → 19** | **64 → 40** | 72 → 72 |

Breathe presents **64.6% more frames/s** and reduces mean requested repaint area
from 100,659 to 26,687 pixels/frame (**73.5% less**). Mean raster work falls from
23.29 to 15.32 ms; frame-end presentation/drain from 10.26 to 2.22 ms. Work
exceeded 16.667 ms on 1,403/1,609 baseline frames (87.2%) and 1,668/2,647
optimized frames (63.0%). The larger absolute miss count reflects the increased
number of frames; this is still **not a sustained 60 Hz app**. P99 remains
limited by conservative large redraws.

Machine-readable evidence: [baseline apps](display-data/instrumented-baseline.jsonl)
and [optimized apps](display-data/optimized-apps.jsonl). No app trace had pending
DMA, unmatched completion callbacks or overlapping submissions at completion.

## Transport evidence

The [full transport table](display-data/transport.md) and JSONL alongside it
contain nine full-screen source/buffer runs plus three region sizes, all at
40 MHz and each exceeding ten seconds. SRAM throughput improves from
11.88 MB/s with 3,728-byte chunks to 16.95 MB/s with 14,912-byte chunks.
PSRAM-copy throughput at the latter size is 13.23 MB/s; generated pixels reach
10.52 MB/s. Generation/copy and DMA are serialized in this baseline harness.
Neither result is evidence that the bus itself becomes slower with that source.
Completed bytes equal submitted bytes in every completed run, with no unmatched
callbacks or overlapping submissions.

The original capture host timed out at 120 seconds during the approximately
three-minute sweep, although it retained all nine completed run records while
attempting cleanup. The host now extends the timeout on each completed PERF_RUN
(with regression coverage), so a progressing sweep is accepted while a stall
still times out. Region runs completed normally with the fixed host.

## Reproduction

```powershell
.\tools\build.ps1 -Port COM3 -Flash
python tools/display_perf.py --port COM3 --output build/repeat-apps.jsonl --command "perf display info" --command "perf display app face0 60" --command "perf display app level 60" --command "perf display app breathe 60"
python tools/display_perf_report.py build/repeat-apps.jsonl -o build/repeat-apps.md
python tools/display_regression.py --port COM3 --output build/repeat-regression.jsonl
.\tools\test.ps1
python -m unittest discover -s tests -p 'test_*.py'
```

Use one serial process at a time. Capture defaults to the confirmed watch USB
serial; a port number alone does not bypass identity checks. Synthetic workloads
temporarily suspend UI processing, then restore rendering. App trace workloads
run through the normal OS scheduler with radios active.

## Regression evidence

| Check | Result |
|---|---|
| ESP-IDF firmware build and application-only flash | Passed; esptool verified the written hash |
| 60-second reset/boot capture | Passed; READY and normal OS start observed |
| Three subsequent reset/boot cycles | Passed; READY each time, no captured runtime faults, face 4/brightness 75 retained; these were warm USB resets, not power-cycle cold boots |
| Existing native unit suites | Passed, including board transforms, Pip, motion wake, notifications, weather and Ultra rendering |
| Arc rendering and retained damage | Exact pixel comparisons, clips and 1,198 forward/backward incremental frames passed |
| Required full redraw during list scrolling | Simulator `--listrects` assertion passed |
| Python discovery | 24 tests passed |
| Native USB app inventory/selection | All 26 apps reached their requested app IDs |
| Watchface selection and overlays | All 13 faces and four overlay open/close command sequences passed |
| USB gesture/command responsiveness | Passed; 116 commands, maximum acknowledgement 422 ms including transitions |
| Sensors and display settings after regression | IMU valid; brightness setting and panel command both 75; display awake |
| User state restoration | Original face 4 restored, returned home, normal sleep timers restored |
| Repeated animated soak | Passed: 30 Breathe traces, 1,800.98 seconds of trace coverage |
| Physical screen/touch/tearing and battery current | Not measured remotely |

The app/face regression checks state and command completion, not physical
appearance. BLE reported no connected peers; SD was not mounted. Their active
workload behavior was not validated by opening the corresponding app.

To repeat the long animated workload, generate a script with thirty copies of
`perf display app level 2`, `perf display app breathe 60`, and
`perf display info`, then pass it to `display_perf.py --script PATH`. Switching
to Level between Breathe sessions ensures the one-minute animation restarts;
simply tracing Breathe for 1,800 seconds would mostly measure its finished,
static screen. The script represents 30 minutes of Breathe trace windows plus
one minute of intervening Level traces and command overhead.

`python tools/display_soak_report.py build/soak.jsonl -o build/soak-summary.json`
checks duration, trace count, transfer accounting and captured runtime faults.
Inspect the JSON `status`; it stays `incomplete` until the required coverage is
present and reports `fail` for cancelled, pending or inconsistent transfers.
Memory/stack ranges are evidence for review, not a substitute for power or
physical display measurements.

The completed [soak summary](display-data/soak-summary.json) is generated from
the [full captured JSONL](display-data/soak.jsonl). It contains 78,569 Breathe
presentations, 1,688,044 completed DMA transactions and 4,180,890,040 completed
payload bytes. The weighted rate was **43.63 presentations/s**, with individual
minute windows between **43.32 and 43.80**. Longest measured frame work was
**74.107 ms**. There were no cancelled/pending/unmatched/overlapping transfers,
accounting mismatches or captured watchdog, panic, brownout or reboot messages.

Internal free memory ranged from 59,971 to 60,103 bytes: a single 132-byte
decrease remained bounded through the remainder of the run. The largest DMA
block stayed at 31,744 bytes. PSRAM free varied from 476,364 to 476,384 bytes
after the all-face tour populated the existing caches. Main/flush task stack
high-water margins stayed at 3,068/1,784 bytes. These measurements showed no
continuing decline during this workload; they are not a proof of no leak under
all possible apps and radio loads.

The [reset validation record](display-data/reset-validation.json) includes the
final board/settings responses and hashes of the three local boot logs. Touch
and IMU devices responded after reset. RTC validity was already zero at the
baseline and remained zero; setting/calibrating the RTC was outside this change.

## Limits and next experiment

All five controller probe cycles returned FF. Identity remains **unknown**;
the legacy working fallback is preserved. No controller-specific init change,
continuation-write experiment, clock sweep or overclock was attempted.
The controller read protocol must be electrically verified before those tests.
No physical TE connection is available in the current board map.

Read-only comparison with [Waveshare Demo V3](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/Resources-And-Documents)
found matching reset timing, `03 00 REG 00` framing and per-bit direction changes.
The next bounded probe experiment is to compare per-bit versus whole-byte D0
input turnaround, each with pull-up and pull-down, before SPI driver ownership.
Read 04/DA/DB/DC five times per combination. If bytes simply follow pull bias,
the return line is undriven. Compare CS/CLK/D0 traces against the
[SH8601Z interface diagrams](https://files.waveshare.com/wiki/common/SH8601Z0_V0.0.pdf)
before selecting a controller. Do not send controller-specific read-enable
commands while the identity remains unknown.

Physical tearing, touch alignment and idle/active battery current require
on-device observation and measurement. USB supply voltage is not a power
measurement. Successful Wi-Fi traffic, active BLE streaming, SD-card stress,
power-cycle cold boots and thermal qualification are distinct from the tests
reported here. The benchmark does not yet expose renderer-discard mode,
command-byte overhead, queue-empty events, CPU utilization, dirty-area
percentiles or complete-frame cadence distributions. It must not be presented
as full completion of every acceptance gate in the supplied runbook.
