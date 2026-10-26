# Rendering and power work on `optimizations`

This worktree integrates `watch-test` at `e4700d5`, including the latest Orrery
scrubbing, curved labels, PSRAM ownership, Settings sprites, and power updates.
Only the optimization delta against the inherited dirty snapshot is replayed;
the original worktree at `A:\makerspace\smartwatch2.0` is unchanged. The work
lives in `A:\makerspace\optimizations`. The exact pre-rebase tree and two local
checkpoint refs are recorded in `build/rebase-20261017/manifest.json` and its ZIP.

The target is **40 complete frames per second**, with **30 fps as the minimum**,
motion exposure on moving content, and a **200 MHz CPU work budget**. These are
acceptance requirements, not results established by selecting `NOS_FPS=40`.
The S3 supports 80/160/240 MHz CPU configurations, so a 200 MHz budget must be
validated conservatively; scaling a 240 MHz timing is an estimate.

Current validation status: **the timing requirements still fail**. Revision 28a
is flashed. Its 28c capture has eight valid Activity/Heart windows at 290 and
900 px/s in both directions: 449 painted frames, 433 measured intervals and
60 intervals beyond 33.3 ms. Maximum work/presentation interval is
41.721/48.915 ms. Live full-column V ran in none of these windows; the sharp
history blend did run. Weather's first window contained no scrolling or painted
frames and is excluded; the remaining three Weather windows were not attempted.
Separate cleanup restored HOME face 8/page 0, ordinary sleep and guard off.
COM3 is closed. This does not qualify general-app blur, power or 200 MHz work.

The latest complete list-speed sweep is iteration 19b. Iteration 22 has complete,
limited Settings and embedded Apps damage-path ABBAs at 290 px/s. Both have no
30 fps floor miss; Settings still misses 25 ms deadlines, and Apps completion
intervals still exceed 25 ms. Earlier iteration 20 attempts stopped before
enabling that candidate because diagnostic replies were truncated. Globe timing
remains at iteration 14.
Iteration 24 additionally validates the corrected physical HOME navigation and
measures the revised vertical kernel; these are not an updated app-frame timing
qualification. Iteration 25's vertical kernel improves again, but its stronger
temporal candidate fails the 30 fps floor in all four tested Activity windows.
The eight-window sentinel is incomplete after its last endpoint assertion fails;
the broader matrix was not run. The watch was restored to temporal mode 0.
Host pixel and resource checks pass as detailed below.
The revised continuous Settings shutter still misses the 30 fps floor and
remains disabled. Embedded Apps has no floor misses in the four tested slow
scrolls, but three fast-scroll cells miss it; all eight cells miss 40 fps slots.
An experimental rotation kernel reduced normalized rotation work by about 21%
in the completed forward Settings comparisons, with inconsistent whole-frame
gains. Its reverse-order and launcher comparisons remain incomplete after a USB
acknowledgement gap; it also remains disabled. Iteration 19b's bounded startup
and UI captures recorded no allocation failure, but weather timed out. Iteration
17 completed one weather transaction; unrestricted network reliability remains
unqualified.
These builds have complete source and exact ELF/BIN archives under
`build/artifacts/`. COM3 remains reserved for this run.
The original build 164 has a verified full 16 MiB recovery
image; all experimental flashes were restricted to the guarded active app slot.

The exact Sun specialization and validated integer shading reduced matched
static Sun globe rendering from 39.654 ms to 25.772 ms (two accepted renders per
combination, about 35%). This is a shader-stage result, not a complete-frame
rate. The target independently reported zero coefficient and shade mismatches.
Repeated reverse-order and Saturn shader comparisons are incomplete because of
USB acknowledgement gaps; raw partial evidence is retained and excluded where
preconditions could not be recovered.

| Iteration 14 quiet workload | Painted frames / present intervals | Maximum complete-frame work | Maximum presentation interval | 30 fps floor violations |
|---|---:|---:|---:|---:|
| Sun motion | 40 / 29 | 27.455 ms | 36.723 ms | 1 |
| Saturn motion, stale content | 14 / 7 | 15.209 ms | 26.954 ms | 0 |
| Settings motion | 85 / 84 | 38.797 ms | 44.495 ms | 6 |

Saturn's sparse repainting is a failure of content freshness, not an FPS pass.
Detail 4 improved Sun freshness, but its two tested budgets still missed the
presentation floor. Testing stopped when AES reported an allocation failure
during a weather request. A bounded reset/idle inspection reproduced internal
heap pressure without opening cards. TLS already uses PSRAM; AES also requires
internal DMA storage. The exact failed allocation is not known from that log.

Iteration 15 added allocation-failure diagnostics. At HOME, face 8, its idle
network interval recorded 14 failures; the final request was 124 bytes with
`INTERNAL | DMA | 8BIT` capabilities. Only 295 DMA-capable bytes remained free,
with a largest block of 208 bytes, despite roughly 3 MB of free PSRAM. The SDK
allocator name does not identify whether that request came from AES or Wi-Fi.
All renderer allocation counters remained zero. Further UI tests stopped.

These are finite diagnostic windows at an actual 240 MHz CPU clock. Neither
200 MHz qualification nor power/current savings have been established. Transfer
completion measures pixels arriving in panel GRAM, not an optical or vblank
measurement. Full evidence, window limits, capture gaps and recovery hashes are
in [OPTIMIZATIONS_DEVICE.md](OPTIMIZATIONS_DEVICE.md).

Iteration 13 host checks passed 4,199 damage/full-frame comparisons, 9,504 globe
images, 33,554,432 shade comparisons, delayed asset lifecycle and repeated memory
reopening. The memory diagnostic peaked at 6,617,002 of 6,815,744 tracked bytes
with no allocation failure. It explicitly excludes 14 legacy timing assertions;
its five pre-existing motion-shape audit failures remain unresolved. These
correctness checks are not FPS acceptance tests.

Iteration 15 is a candidate, not a measured improvement. Settings now uses its
ready sharp cache for settled rows even on zero-motion frames, removing the
repeated cache/live antialiasing change during slow dragging. Launcher icons blend
between the three existing sizes across narrow transition bands. Its palette has
64 levels, preserves the old 17 anchor rows, and interpolates RGB565 channels between
them; this adds 24,576 tracked PSRAM bytes and no sprite images. Independent
source-image checks reduce the worst measured luminance step by about 3x, but
integer input/pixel stepping and temporal sharp/blur alternation still remain.

Iteration 15 passed 7,218 combined damage/full-frame comparisons, focused Settings
cache/blur/fault checks and the optimization unit suite. Its measured host memory
peak was 6,642,090 of 6,815,744 tracked bytes, with zero allocation failures.
These checks do not cover the SDK's separate internal DMA heap. An obsolete
launcher palette assertion initially failed; the corrected test independently
checks preserved anchors and interpolated channel values. Both logs are retained.

Iteration 16's 8-row rendering candidate was rejected before flashing. Each
height's damage/full comparison passed, but comparing 8 against 16 rows exposed
visible differences in legacy streaming blur. Smaller raster strips also lose
the retained layer's direct internal-buffer path. These are reasons to retain
the canonical rendering geometry, not evidence of an accepted optimization.

Iteration 16b instead keeps 16-row raster buffers and splits only outgoing DMA
transfers into pieces of at most 8 rows. The two raw and two rotated buffers use
44,736 rather than 59,648 bytes, reclaiming 14,912 internal bytes. Each raw stripe
remains owned until every piece has been read; final completion still waits for
every DMA callback. Firmware and simulator share the geometry definitions. The
additional display transactions need hardware timing; no frame-rate improvement
is established yet. All 7,218 host damage/full-frame comparisons and the focused
optimization suite pass. Host peak allocation remains 6,642,090 of 6,815,744 bytes.

The verified 16b firmware completed a 175-second idle capture with no allocation
failure. DMA-capable free memory stayed between 26,275 and 26,719 bytes, with a
20,480-byte largest block. There was no IP/TLS/weather completion after an initial
Wi-Fi authentication disconnect, so this does not establish that the earlier
network allocation failure is fixed. Raw evidence and an independently hashed
analysis are preserved in `build/hardware-iteration16b-memory.jsonl` and
`build/hardware-iteration16b-memory-analysis.json`.

In 16b's isolated 150/290/900/1,800 px/s sweeps, both directions were tested with
the same blur defaults. Launcher maximum presentation intervals were
26.868–29.047 ms with no 30 fps floor violation, but every cell had 25 ms deadline
misses. Settings had four failing cells, with maxima of 35.092–47.076 ms. The
47.076 ms cell includes a weather connection failure and its logging; it remains
in the evidence. All allocation counters stayed zero. The repeated older slow
trajectory yielded Settings 37.767 ms and launcher 26.815 ms maximum intervals.
These differing cache/network contexts do not isolate the DMA split's cost.
Raw evidence is `build/hardware-iteration16b-isolated-lists.jsonl`; its analysis
pins the raw hash. The watch was returned to HOME/default controls and COM3 closed.

Iteration 17 passed 7,218 damage/full comparisons and 42 optimization Python
tests, plus the native and focused suites. Tracked peak memory remains 6,642,090
of 6,815,744 bytes with zero allocation failures. A disabled-by-default pipeline
probe records worker submissions, DMA completions and both frames around the
worst presentation interval. A measured 42.017 ms Settings interval consisted of
frame starts 24.987 ms apart, followed by 10.175 ms and 27.205 ms completion work:
the dirty payload jumped from 29,120 to 313,448 bytes. Regular frame starts alone
therefore do not establish regular completed frames. Probe stage measurements
overlap and must not be added together. All raw captures, including ACK gaps,
are pinned by `build/hardware-iteration17-analysis.json`.

Iterations 19–22 placed the face at the physical right end, with Apps immediately
to its left and Activity, Heart, Weather, Music and Timer further left. Rightward
swipes opened Apps; iteration 23 corrects this order as described below.
Vertical swipes on Apps scroll its existing list. Opening an app and
returning preserves its list position. This reuses the launcher cache rather
than allocating another page image. The button on the face opens Apps, and the
button on another HOME page returns to the face. Explicitly unlocked Orrery
gestures retain ownership; the ordinary locked face supports page swipes.
Hidden face calculation stops behind Apps, including an outgoing app iris, and
resumes before the first face pixel reappears. Navigation lifecycle checks cover
immediate input after reveal, AOD cancellation, page limits, widget tap mapping,
and double-press behavior. The two dedicated navigation scripts compare 2,125
damage/full frames. The updated legacy blur script exercises two additional
pager legs; its motion audit now exposes the existing three-one-pixel creep tail
four times instead of twice (seven total failures instead of five).

Iteration 19 passed 8,965 tour, stress, Orrery, integration and page
damage/full comparisons, plus the 2,125 dedicated navigation comparisons and
45 optimization Python tests. The focused native, asset, IO, face, Settings and
memory gates also passed. Peak tracked memory is 6,642,090 of 6,815,744 bytes,
with no allocation failures. The seven motion-shape failures above and 14
excluded legacy timing assertions remain explicit; these are correctness and
memory results, not frame-rate acceptance. Exact results and executable hash
are in `build/iteration19-gates/summary.json`.

The updated page tour exposed another desktop reference-timing dependency:
the page cache's 20 ms admission decision was not recorded. It could publish
the Music image four frames apart, then invalidate it four frames apart at the
two-second refresh, producing three genuinely different images. Quality-tape
kind 5 now records only that discretionary admission decision. Mandatory
guards and allocation failure still take precedence. The 1,683-frame page
comparison passes; a private build of the old implementation reproduces the
original failure under the same strict parser. Target admission is unchanged.

