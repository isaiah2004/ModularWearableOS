# Sports update

Implemented and built from this recovered `smartwatch2.0` workspace, preserving
its recovered source state, integrated home pager, Control Center paging,
Solar/Orrery, Developer Tools and sports apps. Current host and firmware checks
are recorded in [validation](SPORTS_RELIABILITY_VALIDATION.md). This build was
flashed on 21 October with preferences checkpointed and NVS excluded.

## Workout recording

Sports offers 40 selectable profiles. Choosing a sport labels the session; it
does not mean an automatic classifier recognized that sport.

| Shared measurement family | Profiles |
|---|---|
| Outdoor locomotion | Walk, run, hike, trail run, trekking, race walk, Nordic walk |
| Indoor locomotion | Treadmill, indoor walk, stairs |
| Cycling | Cycle, mountain bike, gravel bike, indoor cycle |
| Racket | Badminton, tennis, table tennis, squash, pickleball, padel |
| Team/field | Football/soccer, cricket, basketball, volleyball, handball, field hockey, rugby |
| Conditioning | Workout, strength, HIIT, circuit, aerobics, dance, jump rope, boxing fitness |
| Controlled movement | Yoga, Pilates, stretching |
| Row/paddle | Indoor rowing, kayak |

The first six profile IDs remain compatible with existing records. The catalog,
ready screen, live session, pause/finish flow, saved history and detail pages use
the background session service. Leaving Sports or sleeping the display does not
finish a workout. Manual pauses exclude that time from recording time; total
observed duration is separate. Laps and history are bounded.

All profiles can record time and valid heart rate. Outdoor profiles use accepted
GNSS observations for distance and speed/pace; missing fixes leave coverage gaps.
Step-enabled profiles show session steps/cadence when available. Indoor cycling
does not invent watts or pedal cadence, and treadmill does not invent belt
distance. Profile-family speed outlier gates now allow team-sport sprints and
new cycling profiles while retaining freshness, jump and gap checks.

Fresh wrist IMU samples feed an orientation-insensitive movement engine independently
of UI refresh. It reports estimated movement time, valid coverage and movement
bursts. These are wrist movement episodes, not shots, rallies, repetitions,
shuttle/ball speed, impact force or measured exertion. Non-playing-wrist motion
is useful evidence but does not reproduce the racket hand's kinematics.

## Wrist, playing hand and calories

WRIST + HAND SETUP keeps worn wrist, dominant hand and sport playing hand
independent. Cricket additionally separates batting and bowling hands. These
settings, auto-resume choice, entered mass and category MET are frozen in each
session rather than rewritten when preferences change later.

Manual worn-wrist selection remains available. Experimental guided detection
requires learning references on BOTH wrists: screen up, 12 o'clock toward the
elbow, hold still, rotate the palm upward once, then hold still again. It matches
that guided gesture to saved references and requires explicit confirmation. It
abstains for missing/overlapping references, sensor gaps, unsuitable poses or poor
matches. This is not passive anatomical detection, a validated accuracy score or
a way to infer the playing/dominant hand. Reversing the strap can create ambiguity;
keep the learned orientation and confirm the suggestion.

The user confirmed BOTH copied `0002.csv` and `0005.csv` are badminton played with
the RIGHT hand while the watch was on the LEFT wrist. Dominant hand was not stated.
Exact C replay with unchanged parameters estimates 68.040 seconds/15 movement
episodes in 254.040 seconds for 0002, and 223.760 seconds/8 episodes in 267.760
seconds for 0005. Their original SHA-256 hashes are unchanged. Reports, sidecars
and labeled plots are in
`build/sports-reliability/audit-confirmed-badminton-v2/`. These unlabeled movement
traces establish signal plausibility, not shot recognition or sport accuracy.

Workout calories require a user-entered mass; the default is unset. They are
labeled GROSS CATEGORY ESTIMATE and use a frozen broad category MET:
`kcal = MET * 3.5 * mass_kg / 200 * observed_running_minutes`.
Manual pauses and reboot downtime are excluded. Unpaused waiting/recovery still
uses the category assumption and can bias the estimate substantially. Motion
bursts and heart rate do not derive or personalize these METs. Some profiles use
explicitly documented proxies. See the 40-row source/code/assumption table and
primary references in [sports research](research/SPORTS_EXPANSION_RESEARCH.md).

The separate Energy app retains its existing daily step estimate of 0.04 kcal
per detected step, food entries and water amounts. That daily step estimate and
the workout gross category estimate have different definitions; they are not a
measured total daily energy expenditure. Food/water corrections retain history.

## Recovery, settings and diagnostics

Workout checkpoints alternate checksummed SD banks with fsync/readback verification
and the existing mount lease/SD lock. Disk V2 freezes placement, movement, energy
assumptions and recovery count. Explicit V1 migration preserves existing records;
old sessions recover conservatively paused. Changed/foreign banks are protected,
and unsuccessful saves stay dirty for retry instead of reporting success.

Auto-resume defaults on for new sessions. After restart, a previously running
session resumes only if its frozen auto-resume setting permits it; a paused
session stays paused. Reboot downtime and sensor continuity are excluded, and a
recovery diagnostic identifies the transition. Periodic checkpointing still means
uncheckpointed recent data can be lost in a sudden reset.

The separate raw IMU recorder supports 25/50/100 Hz CSV acquisition with an
asynchronous SD writer, bounded queue and checked recording-intent banks. Active
intent resumes into a NEW linked segment after restart/retry; an existing CSV,
including a torn tail, is preserved. Segments roll over at the configured size.
Stopping must persist the inactive intent before success is acknowledged. Failed
storage stays visible and retryable. Task creation is serialized, and rollover
stops acceptance before its final queue drain. Missing/full/faulty storage can
still lose samples; counters and errors expose that limitation.

