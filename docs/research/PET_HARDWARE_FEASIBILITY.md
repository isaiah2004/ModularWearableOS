# Pip: hardware feasibility and implementation boundaries

Research date: 2026-09-13. Target: Waveshare ESP32-S3-Touch-AMOLED-1.43-B.
This is a read-only hardware/code study; no USB operations or firmware changes
were performed. Thresholds below are proposed starting values, not calibrated
measurements or claims about the user's activity or battery.

## Decisions that should shape the plan

Pip can react convincingly to touch, wrist motion, time, observed low voltage,
and delivered notification events. The stock board cannot directly report
charging current or charge completion. A reliable walk feature must run as a
background service, independent of the selected face. Phone notifications
need a real firmware delivery path before Pip can react to them.

| Requested behavior | Evidence available now | Implementation boundary |
|---|---|---|
| Energy follows watch battery | Calibrated ADC system-rail voltage; crude percentage | Use a confidence-bearing energy estimate, not exact state of charge |
| Happy when connected to power | USB host activity; possible elevated system rail | Call this a power-connection reaction; charging remains unknown |
| Mad after missed charging | Observed low-voltage duration and valid timestamps | Only a brief recoverable sulk after an observed request; never infer neglect from a reboot or missing history |
| Enjoys walks | Live accelerometer/gyro; official optional pedometer code | Validate cadence/step evidence; reward activity without claiming health metrics |
| Responds to notifications | Eight-entry notification store | No current device producer; add delivery, sequence IDs, and cooldowns |
| Knows bedtime | RTC-backed local time when valid | Separate monotonic action timers from wall-clock routines |
| Remembers the relationship | Small NVS blob; optional FAT32 SD | NVS is authoritative; SD is an optional journal |

## Battery, external power, and charging are different signals

**Schematic facts.** GPIO4/BAT_ADC connects to **VCC** through R12=200 kΩ and
R17=100 kΩ, giving a 3:1 divider. VCC is the system rail: USB feeds it through
D2, and the battery path uses Q1. The ETA6098 charger has a STAT output, but
STAT drives LED2 and is not routed to an ESP32 GPIO. VBAT and USB_5V are
exposed as power nets; neither has a separate fitted MCU voltage measurement.
There is no digital fuel-gauge interface in this schematic. These are direct
observations of the drawing, not assumptions from the BAT_ADC label.
[Waveshare schematic, POWER/BAT/Type-C blocks](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-Touch-AMOLED-1.43-Schematic.pdf)