Iteration 19b contains exactly one further target-only change: `nav_state` is
classified as a quiet query so it can observe AOD without waking it. Firmware
build and source-archive comparison passed. Its archived
`host-validation.json` pins the reused iteration 19 evidence and exact source
delta. The optimized Settings shutter, alternate rotation and detailed pipeline
probe all remain disabled by default pending device qualification.

Iteration 19b was flashed through the guarded active-app-only procedure, with
exact readback and unchanged boot/layout/OTA/outside-region checks. On-watch
navigation checks passed: right swipe opens embedded Apps, left returns to the
face, vertical gestures stay in Apps, app return preserves its row, button and
double-button actions work, and AOD returns to the face. A separate observation
confirmed AOD remains active after `nav_state`. Its later cleanup query had an
ACK gap; an explicit read-only recovery established identity, state and clean
heap before continuing. Successful navigation and timing stages were not rerun.

| Iteration 19b workload, both directions | Maximum completion interval | 30 fps floor misses / measured intervals |
|---|---:|---:|
| Embedded Apps, 150 px/s | 32.668 / 33.293 ms | 0/74 and 0/74 |
| Embedded Apps, 290 px/s | 30.814 / 30.429 ms | 0/58 and 0/59 |
| Embedded Apps, 900 px/s | 30.044 / 34.793 ms | 0/32 and 1/33 |
| Embedded Apps, 1,800 px/s | 35.709 / 34.608 ms | 2/26 and 4/27 |
| Default Settings, 150 px/s | 27.733 / 42.908 ms | 0/93 and 1/99 |
| Default Settings, 290 px/s | 40.562 / 28.518 ms | 1/68 and 0/63 |

The revised continuous shutter's repeated warm 150 px/s negative-direction cell
reached 37.311 ms complete-frame work and a 37.453 ms completion interval, with
25 floor misses in 71 measured intervals. The bounded experiment stopped there:
nine cells completed, seven remaining cells and the final default reversal were
not run. It is a failed qualification, not a complete matched ABBA comparison.
No allocation failure was recorded. The watch was restored to default controls,
HOME face 8, normal sleep policy, and COM3 closed. Actual CPU clock remains
240 MHz; none of these results qualifies the 200 MHz budget or optical timing.

The first iteration 16 stress comparison also exposed an independent reference
problem: untaped 750 us cache-bake deadlines published rows on different frames
under desktop load. Instrumentation reproduced the same three original failing
hash pairs exactly. Desktop-only decision hooks now record/replay the Settings
and launcher budget exits while leaving mandatory allocation/admission exits
unconditional. Controlled-load replay passes all 895 stress frames. Target
firmware still uses the actual elapsed budget, and both original and diagnostic
logs remain available.

Iterations 19–22 used list blur thresholds nominally 480/1,120/2,240 px/s at the
configured 40 fps normalization, selecting 16/32/48 px directional kernels.
The code differences rounded positions before normalizing by elapsed time;
there is no hysteresis, so individual frames can cross the first threshold at
speeds just above 440 px/s. Slower changed pixels with valid history receive
75% current / 25% previous exposure outside directional coverage. Unchanged
pixels stay sharp; cold pixels have no valid prior sample.

| Content speed | Ideal displacement at 40 fps | Iteration 19–22 cached kernel | Proposed half-frame exposure |
|---|---:|---|---:|
| 150 px/s | 3.75 px | Sharp plus eligible temporal blend | 1.875 px |
| 290 px/s | 7.25 px | Sharp plus eligible temporal blend | 3.625 px |
| 900 px/s | 22.5 px | 16 px | 11.25 px |
| 1,800 px/s | 45 px | 32 px | 22.5 px |

Continuous exposure over linearly reconstructed sharp images has an independent
pixel oracle. A vertical prefix sum removes stripe-startup scans and replaces
three blurred variants. For all 36 exported Settings entries, image payload falls
from 2,321,242 to 1,030,410 bytes (55.6%), excluding shared workspaces and allocation
overhead. This is a storage result, not a speed improvement. Iteration 17's warm
experimental captures reached 45.567–72.359 ms of complete-frame work across the
requested speed bands. Actual filtered-pixel counters increased with no new
warm fallback calls, so the slowdown was in the active filter. It remains off.
A revised experimental loop computes source-row bounds and pointers once per
row and removes one high-word multiply per pixel. It passes the same independent
pixel and history-exclusion oracles, but iteration 19b's warm target capture
still failed as recorded above. Host speedups do not qualify target FPS.

The user approved lower-resolution globe shading during motion with full detail
restored at rest. Iteration 14 is implemented and is **not yet qualified**:
cooperative geometry/shading jobs, complete-image publication, 2x2/4x4 motion
sampling, exact full-detail restoration, bounded previews, and input-age
telemetry. Hardware also exposed a 57.480 ms Saturn body-preparation call during
minute refresh; preparation is now resumable as well. A fast shader alone does
not remove that stall. Motion approximation is limited to the approved globe
path; frame timing and image freshness remain separate acceptance checks.

Iteration 14 host checks passed 7,218 damage/full-frame comparisons, including
3,019 frames of deliberately slow preparation, preview reversals, cancellation,
late assets and mode changes. Independent image tests cover 198 completed exact
renders, 198 coarse renders and 198 exact restorations; preparation matches 99
complete original body states and cancels at all 27 phases. The first parallel
tour attempt had two hash differences that did not reproduce; the diagnostic
rerun had identical raw pixels and both subsequent tours passed. That initial
failure is retained in the logs. These are correctness checks, not device timing
or visual smoothness certification. Slow-scroll brightness and blur transitions
in Settings and the launcher are also under investigation after user feedback.

## Renderer

The proposed "cinematic blur at 30 FPS" plan mostly describes mechanisms already
present here. RGB565 output, nonrecursive 75/25 exposure, PSRAM history, explicit
DMA ownership, deadline pacing and stage/worst-frame profiling are implemented.
This project additionally has per-item sharp/blurred images, incremental cache
publication, retained layers and round-screen transfer clipping. Its cadence is
40 Hz (25 ms); selecting that rate does not prove every deadline is met.

The output pipeline uses two 16-row raw buffers and two 8-row rotated DMA
buffers, rather than two complete output frames. Three-to-five linear-light
temporal samples, video timestamp reconstruction/scene-cut handling and
black-frame insertion are not integrated into this UI animation path. The
current temporal filter blends encoded RGB565; the disabled Settings shutter
is a spatial exposure over a reconstructed sharp sprite, not a video decoder.

The plan's memory arithmetic needs correction: 466 x 466 RGB565 is 434,312
bytes (424.13 KiB), and two frames are 868,624 bytes. Complete rectangular payload
is 13.03 MB/s at 30 fps, 17.37 MB/s at 40 fps and 26.06 MB/s at 60 fps, before
commands and gaps. Current round clipping sends 358,376 bytes for a full visible
repaint. Extra complete buffers must be budgeted against actual cache ownership
and SDK reserve; nominal 8 MiB capacity does not establish available headroom.

Iteration 20 adds an optional tighter damage path, disabled by default. It
captures changed command bounds before bounding-box coalescing into two 960-byte
maps of 16-row bands. Small horizontal gaps (up to 64 px) merge to limit display
transactions; more than two remaining runs collapse conservatively into one
bounding span. Previous sharp damage is included once to retire temporal tails.
Full paints and current/previous stateful streaming blur keep the original
renderer. No additional image buffer or frame-time allocation is introduced.

The isolated same-input/same-quality comparison matched all 2,384 frames against
both iteration 19 and the candidate's default path. At 150/290 px/s, it reduced
Settings pixel payload by 14.53%/11.99% and embedded Apps by 33.49%/29.61%, while
keeping transaction counts close to baseline. These are host workload results,
not measured speedups. The integrated build passed 11,126 damage/full comparisons
(8,965 with the candidate enabled and 2,161 in default mode), 47 optimization
Python tests and the focused gates. Its two internal maps add 1,920 bytes; the
tracked PSRAM peak remains 6,642,090 with zero allocation failures. The limited
iteration 22 target comparison is below; broader qualification remains pending.
`NOSDAMAGE` reports actual painted band/fallback counts
over the same quiet capture window; a runtime switch alone is not proof of use.

Iteration 20's two hardware campaigns are diagnostic-only. The first completed
one default-path cell and stopped; its inherited read-only retry also violated
the intended strict capture protocol. A new helper continuously drained USB on
a separate thread and performed no retries. Its two default-path 290 px/s cells
reported maximum completion intervals of 27.594 and 26.904 ms, with no 30 fps
floor miss in those finite windows. The subsequent compact state reply lost
bytes inside the line, so that campaign also stopped before enabling the new
damage path. No candidate-on or embedded Apps comparison exists for iteration
20, and these windows do not establish a speedup.

The threaded reader received the already-truncated reply in about 2.3 ms; no
50 ms host flush stall occurred in that session. Thus the old synchronous
reader's possible 50 ms delay is insufficient to explain all console failures.
Raw bytes, exact helper versions, timing and limitations are retained in
`build/hardware-iteration20-analysis.json` and the device report. Final compact
cleanup confirmed HOME face 8, normal sleep, and all experimental controls off;
COM3 is closed and reserved. Fresh system heap counters were unavailable during
that cleanup. The latest complete preflight had zero recorded system allocation
failures; that statement is limited to its observation time.

Two follow-up prototypes remain outside production. An eight-word damage-map
scan preserves existing spans with no additional RAM and passes independent
coverage/adversarial checks. A Settings palette cache uses 4,560 target bytes to
interpolate 64 brightness levels while preserving the original 17 anchors and
pinning descriptors for each frame. Its isolated brightness-row maximum palette
energy step is about three times smaller; live/pressed fallback and pixel
position stepping remain. Neither prototype has target timing or power evidence.

Iteration 21 changes only console diagnostics: all renderer, asset, managed
component and configuration bytes match the iteration 20 archive. Link-time
wrappers preserve the SDK functions' arguments and return values while counting
false USB connection checks and failed transmit enqueues. A separate bounded
nonce/length/CRC reply uses the driver directly; its accepted-byte count means
queued data, not delivery. The new module uses 84 static internal bytes; its
target query function has a 1,008-byte stack frame within an 8,192-byte main
task stack. Runtime main-stack high-water is not exposed by this build.

The first finite console-only sequence completed without retries: two validated
direct replies bracketed state, navigation, asset state and one full info query.
The five intervening command receipts, queue acceptances and dispatches matched;
60 connection checks had no false result and 3,031 zero-wait writes had no
failure. This did not reproduce the intermittent corruption. The observed heap
snapshot recorded zero allocator failures. These are transport diagnostics,
not FPS, power, or damage-renderer qualification; per-character instrumentation
also adds overhead when console output is active.

A separate iteration 21 integrity run completed 64 alternating state/navigation
queries and one final info query, bracketed by valid direct diagnostics. All
66 intervening receipts, enqueues and dispatches matched; 303 connection checks
and 18,934 zero-wait writes recorded no failure. The watch stayed in normal
idle/AOD throughout. This does not test the active Settings condition that lost
bytes in iteration 20, and diagnostic instrumentation may itself alter transient
timing. No causal repair is established by these successful windows.

