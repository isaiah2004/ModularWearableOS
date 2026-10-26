---
title: "Calorie trace"
tags: [nos, os-map]
updated: 2026-10-23
---

# Calorie trace

## Summary

Trace of three complaints from a BADMINTON session recorded on the 1.43-B on 2026-10-23. All three come from design gaps, not from a crash or a lost recording. All paths are repo-relative. Scope was read-only: no build, flash or serial access.

| # | Complaint | Root cause (one line) |
|---|---|---|
| 1 | Recording calculated no calories | `workout.c` only computes `energy_kcal_est` when `setup.mass_kg != 0`. The default body mass is 0 (unset). The Sports UI never prompts for it at Start. Even when it is computed, it is shown only deep in the DETAILS list. |
| 2 | No activity graph for the game | No per-time data structure exists for a session. `workout_snapshot` holds scalars plus up to 20 manual laps. No Sports screen draws a chart. The only activity chart is the whole-day hourly STEP histogram, and the pedometer rejects badminton arm and footwork motion. |
| 3 | Daily calories lacked passive (basal) calories | Nothing in the code computes BMR. The daily total is `steps * 0.04` (`svc.c:445`) and is labelled "ACTIVITY CALORIES ONLY". The watch stores no age, sex or height, and only an optional body mass. Workout energy is not merged into the daily total either. |

What survives on the SD card (for a retroactive fix):
- The finished session summary, in `/sdcard/watch/workouts/workouts.{0,1}.bin`.
- Raw 6-axis IMU records at about 150 Hz, plus heart-rate records if an HR strap was fitted, in `/sdcard/watch/sports/20261023-HH-sensors.spt`.
- A per-minute cumulative step, kcal and active-minute record in `/sdcard/watch/sports/20261023-events.spt`.

Together these are enough to recompute kcal and draw a per-minute intensity graph for this game. The user's age, sex, height and weight must be supplied, because the watch holds none of them (except an optional mass).

---

## Complaint 1: "The recording did not calculate any calories"

### What happens (call graph)

1. The Sports app is `scr_workout` (`nos/apps/fit_workout.c:1066`), registered as "Sports" (`nos/apps/registry.c:10`).
2. `ready_enter` and `ready_input` call `configure_session()` (`fit_workout.c:102-115`). It builds a `workout_setup` with `.mass_kg = nos_prefs.sports_mass_kg` and passes it to `workout_setup_set()`.
3. `workout_start(WORKOUT_BADMINTON)` (`nos/workout.c:813-831`) does `current.setup=next_setup` and `current.met_tenths=profiles[sport].met_tenths`.
   - BADMINTON is `{"BADMINTON","TIME / HEART + MOVEMENT",0,0,0,3,55}` at `workout.c:65`. That means indoor, no steps, family RACKET, 5.5 MET.
4. `workout_tick()` runs every `svc_tick` (`nos/svc.c:1321`) and calls `advance_time()` (`workout.c:268-280`):
   ```c
   current.energy_kcal_est = current.setup.mass_kg && current.met_tenths ?
       (float)met_tenths*0.1f*3.5f*(float)mass_kg/200.0f * (elapsed_ms/60000.0f) : 0;
   ```
5. If `mass_kg == 0` the result is exactly 0 and nothing else computes energy.

### Why the user saw nothing

- **Mass is never defaulted.**
  - `next_setup` defaults to `{0,0,0,0,1,0,0}` (`workout.c:123`); `mass_kg` is the sixth field and is 0.
  - `prefs_defaults()` (`nos/prefs.c:134-`) never sets `sports_mass_kg`, and it stays 0 via `memset`. `normalize()` forces it to 0 for `rev<3` (`prefs.c:175`).
  - Mass is entered only at Sports > pick sport > "WRIST + HAND SETUP" > "BODY MASS / ENERGY ESTIMATE", which shows "Not set / calories unavailable" (`fit_workout.c:631-635`).
  - The mass editor is `mass_input` (`fit_workout.c:711-727`).
  - The READY screen does not mention it. It reads "ACTIVE TIME + VALID HEART RATE" (`fit_workout.c:524`).
- **Calories are hidden even when computed.**
  - The live screen (`live_draw`, `fit_workout.c:349-390`) has no kcal field.
  - `summary_draw` (`fit_workout.c:391-419`) and the history rows (`history_row`, `fit_workout.c:845-855`, which show duration and laps) have none either.
  - The only display is `detail_enter` (`fit_workout.c:1013-1019`), guarded by `if (s->energy_kcal_est > 0)`. With mass 0 the rows are not rendered at all, with no "unavailable" row.
