# Sports sessions

Open **Sports** from Apps, choose a profile, then press **START**. The outdoor
profiles show GPS readiness first. **START ANYWAY** begins the active timer without
a GPS fix; distance and pace appear only when accepted GPS data is available.
The SD history must finish loading before the first session can start. If the card
is unavailable, the ready screen shows **INSERT CARD THEN RETRY** and offers
**RETRY**. This preserves existing card history instead of replacing unread data.
If both saved journal copies are corrupt, Sports preserves them and asks for a
valid SD backup before the recorder can initialize.

| Profile | Recorded session metrics |
| --- | --- |
| Walk | Active time, accepted GPS distance and pace, steps, cadence, valid heart rate |
| Run | Active time, accepted GPS distance and pace, steps, cadence, valid heart rate |
| Cycle | Active time, accepted GPS distance and speed, valid heart rate |
| Hike | Active time, accepted GPS distance and pace, steps, cadence, valid heart rate |
| Treadmill | Active time, steps, step cadence, valid heart rate |
| Workout | Active time and valid heart rate |

The indoor profiles do not estimate distance from steps. Cycling does not infer
pedal cadence from wrist steps. Unsupported or unavailable measurements display
`--`; there are no calorie, repetition, swimming, VO2 max, or heart-rate-zone claims.
The existing Units setting controls kilometres and km/h or miles and mph, with
pace shown per kilometre or per mile. Stored session measurements remain metric.

During a session, **LAP** marks a cumulative split and **PAUSE** stops active time.
The paused screen offers **RESUME** and **FINISH**. Finishing requires a second
confirmation and automatically includes the final partial lap when space remains.
The recorder keeps at most 20 splits. Paused time is excluded from active time and
shown separately through total duration in **DETAILS**. There is no automatic pause.

Swiping back, returning Home, opening another app, and viewing details leave the
session running. Reopening Sports returns to its live or paused screen. Tap the
lap/details footer to inspect a snapshot of the current session without stopping
it. The session continues while that snapshot is open.

After finishing, the summary distinguishes saving, saved, and save failure. Only a
verified SD write is labelled **SAVED TO SD**. A failed save retains the session in
memory and offers **RETRY**; it does not permit another workout to overwrite the
result. Closing the app leaves this pending result available when Sports reopens.
Use **DONE** after a successful save to choose another sport.
Replacing the SD card during this boot does not authorize overwriting a different
workout journal. If Sports asks for the **ORIGINAL SD**, return that card and retry.
An already initialized recorder can continue a session in RAM while it waits.

All six sport profiles appear together on the selector. **Recent workouts** is the
button beneath them. It holds the latest 12 sessions and shows each session's save
status. Tap a session for active time, total duration, supported averages, quality coverage, the UTC
start time, and lap splits. Averages for outdoor pace and speed use accepted GPS
time; gaps are not treated as measured zero speed. Distance can be partial when
GPS is unavailable, weak, stale, or rejected by the quality filters.

The recorder checkpoints active sessions through the SD writer. A recovered active
checkpoint reopens paused, with **RECOVERED / PAUSED** on screen. Recovery reflects
the last successfully verified checkpoint and can lose activity since that point.
The Sports session files live separately from settings and do not replace the
authoritative alternating `/watch/settings.0.bin` and `/watch/settings.1.bin` files.

This implementation is exercised in the desktop simulator; no watch is connected
and no firmware is flashed for this work. GPS and wrist heart-rate thresholds are
engineering defaults, not a claim of sensor accuracy. The evidence and relevant
limitations are documented in [Sport mode research](research/SPORT_MODE_RESEARCH.md).

## Run and verify in the simulator

```powershell
.\sim\nos_build.ps1 -Strict -Out nos_sports.exe
.\build\nos_sports.exe --open sports --cost-hud
.\tools\test_workouts.ps1 -SkipBuild
python tools/test_workout_fuzz.py --seeds 100 --actions 1000
```

The simulator and the six-profile selector use the same portable recorder as the
target build. Interactive workout files are under `build/nos_out/workouts` by
default; scripted checks isolate their own stores. Pass `--workout-store DIR` to
choose a persistent simulator store. Test coverage and timing limits are in
[Sports simulator validation](SPORT_MODE_VALIDATION.md).

## Export saved summaries and laps

On SD, workout checkpoint banks are `/watch/workouts/workouts.0.bin` and
`/watch/workouts/workouts.1.bin`. Copy both files together or point the exporter at
their directory. It chooses the newest valid bank and warns if it must fall back.

```powershell
python tools/workouts_export.py E:/watch/workouts --out workouts.json
python tools/workouts_export.py E:/watch/workouts --out workouts.csv --laps-out laps.csv
```

Exports retain UTC, active and observed duration, SI measurements, valid-sensor
coverage, and cumulative and individual lap splits. Missing metrics stay empty or
null. Use `--include-checkpoint` to include an unfinished session explicitly. The
exporter reads the original files without changing them and refuses to overwrite
an existing output. This version saves session summaries and laps; it does not
save a workout route or produce a workout GPX file.