An active iteration 21 attempt subsequently lost a navigation reply before
entering Settings or collecting any timed cell. The separate direct diagnostic
remained valid: command receipt, successful enqueue and UI dispatch each advanced
by the expected ten commands, including the missing-reply command and the final
diagnostic. False connection checks increased by one; queue and driver enqueue
failure counts stayed zero. The local VFS rejects writes before enqueueing when
that predicate is false. This strongly implicates that gate, but the aggregate
counters do not identify one particular rejected write. Cleanup again confirmed
default controls, HOME face 8, normal sleep and a closed COM3. That attempt did
not activate the damage-band candidate in a timed hardware window.

Iteration 22 adds a default-off reply guard around synchronous command execution
on the UI task. It still records the real USB connection predicate; only a false
result inside that task's active command scope is overridden. Other tasks, ISR
calls and rendering outside command dispatch retain the original result. Driver
arguments, timeout and return values are unchanged. Direct CRC replies confirm
guard changes and keep raw false observations separate from override counts.
This does not repair the SDK connection monitor or guarantee host delivery.
The linked target confirms the intended call routing; the module uses 96 static
internal bytes and a 1,024-byte diagnostic stack frame. Focused host checks pass,
and renderer/configuration bytes remain identical to iteration 20. The exact
ELF, source archive and inherited pixel evidence are recorded under
`build/artifacts/iteration22-usb-reply-guard/`.

Its guarded app-slot flash passed exact image and unchanged outside-slot checks.
A bounded run then completed 64 alternating state/navigation queries while the
watch was awake and active, with complete identity and heap diagnostics and no
retries. All replies remained intact, but both raw false checks and overrides
stayed zero: the protective branch was not exercised. This is not proof that
the intermittent console failure is fixed. Ordinary cleanup confirmed default
controls, HOME face 8/page 0 and normal sleep; the last command independently
confirmed guard off by CRC. No renderer timing was collected in that run.

The subsequent iteration 22 Settings comparison completed all eight ABBA
windows (off/on/on/off, both directions at 290 px/s). Exact starting scroll
positions matched, and pipeline/damage counts matched each stopped capture.
All 289 candidate painted frames used the band path without fallback. Across
its four windows, deadline misses fell from 11/361 frames off to 2/366 on;
both modes had zero 30 fps floor misses (282 off and 285 on measured intervals).
Worst complete-frame work was 27.585 ms off and 25.982 ms on. Worst completion
interval was 28.241 ms off and 27.776 ms on. These bounded results still do not
qualify 40 fps, cold caches, other speeds or the 200 MHz budget.

Normalized Settings payload fell from 196,306 to 189,820 bytes per painted frame
(3.3%), and transfer chunks from 46.916 to 42.097 (10.3%). Positive-direction
payload increased about 1.5%; the host's 12--15% savings must not be substituted
for these device results. Actual poses/frame counts and cache preparation differ
with timing. All four observed false connection checks were overridden while
ordinary replies remained complete; queue/driver enqueue failures stayed zero.
This exercises the scoped guard in one completed campaign, not a guarantee of
future console delivery. Cleanup restored defaults and confirmed guard off.
The exact cell data, traffic and worst-frame pairs are pinned in
`build/hardware-iteration22-settings-cells.json`; the damage mode remains off by
default pending broader qualification.

The separate embedded Apps ABBA also completed all eight 290 px/s windows.
The band path handled all 284 candidate painted frames without fallback.
Deadline misses fell from 138/356 frames off to 0/364 on. Neither mode had a
30 fps floor miss (274 off and 280 on measured intervals). Maximum frame work
fell from 28.087 to 23.625 ms, and maximum completion interval from 28.964 to
26.863 ms. Payload per painted frame fell from 264,165 to 174,112 bytes (34.09%);
chunks rose from 55.227 to 55.479 (0.46%). Rotation fell from 5.170 to 3.481 ms
per painted frame, overlapping other stages rather than adding to them.

Embedded launcher rendering does not update the generic `BLUR list` counters:
they remained zero. Enabled controls and unchanged renderer source do not
replace per-frame blur coverage or optical measurements. Six further false
connection checks were overridden with intact replies; no allocator or queue/
enqueue failure was recorded. Cleanup confirmed default controls, HOME face 8,
page 0, normal sleep and guard off; COM3 is closed. Exact evidence is in
`build/hardware-iteration22-embedded-analysis.json`. Neither 290 px/s comparison
qualifies cold startup, fast swipes, all other screens, the 200 MHz budget or
power consumption.

Physical touch is a separate validation gap. On the active 143B board path,
failed I2C touch reads currently become `down=0`; a failure during contact can
therefore generate a release and restart the velocity estimator when reads
recover. Error 259 means an incomplete/invalid driver state, not specifically a
benign idle NACK, and its diagnostic field is sticky. Current counters do not
associate errors with finger state or time. Injected gesture captures bypass
this sampling path, so they cannot prove physical-touch continuity or establish
that these failures caused the user's flicker. No touch behavior was changed by
the console diagnostics.

A private actual-source replay also confirms that an error can clear the
wake-touch swallow latch, allowing a recovered held contact into the UI as a
new press. A safe correction needs contact validity separate from contact
state, genuine gesture cancellation, and quarantine until a valid lift. A
synthetic release is insufficient: some controls commit on release even with
zero velocity. The private policy model passes 781,250 event sequences, but
does not implement those app cancellation handlers or diagnose the I2C cause.
No production touch policy was changed. Details are in
`build/blur-investigation/touch-validity/REPORT.md`.

`nos/render_config.h` defines the presentation cadence and CPU budget. The
firmware, simulator, authored frame durations, and GIF timing use that cadence.

The renderer has three complementary caches:

| Storage | Purpose | Bound |
|---|---|---|
| Retained screen layers | Up to three RGB565 rasters allocated on demand; reused before growing, released for specialized screens/cards; ordered signatures invalidate 16-row bands | Up to 1,302,936 pixel bytes, plus recorder/metadata; zero at boot |
| Item/band cache | Sharp and directional-blurred RGB565 content across generic list owners; visible before predicted items; LRU eviction; tracked allocator admission and release | Demand-sized from a 1 MiB minimum to a 2,662,400-byte ceiling, including shared staging; 32 KiB metadata alone for specialized blur coverage |
| Sharp frame history | Minimal 75% current / 25% previous temporal exposure for cold caches and other animated content | 490,232 bytes |

Upstream Settings, notification, launcher, home-page, and Orrery caches retain
their specialized rendering and memory lifetimes. `SCR_NATIVE_CACHE` prevents
duplicate generic screen rasters and preserves ink-dim replay for those screens.
Directional blur paths claim only the pixels they actually draw so the temporal
fallback does not filter them twice. Generic layers use the same final dim on
cached and live paths. All frees use the tracked allocator's ownership API.

The history filter is **a two-sample temporal approximation**, not a velocity
reconstruction or a physically sampled shutter. It retains the preceding sharp
image, never its own blurred output. Previous damage is repainted once more to
erase the tail. Completed directional cache spans bypass that additional filter.
The first observed pixel seeds history. At rest, unchanged pixels remain exact.
Always-on display uses sharp rendering.

The IDF toolchain disables built-in `memcpy`, so the first implementation's
four-byte pixel copies became external calls inside the hot loop. The replacement
uses direct, naturally aligned RGB565 loads/stores, checks valid/nonoverlapping
coverage once per 32-pixel span, keeps counters local to that span, and bulk-seeds
new pixels. Already blurred spans have a separate update loop without exposure
arithmetic. Pixel-reference tests and Xtensa disassembly verify these changes;
iteration 5 measured a substantial improvement, but still missed deadlines.
Iteration 6 uses aligned word comparisons, keeps exposure arithmetic behind the
changed-pixel branch, and retains exact generic overlap behavior for arbitrary
callers. The compositor proves its final rectangles/stripes are disjoint and uses
a separate row-span path with 1,864 additional internal bytes for conservative
initialized intervals. Warm spans avoid PSRAM coverage bitmaps entirely; already
directionally filtered spans bulk-copy history without comparing pixels. A
separate `copied` counter records their intentionally unclassified changes.
Coverage span queries also reuse results until the next Y boundary or metadata
mutation, using 272 bytes of internal storage. Iteration 6 measured history
medians of roughly 6–13 ms in the repeated-list cases, down from 16–18 ms in
iteration 5. These measurements still do not establish the frame-time target.

Iteration 7 adds another 1,864-byte internal table recording fully initialized
32-pixel history blocks that are entirely black or white. A matching current
block can bypass history reads, writes and exposure without changing pixels.
Generic, partial and cold writes conservatively invalidate those tags. The
runtime `history_uniform 0|1` switch isolates this optimization while leaving
motion blur enabled. Telemetry separates logical history read bytes and write
bytes avoided; these are not measurements of physical cache/bus traffic.

Iteration 9 computes two rounded RGB565 exposures together by splitting each
channel into a quotient and two remainder bits. An independent channel oracle
checks 10,012,288 cases; aligned/unaligned span tests also compare sharp history
and every statistic. The Xtensa changed-pair loop falls from 55 to 33 instructions
and uses two aligned word stores. `history_packed 0|1` selects an exact legacy
control outside the pixel loop. Iteration 9's closest damage-matched Settings
window supports a modest gain, as summarized above; it does not meet the target.

The display worker can also apply exposure while the UI renders the next stripe.
After final drawing, the UI snapshots directional coverage into an immutable
960-byte bitmap per slot. Two complete job structures consume 2,064 internal
bytes on the target. The worker updates sharp history, applies exposure, rotates
pixels and then releases the raw stripe. DMA separately owns the rotated buffer.
An ordered exposure fence precedes statistics and the next history generation;
the existing final DMA fence still measures completed transfer. Synchronous
fallback also fences the preceding worker before touching history. No UI coverage
metadata is read from the worker. Touch priority remains 5; the longer pixel
worker runs at priority 4 so touch sampling can preempt it.

The simulator deliberately queues two jobs before consuming them. Worker and
`--sync-exposure` runs produce identical frame hashes across all 4,199 tour,
stress and Orrery reference frames with the same recorded quality decisions.
The independent delayed-job oracle also erases producer coverage before applying
the job and checks pixels, history and statistics. Runtime `exposure_worker 0|1`
allows hardware controls. `history_us` now sums UI snapshot preparation and
worker elapsed work stages, including preemption. It overlaps UI raster,
rotation and DMA, and must not be added to raster or end-to-end time. The report
retains `NOSWORKER`/`NOSF` controls and omits unsupported 200 MHz extrapolation
for asynchronous frames.

Synchronous exposure remains the firmware default after the hardware A/B.
At constant full damage, off-to-on median end-to-end/history changed from
24.568/10.692 ms to 27.892/13.981 ms; reverse-order worker-on and worker-off
medians were 28.257/14.540 and 27.732/12.812 ms. Trace and scene-phase variation
remain, but damage size is identical. Raw-stripe waiting includes worker
exposure, rotation and DMA backpressure; it is not a bus-only measurement.

Screen layers skip unchanged bands during repeated translated/dimmed rendering.
Lists with an active directional-cache request bypass screen-layer caching so
the already-prepared item rasters and their blur-coverage spans remain visible
to the compositor. Recorder hashes include ink dimming to prevent frozen fades.
Screen-wide backdrop fades scale the completed RGB565 image consistently on
cached and live paths; per-element ink dimming stays within the drawing callback.
This avoids different antialiasing/overlap results when cache availability changes.
Cold, untranslated full bands can also borrow the existing internal output
buffer for composition, then write the completed sharp band to PSRAM once.
Undimmed output already contains the final band and needs no return copy.
Translated, packed, partial-height and restricted visible clips retain the
original path. Only actual panel output permits scratch writes into invisible
round-screen corners. This uses no additional raster allocation; independent
clipping, AA/dim and reuse tests exercise both paths.