- This is intentional per the docs. `docs/SPORTS_UPDATE.md:69` says "Workout calories require a user-entered mass; the default is unset." `tools/test_workout_sim.py:62-64` and `tests/workout_test.c:554-555` assert kcal is 0 without mass.
- **Heart rate is never used for energy.**
  - `sample_heart` (`workout.c:314-354`) accumulates `hr_integral`, `hr_active_ms`, `avg_bpm` and `peak_bpm`. `averages()` (`workout.c:258-267`) turns these into `avg_bpm`.
  - Nothing consumes them for energy.
  - `energy_kcal_est` is a pure category-MET x time estimate. It has no intensity term, which is also why the docs warn "Waiting time may overestimate effort" (`fit_workout.c:1018`).
- Heart-rate hardware is an optional MAX30102 strap module (`firmware/main/ext/ext_hr.c`, `EXT_HR`).
  - Without it, `hal_hr_get()` returns 0 when `H==NULL` (`ext_hr.c:343-347`). Every second `sample_heart` then rejects with `WORKOUT_REJECT_HR_MISSING` (`workout.c:318`).
  - With it fitted, wrist PPG during badminton is mostly motion-corrupted. The gates are quality >= 0.65, a spike gate of >30 bpm needing 3 confirmations, and a freshness limit (`workout.c:314-342`). Expect poor HR coverage either way.
  - Whether the strap is fitted is not provable from source. The saved session shows it: `hr_active_ms`, `avg_bpm` and `quality.rejected[HR_MISSING]` (`workout.h:57-68`, `workout.h:64-68`).
- This session's actual mass setting cannot be confirmed from source. The overwhelmingly likely state is mass unset, so energy is 0 and the session shows "Unavailable" for heart rate if no strap.

### Data available on SD (retroactive)

| What | Path (card root) | Format | Use |
|---|---|---|---|
| Session summary, last 12 sessions | `/watch/workouts/workouts.0.bin` and `.1.bin` (alternating, higher `sequence` wins) | `disk_record`, 4304 B, LE, FNV-1a at offset 4296 (`workout.c:98-111`, `workout.h:13-22`). The header is 6 x u32 (magic `0x574f5331`, version 3, bytes, sequence, count, next_id) + u64 `hr_integral` at 24. `session` is at 32 and `history[0..11]` follows at 32+328*(1+i). | `sport`=8 (BADMINTON), `started_epoch`/`ended_epoch` (UTC), `elapsed_ms` (active time), `total_ms`, `avg_bpm`, `hr_active_ms`, `movement_ms`, `movement_bursts`, `setup.mass_kg` (snapshot byte 241), `met_tenths` (243), `energy_kcal_est` (256), `quality.rejected[]` (268) |
| Parser | `tools/workouts_export.py <dir> --out x.json` (`--laps-out`) | JSON or CSV. It exports `mass_kg`, `energy_kcal_est`, `category_met`, `movement_seconds_est` and others. | Confirms mass=null and kcal=null for the session |
| Raw 6-axis IMU, about 150 Hz | `/watch/sports/20261023-HH-sensors.spt` (HH = local hour, 24 files/day) | 64-byte `sports_record` (`nos/sports.h:14-18`): `<6I2H8fI` = magic `0x53505431`, boot, seq, uptime_ms, epoch, day, minute, type, `v[8]`, FNV-1a. `type=14` (SP_IMU): `v0..5` = ax, ay, az (g), gx, gy, gz (dps). | Per-minute intensity and graph (see Fix) |
| HR records | same sensors files, `type=18` (SP_HEART) | `v0`=on_wrist, `v1`=bpm, `v2`=spo2, `v3`=quality. Emitted at about 8 Hz while the HR task runs (`ext_hr.c:296`). Raw red/IR `type=21` (SP_PPG) at 100 Hz (`ext_hr.c:284`). | Keytel kcal, if the strap was present and the signal good |
| Per-minute rollup | `/watch/sports/20261023-events.spt` | `type=4` (SP_ACTIVITY): `v0`=steps today, `v1`=`steps*0.04` kcal today, `v2`=active_min. Written once per minute (`sports.c:384-391`). | Step progression during the game window |
| Minute IMU snapshots | `/watch/logs/*.csv` (`firmware/main/board.c:713-748`) | One row/min: `utc_epoch,uptime_ms,system_mv,imu_valid,ax..gz`. Too sparse for intensity. | Not useful |
| Prefs | `/watch/settings.{0,1}.bin` (`nos/prefs_disk.c:16-20`): a 16 B header (magic `0x4e4f5350`, format 1, sequence, size), then the 52-byte `nos_prefs_t`. `sports_mass_kg` is at struct offset 36, so file offset 52, in the bank with the higher sequence. | | Check whether mass was ever set |

