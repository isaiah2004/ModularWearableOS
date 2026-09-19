# Pip: implementation audit and validation

The device performance and image hashes below describe the original pet
deployment. The subsequent animation optimization is documented in
[Pip animation performance](../PIP_PERFORMANCE.md); `../device-validation.json`
and `dist/v1/manifest.json` identify the currently tested and packaged image.

This record accompanies the [case studies and revised plan](PET_IMPLEMENTATION_PLAN.md).
It separates source research, deterministic tests, rendered previews, device
instrumentation and physical wear evaluation. The reports' proposed tests are
not all claimed as completed here.

## Implemented result

The implementation contains 21 procedural performances, a background care
service, persistent company/stimulation/rest/comfort needs, battery-aware
energy, a recoverable grumpy mood, observed charging requests, estimated
walking rewards, arrival-based notification reactions, alarm events, a
three-page care app, adjustable local-time routines and checked two-slot
memory saves. The existing Pip familiarity and seed survive separately.

The care engine emits a discrete contextual mood. The expression model then
smooths valence, arousal and other continuous targets; a complete ALMA-like
affect engine was not necessary. No cloud inference or new sensor is required.

## Revisions from implementation and test findings

| Finding | Revision and evidence |
|---|---|
| The old joined eye shape had outline seams | A continuous rounded shape replaces it; reference raster/clipping tests and clean-frame comparisons check the result |
| Smoothed blinks could fail to close | Blink closure is applied after slower pose smoothing; regression checks include low frame rates |
| Sustained strokes restarted the same reaction | Renew its expiry without restarting its phase; looping stroke and held-care scenarios pass |
| Startle shared a cooldown with sustained motion | Separate the startle cooldown so a later sustained shake can still become dizzy |
| A moving face-down watch could become sleepy | Require stable face-down evidence; protect active startle/dizzy recovery |
| Care's custom routine did not reach the animation layer | Add an authoritative quiet-hour input with next-step wake on routine end; daytime quiet and 23:00 awake tests pass |
| The lonely mood missed the animation threshold | Use a negative context beyond the threshold and include it in idle selection |
| A notification could interrupt a care performance | Defer cosmetic event selection through active petting, play, happy, relief and excitement performances |
| A happy mood concealed an unresolved grievance for a minute | Use a six-second positive interval when power remains overdue or social satisfaction remains very low |
| Hello only repaired social need once per day | Add a separately throttled 30-point greeting reward; it does not block the next petting reward |
| Flat offline decay penalized a quiet night | Compute awake/quiet overlap, including half-hour timezone context, with bounded reduced offline consequences |
| First notification poll could invent an arrival | Prime the arrival baseline, ignore initial zero, and count producer sequence changes |
| Notification history could displace meaningful care memories | Keep arrival content and routine notification events out of permanent care history |
| Repeated snapshots advanced the alternating slot even after failure | Commit generation only after a successful checked write; four-failure retry test preserves the previous slot |
| Battery app still inferred Full from USB-raised voltage | Shared battery adapter now exposes external/unknown evidence and hides an unqualified percentage |
| Care page text clipped against the round panel | Shorten the memories title and charging help text; rerender and inspect all three pages |
| Device tour inspected the old app before its fade completed | Poll the settled app state with a timeout; the corrected full tour passes |
| Device render profiling was slower than the host preview | Optimize solid eye spans, preserve exact fringe coverage, then repeat pixel and device profiling checks |

## Automated and simulator coverage

`tools/test.ps1` compiles portable suites with C99, optimization, `-Wall
-Wextra -Werror`. The suites cover board protocols and rotation/touch inverse,
Pip expressions, care, cadence estimation, notification sequencing and
qualified battery states. Existing watch-control Python unit tests also pass.

Care scenarios include eleven hours of neglect followed by repair, six-hour
traces at different service cadences, known/unknown and backward/forward time,
custom day/night windows, quiet offline nights, capped long absence,
corrupted records, failed-save retry, reboot reward cooldowns, sustained
qualified low power, a 30-minute observed charging request, missing evidence,
power relief, walking budgets and notification/alarm priority. These are
deterministic software scenarios, not measurements of a person walking or a
battery discharging.

`sim.exe --pip-checks` passes looping strokes, below-eye gallery swipes,
cancelled edge gestures, care-footer navigation and greeting return. The
render check compares **2,520 frames** against clean full draws: 120 frames at
50 ms for each of 21 forced performances, covering later gestures and
recovery. The paths produce equal RGB565 pixels. This checks damage tracking
and drawing consistency; it does not prove that the physical panel never tears.

The [interactive preview](assets/pip-preview.html) contains 840 actual
simulator-rendered frames, four seconds per expression sampled at 10 fps.
The original simulation advances at 20 fps. [Contact sheet](assets/pip-contact-sheet.png)
and [care pages](assets/pip-care-screens.png) provide still review artifacts.
No generated illustration replaces the implementation in these previews.

## Device observations