Item generation uses bounded eight-row slices and publishes only complete,
validated content. Each item has sharp, published blur and staging blur buffers.
The last completed blur remains visible while a new shutter level is prepared;
only a completed result swaps into publication. Missing visible publications
receive work first, then missing predicted bands, then refinements of already
usable blur levels. The six-slice / 1.5 ms work limits remain in place.

The default 1 MiB pool fits three 440 × 90 bands with 24 context rows on each
side (sharp context plus two 90-row filtered buffers). It cannot hold one
three-buffer 466 × 466 item, or two complete five-to-six-band viewports.
Specialized home-page caches handle full-page blur; generic cache misses use
the native draw and temporal fallback. Prediction is prioritized
preparation rather than a promise that every incoming band is warm. Mixed list
rendering retains each ready band's directional blur while only cold bands use
the exact native body clipped to their pixels; headers and scrollbars stay live
above the bands. Cold regions receive temporal fallback without forcing warm
neighbors into a whole-list fallback. Live titles, notification counts and
scrollbar chrome, banners and toasts revoke directional-coverage claims in their clipped bounding
boxes so moving foreground pixels still receive temporal exposure. Background
pixels inside these small boxes can conservatively receive both filters.
RGB565 accents and drag/momentum motion remain supported. Partial or stale
snapshots never pretend to be ready. Idle caches
retain their last useful blur preset and do no recurring signature/filter work
after warming. Legacy draw callbacks still require validation before reuse;
explicit immutable content revisions are a further optimization opportunity.
Filtering uses shifts for the 2/8/16-pixel shutters and constant-divisor arithmetic
for the 40-pixel shutter instead of three variable divisions per output pixel.

Full-frame transfers now omit the invisible corners of the round display.
At 16 rows per stripe, **179,188 rather than 217,156 pixels** are sent: **17.5%
less payload**. Bounds round outward to even panel windows and retain every
visible pixel and the antialiasing fringe. The display clock remains 40 MHz;
the earlier 80 MHz trial failed the owner's visual acceptance.

Other changes restrict lap iteration to visible content and reject heart-trace
groups outside the stripe before reading all their dots. The four internal
raster/DMA buffers retain their existing
59,648-byte allocation. Concurrent dot-mask creation now frees unpublished
allocation losers, including the initial mask table, while keeping published
masks immutable and alive.

Launcher entry no longer synchronously bakes every icon/name/blur variant.
Home and launcher ticks advance jobs in eight-row slices, prioritizing visible
and predicted rows. The defaults allow 750 microseconds and at most 16 steps
per call; an individual slice or allocation remains indivisible. Sharp assets
and complete blur sets publish atomically, and allocation failures park the item
instead of retrying every frame. The current 18-app cache uses 1,275,529 bytes,
excluding shared dot masks. Independent tests compare every generated asset
against the original full baker, including cropping and failure paths.

Settings also replaces its synchronous 36-entry open bake with a resumable job.
Raster, grayscale conversion, directional filtering, cropping and publication
advance in four-row slices with a 750-microsecond/16-step cooperative limit.
Visible and predicted rows have priority. All four cropped images publish
together after signature validation; the independent original baker matches
every byte of all 144 images. Cold rows keep their native rendering and temporal
exposure while warming; warm fast scrolling retains directional blur. Allocations
and individual slices remain indivisible, so device measurement is still needed
to establish the maximum work of each call.

Cache admission preserves 512 KiB for the next owner and accounts for tracked
allocation headers, the physical PSRAM reserve and the largest contiguous block.
Expected admission skips do not call the allocator. Actual allocation failures
remain visible in whole-heap telemetry. Generic layer/pixel caches are released
when a specialized screen settles. An unlocked foreground Orrery releases
inactive Settings/notification caches and suspends list prewarming; locking it
restores prewarming. Card preparation also drops distant launcher assets and
optional orbit scratch/ink while retaining published orbit coverage and scene
commands. No allocator callback evicts data while a renderer may be using it.

Orrery cards cache invariant label widths and row bounds during each layout
search, and reject candidates whose distance cannot beat the current winner.
An optional 1,896-byte, lifecycle-owned PSRAM table caches the original float
division results for the main and preview globe coordinates. Allocation denial
keeps the exact scalar path. Independent comparisons cover 99 complete body
states, 33 LUTs, 396 globe images (including texture, night and ring paths),
66 previews and every coordinate bit for radii 1–118. These validate output,
not target execution time.

Iteration 9 additionally constructs the correctly rounded `z / 32767` float
from integer significand arithmetic for globe LUT values. All 65,536 possible
inputs and the independent globe-image comparisons match the original. This
removes a software floating-point division without a new lookup allocation.
`card_qz 0|1` dispatches once per globe; `card_qz_check` checks all input bits on
the actual target. Its hardware speed remains subject to the measured controls.

Iteration 10 adds card-local raster guards before small-text width/layout,
hero layout, wrapped notes and fact-row geometry/formatting when their rows miss
the current stripe. Recorder commands remain unchanged, so damage signatures and
blur coverage retain their existing behavior. Independent tests compare 131,208
original/candidate page images, including translated/clipped/dimmed stripes and
canaries, plus all 44 recorder streams. No new persistent raster cache is added.
Optional `card_profile 1` / `card_profile report` diagnostics measure allocation,
lighting-table setup, body preparation, extra geometry, file open/read/close,
LUT, ring, globe and preview stages; profiling is disabled by default. These
measurements separate synchronous cold allocation, 512 `powf` evaluations and
Saturn ring construction from ordinary tick cost. Iteration 11 reports background
file I/O separately as `CARDASSET asynchronous=1`; those durations are not added
to UI stage or frame maxima. A profiler epoch rejects completions from jobs
submitted before a reset.

Iteration 11 also stores the original two lighting-coefficient expressions in
an optional 2,048-byte tracked PSRAM table, admitted with 16 KiB headroom. The
original 512-byte lighting table and pixel arithmetic remain unchanged. The
globe dispatch chooses cached or original arithmetic once per render; a missing
table falls back to the original implementation. Independent original render
fixtures pass 2,376 globe comparisons with cached, disabled and absent tables
and both QZ paths. `card_light_check` compares all 512 coefficient words on the
target; `card_light 0|1` supports same-image timing controls. Target performance
and exact target rounding require verification in the iteration-11 run.

Texture reads now use one private background job. Successful submission moves
unpublished destination pages out of the card; only a completed matching
generation can return them to the renderer. Exit, body changes and reentry
cancel without waiting on SD. Late completions free their own pages. The UI
polls completions while awake, in AOD and while off; explicit shutdown drains
the worker after the panel is off. No UI drawing callback performs file I/O.
The worker's 6,144-byte retained PSRAM stack is charged in both firmware and
simulator, and admission also reserves internal memory for its task control
block. A single read is limited to 32 KiB, which bounds bytes, not elapsed time.

Each open worker stream holds an SD mount lease until close. USB/BLE transfer
commands hold the same lease across their session without holding the SD mutex
while waiting on transport. Manual unmount refuses an active lease, shutdown
stops admitting new leases, and the periodic logger uses a try-lock and defers
while a lease exists. Portable mailbox tests include 200 real-thread
cancellation races; card tests use delayed completion and verify exit/reentry,
Earth night maps, Saturn rings, exact completed bytes and shutdown ownership.
The worker's target stack/error paths and contention still need device evidence.
`asset_state` reports ownership, task stack minimum and admission/create failures.

The immutable background shade cache now retains the original 512 quantized
lighting bytes alongside coefficients and Q16 multipliers (4,608 payload bytes).
A ready cache supplies a 512-byte copy during card entry instead of repeating
512 `powf` calls. Pending/rejected cache states retain the original fallback;
the target lighting diagnostic independently recomputes the original expressions.

The cooperative card pipeline uses the existing body and neighbour buffers as
exclusive staging storage. A complete image remains visible until the next
snapshot is ready; pose, mode, texture generation and pixel revision publish
together. Preview preparation cancels the main job before borrowing its storage.
Late texture or minute updates retain their dirty state across publication and
cancellation. There is no additional full-image PSRAM allocation. Preparation
adds 656 bytes of target metadata; its individual ephemeris calls remain
indivisible and require target measurements.

Preparation, geometry, shading, clearing, ring overlays, halftoning and previews
share a default 3 ms per-frame work budget, checked between small units of work.
The simulator uses a deterministic unit count instead of wall-clock scheduling.
Moving globes use a 2x2 sampling lattice by default, with a 4x4 diagnostic option;
after 125 ms of unchanged pose, full-detail restoration is scheduled. Restoration
latency includes the remaining rendering work and must be measured separately.
Exact geometry mirrors symmetric LUT/ring entries without adding a pixel cache.
`card_geometry_check` checks these entries against original expressions on the
target, only while the exact geometry is complete and idle.

A short-lived reprojection of the complete front image supplies intermediate
poses within 5 degrees of spin, 3 degrees of tilt and 200 ms of continuous pose
error. Its lighting and ring occlusion remain those of the source image. Outside
those limits the front is held and counted as stale; this fallback does not meet
the smooth-motion requirement. `card_state` reports preparation, publications,
restoration, source age and suppressed motion. Painted frame counts or changing
proxy keys alone cannot prove fresh, correct globe images at the target rate.

Iteration 10's baked asteroid basis (`orrery_basis_target.inc`, `orrery_basis.h`, the `orrery_basis_*` checks and
`tools/test_orrery_basis.py`) was removed: `belt_init` now derives the basis at runtime from `sky/belt.bin`,
500 rows per tick, through `belt_basis_calculate()`, and publishes it whole (see ORRERY.md). The belt
bit-for-bit comparison against the original inline formula now lives in `tests/test_face_contracts.py` (`belt`).

This gate removes basis initialization math from normal entry only after
successful background verification. The first dated propagation of all 3,999
bodies, ephemerides, projection and first scene caches remain cold. The initial
epoch includes milliseconds; preparing an earlier or integer-second epoch is
not bit-exact. Incremental dated prewarm needs the precise future presentation
epoch and atomic publication of complete results, or must retain the original
full-solve fallback. No dated prewarm or time-semantics change is implemented.

Dirty vertical ranges align outward to the common stripe grid before raster and
history work. This fixes native vertical-blur edge differences between a partial
repaint and a full repaint; the extra rows count toward actual transport/work.
Pixel-reference runs record adaptive iris/H/V choices and replay those choices
for the full repaint, isolating pixels from the different work estimates. Missing
decisions, differing frame IDs/counts and failed child runs fail the comparison.
Normal simulator timing assertions remain active; reference runs explicitly
exclude only work-dependent assertions and do not certify frame timing.

## Scheduling and power

An ordered flush-queue fence now waits for the final submitted DMA transfer.
The former semaphore-only drain could finish before the last transfer was
submitted. Timing records include frame IDs, start and final completion times,
host iteration cost, target misses, floor violations and cache allocation errors.