Exporter and retrieval:
- `tools/sports_export.py /path/watch/sports --day 20261023 --start HH:MM --end HH:MM --out x.csv` (`tools/sports_export.py:8-9`). It decodes IMU, HEART and ACTIVITY.
- Files can be pulled with `python tools/sdsync.py --pull /watch/sports --out <dir>` (`tools/sdsync.py` usage header).

### Minimal correct fix

The right formula depends on what data the session has:

- **HR available (quality-gated, bpm >= about 90):** Keytel et al. 2005, gross kcal/min:
  - male = (-55.0969 + 0.6309*HR + 0.1988*kg + 0.2017*age) / 4.184
  - female = (-20.4022 + 0.4472*HR - 0.1263*kg + 0.074*age) / 4.184
  - Integrate per accepted 1 s sample (use the existing dt gating in `sample_heart`, `workout.c:344-349`).
  - Example for a 70 kg, 30-year-old male at 140 bpm: about 12.7 kcal/min. Clamp at >= 0 and use only for bpm >= about 90 (below that Keytel under-estimates).
- **Otherwise (HR absent or rejected), use the MET fallback:**
  - `kcal = MET x 3.5 x kg / 200 x minutes` (`workout.c:276-278` already does this at the flat category MET 5.5).
  - Better: scale the MET by the minute's movement fraction. Use `workout_motion` `movement_ms` per minute to interpolate between about 2.0 (standing) and 5.5-7.0 (play).
  - Example: 70 kg x 5.5 MET = 6.74 kcal/min, about 404 kcal per hour.
- **Required code changes:**
  1. Default and prompt for mass. Seed `sports_mass_kg=70` flagged "estimate", or refuse Start with a prompt. At minimum, show "KCAL --  SET BODY MASS" on `ready_draw`, `live_draw` and `summary_draw`, and always add the ENERGY row in `detail_enter`.
  2. Add profile fields (see Complaint 3).
  3. Replace `advance_time`'s closed-form estimate with integration in `metrics()` (`workout.c:461-467`, 1 Hz).
  4. Display kcal on `live_draw`, `summary_draw` and `history_row`.
  5. Bump the disk format: add the algorithm id, kcal split (HR vs MET seconds) and frozen age/sex/height to the snapshot. Use the V1/V2 -> V3 in-place expansion pattern at `workout.c:542-562`.
  6. Make `sample_steps` run for every sport so a session-step delta exists. `workout.c:288` skips it when `uses_steps==0`.

**Retroactive recompute for today's game:**
1. Pull `/watch/workouts` and run `workouts_export.py`. Read `started_epoch`, `ended_epoch`, `elapsed_ms`, `avg_bpm` and `hr_active_ms`.
2. Pull `/watch/sports` and run `sports_export.py --day 20261023`. Filter `type==HEART` and `quality>=0.65`, and apply Keytel.
3. For uncovered time, apply the MET fallback with per-minute movement from `type==IMU`.

---

## Complaint 2: "There is no activity graph to see how much I moved during the game"

### What happens

- **Sports (`fit_workout.c`)** has no chart code at all. Its screens are the live view, summary, details list (`detail_enter`, `fit_workout.c:955-1040`), history list and setup. Only scalars are shown.
- **`workout_snapshot` (`nos/workout.h:71-96`)** stores:
  - totals: `elapsed_ms`, `steps`, `distance_m`, `hr_active_ms`, `avg_bpm`, `peak_bpm`;
  - motion aggregates `motion_valid_ms`, `movement_ms` and `movement_bursts` (accumulated by `workout_motion_feed`, `nos/workout_motion.c:9-52`);
  - up to 20 manual laps `{elapsed_ms, distance_m}`.
  There is no time series and no per-minute bucket.
- **The per-session IMU feed** is `imu_take()` -> `workout_motion_sample()` (`firmware/main/board.c:300-302`). It folds only into counters; each sample is discarded.
- **The only graph that exists for movement is the Activity app's hourly STEP histogram.**
  - It comes from `svc_activity().hourly[24]` (`fit_activity.c:414-416`, `:470`).
  - The values are `act_tick` step deltas per clock hour (`svc.c:529-566`).
  - It cannot show a game's intensity because:
    - It is whole-day, hourly, and step-only.
    - The pedometer (`core/walk.c`) ends the bout and sets `valid=0` whenever gyro magnitude exceeds 250 dps (`walk.c:78`). It needs 8 regular peaks 300-1500 ms apart with CV <= 0.25 (`walk.c:9-10`, `:46-61`). Badminton swings and irregular court footwork break both, so steps are about zero.
    - `active_min` increments only for minutes with more than 60 steps (`svc.c:553`).