`tools/imu/recover.py` creates a separate recovered copy of complete rows;
`tools/imu/audit.py` audits/replays copies without altering originals. SD recording
is not supported during deep sleep/power-off.

Development diagnostics use a bounded PSRAM queue, checksummed binary records,
eight rotating files and RTC breadcrumbs for supported warm resets. Boot/reset
context, navigation/actions, power/source/heap observations, workout recovery,
recorder lifecycle, queue drops and storage errors are recorded. RTC breadcrumbs
do not survive loss of power. Diagnostics are best effort and bounded; logging
itself consumes power. Decode copied files with `tools/diag_export.py`.

Settings remain authoritative on `/watch/settings.0.bin` and
`/watch/settings.1.bin` on SD, loaded into RAM at boot. Alternating checksums,
fsync/readback, mount lease and SD locking remain. NVS is a migration/card-absent
fallback; failed SD saves remain dirty. The firmware flash workflow must preserve
NVS and checkpoint existing preferences.

## Power and history

Walking and recording remain background services: they do not wake the display
or prevent its inactivity timeout. Normal wake sources are touch, button, enabled
raise and knock. Sensor idle uses configured 80 MHz after its entry transition,
with a dim face refreshed at 1 Hz; active rendering uses configured 240 MHz.
Display-off continues servicing background work, and alarms/calls/timers can
request an awake screen. Deep sleep stops recording.

Low-battery policy enters at a fresh valid battery reading of 15% or below and
clears at 20% or external power. It suppresses raise/knock wake and chooses
display-off while preserving touch/button wake and the user's saved AOD/sensor
preferences. Invalid, stale or USB-influenced percent estimates do not invent a
new low-battery decision. CPU power-management failures remain errors, not
successful throttling claims.

Battery history and Energy fetch bounded history batches asynchronously and show
loading/unavailable states; chart drawing no longer synchronously scans a large
day file. Level/voltage history respects validity and distinguishes external
power. The board measures a system rail, not battery current: USB can invalidate
percentage, and there is no measured charging current, mWh consumption or reliable
remaining-runtime estimate. Battery-life improvements still require device trials.

### Recovered battery evidence

The verified 20 October journal contains 122,407 checked records (122,261 unique,
146 identical duplicates), with no integrity errors or truncated tail. During
19:30–21:54 IST the CSV rail fell from 4.020 V to 3.564 V, with a 3.554 V minimum.
Within 139.14 minutes of bounded journal coverage, SENSOR_IDLE accounted for
97.95 minutes and ACTIVE for 41.01 minutes; the logged 80 MHz CPU ceiling covered
97.76 minutes. This records the requested ceiling, not instantaneous frequency.
The idle panel remained
at least 128/255 for 90.96 minutes. No OFF/deep-sleep state was observed in that
decline. All 37 valid battery observations at 15% or below remained ACTIVE or
SENSOR_IDLE, down to a reported 4%, supporting the new dark-screen policy.

Badminton session 0005 has 267.76 seconds of journal coverage: 213.73 seconds
ACTIVE and 54.03 seconds SENSOR_IDLE, with the idle panel almost entirely at
165/255. Its five valid battery observations were 9–12%. Morning session 0002
predates journal coverage, so equivalent power-state comparisons are unavailable.
Durations exclude boot boundaries and evidence gaps; CPU values are configured
ceilings. Rail voltage and no-detected-external-power flags do not establish
capacity, current, full-to-empty runtime or physical unplugging. Legacy logs omit
reset reasons, so short runs and boot IDs are not identified as crashes. Reports,
JSON and PNG/SVG plots are under
`build/sports-reliability/power-analysis-verified/`; readable findings are in
`build/sports-reliability/power-findings.json`.

Frame pacing now checks timer-arm errors and bounds callback waits, clears stale
notifications, and rebases pacing after a failure. `FRAME_PACE` diagnostics record
the requested delay, error and failure phase. Host fault-injection tests cover
arm failure, missing callbacks and stale signals. This hardening does not establish
the cause of the previously reported catalog hang or prove it fixed.

Control Center page two and the integrated home pager remain available. Quick
reports, sensor journals and wrist tuning retain their existing labeled-trial
workflows; collecting labels does not automatically train a new classifier.

## Exports and device acceptance

Copied workout banks export with `tools/workouts_export.py` (V1/V2 JSON or summary
and lap CSV); sports journals use `tools/sports_export.py`; diagnostics use
`tools/diag_export.py`. Outputs preserve validity/missing-data distinctions and
refuse overwriting existing exports. See each tool's help for arguments.

The final firmware passed compilation and partition fit and is on the watch.
A clean 600.06-second device recording at 100 Hz crossed a production 4 MiB
rollover without a reset. An intentional hardware reset resumed into a new
CSV; STOP remained inactive after a second reset. Preserved app navigation and
repeated Battery History openings were exercised. See the validation report
for copied-file integrity results and transport/debugger caveats.

The combined raw-recording and USB-navigation capture missed the 30 FPS target
(presentation intervals p95 59 ms, p99 72 ms). Labeled wrist/sport trials, physical
SD-fault trials and unplugged battery endurance remain unverified. Host fault
injection and a short connected device run do not establish these outcomes.
Future builds and flashes must still use this workspace and the ESP-IDF
environment with `tools/flash_watch.py --port COM3` or
`tools/build.ps1 -Port COM3 -Flash`, preserving SD checkpoints and excluding NVS.