`bench begin` / `bench end` collect completion metrics without periodic performance
logging. The arm/acknowledgement frame and the final report frame are excluded;
power changes or enabling per-frame tracing abort the capture. Gesture replies
and ordinary peripheral logging can still perturb it. Idle frames break painted
interval continuity, so sparse captures and zero floor misses do not establish
animation FPS. Content freshness is a separate mandatory card check.

An overrun starts the next complete frame immediately and records the missed
target; it does not wait for a second 25 ms slot or silently count the overrun
as a passing frame. Preference and pedometer persistence is deferred while the
display is awake; failed preference saves remain pending. Idle history tracking
only clears coverage rows that are actually rendered.

Conservative runtime DFS is enabled in the source: 240 MHz maximum, 80 MHz
minimum, automatic light sleep disabled. The UI owns a CPU-frequency lock from
before input/wake/housekeeping through raster, core-1 rotation, the ordered DMA
fence and post-paint work. Every OFF/AOD/pacing wait releases it. Boot starts
protected; the background dot prewarmer uses its own lock handle. Initialization
first establishes fixed 240 MHz, and failures restore that fixed configuration
and report `NOSPM` diagnostics rather than silently rendering at a lower speed.

RTOS/radio activity may retain the maximum frequency even after the UI releases
its lock. Lower-frequency residency and board-energy savings require device
measurement; no battery-life improvement is established by this configuration.
See [device details](OPTIMIZATIONS_DEVICE.md) for validation and flash-operation
limitations.

## Reproduce checks

```powershell
.\tools\test_optimizations.ps1
.\sim\nos_build.ps1 -Strict -Out nos_optimized.exe
.\build\nos_optimized.exe --damagecheck tools/nos_tour.txt --out build/optimized-damage --no-media
.\build\nos_optimized.exe --damagecheck tools/nos_optimization_stress.txt --out build/optimization-stress --no-media
```

The rebased renderer passes the 2,097-frame tour, 895-frame stress, 1,207-frame
Orrery face and 1,263-frame card/return comparisons against full repaint with
recorded adaptive decisions. The independent Orrery raster oracle also passes.
The 7,014-frame memory-only stress run reports zero allocation failures, all
five cards fully allocated/released, and a 6,602,818-byte payload peak against
the 6,815,744-byte charged budget (headers reduce the remaining headroom).
Another 42 fault/fragmentation/notification cases pass, and the interactive-face
lifecycle test verifies list-cache release and automatic recovery. The explicit
memory-only runner lists the 14 legacy model `nolate` assertions it excludes;
the original timing suite still fails and is not reported as passing.
After incremental Settings baking, the default memory runner also retains the
old strict directional-only gate and reports 36/75 fast frames. Its explicit
`--temporal-fallback` mode audits each of those 75 frames for compositor history
processing and actual temporal blends. The warm Settings test independently
requires 100% directional coverage; a separate immediate cold fling audits 47
fast frames. This does not turn the old timing/directional-only suite into a pass.

Tests exercise RGB565 filtering, cache eviction,
allocation failure, complete-snapshot publication, history tail retirement,
translation/clipping, visible geometry, and every round-glass stripe height.
Rapid shutter changes are checked for uninterrupted published blur and unchanged
pixels while replacement work is incomplete. Deterministic race injection
checks foreground/background mask publication and ownership.
The regression runner also checks eight power-lock lifecycle/failure scenarios,
history allocation-failure reporting, cached/live fade equivalence, and the
MD5-verified flash-backup packet protocol.

`tools/test_sim_script_io.py --sim build/nos_optimized.exe` rejects missing
top-level scripts, missing includes and excessive include depth. Previously a
missing script could yield two identical boot frames and a false pixel-check
pass. The rejected iteration-10 `tools/nos_stress.txt` invocation is not evidence;
the actual `tools/nos_optimization_stress.txt` workload passed all 895 frames.

The original aggregate `tools/test.ps1` also encounters a Pip eye damage-bounds
assertion at `tests/pip_eye_shape_test.c:117`. The same assertion was reproduced
in the untouched captured baseline. Pip is not part of the NOS firmware build.
Do not describe that aggregate suite as passing.

Simulator `frames.csv` exposes PC-stage timings, history work, reused bands,
cache memory and allocation errors. Its primitive model scales known CPU work
to 200 MHz but keeps transport cost unchanged. **The model does not calibrate
new history/signature/cache kernels and cannot certify no dropped frames.**
`tools/nos_perf_report.py` handles actual device captures and rejects missing
provenance, incomplete coverage and allocation failures as acceptance evidence.

Fresh device measurements must include cold and warm lists, immediate navigation,
reversals, all faces and transitions, changing notifications and sensor values,
radio traffic, wake, and long runs that expose memory pressure. Report target
misses and 30 fps floor violations separately. A finite passing run is evidence
for that workload, not a proof that a frame can never be dropped.

## Shared vertical blur and navigation correction (iteration 23)

The reported regression is inside scrolling/animating apps generally. The fix
therefore changes shared rendering paths, with Activity, Heart Rate, Weather,
About and Modules included in the host vertical-gesture comparison.

Physical HOME order is now **Activity → Heart → Weather → Music → Timer →
Watch Face → Apps**. Leftward finger travel from the face opens Apps; rightward
travel reaches Timer. Logical page IDs remain face 0, Apps 1 and widgets 2–6;
widgets occupy signed physical slots −5 through −1. Button, AOD, list-position
restoration, widget taps and unlocked Orrery input ownership remain tested.

Two motion APIs had retained frame-relative thresholds while cadence changed
from 30 to 40 Hz. Live pager exposure now divides actual displacement by elapsed
time and normalizes to the authored 1/30-second exposure. Cached-list APIs
convert their already cadence-normalized input back to pixels per second.
The notification cold fallback also converts to the live API's exposure units.
This restores physical thresholds rather than changing them with frame rate:

| Path | Nominal thresholds in px/s | Effect |
|---|---|---|
| Live vertical pages | 240 onset | Full-column spatial EMA; budget admission required |
| Specialized Settings/Apps cache | 360 / 840 / 1,680 | 16 / 32 / 48 px cached filters |
| General retained item cache | 15 / 150 / 720 / 1,800 | 2 / 8 / 16 / 40 px filters |
| Early notification ladder | 150 / 300 / 960 / 1,920 | Existing notification levels |

Rounded content positions can still place individual frames on either side of
a threshold. This change does not establish continuous shutter width or remove
all brightness quantization. The optional continuous Settings shutter remains
disabled after its recorded timing failures.

The generic layer cache now uses live rendering when at least half the recorded
rows change. An admitted streaming blur command also forces live rendering,
including on a cold layer, preserving filter continuation and actual directional
coverage. Static and sparse screens retain their cache. A settled cache rebuild
can still add work; this is not a guarantee against every end-of-motion spike.

The old live reduced vertical kernel sampled every fourth row and pooled four
columns. A forced preview showed missing small text and visible blocks. Both
live vertical modes now use one full-column, every-row fixed-point EMA, with
per-column continuation across changing round-panel clips. Primary/fallback
mode changes produce identical pixels. This adds 2,768 bytes to the existing
PSRAM blur workspace, with no full-frame history allocation. Cached HOME page
blur is unchanged. The spatial tail follows increasing raster Y; it is not
motion compensated or a seven-frame temporal shutter.

Admission keeps the 26 ms CPU budget and conservative full-region accounting.
A smaller base is trusted only after the immediately preceding large live
repaint of the same screen; partial/cached/transition contexts invalidate it.
The new kernel's provisional 150/100 ns per pixel estimates deny the controlled
full-screen app requests. The isolated iteration 23a target benchmark measured
155–170 ns per pixel on average across black, sparse, white and RGB patterns;
its slowest pass was 37.231 ms for 217,156 pixels (171.448 ns per pixel) at
240 MHz. Both provisional estimates were below that worst case. This kernel
cannot be enabled indiscriminately within the frame budget. All 16 target
checksums matched the archived host implementation and an independent scalar
RGB recurrence. This measures filtering alone, excluding drawing and display
transfer; forced preview output is not evidence of production admission or FPS.

Iteration 24 specializes the four filter lengths outside the pixel loop and
keeps stripe geometry out of its register set. The target disassembly reduces
the steady loop from approximately 35 to 21 instructions and removes its three
per-pixel stack loads. It adds 318 bytes of flash and no RAM. The 497-frame
forced-filter output is byte-identical to iteration 23a, including damage/full
repaint equivalence. Its completed target benchmark measures 104–108 ns per
pixel on average, 31–39% faster than iteration 23a across matched cells. The
slowest full rectangular pass is 24.166 ms (111.284 ns per pixel), still before
drawing or display transfer. All target checksums match both host references.
Artifact: `iteration24-vertical-span-kernel`, ELF
`95ecd2a6d3dd901bdb0bfa332b17f02d52d068c583425fe3f26d8def9e6f2d95`.
On-watch navigation also passes all 29 fresh state checks: both directions,
outer edges, buttons, and embedded Apps vertical gestures. No reset was used
for that navigation capture. The target's touch transport error counter still
grows, so injected-gesture results do not certify physical touch continuity.

Validation before the device benchmark:

- 16,419 integrated damage/full-repaint frames match, including 5,121 frames
  across five vertical apps and four requested speed bands. These pixel runs
  explicitly exclude idle/maxdirty/nolate work assertions.
- An additional 497-frame Activity/Heart comparison forces the new filter
  active using an explicit quality tape. All damage/full outputs match; this
  proves compositor integration, not affordability.
- Independent scalar RGB tests cover all four tail lengths, stripes 1–32,
  changing clips and one-pixel strokes. Primary/fallback mode-switch pixels and
  actual raster counters match. Cached output at 150/290/900/1,800 px/s is
  identical at 30/40/60 Hz, including held policy and skipped ticks.
- The optimization runner passes 53 NOS Python tests plus its native, power,
  backup, asset and lifecycle groups. Memory/lifecycle testing reports a
  6,644,858-byte tracked peak against 6,815,744 with zero allocation failures.
  Its 14 excluded legacy timing assertions and seven independent motion-shape
  failures remain explicit. The first memory run failed stale HOME assertions;
  the corrected navigation run passes without changing memory assertions.

The first target build caught four `int32_t` printf argument mismatches; explicit
casts fix them. The successful 23a archive is
`build/artifacts/iteration23a-vertical-blur-diagnostics`, source tree
`a656e8b793f7273ad02210039dec373587708a85eaca6214599a6af8876bd512`, ELF
`9f770cfb71772873c679d8504ce39f732b0885290048955385bed3bcb876e477`.
The failed 23 source snapshot remains preserved. Exact quiet-window diagnostic
fields distinguish admitted filters from actually processed raster area and
actual temporal blends. Processed area can include overlap/offscreen pixels;
it is not unique visible or optical blur coverage.

Iteration 25 removes redundant continuation checks from all but the first row
of each vertical-filter stripe. It publishes per-column row tags once at the
end, retaining exact behavior for interleaved damage rectangles and changing
clips. Two thousand randomized calls verify pixels, fractional sums and tags
after every call; there is no API or workspace-size change.
The isolated target benchmark measures 89.723–95.672 ns per pixel on average;
its slowest full rectangular pass is 21.529 ms (99.141 ns per pixel) at 240 MHz.
All 16 checksum pairs match iteration 24 and both host references. This remains
a filter-only measurement, not an app-frame result.