- **Other charts do not help.**
  - The Energy app chart (`fit_energy.c:97`, `draw_chart`) plots `sports_bin.burn`, the latest cumulative `steps*0.04` per 24 bins (`sports.c:409-410`). It is a monotone staircase labelled "BURNED: DAILY ESTIMATE", with no per-interval activity.
  - The Heart app's 24 h history is one background sample every 10 minutes (`ext_hr.c:34`, `svc.c:595-613`).
  - The IMU app records raw CSV only when the user starts it (`nos/apps/out_imu.c:184`, `hal_imu_record_start`). It is a separate recorder, unrelated to Sports.

So the app cannot show how much was moved during the game, even though the raw data exists on SD.

### Data available on SD (retroactive)

- **The 64-byte `sports_record` raw IMU stream is always on.**
  - `sports_imu()` is called unconditionally by `imu_take()` (`board.c:300`) at every raw read. The 20 ms board loop reads three times (`board.c:988-1030`), giving about 150 records/s.
  - It is queued as a raw type (`sports.c:56`) and drained by `task-sports-sd` (`sports.c:215-300`).
  - Files are rotated hourly: `/sdcard/watch/sports/YYYYMMDD-HH-sensors.spt` (`sports.c:215-218`, `:255`). `HH` is the local hour. Records carry UTC `epoch` plus local `day`/`minute`.
  - The writer batches 512 records/s (`DRAIN_BATCH`). The queue has headroom of about 896 raw entries. If the SD stalls, `status.dropped` counts the lost records and gaps appear (Sensor Logs screen shows "LOST n").
- **The workout epoch window** (`started_epoch`/`ended_epoch` in `workouts.N.bin`) selects the right records.
- **HR records** (`type=18`) and **minute SP_ACTIVITY step counts** (events file) are on the same SD, as in Complaint 1.

### Minimal correct fix

- **Per-minute intensity buckets (recommended).**
  - In `workout.c` `metrics()` (1 Hz), accumulate for each minute: `movement_ms` delta (from `motion.movement_ms`), mean accepted BPM, kcal added, and steps.
  - Store them in a bounded structure. For example, a 48-entry bucket array in `workout_snapshot` (V4 append). When the session exceeds 48 buckets, merge pairs and double the bucket length.
  - This adds about 96 bytes per snapshot, which is cheap given the existing 4.3 KB checkpoint.
  - Alternative that avoids a disk-format bump: emit a new journal record type `SP_WORKOUT` (28, appended to the enum in `sports.h:10-13`) once per minute via `sports_emit`. It then goes to the events file with epoch and joins the existing export tooling. `sports_name`/`sports_field` in `sports.c:158-190` and `NAMES`/`FIELDS` in `tools/sports_export.py:12-34` must be extended.
- **Draw it.**
  - Add a bar chart using the existing `dot_bar` helpers (`fit_activity.c:470-514`) to `detail_enter`/`detail_draw`, and optionally a rolling live strip in `live_draw`.
  - Bar height is the minute's movement fraction; colour or an overlay shows BPM.
  - Chart axes: x = minute of session, y = movement fraction (0-1), optional line = BPM.
- **Per-minute intensity from raw IMU (retroactive or on-watch):**
  - For each second of IMU data, compute `|‖a‖-1|` and ‖g‖.
  - Classify with the existing `workout_motion` rule (`workout_motion.c:33`): moving if `|‖a‖-1| > 0.16 g (0.10 g when already active)` or `g > 55 dps (35 dps when active)`, with 300 ms entry and 600 ms exit hysteresis.
  - Plot the fraction of moving time per minute, or the minute's mean RMS acceleration.

**Retroactive graph for today's game:**
1. Pull `/watch/sports` and keep the sensors files covering `[started_epoch, ended_epoch]`.
2. Bucket `type==14` rows by `epoch//60`. Compute movement fraction, RMS accel and RMS gyro per bucket.
3. Optionally overlay `type==18` bpm (where `quality>=0.65` and `on_wrist==1`).
4. Render as a bar chart (host-side script; none exists yet).

---

## Complaint 3: "The daily calorie total did not include passive (resting/basal) calories"

### What happens

