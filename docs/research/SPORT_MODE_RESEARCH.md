# Sport mode: evidence and implementation decisions

Research checked 20 October 2026. Scope: this recovered workspace and its simulator;
no watch, serial connection, firmware flash, or claim of hardware validation.
This is a design rationale, not a statement that every recommendation is shipped.

## What makes this a workout recorder

The existing `nos/sports.c` is a diagnostic/event journal. Its daily activity
summaries and `nos/apps/fit_activity.c` do not define a workout's start, pauses,
laps, route, finish, or saved result. Build a separate session service, with the
screen as a client, and preserve the existing journal and settings checkpoint.

Use explicit `idle -> recording <-> paused -> finished` transitions. Finishing
freezes the result; saving and discarding are explicit actions. Leaving the screen
must not finish the workout. Keep elapsed time and active recording time separate;
derive both from monotonic time, not the number of rendered frames or wall-clock
differences. Timestamp records with UTC only when available. A wall-clock correction
must not change elapsed duration. Persist enough state to distinguish an
interrupted workout from a completed one; never invent activity during downtime.

This separation is supported by two inspected open projects. OpenTracks maintains
recording state and sensor ownership in a service, exposes snapshots to the UI,
and refreshes presentation data once a second. Its statistics updater explicitly
handles manual segment starts/ends and clears the preceding point at boundaries.
InfiniTime's stopwatch controller accumulates prior elapsed time on pause and uses
an RTOS tick delta while running, with bounded lap history. These are architectural
references; no source code was copied. See the immutable source links below.

## Sports and honest measurements

| Profile | Useful initial measurements | Missing-data policy |
| --- | --- | --- |
| Outdoor walk | Time, accepted GNSS distance, pace, session steps, valid HR, manual laps | GNSS loss leaves a route gap; steps are not silently converted into replacement GNSS distance |
| Outdoor run | Time, accepted GNSS distance, pace, session steps/cadence, valid HR, manual laps | No running form, VO2max, or training-load claims |
| Outdoor cycle | Time, accepted GNSS distance, speed, valid HR, manual laps | No pedal cadence or power from wrist steps |
| Hike | Time, accepted GNSS distance, pace, session steps, valid HR | Absolute altitude may be displayed as a sensor reading; cumulative ascent needs a separately tested filter |
| Treadmill | Active time, session steps/cadence, valid HR, manual laps | No GNSS distance, pace, or inferred belt distance without a calibrated stride/footpod source |
| Indoor workout | Time, valid HR, manual laps | No GNSS route, travel distance, or pace; do not fabricate repetitions or exercise recognition |

The table is an engineering scope decision. Selecting a profile is a user label,
not evidence that an activity classifier recognized the sport. Swimming is outside
this scope: the implementation has no validated pool-length or stroke detector,
and the simulator cannot establish water resistance or underwater sensing.

## Distance and speed