The same candidate adds optional cadence-aware temporal weighting. Normal
40 Hz motion uses 21/32 current plus 11/32 previous, instead of 3/4 plus 1/4.
The mean sample age becomes 8.594 ms, near the authored 30 Hz value of 8.333 ms.
Weight is latched once per renderer generation and follows the quantized motion
interval; skipped logical ticks reduce the previous-image weight. A long stall
is still capped at four logical ticks, so this is not wall-time shutter control.
Changing the weight does not recover the exact 30 Hz two-sample blur shape.
Directional coverage still avoids an additional temporal blend; on a coverage
transition the previous pre-temporal image may itself contain spatial blur.

The `history_cadence 0|1` diagnostic selects an A/B mode outside active captures.
Its reply reports the selected mode and the last begun frame's interval/weight;
a fresh `info` after the following frame confirms the applied weight. Mode 0
and weight 8 retain the original optimized 75/25 path. No history allocation is
added. Iteration 25 defaulted to 1 for measurement. Its target sentinel rejected
that policy; runtime was restored to 0, and iteration 26 restores source default
0 as well. The optional mode remains available for controlled experiments.

The Activity sentinel measured roughly 49–55 ms worst complete-frame work even
with mode 0. All four valid mode-1 windows exceeded the 30 fps frame and
presentation limits. Every window executed zero live vertical-filter frames;
budget admission skipped the filter. The eighth window failed its required
return-to-page-0 endpoint, so the eight-cell run is incomplete and was not
retried. Raw results are retained in
`build/hardware-iteration25-activity-sentinel-windows.json` and its companion
analysis. A separately authorized cleanup restored HOME, mode 0, normal AOD
and autosleep, and closed COM3.

Quiet worst-pair measurements include 11–13 ms of history work inside
46–50 ms of raster work. These overlapping stages must not be added together.
The existing `queuewait_sum_us` measures the flush worker waiting for the UI
producer, not producer backpressure. Its 24–26 ms wait alongside sub-millisecond
UI buffer and DMA waits identifies slow production in those frames. It does
not establish the cost of each drawing primitive.

Current iteration 25 host checks pass all 16,419 damage/full-repaint frames,
memory/lifecycle, script-I/O and the optimization runner. Five dedicated history
test methods include independent scalar arithmetic, cadence/skipped ticks,
generation latching, mode switches, coverage changes, tail retirement and
aligned/unaligned/generic/disjoint equivalence. Forced-filter 497-frame mode-0
output is byte-identical to iteration 24; mode-1 damage/full outputs also match.
The memory-only runner continues to exclude 14 timing assertions and reports
the same seven independent motion-shape failures. These remain failures, not
performance passes. Artifact: `iteration25-cadence-exposure`, ELF
`c6bea2f085f18874bf73fc8320d0b34121fba7c4f6f14e74df2ede1d2a1e8d57`.

The supplied GPU shader averages seven equal-weight textures; `image_7` and
`elapsed_time` are unused. If the host supplies the current plus six previous
frames, its history spans 150 ms at 40 Hz and 200 ms at 30 Hz, with mean sample
ages of 75/100 ms. The watch's 75/25 current/previous fallback has a mean age of
6.25 ms at 40 Hz versus 8.333 ms at 30 Hz and stores one prior source image.
The fixed weight therefore shortened its temporal centroid by 25% when cadence
increased. The tested cadence-dependent weighting failed timing; matching the
centroid alone would not make the two-sample exposure shapes identical.
Neither implementation adds
motion compensation by itself. Six extra RGB565 histories require about
2.49 MiB before metadata, far beyond the measured remaining cache budget.
The shader's `Linear` sampler specifies texture interpolation, not explicit
linear-light exposure; see Microsoft's
[D3D11 filter definition](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_filter).

## Exact sharp-renderer candidate (iteration 26)

Progress rings now retain immutable layout metadata in two optional PSRAM slots
(2,088 bytes total, admitted only with a 512 KiB tracked reserve). Warm draws
reuse original sine/cosine values and conservative extents, removing repeated
software division and trigonometry from every stripe. Live fill, colors and
translated multiply-add expressions remain unchanged. Early stripe/column
culling skips invisible polygons before edge setup. Disabled, oversized and
allocation-denied cases retain the original path. `prog_geometry 0|1` provides
a controlled comparison; `NOSPROG` reports the mode and allocated bytes.

Convex polygon rasterization retains exact edge coverage at both ends of each
row and fills the proven solid interval directly. It preserves the original
floating expression, antialiasing, damage records and cost counters, with no
new buffer. Host property tests compare 30,000 polygon cases and 14,448 ring
cases against the original implementation, including FMA builds, clipping,
degenerate edges, packed buffers and changing fill. Seven geometry tests pass;
this is pixel evidence, not target timing.

History uniform-color scans now reduce bounded groups of words without the
target compiler's former six stack accesses, and omit empty known-span calls.
Six history tests pass, including 457,108 scalar scan cases. The history change
adds 179 bytes of code/literals and no RAM. The polygon interval change adds
249 bytes of code/literals and no buffers relative to the geometry-only build.

Quiet pipeline reports also retain record, damage comparison, layer work and
producer queue-submit durations for each worst presentation pair. Existing
worker receive-queue wait is explicitly identified as worker idle time. These
measurements are intended to distinguish rendering costs from transport waits.
The target geometry comparison is incomplete after six of eight windows. The
four cache-enabled Activity windows confirmed 2,088 allocated metadata bytes
throughout and passed their exact page endpoints. Their p95 work was 42–46 ms
versus 49 ms in the two preceding cache-disabled windows. Worst presentation
intervals still reached 55.406 ms; all six windows failed the 30 fps floor and
executed zero live vertical-filter frames. This supports investigating the
remaining drawing costs, not accepting the frame-rate requirement or claiming
a completed ABBA. Before the seventh capture was written, an unterminated
background weather log triggered the strict host transport guard. No capture or
gesture was replayed. Raw evidence is retained in
`build/hardware-iteration26-geometry-sentinel-windows.json` and its companion
analysis. Separate bounded cleanup restored HOME, geometry on, history mode 0,
normal AOD/autosleep and disabled diagnostic overrides; COM3 is closed.
Firmware, simulator and USB link checks pass. Host checks pass 16,419
damage/full-repaint frames, memory/lifecycle, script I/O and 59 NOS Python test
methods plus the existing native/power/asset groups. Six additional 497-frame
paths (geometry off/on, both history modes, damage/full) match the archived
iteration 25 pixels exactly under the same forced-quality decisions. The memory
runner still reports seven motion-shape failures and excludes 14 timing gates.
Artifact: `iteration26-sharp-raster`, tree
`09ac56be27236f92066189048df2d825a32d098cd6b6b0a2b345087504af9557`, ELF
`3fd451d871e77abfab46ea597bf10a83f9af486f9266b8ab862610e29ec12faf`.

A separate iteration 26b host run used the same firmware, fixed geometry on and
history mode 0. All four Activity and four Heart Rate windows passed their
endpoint/identity checks; both apps still failed the floor and executed no live
vertical-filter frames. Activity maxima were 42.524–44.715 ms work and
44.594–56.761 ms presentation; Heart maxima were 35.978–40.171 ms work and
41.994–45.471 ms presentation. Both 290 px/s Weather windows were valid and
also missed the floor. The 900 px/s Weather forward gesture reached page 2,
while the host incorrectly required page 1 as for a snap pager. That window's
timing is retained as invalid-pose evidence; the reverse window was not run.
Weather uses momentum scrolling, so a future endpoint contract must reflect
its actual policy. This is a 10-valid-window incomplete run, not a complete
12-window qualification. The raw/analysis prefix is
`build/hardware-iteration26b-candidate-apps12`.

## Shared text and exact temporal arithmetic (iteration 27)

Both normal-font lookups first try a verified direct index for contiguous
characters. Sparse fonts and duplicate character entries retain the original
binary search, including its selected duplicate entry. Tests compare 2,771,968
lookup cases across the baked fonts and synthetic tables. Dot text computes
its stripe row bounds once per string rather than twice per glyph; icons use
their own unchanged origin. Glyph-specific row and ghost clamps, floating
operation order, antialiasing and recording remain unchanged. These changes
add 270 bytes of code/literals without persistent storage or new allocation.

The optional Q5 temporal blend now packs the six channels of a pixel pair into
two independent groups with sufficient guard bits for rounding. It retains
exact results for weights 0–32 while reducing the target dense loop from 61 to
44 instructions per pair and eliminating its two stack accesses. The equal-pair
loop falls from 10 to 9 instructions. Independent scalar tests cover 4,922,016
pairs; all seven history tests pass. Code/literals shrink by 58 bytes and RAM
does not grow. This is not a measured cycle reduction, and the temporal policy
still defaults to mode 0.

Firmware/simulator/link checks pass, as do all 16,419 damage/full reference
frames, memory/lifecycle/I/O checks and six 497-frame forced-filter comparisons
against iteration 25. The same seven motion-shape failures and 14 excluded
timing assertions remain explicit. Artifact: `iteration27-shared-text-history`,
tree `9a78530ab66920c3919d26f9f7b8f63885906cb54b5cd906c1026dbd9c6edbc1`,
ELF `ff940e86ad637512177aae8b94ca376cfd9b5607fdb1cafa532c7ed8f9192660`.
The on-watch worker experiment completed all eight Activity 290 px/s ABBA
windows with valid endpoints and matching firmware/configuration. Worker 0
reached 48.440 ms maximum work and 55.320 ms maximum presentation gap, with
172 floor misses over 228 intervals. Worker 1 reduced those maxima to 38.374
and 49.989 ms, but still had 141 floor misses over 252 intervals. Every
candidate window failed the 30 FPS floor; live vertical filtering executed in
neither mode. History blending ran, while per-job asynchronous execution and
fencing are inferred from code and the queued-job oracle rather than measured
with independent counters. Worker 0 was restored and COM3 closed.

The worst worker-1 presentation gap follows a cheap 11.317 ms frame with a
36.306 ms frame, separated by a 25.000 ms start interval:
25.000 + 36.306 - 11.317 = 49.989 ms. This exposes varying completion times
under the existing start cadence, not a separate 50 ms pacing delay. Evidence:
`build/hardware-iteration27-worker-sentinel-analysis.json`.

## General app cache selection and page setup (iteration 28)

Seven general-app callers previously used the specialized three-image ladder
to decide whether to draw the generic cache. At 150–290 px/s the generic cache
selected its 8-pixel shutter, but the callers waited until 360 px/s before
displaying cached blur. Weather, Modules, its play list, About, Diagnostics,
Alarms and Stopwatch now capture `ui_fblur_active` immediately after their
own cache tick. This describes the requested shutter; individual cold items
still draw live. Weather swaps and entry builds, pressed alarm rows and
Stopwatch insertion/build animations explicitly remain live. Existing About,
Modules and Diagnostics entry/press guards remain in place. The independent
cache tests cover disabled/held policy and RGB565 output at 30/40/60 Hz; a new
caller test executes all seven actual tick functions against controlled cache
selection and checks the authored live states.

The first generic arena allocation now requests the block-rounded storage for
the current visible items plus two incoming items. It retains the existing
1 MiB minimum, 2.6 MiB ceiling and 512 KiB admission reserve. Existing arenas
are never resized during a gesture, preserving published blur. An isolated
Weather simulation increased from three published bands in 1,048,576 bytes
to eight in 2,162,688 bytes; its peak grew from 5,155,087 to 6,269,199 bytes,
with zero allocation failures, and returning HOME reclaimed the payload.
This does not guarantee a complete viewport on the watch: the observed
iteration-26b memory state permits only four bands with the existing reserve.
Retained allocations from a smaller previous owner also remain small until
the normal release point. Capacity shortfalls remain explicit.