- **The only daily calorie number** is `svc_activity_t.kcal` (`nos/svc.h:93`), set in `act_publish()`:
  ```c
  s_act.kcal = steps * KCAL_STEP;   // svc.c:445, KCAL_STEP = 0.04f (svc.c:46)
  ```
  `steps` is the pedometer count for today. `act_publish` runs from `act_tick` once a second on the services path (`svc_tick`, `svc.c:1303-1321`). On the device that is `nos_bg_tick = services_tick` (`nos/apps/registry.c:62-71`), driven:
  - by the UI loop while the screen is on, and
  - at 1 Hz when the screen is OFF (`firmware/main/nos_main.c:2094`) or in AOD/sensor-idle (`nos_main.c:~2105-2120`).
  So it keeps running with the screen off.
- **There is no BMR code anywhere.** A repo search for `bmr`, `basal`, `mifflin`, `keytel`, `height`, `age`, `sex`, `weight` and `birth` in `nos/`, `core/`, `firmware/main` and `sim/` finds none.
- **Consumers of `kcal`:**
  - Activity app tile (`fit_activity.c:406`).
  - Energy app burn total: `burn=svc_activity()->kcal` for today (`fit_energy.c:144`). Footer text: "ACTIVITY CALORIES ONLY" (`fit_energy.c:156`).
  - Face segment ring against a 400 kcal goal (`nos/faces/face_segment.c:263-267`).
  - Background job "Calorie estimate", "Step-based estimate / 0.04 kcal per step" (`nos/background.c:24`, `:132`).
  - The per-minute journal record SP_ACTIVITY `v1` (`sports.c:384-391`).
  The Energy app aggregates the journal into 24 bins by taking the latest cumulative point (`sports.c:409-410`).
- **Workout energy is not merged into the daily total.** `docs/SPORTS_UPDATE.md:78-81` states the two are deliberately separate. `docs/research/SPORT_MODE_RESEARCH.md:132-133` says the daily step estimate "must not be added again to workout estimates". There is no code that combines them (`workout.energy_kcal_est` is read only by `fit_workout.c:1013-1019` and `background.c:155`).
- **The profile fields do not exist.** `nos_prefs_t` (`nos/prefs.h:10-47`) holds only `sports_mass_kg` (uint8, 0 = unset) plus step goal, units and so on. There is no age/birth year, sex or height. The B watch has `wrist_reserved[1]` as its only spare byte. The prefs checkpoint (`prefs_disk.c:16-20`) has a 256-byte capacity, so growth is possible. The ABI is guarded by `_Static_assert(sizeof==52)` (`prefs.c:10-18`) and by the `EDIT_FIELDS` overlay list (`prefs.c:37-39`).
- **Badminton itself adds almost nothing to `kcal`.** The pedometer largely rejects the motion (see Complaint 2), so steps and `steps*0.04` are close to zero for the game. Active minutes also stay near zero (`svc.c:553`).

### Why the user saw what they saw

The Activity and Energy totals show only `steps*0.04`. They never include resting energy (about 1.0-1.2 kcal/min for an average adult), and they do not capture the game, because step counting fails on a swinging wrist. So the number looks far too small.

### Data available on SD (retroactive)

- **The SP_ACTIVITY minute rows** (`20261023-events.spt`, `type=4`): `steps`, `steps*0.04` kcal and `active_min` at each minute, and thus today's step-only total. `tools/sports_export.py` already names them `steps estimated_active_kcal active_minutes` (`sports_export.py:21`).
- **NVS** key `nosact` (`svc.c:431-436`, `ACT_VERSION 2`) holds today's hourly steps and `active_min`. NVS key `pedometer` holds the day's step total. NVS is not on the SD and needs a flash/NVS dump to read.
- **Elapsed time since local midnight** is derivable from any record's `day` and `minute` fields.
- **The workout window and workout energy**, once recomputed per Complaint 1, supply the active part.
- **BMR itself needs only the profile.** Age, sex, height and weight must come from the user; the watch holds at most the mass.

### Minimal correct fix

1. **New profile fields in prefs** (bump `PREFS_VERSION` to 5; update the `_Static_assert`s and `EDIT_FIELDS`; default them to `unset`):
   - `profile_age_y` (uint8, or `birth_year` uint16),
   - `profile_sex` (0 unset, 1 male, 2 female),
   - `profile_height_cm` (uint8, 100-250).
   - Reuse `sports_mass_kg` as weight, or promote it to a `profile_weight_dkg` uint16 for 0.1 kg resolution. Add Settings rows (the Sports mass editor in `fit_workout.c:697-727` is a ready template).
2. **BMR (Mifflin-St Jeor), kcal/day:** `10*kg + 6.25*cm - 5*age + s`, with `s = +5` (male) or `-161` (female). When sex is unset, use the midpoint `-78` and flag the result "approx".
   - Example: 70 kg, 175 cm, 30 y, male = 700 + 1093.75 - 150 + 5 = 1648.75 kcal/day = 1.145 kcal/min.