**Charger facts.** ETA6098 performs its charge cycle autonomously, uses an
external ISET resistor, and exposes charging status through STAT. Its
charge-termination state is not software-readable over I2C. Connecting USB
does not prove that current is entering the cell: it may be full, absent, or
not charging. No charger settings should be changed for pet behavior.
[ETA6098 manufacturer datasheet](https://www.eta-semi.com/wp-content/uploads/2022/03/ETA6098_V1.1.pdf)

**Current code.** `firmware/main/hal_esp32.c:132` correctly notes that the ADC
measures the system rail, although `core/hal.h` still calls it battery-terminal
millivolts. `hal_battery_percent()` maps 3.3–4.2 V linearly and returns 50 on a
failed reading. That default must not be treated as a measured 50% battery.
The factory ADC example also names its result system voltage. Historical
USB-connected logs around 4.18 V do not by themselves validate the expected
USB/battery separation. Measure this actual unit before enabling an automatic
charging classifier.

**USB evidence.** IDF's `usb_serial_jtag_is_connected()` detects USB host SOF
packets. A power bank produces no such packets, so false means neither
"unplugged" nor "not charging". True establishes a live host connection,
not battery current. The API adds tick overhead and should not be the sole
power detector. [IDF API contract](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_driver_usb_serial_jtag/include/driver/usb_serial_jtag.h#L94)

### Proposed power snapshot and classifier

Publish a cached snapshot containing `system_mv`, `sample_age_ms`,
`voltage_valid`, `voltage_calibrated`, `external_power_evidence`,
`estimated_percent`, `percent_valid`, and `charging=UNKNOWN`. Keep the
last valid percentage during an external-power interval, but mark it stale;
do not make the battery jump to 100% when the system rail rises.

Use one ADC owner, not a reading from every render stripe or multiple tasks.
The existing renderer, profiler, and board SD logger can all reach the ADC
calibration handle. IDF documents that conversion functions sharing that
handle are not thread-safe. The ESP32-S3 supports curve-fitting calibration,
and multisampling helps with noise.
[IDF ADC calibration and thread-safety guidance](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/peripherals/adc_calibration.html)

Candidate settings to evaluate on the actual unit:

- Sample eight ADC values at 1 Hz in a service; discard failed conversions,
  take a median, then smooth with a 5–10 s time constant. Publish the last
  value plus validity and age. No valid sample for 3 s means unknown.
- A calibrated system rail above 4.40 V for 3 s is evidence of external power,
  subject to confirming the schematic matches this unit. Below 4.25 V for
  10 s clears that evidence; intermediate values remain ambiguous. These
  thresholds are illustrative and require plugged/unplugged measurements.
- Determine an approximate discharge curve from this cell under normal watch
  load. Until then, a coarse low/medium/high energy state is more honest than
  detailed numerical percentages. Do not extrapolate time remaining.
- For low-energy behavior, use sustained voltage below a calibrated threshold
  for 30 s, with at least 100 mV of hysteresis. Suppress an episode during
  transient radio/display load, invalid readings, and ambiguous power state.
- Pip's internal stamina can follow the coarse energy state slowly. A power
  connection can trigger immediate relief while stamina recovers gradually;
  the expression is fictional, not a claim of measured charging progress.

For the requested missed-charge personality, first record an **observed**
low-energy request, then wait, for example, 10 powered-on minutes without
power evidence before a short grumpy look. Cap it to one episode per low
energy interval, forgive on interaction or power connection, and never
disable watch functions. Invalid time, deep sleep, shutdown, and unknown
voltage must pause this accounting. Learning a preferred charge time needs
several valid sessions and an explicit user-facing routine; a single nightly
timestamp does not establish a habit.

True charging detection would require a later hardware addition such as a
properly interfaced STAT input and/or a battery fuel gauge. That is a separate
wiring task, not a firmware-only capability of the assembled B board.

## Motion and walking

**Available now.** The board service configures accelerometer ±8 g and gyro
±512 degrees/s at 125 Hz, then polls a 12-byte sample about every 20 ms.
The HAL exposes a cached sample with a 250 ms freshness check. The effective
software sampling interval can lengthen during RTC/SD work; it is not a
uniform 50 Hz stream. `hal_step_count()` currently returns zero. Existing
Pip updates motion while its face renders, so simply putting walk logic into
`pip_step()` would miss activity in other apps or display modes.

**Official implementation exists.** The downloaded factory `qmi8658c.c`
includes `qmi8658_config_pedometer()`, `qmi8658_enable_pedometer()`, and
`qmi8658_read_pedometer()` under `QMI8658_USE_PEDOMETER`, with a 24-bit read
at 0x5A–0x5C. Its init requests 125 Hz parameters. This is preferable as a
protocol reference to a community implementation.
[Official Demo V3 archive](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-AMOLED-1.43-Demo-V3.zip)

QST's newer QMI8658C Rev A explains a hardware pedometer, peak thresholds,
entry-step filtering, two CTRL9 configuration commands, enable control,
interrupts, and a wrapping 24-bit counter. It requires non-synchronized
sampling and scales timing parameters with accelerometer ODR. The document's
reference settings are based on 50 Hz. The older Rev 0.9 PDF linked by the
board documentation lacks the pedometer chapter, so verify the fitted device
and its command acknowledgements. The factory helper's `200/ODR` scaling
differs from directly converting the Rev A reference durations; do not copy
that arithmetic without testing.
[QST QMI8658C Rev A, sections 11.1–11.6](https://www.qstcorp.com/upload/pdf/202210/13-52-27%20QMI8658C%20Datasheet%20Rev%20A%20%281%29.pdf),
[board-linked older datasheet](https://files.waveshare.com/wiki/common/QMI8658C_datasheet_rev_0.9.pdf)

### Recommended implementation path

1. Add a background activity service with timestamped input and monotonic
   counters, independent of face rendering. Give each IMU sample a sequence
   number or timestamp so a repeatedly read cached sample is processed once.
2. Prefer the internal pedometer if bounded CTRL9 handshakes and readback
   succeed; on any configuration failure restore the existing raw IMU setup.
   Do not make an optional pedometer error abort boot. Read its counter at
   1–2 Hz and handle wrap/reset explicitly; reject an implausible jump.
3. Use motion confidence to form activity bouts. Reward a sustained bout or
   milestone rather than every noisy candidate step. Preserve estimated
   activity separately from any future validated daily step count.
4. Keep raw-data software classification available for simulator testing and
   as a fallback. Report its result as estimated movement or walking-like
   activity. No distance, calorie, fitness, or medical conclusions follow.

### Candidate software classifier for a first field trial

These are engineering hypotheses requiring logs, not validated thresholds:

- Process samples at approximately 50 Hz with actual elapsed time. Reject
  non-finite data, clipping, duplicates, and gaps above 100 ms; reset cadence
  after a gap rather than interpolating imaginary steps.
- Form orientation-independent magnitude `sqrt(ax²+ay²+az²)`. Remove a
  slowly changing baseline (about 0.7 s time constant), then smooth the
  residual (about 0.05 s). Avoid estimating position by double integration.
- Start candidate peaks near 0.10 g above the local baseline and require
  about 0.18 g peak-to-peak prominence. Use a noise-adaptive threshold with
  reasonable lower/upper bounds; refine with wrist recordings.
- Accept candidate intervals of 0.30–1.50 s. Require eight coherent candidates
  and interval coefficient of variation below about 0.30 before confirming
  a bout; retroactively credit the entry candidates once confirmed.
- Pause/reset after 2.5 s without a candidate. Initially grant one small
  happiness reward after 30 s of confirmed activity, then at most once per
  additional minute. Limit daily reward so shaking is not advantageous.
- Use gyro spikes and the existing shake/dizzy detector to flag uncertain
  bouts, not to reject all vigorous motion. Walking arm swing also produces
  rotation; arbitrary gyro gating can erase genuine activity.

Expected false positives: tooth brushing, cooking, clapping, shaking Pip,
vehicle vibration, drumming, and repetitive desk work. Expected false
negatives: pushing a stroller/cart, hands in pockets, carrying objects,
very slow gait, a loose strap, and short walks below the entry threshold.
The available sensors do not establish whether the watch is worn, whether
the user is outdoors, or that every motion peak is a footstep. A manually
started "walk together" session can improve the interaction without hiding
that uncertainty.

If real steps are added to the existing Activity app, review its inherited
calorie/distance/stand-hour estimates. That app currently derives several
values from steps using fixed heuristics; enabling live input does not turn
those into measured quantities.

## Time and notifications

The current RTC service restores UTC after validating its calendar and
oscillator status. Local time comes from the configured timezone. Use valid
wall time only for bedtime, day boundaries, and remembered routines. Use
monotonic time for animation, cooldowns, and observed interaction duration.
On clock changes, do not award another daily milestone or count a negative
elapsed interval. Clamp offline catch-up and treat unknown time as a neutral
return, not neglect.

The notification module is an eight-entry RAM list with no event sequence,
timestamp, or synchronization. A repository-wide search found only simulator
calls to `notif_push()`; there are no firmware callers despite the header
mentioning BLE/ANCS. The existing BLE media/time path is not proof of phone
notification forwarding.

Add a validated ingress path that queues events onto the UI/service owner.
Give each event a sequence ID; a full eight-entry store can receive a new
event without changing its count. Suggested pet response: orient toward a
small badge, blink once, settle. Coalesce bursts over 5 s, allow at most one
prominent response per 30 s, and respect quiet hours. Do not infer an emotion
from message text or keep message content in Pip's long-term memory.
If only USB injection is implemented, describe notification reactions as
testable locally, with phone forwarding still pending.

## Persistence, sleep, and resource budget

The current pet memory is a 20-byte versioned blob containing familiarity,
visits, and random seed. `face_pip_service()` saves dirty state after five
minutes or before explicit sleep. It does not yet persist energy history,
activity history, or last-valid UTC. The NVS partition is 24 KiB and shared
with settings; SD is not required for a functioning pet.

Use a compact versioned snapshot, ideally no more than 512 bytes, containing
durable relationship state, last-valid timestamps, daily activity totals,
and bounded cooldown/episode information. Validate length, ranges, version,
and checksum. Change the HAL save contract to report errors: the current
function ignores set/commit results and callers clear dirty state regardless.
NVS is designed for small records, uses wear levelling, and recovers from
power loss except that the in-progress value may be lost.
[IDF NVS behavior](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/storage/nvs_flash.html)

Keep volatile animation state in RAM. Save meaningful milestones and a
coalesced checkpoint every 5–15 minutes, plus orderly sleep. Never commit per
frame or per step. Optional SD can hold a bounded event journal with rotations;
perform writes on a worker, retain NVS as authority, and continue normally
after card removal/full-card errors. Current one-minute CSV logging blocks
the same board loop that samples the IMU: split that work or use the internal
pedometer/FIFO before claiming uninterrupted activity capture.

Current explicit sleep stops the board task, disables both IMU sensors,
turns off the panel, and wakes only on BOOT. Consequently it cannot count
steps or observe charging transitions during that period. GPIO8 is wired to
an IMU interrupt and GPIO15 to RTC interrupt; both are S3 RTC-capable GPIOs,
so a later interrupt-wake design is feasible. That requires retaining the
appropriate sensor mode, configuring polarity/latching, and testing actual
sleep current and wake loops. It is not present behavior.
[IDF sleep/wake GPIO support](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/sleep_modes.html)

Proposed budgets, to be measured rather than promised:

| Resource | Initial target |
|---|---|
| Pet/activity state | Under 4 KiB RAM, including bounded history and event queue |
| Activity computation | Under 0.2 ms per sample; no allocations in sample/render loops |
| Needs/routines service | 1 Hz, even when Pip is hidden; no rendering work while hidden |
| Foreground animation | Stable 25–30 fps first; faster only if measured frame time permits |
| AOD | Existing 1 Hz display cadence, minimal glow, no autonomous wake spam |
| Persistent snapshot | At most 512 bytes; infrequent commits |
| Optional telemetry | Bounded buffers and rotated files; never synchronous UI writes |
| Battery lifetime | Measure awake, AOD, radios, SD, and sleep separately; no hour estimate yet |

## QSPI: 40 MHz is the useful supported setting

`LCD_SPI_HZ` is 40 MHz. ESP32-S3 SPI2 uses an 80 MHz APB source by default,
and the IDF 5.5.4 integer-divider algorithm calculates 40 MHz for a 50 MHz
request. The next faster attainable clock is 80 MHz. Changing the literal
to 50 MHz provides no gain.
[IDF divider implementation](https://github.com/espressif/esp-idf/blob/v5.5.4/components/hal/esp32s3/include/hal/spi_ll.h#L783),
[IDF SPI clock documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/peripherals/spi_master.html#spi-clock-frequency)

Waveshare's linked SH8601Z datasheet specifies a 20 ns minimum quad-write
cycle, equivalent to 50 MHz. The attached panel's 0xff ID selects the CO5300
fallback and is not positive SH8601 identification, so that document alone
does not establish the installed controller's limit. Retain 40 MHz.
[SH8601Z datasheet, page 24](https://files.waveshare.com/wiki/common/SH8601Z0_V0.0.pdf)

Calculated from 466×466×2 bytes and four data lanes, the minimum full-frame
wire time at 40 MHz is 21.72 ms, or 46 full frames/s before drawing and
transaction costs. Each 16-row stripe is 14,912 bytes and needs 0.746 ms on
the wire. The software rotation also costs CPU time. Keep damage restricted
to the eyes and brief particles, cache the clock, and preserve a single
latched pose across render stripes. More elaborate behavior need not imply
more pixels or allocations.

## Required revisions and verification before claiming completion

1. Replace the implicit battery/charging assumptions with validity-bearing
   observations and explicit unknown states. Verify plugged/unplugged ADC
   readings at several battery levels before missed-charge behavior goes live.
2. Run needs/activity updates outside the face draw path. Preserve one owner
   for mutable state and one owner for ADC conversion.
3. Choose and validate the pedometer path. Replay timestamped stationary,
   walking, irregular, duplicate, missing, and shake sequences; include counter
   wrap/reset and peripheral failure. Compare physical walks with counted
   steps, and record false positives without inventing accuracy percentages.
4. Add notification delivery before promising phone reactions. Test a ninth
   event in a full store, reconnect duplicates, bursts, and quiet hours.
5. Persist the new data with migration and save-error handling. Verify reboot,
   invalid RTC, clock jumps, missing SD, full SD, and interrupted save behavior.
6. Keep sleep behavior explicit: activity is unobserved when sensors are off.
   Treat wake-on-motion and low-power pedometry as separately validated work.
7. Measure Pip frame times, input latency, heap low-water mark, and actual
   battery current with radios and logging enabled. Retain the 40 MHz clock.

The suggested personality is implementable with current compute and storage.
The hard limits are the meaning and continuity of its observations, not the
number of expression states. Keep those boundaries visible in debug output
and validation records so the character can feel alive without pretending
the hardware measures things it does not.

## Implementation follow-up: hardware inputs

The later implementation phase adds the following interfaces in
`firmware/main/hal_esp32.c`, `firmware/main/board.c`, and `core/walk.c`.
The research observations above describe the pre-implementation snapshot;
this section records which gaps were addressed. Firmware build and physical
validation remain the integrating agent's responsibility.

| Interface | Implemented contract |
|---|---|
| `hal_power_get(hal_power *)` | Cached `rail_mv`, last estimated `battery_percent`, `battery_valid`, `external_power`, `sample_valid`, `age_ms` |
| `external_power` | -1 unknown; 0 inferred battery; 1 USB host or sustained high-rail evidence. Never charge-current status |
| `hal_activity_get(hal_activity *)` | Estimated `steps` and `active_seconds` since boot, plus `walking` and `valid` |
| `hal_epoch_seconds()` | UTC seconds in 2024–2099, otherwise 0 |
| `hal_store_save_checked()` | 1 only when NVS set and commit succeed; legacy void save delegates to it |
| `hal_notification_post(app,title)` | Nonblocking bounded queue, returns 1 on acceptance, 0 on invalid/full/unavailable |
| `hal_notification_poll(app,an,title,tn)` | Nonblocking dequeue for main-thread consumption, returns 1 for an event, 0 for empty/invalid buffers |

Only the board service converts ADC readings, once per second. The sampler
uses eight conversions, requires five successful plausible results, takes
their median, and applies a 1/5 EMA. It requires calibration and ten stable
3300–4250 mV samples without USB host evidence before publishing a valid
battery estimate. A median above 4400 mV for three samples establishes
high-rail evidence. Sample gaps longer than 3 s reset stability; cached data
older than 3 s becomes unknown. The legacy percent getter retains its last
estimate (initially 50), so consumers that care about correctness must read
the validity-bearing snapshot.

The new cadence classifier runs on each fresh board IMU read, even when
another app is visible. Its entry gate is eight peaks with intervals
300–1500 ms and coefficient of variation no greater than 0.25. It excludes
gaps above 100 ms, implausible magnitudes, and rotation above 250 degrees/s.
The hardware pedometer registers remain unchanged. Stationary/noisy input,
sustained cadence, sideways orientation, fast shakes with and without gyro,
short bouts, gaps, 32-bit clock rollover, duplicate timestamps, and invalid
values pass the strict host test in `tests/walk_test.c`. This verifies
algorithm behavior on constructed signals, not real-world step accuracy.

Notification ingress uses an eight-entry FreeRTOS queue. App names are 1–19
bytes; titles are 1–39 bytes. Empty fields, NUL/control characters, and
oversized input are rejected. The app field cannot contain `|`; the first
separator divides app from title. No producer calls `notif_push()` directly.

- HTTP: `POST /notify`, body `app|title`, 3–59 bytes total. Success returns
  `queued`; malformed input returns 400; a full/unavailable queue returns
  503. A receive timeout returns 408 rather than retrying indefinitely.
  It shares the existing local watch HTTP server and access model.
- BLE: the existing TIME characteristic reserves **long frames of at least
  seven bytes beginning with 0xF2** for `0xF2` followed by `app|title`.
  Maximum size is 60 bytes including the marker. Accepted writes queue an
  event; malformed/oversize/full cases return ATT errors. A sender needs
  sufficient negotiated ATT MTU or a supported long write for larger events.
  Legacy four- and six-byte clock writes are unchanged. Because the original
  clock protocol had no message-type field, an extended time/location frame
  beginning with 0xF2 is now reserved; clients must send the six-byte time
  form for such timestamps. This limitation should be removed with a future
  dedicated notification characteristic or fully framed protocol.

Main-thread polling, notification-store sequencing, simulator stubs, daily
activity persistence, personality integration, and full firmware validation
are separate integration work. No physical battery/step calibration or
charging-current detection is claimed by these interfaces.