Activity derives formatted values and chart columns lazily once per frame,
invalidating on live data, units and animation updates. This removes repeated
formatting and software division across recording and raster passes without
changing coordinate-dependent floating-point order. Activity and Heart Rate
also skip a page's setup when its conservative vertical extent misses the
current raster strip. Recording, motion blur and scrollbars keep their
original order. Native/FMA reference checks cover 20,000 metadata states and
6,400 page pixel/record cases, including translated and packed clips. The
16-row call-count model falls from 60 page bodies to 31–32 between pages;
this is a setup count, not measured target timing. Together these app changes
add 366 bytes of code/literals and 120 bytes of static state, with no new
allocation. Arena sizing adds 152 bytes of code/literals and no static state.

The source is frozen as `iteration28-scroll-cache`, tree
`c335c20d2b97bbc4ae26b5d302cf12e538363aa76d0cb430235e42693763d190`.
Firmware and simulator builds, 70 NOS unit-test methods and the existing
native/power/asset gates pass. All 16,419 damage/full reference frames,
memory/lifecycle/I/O gates and six 497-frame Activity/Heart forced-filter
comparisons also pass. The known seven motion-shape failures and 14 excluded
timing assertions remain explicit. The artifact is withheld from flashing:
review found that Modules reused its draw flag to defer sensor sampling, and
Weather/About bake callbacks suppressed authored animations that the new
slow-motion gate would expose. Revision 28a separates sampling policy and
preserves those live animation phases before target validation. Worker/history
policy remains at mode 0. The 30 FPS floor and power requirement are not yet
met or qualified.

### Revision 28a: shared staging and live animation correctness

Revision 28a is frozen as tree
`acc5ea791bdaecb7423ac8e1d7e6584dbd64a49db578da95ea9630a8ce6180eb`.
It retains the seven caller corrections and Activity/Heart setup changes.
Weather and About now capture their current icon, glint and value-morph phases.
Modules has a separate sampling hold: the earlier blur draw threshold does not
postpone slow-scroll sensor updates. Pixel and signature tests distinguish
changing bands from unchanged neighbors, including native/FMA paths.

The generic cache now keeps sharp pixels and a completed blur per item, with
one shared temporary buffer. Publication swaps buffer and allocation ownership;
it does not copy an entire image. Seven 418-by-90 Weather bands with their
24-row context and cache header need 1,486,848 bytes instead of 1,896,448.
The observed watch memory state models five resident bands instead of four;
this is capacity evidence, not proof of full visible blur coverage. The arena
still preserves its reserve, ceiling and existing allocations during a gesture.
No HOME pre-render storage or generic layer is forcibly evicted.

Preserving live animations exposed a scheduling defect: a changing 138-row
capture exhausted exactly three six-slice ticks, failed its signature check,
then restarted ahead of later static items. An actual Weather replay left a
later static band unallocated after 120 ticks. Rejected captures now yield
until their signature remains unchanged for at least their minimum capture
duration. The same rule handles changes during filtering. Explicit app
invalidation can still request fresh work immediately. Repeated same-frame
calls cannot count as multiple stable observations; frame gaps reset the wait.
Selected partial captures also check their source once per contributing frame,
preventing an A-to-B-to-A change from publishing mixed rows despite matching
start/end signatures. The final completion check remains in place.

The scheduler retains visible-missing, incoming-missing, then refinement order.
At most six capture/filter slices run per tick. Its 1.5 ms time check runs
between bounded operations, including retry probes; it cannot preempt a
callback already executing. This caps the preparation loop, not all cache CPU
work: visible-item validation precedes that timer, and ready/draw calls can
validate lazily. Those signature costs must also be included in frame timing.
Continuously changing content remains live with
the existing temporal fallback. Tests prove later static Weather bands and
incoming content publish during animation, About's later static content makes
progress during its glint, and a stopped Weather icon recovers with exact sharp
pixels and an independent scalar RGB565 blur reference.

All 15 focused cache, animation, scheduling and cadence methods pass, including
500 independent filter cases, 8,000 allocation/fragmentation operations,
unequal-buffer swaps and failed-growth rollback. Target compilation passes;
cache metadata uses 17,068 bytes of its existing 32 KiB header. Firmware and
simulator builds pass, as do 83 NOS unit-test methods, the native/power/asset
gates, 16,419 damage/full frames, six 497-frame forced Activity/Heart reference
comparisons and memory/lifecycle/I/O checks. The seven known motion-shape
failures and 14 excluded timing assertions remain explicit. The finalized ELF
is `dfe1d508e890ef28ef8dec5fd7146d4672ff9fbb9077dc50a966ca34989cae73`.
The guarded app-only flash passed, including the complete post-write slot and
unchanged regions outside the slot. The first capture attempt stopped before
any of its 12 windows: the host incorrectly required `MEM_CACHE directional`
from `info`, while the firmware emits that line from `mem`. This is a host
contract failure, with no performance result. A second host attempt correctly
queried `mem` but stopped at its initial state check: unlike `info` and `state`,
the firmware's command dispatcher wakes the watch for `mem`. Sampling an AOD
state before that command and an awake state afterward is therefore not a
stable pair. Both attempts completed zero windows; their evidence is retained.
The next host setup uses quiet identity/health checks, performs its explicit
awake setup, then checks the paired cache snapshots. Window state checks remain
strict. The diagnostic guard was disabled and COM3 closed after each stop.
The corrected 28c host passed all 46 named offline checks and completed eight
valid Activity/Heart windows before Weather produced no scrolling or painted
frames. Those eight cells used worker 0, history cadence 0 and geometry 1;
fresh poses confirmed 0-to-1-to-0 traversal at each speed. They had no allocator
failure or DMA completion mismatch.

| 28a firmware, 28c capture | Painted / intervals | Maximum work | Maximum presentation interval | Intervals over 33.3 ms | Live V frames |
|---|---:|---:|---:|---:|---:|
| Activity, four 290/900 px/s direction cells | 219 / 211 | 41.721 ms | 48.915 ms | 48 | 0 |
| Heart Rate, four 290/900 px/s direction cells | 230 / 222 | 34.302 ms | 41.702 ms | 12 | 0 |

The history blend processed 5,790,200 changed pixels across these cells.
Activity/Heart do not yet use the generic band renderer: their cache reports
remained at the 32 KiB metadata header with zero published bands. These results
therefore do not measure a generic-cache performance gain. Compared with earlier
runs, content, sampling and frame schedules can differ; this is not a controlled
attribution of a particular timing change to the page-setup optimization.

Weather's excluded capture had 143 UI ticks, zero pose changes and zero
paint/history/DMA work, despite a fresh acknowledged gesture and awake WEATHER
owner. Its empty-data state rejects scrolling in the source, and the connection
logs make that a plausible cause; the existing telemetry does not report its
runtime data-validity state. Zero timing misses in that window are not a pass.
The campaign remains incomplete at eight of twelve valid windows. Separate
guarded cleanup restored ordinary controls, HOME face 8/page 0 and normal AOD,
with guard off last and COM3 closed. Root's independent aggregation is
`build/iteration28c-root-review.json`; the full evidence is in the device report.
This revision does not add
subpixel scrolling, continuous shutter widths, or retained rendering to
Activity/Heart. Their live directional-filter admission and the 30 FPS floor
still require target validation. Power and the 200 MHz-equivalent workload
remain unqualified.

A private Activity content-coordinate cache prototype was also evaluated and
left outside production. It preserves live authored state and matches its own
canonical sharp renderer, but changes a few current AA pixels by at most
1/2/1 RGB565 channel levels. The tested one-pixel translations removed the
corresponding small coordinate-rounding variation. Under controlled five-band
admission, however, preparation plus replay increased estimated operation cost
from 2.57–2.74 ms to 5.21–7.45 ms across cold/warm 290/900 px/s cases. These are
model estimates; the model underprices some measured polygon CPU and is not a
hardware benchmark. Full-width copies of empty background outweighed the
modeled setup savings, so this prototype was not integrated. See
`build/blur-investigation/activity-band-adapter/REPORT.md`.

A follow-up private replay copies only each row's occupied interval when the
destination is known to be cleared; dirty destinations retain dense overwrite.
It preserves the full directional coverage rectangle, including zeros needed
to retire old temporal tails. Native/FMA full-pixel, clipping, recorder and
actual-history checks pass. Modeled total work falls by 7–31% versus the dense
adapter, with no target timing claim and no storage-capacity improvement yet.
Its separate census covers all sixteen settled Activity bands and all eight
contiguous nine-band windows at 8/16-pixel shutters. Exact one-span-per-row
packing would require at most 974,848 bytes including the header and dense
filter stage, or 1,048,576 with one extra packed publication. This is a fixture
storage estimate, not an animation/cold-start coverage guarantee. Production
remains unchanged by this experiment. See
`build/blur-investigation/activity-band-adapter/SPARSE_REPORT.md`.

A separate bounded codec now passes 551 private image cases, 14,855,228 decoded
pixel comparisons, 5,510 opaque replay checks against both a scalar oracle and
the production sprite routine, and 321,152 horizontal/vertical filter-access
checks. Target compilation uses 2,373 bytes of flash and no static RAM. It stores
one occupied span per row, with exact implicit black and a raw fallback, and
publishes a descriptor only after encoding completes. Its retained-storage
savings do not eliminate temporary raw capture/filter storage: a tested
nine-band window with one raw rebuild peaks at 1,183,744 bytes. That fits the
controlled 1,236,992-byte arena but does not establish a 1 MiB minimum fit or
several simultaneous raw jobs. Cache lifecycle integration is still private
and unqualified. Census and packing also consume preparation slices, so their
cold-start latency must be counted. See
`build/blur-investigation/compact-bands/INTEGRATION_PLAN.json`.

The actual-animation audit explains another limit: Activity's first-open hero
bands change every frame for about 1.475 seconds at 40 Hz; the Today and Week
reveals also change faster than complete captures can publish. Larger capacity
alone therefore cannot ensure cold directional coverage. Conservative page
culling in the private band callback preserves all tested signatures and full
context pixels while removing 47.7–59.9% of signature-recording commands and
37.4–64.0% of polygon preparations in four cold/warm scroll runs. Those are
operation counts, not measured watch speedups. The animation and callback
proofs are under `build/blur-investigation/activity-band-adapter/animation-audit`
and `callback-culling`; neither changes production animations.

A separate private lifecycle review found that reclaiming an inactive generic
layer's 434,312-byte pixel buffer could raise modeled full-width cache capacity
from five to seven bands while retaining HOME pre-rendering. The larger arena
can also prevent generic layers from being readmitted at rest, and idle
prewarming complicates the safe first-allocation point. No layer reclamation
or HOME eviction is included in 28a; capacity alone does not establish a net
performance or power benefit.

## Iteration 29: exact cache filter and measured cache use

The next candidate replaces the generic cache's 2/8/16-pixel box-filter
channel loops with exact packed RGB565 arithmetic; the 40-pixel case retains
the scalar path. Horizontal and vertical filtering keep the original rounding,
edge divisor, slice progress and publication rules. Vertical scratch still
occupies the existing 2,796-byte union, with the packed path touching 1,864
bytes. Switching `cache_packed 0|1` during an unfinished job reinitializes its
rolling sums at the current row without changing completed output. This is
an implementation comparison control, not a change to blur strength.