3. **Accrue per minute.** The simplest stateless form needs no persistence and handles reboots and day rollover on its own: `basal_so_far = BMR_day * seconds_since_local_midnight / 86400`. Compute it in `act_publish` using `hal_get_datetime`, and publish `basal_kcal` and `total_kcal = basal + active`.
4. **Avoid double counting.** `0.04 kcal/step` is a NET-ish activity figure. Define:
   - `active = step_kcal(non-workout steps) + sum(workout NET kcal today)`, where workout net = gross - BMR/1440 x minutes (or `(MET-1) x kg x hours`);
   - subtract session-window steps from `step_kcal` (needs `sample_steps` to run for all sports, `workout.c:288`);
   - `total = basal + active`.
5. **Journal and UI.**
   - Extend the SP_ACTIVITY record. Keep `v1` as step-active for compatibility and add `v3`=basal, `v4`=workout net, `v5`=total (8 floats are available). Update `FIELDS` in `tools/sports_export.py`.
   - Make `fit_activity` (KCAL tile) and `fit_energy` (`value()` and `burn`) show `total_kcal`.
   - Keep the `face_segment` 400 kcal ring on ACTIVE kcal. Against a 1700+ kcal total it would be permanently full.
6. **Retroactive for today:** compute `BMR_day * (local seconds since midnight at the last record)/86400` (from the events file's last record), then add the step-only active kcal (last SP_ACTIVITY `v1`) and the recomputed badminton NET kcal, minus the steps inside the session window times 0.04. All of this needs the user's profile values.

---

## Components

### svc-workout (recorder: `nos/workout.c`, `nos/workout.h`)
- **Purpose:** background session recorder: state machine, elapsed time, HR, steps, GPS, laps, frozen setup, and the energy estimate.
- **Where it runs:** logic in the UI task, via `workout_tick()` from `svc_tick` (`svc.c:1321`) once per frame (`nos/nos.c:2341`). Screen-off services tick at 1 Hz (`nos_main.c:2094`) and AOD/sensor-idle at 1 Hz. The SD writer is `task-workout-sd` ("workout_sd", 6144 B stack from PSRAM, priority 2, `tskNO_AFFINITY`, loop every 500 ms, `workout.c:763-777`).
- **Cadence:** metrics sample every `SAMPLE_MS=1000`; checkpoint request every `CHECKPOINT_MS=30000` (`workout.c:46-47`, `:803-806`) and on every state transition; retry every 5 s.
- **Reads:** `hal_activity_get`, `hal_hr_get`, `hal_gps_get`; the IMU via `workout_motion_sample` (`board.c:302`, producer thread, critical section); `nos_prefs` indirectly through `configure_session()`.
- **Writes:** `current`/`history[12]` in RAM, and `/sdcard/watch/workouts/workouts.{0,1}.bin` (checksummed alternating banks, fsync + readback, `write_checkpoint`, `workout.c:593-643`).
- **Files:** `nos/workout.c:268-280` (energy), `:286-354` (steps/HR), `:813-878` (start/finish), `:593-643` (checkpoint).

### svc-workout / app-sports UI (`nos/apps/fit_workout.c`)
- **Purpose:** the Sports launcher app: sport grid, ready, live, summary, history, details and setup (including body mass).
- **Where it runs:** UI task, screen `scr_workout` (`fit_workout.c:1066`).
- **Cadence:** per frame `snapshot()` copies `workout_get()`.
- **Reads:** `workout_get`, `workout_storage_get`, `workout_history_get`, `nos_prefs`. **Writes:** `nos_prefs.sports_*` through `prefs_save()`, and `workout_setup_set`.
- **Files:** `fit_workout.c:102-115`, `:349-419`, `:631-635`, `:697-727`, `:955-1040`.

### svc-activity (daily steps, kcal, hourly: `nos/svc.c`)
- **Purpose:** publishes `svc_activity_t` (steps, km, `kcal=steps*0.04`, active minutes, hourly steps, week). Source of every daily-calorie display.
- **Where it runs:** UI task, `act_tick` once a second from `svc_tick` (`svc.c:1316-1319`). It also runs with the screen off.
- **Reads:** `hal_activity_get`. **Writes:** NVS key `nosact` every 10 minutes while dirty (`svc.c:529-566`, `act_save`).
- **Files:** `nos/svc.c:44-46`, `:426-566`, `:1303-1321`.

### svc-pedometer (`core/pedometer.c`, `core/walk.c`)
- **Purpose:** wrist step detector. It runs in `task-board-sense` ("board-sense", 4096 B stack, priority 2, `tskNO_AFFINITY`, 20 ms cycle, `board.c:~980-1050`, created in `start_board_workers`).
- **Reads:** IMU samples at 50 Hz plus two knock polls per cycle. **Writes:** `s_activity`, and NVS key `pedometer` every 60 s from `task-board-fs` (`board.c:903-918`, `:955`).
- **Behaviour:** discards motion with gyro > 250 dps (`walk.c:78`) and needs 8 coherent peaks. Poor fit for racket sports.

### svc-sports-logger (`nos/sports.c`)
- **Purpose:** the sensor and event journal. It records raw IMU, HEART, PPG, per-minute ACTIVITY and BATTERY records, and food and water entries, and serves the Energy bins.
- **Where it runs:** producers call `sports_emit` from any task (spin-lock protected). `task-sports-sd` ("sports_sd", 4096 B stack from PSRAM, priority 1, `tskNO_AFFINITY`) drains the 1024-entry queue every 1000 ms (batch 512). `sports_tick` runs from `svc_tick` at 1 Hz and emits the minute ACTIVITY/BATTERY records.
- **Writes:** `/sdcard/watch/sports/YYYYMMDD-events.spt` and `YYYYMMDD-HH-sensors.spt`; NVS keys `sportsfood` and `sportsmarks`.
- **Files:** `nos/sports.c:56`, `:215-300`, `:366-392`, `:403-411`.

### app-energy (`nos/apps/fit_energy.c`)
- **Purpose:** daily "BURN / EAT / water" chart plus the sensor-log, battery-history and tuning screens. Shows step-only burn with the footer "ACTIVITY CALORIES ONLY".
- **Where it runs:** UI task. History is fetched through `sports_query_request` (async, `task-sports-sd`).
- **Files:** `fit_energy.c:97`, `:144`, `:156`.

### app-activity (`nos/apps/fit_activity.c`)
- **Purpose:** distance, kcal and active-minute tiles plus the hourly step histogram. Reads `svc_activity()` only; no workout data.
- **Files:** `fit_activity.c:406-416`, `:470`.

### hw-strap-hr (`firmware/main/ext/ext_hr.c`)
- **Purpose:** MAX30102 driver. 100 Hz PPG, beat detection, SpO2 and a 10-minute background sample.
- **Where it runs:** `task-ext` service, `hr_service()` every 120 ms while enabled. It emits SP_HEART and SP_PPG records.
- **Files:** `ext_hr.c:34`, `:284`, `:296`, `:330-347`.

### Tools
- `tools/workouts_export.py` and `tools/sports_export.py` (read-only parsers); `tools/sdsync.py` (SD file transfer, `--pull`).

---

## How it runs

- **Scheduling.**
  - All calorie-relevant logic (`workout_tick`, `act_tick`, `sports_tick`) runs in the UI task from `svc_tick()`, which is installed as `nos_bg_tick` (`registry.c:62-71`). It is called every frame with the screen on (`nos/nos.c:2341`).
  - In PWR_OFF it is called every 1000 ms (`nos_main.c:2092-2094`).
  - In AOD or sensor-idle the frame runner is invoked at 1 Hz (`nos_main.c:2108-2120`).
  - So workout time, steps, HR sampling and the daily kcal keep running with the screen off or the app closed.
- **Background tasks.**
  - `task-board-sense` (priority 2, about 20 ms) produces IMU and pedometer data and feeds `workout_motion_sample` and `sports_imu` for every sample.
  - `task-board-fs` (priority 1) saves the pedometer to NVS every 60 s and writes a 1/min IMU CSV row to `/watch/logs`.
  - `task-workout-sd` (priority 2, 500 ms) writes workout checkpoints.
  - `task-sports-sd` (priority 1, 1 s) drains the journal.
  - The HR strap is serviced on `task-ext`.
- **Energy path today.**
  - Workout: `advance_time()` computes `MET*3.5*mass/200*elapsed_min` each tick, only if mass is non-zero.
  - Daily: `act_publish()` computes `steps*0.04` each second.
  - They are never combined, and BMR is absent.

## Data in / data out

| Data in | Source | Consumer |
|---|---|---|
| Body mass (uint8 kg) | `nos_prefs.sports_mass_kg` (default 0) | `configure_session()` -> `workout_setup.mass_kg` |
| MET (x10) | static table `profiles[]` (`workout.c:56-97`; BADMINTON=55) | `workout_start` -> `current.met_tenths` |
| Steps (today) | pedometer `hal_activity_get` | `act_publish` (kcal), `sample_steps` (only for uses_steps sports) |
| Heart rate (bpm, quality) | `hal_hr_get` (strap) | `sample_heart`: avg/peak only |
| Wrist IMU | `board_imu_raw` | pedometer, `workout_motion_feed`, SP_IMU journal |
| Age, sex, height | not stored anywhere | n/a |

| Data out | Where |
|---|---|
| `energy_kcal_est` | `workout_snapshot`, `workouts.N.bin`; shown only in DETAILS (`fit_workout.c:1013`) and the background "Sports session" job (`background.c:155`) |
| `svc_activity_t.kcal` (steps*0.04) | Activity tile, Energy app, `face_segment` ring, SP_ACTIVITY journal record, background "Calorie estimate" job |
| Per-minute SP_ACTIVITY, raw SP_IMU, SP_HEART | `/sdcard/watch/sports/*.spt` |

## Persistence

| Store | Location | Content | Survives |
|---|---|---|---|
| store-sd-workouts | `/sdcard/watch/workouts/workouts.{0,1}.bin` | Session plus last 12 snapshots (4304 B V3) | Reboot, reflash. Needs the SD card. |
| store-sd-sports-journal | `/sdcard/watch/sports/YYYYMMDD-events.spt`, `YYYYMMDD-HH-sensors.spt` | 64 B records | Reboot, reflash. No pruning code in `sports.c`. |
| store-sd-settings | `/sdcard/watch/settings.{0,1}.bin` | `nos_prefs_t` (52 B) | Reboot, reflash. Mirrored to NVS key `nosprefs`. |
| store-nvs-prefs | NVS keys `nosact`, `pedometer`, `sportsfood`, `sportsmarks` | Daily activity, steps, food, marks | Reboot, reflash (NVS preserved by the checked flash path) |

## Interactions

- [[Health and activity]]: pedometer, `svc_activity`, Activity and Heart apps.
- [[Sports and workouts]]: the recorder, profiles, MET table, SD banks, and the 40-sport catalogue.
- [[Storage and persistence]]: SD journal, workout banks, prefs banks, NVS keys.
- [[Hardware and drivers]]: HR strap, QMI8658 IMU, SD.
- [[Services hub]]: `svc_tick` schedule, `nos_bg_tick`.
- [[Power and battery]]: screen-off 1 Hz service loop that keeps recording alive.
- [[Settings and developer]]: where profile fields would be added.
- [[Shell and faces]]: `face_segment` kcal ring.
- [[Gaps]] and [[OS overview]]: summary entries.

## Gaps

| # | Severity | Gap | Evidence | User impact |
|---|---|---|---|---|
| 1 | high | Body mass defaults to 0, so workout kcal is silently 0. No prompt on READY or Start. | `workout.c:123,276`; `prefs.c:175`; `fit_workout.c:524,631-635` | Badminton showed no calories. |
| 2 | high | No BMR and no age, sex or height profile exists. | `prefs.h:10-47`; repo-wide search finds nothing | Daily total omits resting energy (about 1500-1900 kcal/day). |
| 3 | high | No per-session time series (movement or HR per minute). Sports has no chart. | `workout.h:71-96`; `fit_workout.c:955-1040` | No activity graph for the game. |
| 4 | medium | Workout kcal is not displayed on live, summary or history. DETAILS hides the rows when 0, with no "unavailable" row. | `fit_workout.c:349-419,845-855,1013-1019` | Even with mass set, kcal is hard to find. |
| 5 | medium | HR is recorded (avg, peak, integral) but never used for energy. | `workout.c:314-354` | No intensity term, so waiting time inflates the estimate. |
| 6 | medium | Pedometer rejects racket-sport motion (gyro > 250 dps, 8-peak entry), so daily kcal and active minutes miss the game. | `core/walk.c:9-10,78`; `svc.c:553` | Activity tile and Energy show near-zero for the game. |
| 7 | medium | Session steps are skipped for `uses_steps==0` sports, so no session-step delta exists to avoid double counting with the daily steps. | `workout.c:288` | A combined total cannot be de-duplicated. |
| 8 | low | Energy chart is a cumulative staircase labelled "BURNED: DAILY ESTIMATE" and shows no per-interval activity. | `sports.c:409-410`; `fit_energy.c:97,147` | Chart is not a "how much I moved" view. |
| 9 | low | Adding BMR to `svc_activity_t.kcal` would permanently fill the 400 kcal `face_segment` ring. | `face_segment.c:263-267` | Needs a separate active-kcal field. |
| 10 | low | The raw SP_IMU/SP_PPG journal (about 16 KB/s) has no pruning in `sports.c`. | `sports.c` (no `remove`/`unlink`); `board.c:300` | SD fills over time; drops are counted in `status.dropped`. |