The confirmed target is the ESP32-S3-Touch-AMOLED-1.43-B on COM3, MAC
`28:84:85:b0:8b:30`, with 16 MiB flash and 8 MiB PSRAM. An unrelated COM8
device was identified and unsuccessfully read before the owner corrected the
connection; it was never flashed. All deployment writes targeted the
confirmed COM3 identity. The previous OS and NVS were preserved as described
in [recovery instructions](../RECOVERY.md).

The first care build booted with approximately 78 KiB free internal RAM.
After the full tour it reported approximately 74 KiB free, largest free block
31,744 bytes and minimum free heap around 41 KiB. Touch, IMU and RTC responded;
the SD slot was empty. USB external-power evidence was positive while the
battery estimate remained explicitly invalid, despite a rail near 4.19 V.

The corrected USB tour passed all **26 app states, 13 face selections and four
edge-menu commands**. It delivered twelve notifications while the eight-item
store was already saturated, checked the incremented arrival sequence, and
observed `state=notify`. Petting, play, grumpy, lonely, excited, relieved,
notification, dizzy, sleep and wake commands produced their expected action
states. Care pages and routine commands responded. These are command and
state observations, not camera-based judgments of expression quality.

An actual save/reset/restore test retained familiarity 82, social satisfaction
829, stimulation 720 and the 07:00–22:00 routine. Rest advanced from 804 to 805
during quiet time. Saved generation 6 loaded and advanced to 7 following the
next normal save. RTC returned valid local time with the retained +330-minute
offset. Notification contents are volatile and cleared on reboot as designed.

Before the final eye optimization, the 5–15 second observation window had a
median of 16 fps and 63.7 ms rendering on this device. QSPI remained at 40 MHz.
This measurement motivated the final optimization rather than a panel
overclock. The revised raster passed **5,774 exact old/new pixel comparisons**,
including clipped stripes, fractional positions, recorder bounds and buffer
canaries. The sampled final boot sequence reached **37–46 fps** with roughly
21–26 ms rendering in the 5–15 second observation window: median **42.5 fps**
and **22.25 ms** rendering. These are sampled
device workloads rather than a guarantee of every expression's frame rate.
Build-specific final hashes and observations are recorded in
`../device-validation.json`; firmware packaging matches evidence to its image
hash rather than transferring a pass flag blindly between builds.

The original pet deployment firmware was **6,375,184 bytes**, SHA-256
`eb616ba33d5d937969861dc209bf6f30eabc0e6b8d556067348a6af2604c645e`.
It was flashed, booted, and passed the full 26-app/13-face/four-edge tour again.
A further reset test on this exact image retained familiarity 83, social 959,
stimulation 740, rest 813, the 07:00–22:00 routine and memory generation 14.
The packaged manifest marks boot and USB checks passed while leaving complete
physical validation false until the wear tests below are performed.

Logs include `build/test-pip-care.log`,
`build/pip-care-preview/checks.log`,
`build/pip-care-preview/render-checks.log`,
`build/usb-pip-tour-final.jsonl`, `build/pip-before-reset.jsonl`,
`build/pip-after-reset.jsonl`, and the boot/flash records named in the device
manifest. Local build logs and NVS backups are not part of the public report.
Final-image tour and retention logs are `build/usb-pip-fast-tour.jsonl`,
`build/pip-fast-before-reset.jsonl` and `build/pip-fast-after-reset.jsonl`.

## What still needs physical evaluation

| Requirement | Current evidence | Remaining evaluation |
|---|---|---|
| Expressive personality | Model tests, rendered clips, device action states | Owner judges expression readability and timing on the actual screen |
| Touch alignment | Previously confirmed by owner after the rotation/touch fix; new OS gesture tests pass | Recheck feel of longer petting sessions on the new face |
| Persistent care and grumpiness | Accelerated scenarios and actual reboot retention | Wear for a day and tune how soon loneliness/annoyance feels appropriate |
| Timely charging | Qualified USB evidence and simulated low/overdue/recovery traces | Unplugged discharge traces and real charge transitions; true charge current/full status remain unavailable |
| Walking enjoyment | Cadence rejection tests, activity budget tests, live stationary sensor | Wrist recordings of walking, typing, shaking and transport; thresholds are not calibrated fitness measurements |
| Notifications | Actual USB queue/store/behavior path | End-to-end phone forwarding; HTTP/BLE paths are compiled but not physically exercised in this session |
| Alarm awareness | Actual scheduler hook and care event-priority tests | Physical scheduled alarm with simultaneous touch/notification |
| Power/performance | Device profiling, cached inputs, bounded state | Longer battery-runtime and thermal observation under normal wear |
| Recovery from storage faults | Corruption/failure simulations and normal NVS reboot test | No deliberate physical power-cut corruption test was performed |

The owner can tune routines immediately. Care rates, movement thresholds and
expression timings remain centralized source parameters. A later revision
should use these real wear observations, especially false walking rewards,
unfair power complaints and any expression whose cause is unclear. The
reports' claims about long-term attachment remain hypotheses for Pip.