**Evidence.** Ranacher et al. mathematically show that position error can inflate
the sum of short GNSS segments; sparse sampling can instead miss the true path.
More points alone do not guarantee a better distance. Gilgen-Ammann et al. tested
eight sport watches over 36 walks, runs, and rides: overall distance mean absolute
percentage error ranged from 3.2% to 6.1%, with poorer results in urban/forest
settings. That experiment used one participant and commercial watches; its error
figures are not accuracy estimates for this board. [Ranacher 2016](https://pubmed.ncbi.nlm.nih.gov/27019610/),
[Gilgen-Ammann 2020](https://mhealth.jmir.org/2020/6/e17118/).

**Implementation choices inferred from that evidence:**

1. Keep latitude/longitude as `double`, as the existing `hal_gps` does. The
   float32 diagnostic journal is unsuitable as the authoritative workout route.
2. Require a valid, finite, in-range, fresh fix. Distinguish module presence from
   fix validity; an absent module and a valid zero coordinate are different cases.
   HDOP is dimensionless satellite geometry, not a distance accuracy in metres.
3. Process an observation once. A render frame or a checksum-valid unrelated NMEA
   sentence is not a new position. The current HAL publishes both GGA and RMC and
   increments `sentences` for all valid sentences. Prefer a position acquisition
   sequence/timestamp; otherwise use fix time plus a minimum observation interval
   and test duplicate GGA/RMC observations explicitly.
4. Sum great-circle distances between accepted observations within a continuous
   segment. Reject implausible jumps. Clear the previous-position baseline after
   pause, invalid data, a long sampling gap, or a rejected discontinuity; the first
   good point establishes a new baseline rather than adding a straight connector.
5. Use a short time window or low-pass filter for displayed pace/speed. Keep
   rendering smoothing separate from accumulated distance. Never integrate the
   same cached receiver speed every frame as if it were a fresh observation.
6. A stationary noise gate can reduce drift, but can also erase slow walking and
   tight turns. Retain rejected-point counts and validate that tradeoff with replay
   fixtures. A Kalman filter without a measured error model is not automatically
   more trustworthy than a transparent bounded filter.

Example starting parameters for simulator tests are a 3-second freshness limit,
a 5-second maximum joined gap, and positive HDOP at most 5. Sport-dependent jump
limits should allow plausible sprint/downhill movement. These numbers are
engineering defaults requiring field calibration; none is established by the
papers above. Do not present accepted points as centimetre-accurate ground truth.

The implemented GPS average pace uses valid GNSS seconds divided by accepted
kilometres; GPS average speed uses accepted distance divided by valid GNSS time.
This is a deliberate engineering choice to exclude unmeasured gaps from the
denominator. The UI labels these as GPS averages and exposes valid GPS time;
active session time remains separate. If coverage is incomplete, the distance is
partial, and the GPS average does not describe the complete session. Current pace
is unavailable during a gap and when speed is too small for a meaningful reciprocal.

## Wrist heart rate

**Evidence.** Shcherbina et al. compared seven wrist devices with telemetry and
indirect calorimetry in 60 volunteers. HR generally performed better than energy
expenditure, but error varied with device, task, and participant. This does not
validate this watch's MAX30102 implementation. TROIKA demonstrates that running
PPG is strongly affected by motion and evaluates a specific denoising/spectral
tracking method on 12 subjects; its reported performance cannot be transferred to
an unrelated sensor/algorithm. [Shcherbina 2017](https://pubmed.ncbi.nlm.nih.gov/28538708/),
[Zhang, Pi and Liu 2015](https://arxiv.org/abs/1409.5181).

**Implementation decision:** show HR only when the sensor is present, on-wrist,
within a plausible numeric range, and above an explicit quality threshold. Invalid
and missing samples are not zero BPM and must not enter the average or maximum.
Weight the average by short valid recording intervals and expose valid-HR coverage;
do not extend one accepted sample through a long dropout or pause. The present
`hal_hr` originally had no acquisition timestamp, so quality alone could not prove
sample freshness. This implementation adds `sample_ms` at successful producer
acquisition and rejects stale/repeated readings. The producer also clears an old
beat estimate after three seconds without a detected beat. These gates improve
data handling; they do not validate exercise HR accuracy.
Do not infer individualized HR zones, recovery, VO2max, or calorie expenditure from
uncalibrated HR. Simulator traces validate logic, not physiological accuracy.

## Calories and activity intensity

**Evidence.** The 2024 Adult Compendium contains 1,114 activities, 912 with measured
MET values, and is tailored to ages 19-59. Its authors provide separate resources
for older adults and wheelchair users. The official methods note that Compendium
METs were not designed to determine precise energy expenditure for an individual.
In Shcherbina's validation, no tested wrist device achieved energy-expenditure
error below 20%. [Herrmann et al. 2024](https://doi.org/10.1016/j.jshs.2023.10.010),
[Compendium methods](https://pacompendium.com/corrected-mets/),
[Shcherbina 2017](https://pubmed.ncbi.nlm.nih.gov/28538708/).

**Initial decision:** a useful workout mode does not require a calorie number.
Leave it unavailable without an explicit body mass and defensible intensity
choice. The existing daily `0.04 kcal/step` estimate must not become a cycling,
indoor, or total-energy model, and must not be added again to workout estimates.

If MET estimates are later enabled, save the selected code, MET, body mass, active
duration, and algorithm version. The standard oxygen-cost conversion is
`gross kcal = MET * 3.5 * mass_kg / 200 * active_minutes` (assuming 5 kcal/L O2).
Subtract the 1-MET component only if deliberately reporting **net/active** kcal;
the two definitions differ. Display whole-number estimated kcal and name the
method. A category alone does not establish intensity: the tables distinguish
walking for pleasure (17160, 3.5 MET), cross-country hiking (17080, 6.0 MET), and
cycling at self-selected easy versus vigorous pace (01015, 4.3; 01017, 9.0 MET).
These examples illustrate model inputs, not recommended exercise intensities.
[Walking table](https://pacompendium.com/walking/),
[Cycling table](https://pacompendium.com/bicycling/),
[official calculation resources](https://pacompendium.com/compendium-calculator/).

## Records, resources, and verification

Store session ID, profile, state, start/end UTC validity, monotonic durations,
accepted distance, route segments, laps, steps source, HR coverage, quality/reject
counts, and summary version. Preserve retryable unsaved state after storage
failure. A successful return from a queue insertion is not proof of durable save.
Use the workspace's SD lock and mount lease and keep settings' alternating,
checksummed checkpoint; do not substitute a new settings persistence path.

Keep acquisition and service updates bounded, around sensor cadence (normally
1 Hz for GNSS), independently of 30 FPS rendering. Use bounded route/lap buffers
with an explicit full condition, or stream records; never silently overrun memory.
The existing raw journal can already exceed 0.8 GB/day for IMU alone, so a workout
must not imply that its complete raw stream is always retained.

Deterministic simulator acceptance cases should cover pause/resume, leaving and
returning to the app, repeated finish/save taps, midnight and wall-clock changes,
monotonic wrap, known-distance routes, stationary jitter, duplicate fixes,
teleport/out-of-range/NaN coordinates, stale/poor-quality data, GNSS loss and
reacquisition, missing/off-wrist HR, irregular HR intervals, route/lap capacity,
storage failure followed by retry, and reloading a saved result. A no-sensor
indoor workout must remain useful as a timer with laps. Verify 30 FPS motion and
blur in the simulator separately; it cannot prove physical frame timing, battery
life, GNSS accuracy, step accuracy, or on-wrist HR performance.

Inspected open-project sources, pinned to the last change to each file:

- [OpenTracks recording service, a6d318d](https://github.com/OpenTracksApp/OpenTracks/blob/a6d318d970e3297ada13d6d788132b4d5ad062e6/src/main/java/de/dennisguse/opentracks/services/TrackRecordingService.java):
  state, sensor lifetime, one-second UI snapshots, marker creation.
- [OpenTracks statistics updater, 1ed8070](https://github.com/OpenTracksApp/OpenTracks/blob/1ed807054bf37b1f8867c15dbda1f91c3de32e65/src/main/java/de/dennisguse/opentracks/stats/TrackStatisticsUpdater.java):
  segment boundaries, distance validity, time-weighted HR. This implementation is
  an architectural comparison, not a prescription to bridge missing HR samples.
- [InfiniTime stopwatch controller, e53e4ed](https://github.com/InfiniTimeOrg/InfiniTime/blob/e53e4ed23c54b6ecdd32691a618912b9158161fe/src/components/stopwatch/StopWatchController.cpp):
  monotonic elapsed time, pause accumulation, bounded lap history.

OpenTracks' GitHub landing page now points to Codeberg. The pinned GitHub sources
above were readable during this research and make the inspected revision explicit.