The independent private proof compared 43,269,488 output pixels and
281,046,649 channel-sum combinations with the scalar reference. Production
tests also cover 1,024 mode-switch cases, interrupted jobs and fragmentation.
The separate `NOSCACHECAP v=1` observer counts requested, sharp and blurred
processed draw-call area during the existing quiet capture window. It excludes
recording and offscreen layer baking, but includes overdraw and later-covered
pixels; it does not measure unique final-visible or optical coverage. Existing
`NOSBLURCAP v=1` fields are unchanged. About now exposes its real continuous
0–661-pixel scroll position for local tests that do not require Weather data.

The frozen candidate is `build/artifacts/iteration29-packed-cache-observer`,
source tree `a7b2b390efe9566f9bc4d0cbc6e50fe088208b536bcf1f4b2dd10da794371bfc`,
ELF `43fca3e2d573f50b74fc8698487c19201a3a54dc2180437ed40352b6e2be58b7`.
Firmware and simulator builds pass, along with the native/power/asset checks,
all 83 NOS methods after rerunning a simulator link test once the concurrent
simulator build finished, 16,419 damage/full comparisons and six 497-frame
forced pixel references. Both the initial link failure and successful rerun
are preserved. The seven existing motion-shape failures and 14 excluded
timing assertions remain explicit. Host checks are recorded in the artifact's
`host-validation.json`; they do not qualify frame rate or optical quality.
Device results, when available, belong in `OPTIMIZATIONS_DEVICE.md`. At this
freeze, the installed 28a baseline still fails the required frame floor.

The guarded 29 app-only flash subsequently passed. Its first About comparison
stopped on a host setup bug: expected geometry/history modes were unset even
though the reported device controls were correct. That raw window remains
excluded. After separate cleanup, the corrected 29b host passed 68 offline
methods and completed all eight About windows with the unchanged 29 firmware,
using scalar/packed/packed/scalar blocks at 290 px/s in both directions.

| About, four windows per mode | Painted / intervals | Maximum work | Maximum completion interval | Intervals over 33.3 ms | Cached blur / requested processed area |
|---|---:|---:|---:|---:|---:|
| Scalar cache filter | 981 / 952 | 38.322 ms | 38.499 ms | 166 | 65.28% |
| Packed cache filter | 983 / 953 | 38.460 ms | 53.076 ms | 178 | 82.98% |

Neither mode meets the floor. The observed cache fraction is processed
draw-call area, not unique or final-visible pixels. Cache capacity also changed
between blocks: 1,220,608; 1,515,520; 1,482,752; and 1,454,080 bytes. Fresh entry
invalidates the owner but does not force identical cold allocations. Thus the
higher measured fraction cannot be attributed solely to packed arithmetic,
and this is not a clean kernel-speed benchmark. The longest 53.076 ms interval
joined a cheap 4.948 ms frame to a 33.027 ms frame whose start was 24.997 ms
later; it is not 53 ms of rendering in one frame. Fixed pacing plus changing
work remains a problem. All eight cells retained history blending and had no
allocation, observer-epoch or DMA-completion mismatch. These About results do
not qualify Activity/Heart, physical touch, optical blur, power or a 200 MHz
equivalent workload. Cleanup restored normal controls, packed mode 1, HOME
face 8/page 0 and normal autosleep, disabled the guard last and closed COM3.
Root's independent aggregation is `build/iteration29b-root-review-v2.json`;
the device report preserves the full cells, failure and cleanup evidence.

The compact-band lifecycle experiment is now frozen privately. It passes
11,009,396 scalar pixel checks and its cancellation/rollback tests, but a
controlled 1,236,992-byte arena needs 83 logical preparation ticks to make all
seven visible bands ready. At 40 Hz that is at least 2.075 seconds; the actual
1.5 ms target preparation cap may extend it. Fragmentation still prevents
admitting an incoming band, and fast reversals average only 6.135% cached
coverage. It is not promoted. The private prepared-polygon cache also preserves
native/FMA pixels, but costs 69,984 bytes of PSRAM and leaves per-pixel edge work
unchanged; target benefit is unmeasured. Its setup-operation savings alone do
not justify promotion. Reports are under `build/blur-investigation/compact-bands/integration`
and `build/blur-investigation/prepared-polygons`.

A separately reviewed private ESP32-S3 PIE prototype processes eight continuing
vertical-filter pixels together without changing their RGB565 result or column
state. Its assembled loops contain 42 or 46 instructions per eight pixels;
the current scalar loop has 18 per pixel. The host instruction-semantics model
passes 10,097,152 pixel/state checks and the alignment/stride/clip tests, but
the assembled instructions have not run on the watch. The wrapper keeps the
original first-row/tag path and falls back for incompatible alignment. Merely
padding the state structure does not guarantee alignment: the current PSRAM
allocation contract permits four-byte alignment. Promotion requires a real
alignment guarantee, linked-assembly provenance, target pixel/state/guard
checks, involuntary preemption by another PIE task, and measured kernel and
frame timings. Instruction counts are not speed or frame-floor evidence.
The frozen proof and independent review are in
`build/blur-investigation/simd-vcolumns` and `simd-vcolumns-review`.

## 2026-10-19: native menu preparation and continuous exposure

The current menu work is in `optimizations`; device qualification is still in
progress. Iterations 30b through 32 are measured candidates, not a claim that
every animation meets the 30 FPS floor. The device report below retains the
failed timing windows as well as the improvements.

Settings separates ordinary static labels from live values/toggles. Eight fixed
grayscale control tiles capture only changed sources; combined label/value rows
retain their original paint order. Prepared 4/16/48-pixel boxes interpolate in
ink space before one palette lookup, and fractional Y placement avoids rounded
scroll stalls. Exposure follows measured travel: approximately 1.9/3.6/11.3/22.5
pixels at 150/290/900/1800 px/s. Settings now uses 64 brightness steps. Launcher
retains its authored icon sizes and blends across their size thresholds.

The first implementation regressed on the watch. The common blit now divides
cropped rows into bounded spans, moves bounds/address calculations outside the
pixel loop, and specializes zero-source and zero-phase cases without changing
pixel rounding. Settings also computes revisions and row geometry once per
generation rather than repeating font measurements and easing in each stripe.
The independent prepared-pixel oracle covers 5,818,951 pixels; the fractional
rolling-box oracle covers 954,382 pixels. Actual app tests cover source changes,
full-frame versus stripe composition, reset, and allocation failures.

Fully native rows own their cleared black backing as well as their current ink.
Previously, uncovered gaps revived the old glyph position through history,
creating separated copies. Live titles/scrollbars/overlays remain excluded from
that ownership. Actual compositor tests preserve the current native pixels and
retain temporal fallback for deliberately unprepared rows. The visual comparison
is `build/iteration31-native-visual/cold_vs_warm.png`: a 2.5-second preparation
wait leaves deep incoming rows cold; a verified 36/36 resident capture removes
those history copies. Iteration 33 replaces runtime preparation of canonical
static menu artwork with immutable generated flash images. All 36 Settings
entries and 19 Launcher rows have sharp and prepared-blur views available on
entry; dynamic values and toggles still use the bounded eight-slot RAM cache.

The generated pack contains 418 descriptors, 2,053,700 deduplicated grayscale
pixel bytes, and 133,632 bytes of palettes, plus metadata and the Launcher curve.
It borrows flash pointers explicitly: cache release, trimming, source changes,
and allocation failures never free those pixels. Unsupported font, registry,
or palette identities retain the runtime renderer. Font changes revoke both
static and live-control publications, including changes between two unsupported
fonts. Control admission now uses empty slots before eviction, retains the
nearest reverse-side source, and otherwise evicts the least recently used slot.
The authored immediate one-row reverse reuses its control sources without a
recapture; this does not imply every longer reversal is already resident.

Generation is reproducible from checked-in exporters under `tools/native_assets`:
run `python tools/generate_native_menu_assets.py --export`, then `--verify`.
Normal simulator and firmware builds reject stale artwork inputs. The host
reference checks all Launcher images and complete rendered poses, both Settings
theme palettes, static frames, changing controls, and ROM/runtime ownership
transitions. The generated host pixels are canonical; equality to the old
Xtensa floating-point bake has not been claimed. The diagnostic `native_rom 0|1`
switches sources at a UI safe point and resets mutable caches/history; it is
rejected during live capture or touch. `swipe_cold` discards mutable caches, not
immutable flash artwork or the hardware memory cache.

Iteration 34 groups adjacent exposure rows with identical coverage into one
history call per span. Already blurred native regions copy their sharp history
directly and clear touched uniform tags instead of classifying every block;
unprepared regions retain temporal exposure. The diagnostic
`history_native_copy 0|1` permits device comparison at a capture/touch-safe point.
Launcher size interpolation now uses bounded source rows with both original
rounding stages preserved. These changes allocate no additional frame memory.
Fresh host pixel, history, ownership, and cache tests pass; measured frame timing
is recorded separately in `OPTIMIZATIONS_DEVICE.md`.

Iteration 35 also reuses dynamic-control rolling sums across compatible stripes.
The bounded 4,024-byte pool belongs to the Settings PSRAM cache; source changes,
noncontiguous clips, recording and eviction invalidate continuations. Device
counters confirm reuse, but carry alone did not consistently improve the worst
frame. An opt-in render-priority experiment reduced some fast-reversal peaks
below 33.333 ms, yet a later repeat still missed that floor. It remains opt-in.
Paired rotation stores reduced their concurrent CPU stage by about 20% in the
complete comparison; iteration 36 enables them by default. These stage
measurements are not a power-consumption measurement or a 40 FPS qualification.

Iteration 36 also adds an opt-in virtual-black history representation. It can
leave known-black native blocks untouched in PSRAM until a later consumer needs
their physical pixels. Host correctness checks pass, and device counters confirm
logical write avoidance, but the four mode 0/1/1/0 runs all miss the frame floor:
maximum completion intervals are 36.505 / 35.864 / 35.528 / 35.433 ms. It remains
disabled by default. The measured candidate is flashed and verified; these runs
sampled 240 MHz, not the requested 200 MHz-equivalent budget.

Actual stored menu artwork and live control-cache examples are exported in
`build/cache-gallery/index.html`, with source hashes in `provenance.json`.
The gallery distinguishes stored 0/4/16/48-pixel static variants from runtime
blur of sharp dynamic controls. Speed examples use a steady 40 Hz half-frame
exposure model; they are renderer pixel comparisons, not device recordings.

The completion-aware pacer can advance the next frame's start; it never delays
the DMA completion measurement, drops submitted frames, or alters timestamps.
The engine advances by actual elapsed time so earlier starts do not accelerate
animations. This helps cheap-to-expensive transitions but cannot rescue an
individual expensive render. A 40 FPS target, the 30 FPS floor, physical-touch
quality, optical presentation, and the 200 MHz-equivalent budget remain distinct
acceptance questions. General Activity/Heart vertical rendering is not qualified
by these native-menu changes.

## Primary references

- [Espressif external RAM guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/external-ram.html): external memory and cache constraints.
- [Espressif power management](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/power_management.html): supported frequencies and power-management locks.
- [LVGL image caching](https://lvgl.io/docs/open/9.5/main-modules/images/caching): reuse prepared images with explicit invalidation.
- [LVGL draw layers](https://lvgl.io/docs/open/9.3/details/main-modules/draw/draw_layers.html): retained transforms and bounded render areas.
