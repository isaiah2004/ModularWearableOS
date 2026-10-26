---
title: Gaps
tags: [nos, os-map]
updated: 2026-10-23
---

# Gaps

This is the list of confirmed gaps in NOS on the 1.43-B watch. Each gap was checked against the code by independent reviewers. Severity is the reviewers' verified severity, which is often lower than first reported. Raw data is in `data/gaps.json`.

**146 confirmed gaps.** By severity: critical 0, high 3, medium 40, low 103.

By area: apps 20, services 20, health 17, connectivity 15, storage 13, power 13, hardware 11, sports 9, shell 9, settings 8, engine 6, render 5.

No gap survived verification as critical. The two reported as critical (G02 GPS track loss, G03 unauthenticated BLE file service) were verified as high. 6 reported gaps were rejected on verification; they are listed at the end.

## The three things you hit today

These three came from your own use of the watch. See [[Calorie trace]] for how calories flow through the code.

- **G01, badminton calories show nothing.** Body mass defaults to 0, so the workout calorie figure is 0 and its row is hidden. You must find Sports setup and enter a weight first. Nothing prompts you at Start. Verified medium.
- **G06, no movement graph for a game.** A workout stores only totals and laps, with no per-minute series, and no chart code exists in [[Sports]]. The raw journal can rebuild a curve offline by time window, but only by hand. Verified medium.
- **G04, no passive calories.** Daily KCAL is steps times 0.04 and nothing else. Resting energy is not counted, and [[Activity]] labels the figure plain KCAL. Verified medium.

Closely linked: G07 (workout calories never reach the daily total in [[Energy]]) and G05 (no body profile, so weight is never used by Activity or Energy).

## Critical severity (0)

None.

## High severity (3)

### G02 — GPS track held only in RAM until Stop; no checkpoint or crash recovery

- **Area:** storage
- **Nodes:** svc-track, [[Navigate]], store-sd
- **Votes:** 2/2
- **Evidence:** nos/svc_track.c:46-59 (T.pts RAM buffer, PSRAM), 158-163 (points appended only to RAM), 174 (hal_big_alloc), 205-260 (write_gpx/track_save, the only file write), 273-310 (save only on svc_track_stop). No periodic flush, no boot-time scan for orphan .part files, no resume path. Needs a GPS strap module.
- **Impact:** A long hike or run recording is lost after a reboot, crash or battery death while recording.
- **Fix:** Append points to a .part GPX on SD every N points/minute and recover it at boot.

### G03 — BLE XFR SD service has no authentication (read creds, overwrite/delete SD)

- **Area:** connectivity
- **Nodes:** hw-ble, store-sd, eng-ota
- **Votes:** 2/2
- **Evidence:** ble_xfer.c:73-78 (no ENC/AUTHEN flags); hal_esp32.c:1555-1560 (sm_bonding=0, sm_sc=0, NO_INPUT_OUTPUT); hal_esp32.c:1312-1328 (advertises whenever BLE is on); sd_xfer.c:542-596 (sdls/sdget/sdget2 have no path restriction, so watch/wifi/settings.N.bin is readable). Narrower than first claimed: sdput on watch/wifi/* is refused, sdrm of settings files is blocked, and sdput! of settings must pass checksum validation. Other files can be put or deleted freely.
- **Impact:** A nearby BLE device can read Wi-Fi credentials and tamper with or delete card contents.
- **Fix:** Require pairing/encryption or a challenge token, enable the service only on demand, deny reads of watch/wifi/*.

### G11 — Raw IMU (~150/s) journaled to SD continuously with no gating, decimation or retention

- **Area:** power
- **Nodes:** task-board-sense, store-sports-journal, store-sd, svc-workout
- **Votes:** 2/2
- **Evidence:** board.c:984-986 and 1023-1025 (3 IMU reads per 20 ms cycle), board.c:300 (imu_take calls sports_imu), sports.c:56,145-157 (SP_IMU is a raw type, 64 B record), sports.c:215-240 (hourly *-sensors.spt files, fsync+close each batch), sports_init called unconditionally (svc.c:1299). Only gate is sports_quiesce at shutdown. No pruning anywhere. About 9.6 KB/s, ~800 MB/day. Not checked: whether the board task is parked while the screen is off.
- **Impact:** Extra standby current, SD wear, the card fills and later workout saves fail.
- **Fix:** Gate raw IMU to workouts/explicit logging, decimate, add retention/pruning, never log in OFF or low battery.

## Medium severity (40)

### G28 — Recorder: memos folder never created and failed saves strand .part audio

- **Area:** apps
- **Nodes:** [[Recorder]], hw-strap, store-sd
- **Votes:** 2/2
- **Evidence:** board.c:592 mkdirs only watch and watch/logs; MEMO_DIR (con_memo.c:47) is never created. ext_audio_files.inc:85-100 opens "<path>.part" with no mkdir, so ENOENT gives HAL_REC_ERROR; con_memo.c:175-177 and 349 then show LIBRARY ERROR. record_finish (ext_audio_files.inc:120-136) keeps .part on failure and nothing unlinks it; the library lists only .wav. No free-space check. The recorder needs an external mic strap, so a bare B never reaches this.
- **Impact:** With a mic strap and a card lacking /memos, the first recording fails; failed saves leave orphan .part files taking space.
- **Fix:** Create the folder on mount, check free space before recording, recover or clean up .part files.

### G01 — Badminton/workout calories are zero or hidden by default (user complaint)

- **Area:** sports
- **Nodes:** [[Sports]], svc-workout, store-prefs
- **Votes:** 2/2
- **Evidence:** nos/prefs.c:134-175 (sports_mass_kg defaults to 0); nos/workout.c:274-279 (energy_kcal_est = MET*3.5*mass/200*minutes only when mass is set; badminton MET 5.5 at :65; elapsed time includes idle); nos/apps/fit_workout.c:631-635 (setup row says "Not set / calories unavailable"), :1013-1019 (energy rows drawn only when energy_kcal_est > 0). Only kcal display is the DETAILS block. No mass prompt at Start. HR and movement are not used. Zero default is deliberate and labelled.
- **Impact:** A recorded badminton game shows no calories unless the user first finds Sports setup and enters mass.
- **Fix:** Prompt for mass at first Start, always show an ENERGY row on live/summary/history, use Keytel when HR is valid and movement-scaled MET otherwise.

### G04 — No passive/resting (BMR) calories; daily KCAL is steps*0.04 only (user complaint)

- **Area:** health
- **Nodes:** svc-activity, [[Activity]], [[Energy]], [[Segment]], store-prefs
- **Votes:** 2/2
- **Evidence:** nos/svc.c:46 (KCAL_STEP 0.04) and :445 (s_act.kcal = steps*KCAL_STEP) are the only daily kcal source. fit_activity.c:467 labels it plain "KCAL"; face_segment.c:263 uses it with a fixed 400 ring goal (:268); fit_energy.c:144 uses it as BURNED and only the footer at :156 says "ACTIVITY CALORIES ONLY". No BMR code anywhere.
- **Impact:** Reported daily burn is a fraction of real expenditure (6000 steps = 240 kcal) and cannot be compared with logged food.
- **Fix:** Add profile-based BMR by wall-clock time, expose separate active_kcal and total_kcal, label step energy ACTIVE.

### G05 — No user body profile (height, weight, age, sex); stride and kcal per step fixed

- **Area:** health
- **Nodes:** store-prefs, svc-activity, [[Settings]], [[Sports]]
- **Votes:** 2/2
- **Evidence:** nos/svc.c:44-46 (STRIDE_M 0.75, KCAL_STEP 0.04), :444-445 (used in act_publish). nos/prefs.h:37 has only sports_mass_kg (0 = unset), read only at fit_workout.c:110 for the Sports MET estimate. fit_activity.c:406 and fit_energy.c:144 never see mass. No height/age/sex prefs. HR zones not found (search not exhaustive).
- **Impact:** Distance and calories are one-size-fits-all; weight entered once is ignored by Activity and Energy.
- **Fix:** Add a Profile section (height, weight, birth year, sex) to prefs and Settings and feed stride, BMR and workout energy.

### G06 — No per-session movement/activity graph for workouts (user complaint)

- **Area:** sports
- **Nodes:** svc-workout, [[Sports]], store-sports-journal
- **Votes:** 2/2
- **Evidence:** nos/workout.h:70-96 (snapshot has scalars plus up to WORKOUT_LAPS_MAX laps, no series); history is 12 snapshots (workout.h:10); nos/workout_motion.c:9-52 only increments counters; no chart/sparkline code in fit_workout.c. The journal logs timestamped SP_HEART/SP_IMU/SP_GPS and tools/sports_export.py has --day/--start/--end, so a curve can be rebuilt offline by time window (manual join). Jobs window and exporter claims were not fully checked.
- **Impact:** No post-game intensity, movement or HR curve on the watch; offline needs a manual join.
- **Fix:** Store per-minute bins (movement fraction, mean bpm, kcal) per session, draw them in details/summary, export them.

### G07 — Workout calories never reach the daily Activity/Energy totals

- **Area:** sports
- **Nodes:** svc-workout, svc-activity, [[Energy]]
- **Votes:** 2/2
- **Evidence:** nos/workout.c:276-278 computes energy_kcal_est; consumers are only fit_workout.c:1013-1014 (DETAILS), background.c:154-155 (Jobs trace) and the sim. nos/svc.c:445 sets daily kcal = steps*KCAL_STEP with no workout term; fit_energy.c:144, fit_activity.c:406 and face_segment.c:263 read that value. Pedometer rejection of racket motion not verified. Gross MET energy includes resting metabolism, so a straight merge needs a design decision.
- **Impact:** An hour of badminton adds ~400 kcal to nothing; the day total misses it.
- **Fix:** Merge session net energy into the daily total and subtract session steps to avoid double counting.

### G08 — Failed/unsaved workout, GPS track or IMU recording cannot be discarded and blocks restart, power-off and OTA

- **Area:** sports
- **Nodes:** [[Sports]], [[Navigate]], [[IMU]], svc-workout, svc-track, [[Power sheet]]
- **Votes:** 2/2
- **Evidence:** workout_discard (nos/workout.c:879) has no UI caller (only sim and tests); svc_track_discard (svc_track.c:316) has none; UI offers only RETRY SAVE (fit_workout.c:441-446, out_nav.c:643). imu_rec.c:395-436 retries every 5 s while intent.active; out_imu.c:25,145,184 shows only START in ERROR. firmware/main/nos_main.c:731-749 shutdown_guard returns 2/3 and ota.c:56-69 blocks OTA on the same conditions. Display sleep is not blocked; a hardware reset works; reinserting the original card clears it.
- **Impact:** With a dead or removed SD card the watch cannot be restarted, powered off or updated from the UI.
- **Fix:** Add a confirmed discard/give-up action for each recorder and have finish() clear IMU intent on hard failure.

### G09 — Screen never turns off with default settings: sensor_idle overrides the Always-on display setting

- **Area:** power
- **Nodes:** eng-power, store-prefs, [[Control center]], [[Energy]], [[Dot]]
- **Votes:** 2/2
- **Evidence:** nos/power_policy.h:13-14 returns SENSOR whenever sensor_idle is set, before looking at aod; prefs.c:159 defaults sensor_idle=1, :144 aod=0, :171 forces sensor_idle=1 on migration. nos_main.c:2085 uses it; PWR_SENSOR runs the dim 1 Hz AOD-style face (nos_main.c:705, 2106, 2225). The AOD toggles (set_settings.c:455,526; sys_control.c:231) change only aod. The only sensor_idle toggle is "IDLE 80MHz" in fit_energy.c:155,247. The panel is dimmed, not full brightness.
- **Impact:** The AOD tile/setting appears to do nothing, the OLED stays lit and battery drains faster than expected.
- **Fix:** Make idle policy one visible setting (Off / Always-on / Sensor idle) and default to OFF when AOD is off.

### G10 — Standby has no light sleep, tickless idle or wake interrupts; 20 ms polling loop

- **Area:** power
- **Nodes:** eng-power, hw-imu, hw-touch, hw-rtc, task-board-sense
- **Votes:** 2/2
- **Evidence:** nos_power.c:20,34,49,97 (light_sleep_enable=false); sdkconfig: TICKLESS_IDLE not set (:1941), FREERTOS_HZ=1000 (:1917), BT modem sleep off (:1005). PIN_IMU_INT (8) and PIN_RTC_INT (15) are defined but never used for wake. Screen-off loop in nos_main.c:2091-2130 ends in a 20 ms delay; board.c:967-1036 reads the IMU about 150 times/s. DFS to 80 MHz does exist (PM_ENABLE=y). The mA figures are not measured here.
- **Impact:** Screen-off current stays high; runtime is far below target.
- **Fix:** IMU wake-on-motion/FIFO on GPIO8, batched step processing, slower touch polling, then auto light sleep with modem sleep.

### G101 — Knock-lock (default on) blocks call Accept/Decline and power-sheet buttons; knock-wake also locks

- **Area:** shell
- **Nodes:** eng-nos, [[Call]], [[Power sheet]]
- **Votes:** 1/1
- **Evidence:** nos/nos.c:666 drops new presses while locked (hint toast only, :630-635); nos/prefs.c:158 knock_lock=1; con_call.c:463 Accept/Decline are touch hits and gated; knock_tick (nos.c:646-653) toggles the lock on the first frame after a knock wake (nos_main.c:2047-2049). The physical button still silences a ring. Toggle-on-wake is partly intentional.
- **Impact:** Users must knock twice before answering a call, and a knock wake can lock the watch.
- **Fix:** Exempt ring/power screens and lock only if already ON.

### G107 — Barometer history and trend reset at every reboot although SD holds the readings; STORM only on screen

- **Area:** apps
- **Nodes:** [[Barometer]], hw-env, store-sports-journal
- **Votes:** 1/1
- **Evidence:** firmware/main/ext/ext_env.c:12-13 (ring "not persisted across reboots"), :174 (SP_ENV emitted to the journal), :199-222. out_baro.c:221 reads only hal_env_history; COLLECTING state and the 3 h trend (have3h needs nhist>18, :252-259) depend on the ring. STORM is computed only in out_baro.c:259,386-410; no notification elsewhere.
- **Impact:** Pressure trend is lost at each boot and no storm alert is ever raised.
- **Fix:** Rebuild the ring from SD records and add a background alert.

### G113 — Saved volume and haptic strength are not applied to drivers at boot

- **Area:** settings
- **Nodes:** [[Settings]], hw-speaker, hw-haptic, store-prefs
- **Votes:** 1/1
- **Evidence:** ext_audio.c:337 (driver volume 70), ext_haptic.c:43 (strength 80); prefs defaults 60/70 (prefs.c:139-141). Only callers of hal_audio_volume/hal_haptic_set_strength: set_settings.c:1768-1769 (Reset), 2093-2096 (sliders), set_diag.c:634-643 (Diagnostics), ext_debug_commands.inc:44. None runs after prefs_load.
- **Impact:** After every reboot feedback ignores the saved level while Settings shows it.
- **Fix:** Apply both prefs after prefs_load in the boot sequence.

### G12 — Charging/external power is inferred from USB host presence but labelled CHARGING

- **Area:** power
- **Nodes:** hw-battery, [[Battery]], [[Power sheet]], [[Home pager]], svc-phone, [[Registry]]
- **Votes:** 2/2
- **Evidence:** hal_esp32.c:323 (usb from usb_serial_jtag_is_connected, which is host SOF), :324-326 (PIN_CHG_STAT only on C), :347-351 (external_power=1 on USB or rail >=4.4 V for 3 samples), :355-361. svc.c:164,992 map it to charging; sys_power.c:290 "CHARGING"; set_settings.c:604 "Charging". Counter-evidence: set_battery.c:81-82 says "ON USB POWER", set_diag.c:227 "USB POWER SEEN". A charging cell sits near 4.0-4.2 V, so the 4.4 V path rarely fires and a plain wall adapter likely reads as battery. Low-battery alarm suppression is only partly true (AOD indicator, faces.c:584-585).
- **Impact:** The watch may claim to be charging when it is only powered or full, and miss real charges.
- **Fix:** Use USB POWER wording everywhere, add an explicit external-power field, hide percentage on USB.

### G13 — Battery percentage consumed without validity check (false 50% at boot, stale values)

- **Area:** power
- **Nodes:** hw-battery, [[Power sheet]], [[Control center]], [[Home pager]], [[Modules]], [[Battery]], [[About]], svc-phone, [[Registry]]
- **Votes:** 2/2
- **Evidence:** hal_esp32.c:287 (seed 50), :350-363 (battery_valid after 10 stable samples), :378-388 (hal_power_get keeps percent), svc.c:164 (sends bat to phone unchecked), sys_home.c:474, sys_control.c:345, sys_power.c:168, set_settings.c:603, set_modules.c:795,1186, set_about.c:99-101 all skip validity; only set_battery.c:38-42 honours it. The placeholder is short-lived and the low-battery policy (power_policy.h:9-10) does check validity.
- **Impact:** Header and phone can show a number while the Battery page shows "--"; briefly a false 50% after boot or unplug.
- **Fix:** Route every consumer through one helper returning unknown when !battery_valid; persist the last percent.

### G14 — No critical-battery action (warning, workout/prefs save, stop SD writers, orderly shutdown)

- **Area:** power
- **Nodes:** eng-power, svc-workout, store-sd
- **Votes:** 2/2
- **Evidence:** power_policy.h:6-11 (low latch at <=15%, clear at >=20%); nos_main.c:2029-2037 (log, toast "LOW BATTERY / DISPLAY SAVER" while the screen is on, AOD/SENSOR forced OFF). No other battery threshold; no save of an active workout, no SD writer or radio stop, no shutdown near cutoff. The orderly shutdown path (nos_main.c:713-780) is user-triggered only. A toast does exist. Settings use a checksummed SD checkpoint and sports journals are fsync'd append-only, so corruption risk is lower than claimed.
- **Impact:** A brown-out can lose an active workout or recording.
- **Fix:** Add a critical threshold (~5%) that saves workout and prefs, stops SD writers/recorders and deep-sleeps.

### G15 — Alarms and timers cannot wake a powered-off watch (deep sleep wakes only on GPIO0)

- **Area:** power
- **Nodes:** svc-alarm, hw-rtc, eng-power, [[Power sheet]]
- **Votes:** 2/2
- **Evidence:** nos_main.c:809-811 (restart, otherwise only esp_sleep_enable_ext0_wakeup(GPIO0) then deep sleep); no timer, ext1 or GPIO15 wake anywhere in nos/ or firmware/main; pins.h:56 defines PIN_RTC_INT unused. svc.c:826-853 alarm_tick needs an awake CPU and an exact-minute match; no catch-up. sys_power.c has no alarm warning. Display sleep keeps alarms working; only POWER OFF is affected.
- **Impact:** An alarm set before POWER OFF silently never rings.
- **Fix:** Arm RTC alarm/timer wake plus GPIO15 ext1 before deep sleep, or warn when alarms are armed and report missed alarms at boot.

### G16 — 24 KB NVS may overflow: sportsfood ledger rewrite and other blobs near capacity (unmeasured)

- **Area:** storage
- **Nodes:** store-nvs, store-ledger, [[Energy]], svc-activity
- **Votes:** 2/2
- **Evidence:** partitions.csv:5 (NVS 24 KB, about 630 usable entries). sports.c:44-45,68-69 (ledger 8200 B, ~259-262 entries, rewritten whole at :347 and :361); hal_esp32.c:231-236 routes sportsfood straight to NVS (SD does not help). ESP-IDF writes the new blob before erasing the old, so a rewrite needs about 260 free on top of 260 live. Other blobs (PHY, nosprefs mirror, nosact, voice_hist, sportsmarks etc.) add roughly 150-190+. No on-device nvs_get_stats reading recorded; the simulator reports a fixed 84/756. On failure RAM is rolled back and the UI shows SAVE FAILED (fit_energy.c:239,249). Not specific to card-absent boots.
- **Impact:** Food/water save or undo may return SAVE FAILED with no explanation.
- **Fix:** Measure with the serial sd command or Settings > Modules, then move the ledger/voice_hist/nosact to SD with NVS fallback, shrink records or enlarge the partition.

### G18 — No historic activity sync to Gadgetbridge (actfetch ignored)

- **Area:** services
- **Nodes:** svc-phone, svc-activity, svc-heart
- **Votes:** 2/2
- **Evidence:** nos/svc.c:1140-1146 (gb_act_send emits only realtime {"t":"act","stp":delta,"hrm":..,"rt":1}); :1180-1187 (act handler only enables realtime); :1190 (comment "actfetch ... not used by NOS yet"); no actfetch handler in the tree. Hourly/week data is stored locally ("nosact", svc.c:426-438) but never sent. An implementation exists only in unmerged worktree t3code-a85cc679 (not checked).
- **Impact:** Steps and heart rate never reach the phone's Gadgetbridge database; only live values show.
- **Fix:** Port nos/svc_act.c (10-min slot samples, actfetch reply, reconnect replay) and serve from act_saved and the HR ring.

### G19 — Notifications do not wake the display; banner is deferred and stale at next wake

- **Area:** services
- **Nodes:** [[Notifications]], svc-notif, eng-power, eng-nos
- **Votes:** 2/2
- **Evidence:** power_policy.h:16-17 (wake only on touch, button, raise, knock); nos_main.c:2061,2091-2096; svc.c:230-233 notif_post calls nos_banner; nos.c:509-521 (banner sets active, plays feedback unless DND, calls nos_keep_awake, which only resets the idle timer, nos.c:585). Banner timer runs on nos_t (nos.c:2222), which is frozen in PWR_OFF, so the banner appears at next wake. The notification itself stays in the list and a haptic/sfx cue plays unless DND or no strap.
- **Impact:** A message arriving on a dark watch shows no sign beyond the buzz (nothing without the haptic strap).
- **Fix:** Optional wake-on-notification with a short ON window, or expire stale banners.

### G21 — UI task runs at lowest priority on core 0 under Wi-Fi/NimBLE/esp_timer

- **Area:** engine
- **Nodes:** task-ui, eng-frame, hw-ble
- **Votes:** 2/2
- **Evidence:** firmware/sdkconfig:1707 (main task pinned to core 0, default priority 1 inferred); NimBLE host at configMAX_PRIORITIES-4 on core 0 (hal_esp32.c:1531, sdkconfig:709); Wi-Fi (:1782), BT controller (:949), esp_timer (:1756-1758) on core 0; lwIP prio 18. nos_frame_priority.h mode 3 is off by default (nos_main.c:1307-1313, 1927). Flush (prio 4) and touch (prio 5) are on core 1. No measured dropped frames.
- **Impact:** Possible frame jitter during radio or worker bursts while the screen is on.
- **Fix:** Enable the frame-priority mode or move services/radios to core 1.

### G22 — Any 1500 ms display queue/fence timeout reboots the watch

- **Area:** engine
- **Nodes:** task-display, eng-frame
- **Votes:** 2/2
- **Evidence:** firmware/main/nos_main.c:130 (DISPLAY_WAIT_MS 1500), :133-145 display_fault logs to RTC then esp_restart(); triggers at :146-150, :189-193, :233-244, :307/:336, :369, :436, :455/:468. Panel commands are retried 3 times first (:215-224). Comments at :135 and :240-241 say recycling DMA credits after an uncertain completion is deliberately avoided.
- **Impact:** A real panel or DMA stall reboots the watch mid-use, losing RAM-only state (tracks, timers, notifications).
- **Fix:** Retry or re-init the panel path first and restart only after repeated failures.

### G23 — Strap speaker/haptic/audio presence is assumed on B; alerts are silent with no indicator and the B strap pin map is unverified

- **Area:** hardware
- **Nodes:** hw-strap, hw-speaker, hw-haptic, svc-alarm, [[Call]], [[Notifications]], [[Find phone]], [[Timer]]
- **Votes:** 2/2
- **Evidence:** ext_audio.c:343 (EXT_SPEAKER set unconditionally, "cannot be probed; default on"); ext_haptic.c:107-109 (EXT_HAPTIC set whenever LEDC init succeeds); ext_priv.h:24-44 (strap pins: motor 16, BCLK 1, WS 2, DIN 5, SD 6); pins.h:64 BOARD_HAS_AUDIO 0; set_modules.c:167-169,229 and set_diag.c:67-69 show the parts present. Diagnostics has a user-confirm test (ST_ASK). Pin map comes from docs/strap_layout.html and is unverified on hardware.
- **Impact:** On a bare B the alarm, call ring and notifications give no buzz or sound and the UI never says so.
- **Fix:** Probe or user-set strap presence, show a no-feedback indicator, add screen-flash/phone fallback, verify the strap mapping on hardware.

### G24 — Flash tooling does not verify board identity (B image can be flashed to C)

- **Area:** hardware
- **Nodes:** eng-flash-tools, hw-strap
- **Votes:** 2/2
- **Evidence:** tools/flash_watch.py:454-468 (checkpoint accepts any NOS device answering prefs flush), :90-126 (hostcaps check has no board field; firmware reply at nos_main.c:1188 carries none), :575-582 (esptool write_flash directly); MAC checks only in --boot-recovery and --legacy-c-backup paths (:327-331, :526); tools/build.ps1:52-54 passes only --port. Needs a wrong-port mistake; a wrong image can be reflashed via USB download mode.
- **Impact:** A wrong COM port can flash the wrong board image, relying on manual confirmation.
- **Fix:** Run the board query before checkpoint and abort when the name differs from the build target.

### G25 — Weather location cannot be set in NOS; Wi-Fi weather never starts on a fresh install

- **Area:** connectivity
- **Nodes:** svc-weather, [[Weather]], hw-wifi, store-nvs
- **Votes:** 2/2
- **Evidence:** The only production caller of hal_weather_set_location is main.c:776, which is not in the build (CMakeLists builds nos_main.c). weather_service.c:93 skips every cycle while the location is invalid; :20 status "Set weather location over USB" is shown by out_weather.c:497-498. The BLE time-sync lat/lon (hal_esp32.c:884-891) feeds hal_location only, not the weather service. A paired phone can still supply weather (svc.c:1073).
- **Impact:** A phone-less watch gets no weather and the hint points to a step the NOS build cannot do.
- **Fix:** Use phone-supplied lat/lon or add an on-watch city picker and fix the empty-state text.

### G26 — Unauthenticated /notify, /cmd, /media and /cover HTTP routes; server stays up on every STA connection

- **Area:** connectivity
- **Nodes:** hw-wifi, svc-notif, eng-ota
- **Votes:** 2/2
- **Evidence:** hal_esp32.c:1739-1831 (handlers have no auth; /cmd is read-only, /notify caps app 19 / title 39 bytes, /cover needs exactly 125000 bytes); :1979 web_start on every GOT_IP; no httpd_stop. setup_authorized (watch_setup.inc:62) gates only /api/* setup routes. Reachable only on the same LAN while Wi-Fi is associated.
- **Impact:** Any LAN host can inject notifications, replace the cover image and inject media text.
- **Fix:** Token-gate or remove the routes and stop the server outside setup sessions.

### G27 — Incoming LTE-strap SMS is silent: modem sleeps after 8 s and no notification is posted

- **Area:** connectivity
- **Nodes:** [[Cellular]], hw-lte, svc-notif
- **Votes:** 2/2
- **Evidence:** ext_lte.c:299-303 (want_on clears enabled when idle), :388-395 (AT+QPOWD=1), NOS_EXT_IDLE_MS 8000 (hal_ext.h:35); last_get refreshed only by Cellular page, Diagnostics, Modules and the control toggle; con_cell.c:94-97 keepalive only while the page is open. unread_sms is read only at con_cell.c:217-218; no SMS-to-svc_notif path. SMS is store-and-forward, so texts arrive when the modem next powers up. Only matters with the LTE strap.
- **Impact:** A watch-only text appears only when Cellular is opened, contradicting the "text without your phone" copy.
- **Fix:** Keep the modem registered per saved intent, poll/URC the inbox and post incoming SMS as notifications with a banner.

### G29 — Torch has no auto-off and ignores low battery

- **Area:** apps
- **Nodes:** [[Torch]], eng-power
- **Votes:** 2/2
- **Evidence:** nos/apps/con_torch.c:217-218 (SCR_KEEPAWAKE), :88 (nos_keep_awake each tick), :57 (brightness override); nos_main.c:2080-2085 (keep blocks the timeout), :2226 (override beats user level and the low-battery path); power_policy.h:18-19. s_low_battery only toasts and disables AOD/wake. Nothing closes the torch. Exit by power button or back gesture not checked.
- **Impact:** A forgotten torch drains the battery to empty.
- **Fix:** Add a Torch timeout and dim or close it at low battery.

### G30 — Midnight rollover race between pedometer and svc pollutes week history

- **Area:** health
- **Nodes:** svc-activity, task-board-sense, [[Activity]]
- **Votes:** 1/1
- **Evidence:** board.c:134-178 (calendar_day re-reads the date at most once per second), core/pedometer.c:11-15 (zeroes steps when it sees the new day), svc.c:510-552 (act_tick reads the date unthrottled; act_rollover seeds max_steps/last_steps with yesterday's total, and max_steps only rises). Error appears only when yesterday's total exceeds today's final; the week average is also inflated. No guard found.
- **Impact:** Previous-day bar and average can be wrong on some days.
- **Fix:** Take svc's day from the pedometer day or reset max_steps when steps < last_steps.

### G38 — Heart app dead-end MEASURING: no sensor-lost state and no re-request after display-off

- **Area:** health
- **Nodes:** [[Heart rate]], hw-strap, svc-heart
- **Votes:** 1/1
- **Evidence:** ext_hr.c:324 (EXT_HR set once, never cleared), :343-356 (hal_hr_get always returns 1 when initialised), :238 (8 s lease expires in PWR_OFF). fit_heart.c:620-636 (stale sample leaves mode MEASURING), :671 (keep-awake while measuring), :514,537,612 (only re-request points). heart_tick only renews last_get. Locked-reading collapse after ~35 s not verified.
- **Impact:** A blank MEASURING screen that never recovers and keeps the screen awake until the app is reopened.
- **Fix:** Detect a no-sample timeout, re-request on wake and show a sensor-lost state.

### G42 — IMU full-scale (+/-8 g, 512 dps) clips swings; saturation guard unreachable

- **Area:** sports
- **Nodes:** hw-imu, svc-workout
- **Votes:** 1/1
- **Evidence:** board.c:424-425 (accel +/-8 g, gyro +/-512 dps); board_protocol.c:263-265 (4096 LSB/g, 64 LSB/dps) so max vector is ~13.9 g / ~887 dps; nos/workout_motion.c:13 rejects only >32 g or >4000 dps, so the saturation branch never runs; WORKOUT_REJECT_MOTION_SATURATED stays 0.
- **Impact:** Smash peaks clip silently and quality counters never show it.
- **Fix:** Raise full-scale for sport use and base thresholds on the configured range.

### G50 — Refused power actions close the sheet, use wrong toasts and silently drop calls

- **Area:** power
- **Nodes:** [[Power sheet]], [[Settings]], [[Call]], eng-nos
- **Votes:** 1/1
- **Evidence:** nos_main.c:751 (nos_host_power returns void), :756-757 (refusal toast only), :734 and :740 (return 4 for cancelled voice chat or recorder, mapped to "SAVE RECORDING FIRST"), :745-746 (SD dirty/uncertain/track share code 3). sys_power.c:184-185 calls nos_home() unconditionally; set_settings.c:686,1825-1826 toasts RESTARTING and ignores the result. nos.c:403-426 collapses the stack including scr_call; svc.c:977-980 then drops the ring with no decline sent.
- **Impact:** Misleading refusal messages, dropped restart requests and a phone that keeps ringing.
- **Fix:** Return a typed result, show specific reasons and send REJECT or refuse during a ring.

### G59 — Energy totals show zero when the SD query fails, though NVS holds the entries

- **Area:** storage
- **Nodes:** [[Energy]], store-ledger, store-sd
- **Votes:** 1/1
- **Evidence:** fit_energy.c:36-38 (refresh zeroes bins and shows HISTORY UNAVAILABLE on failure), :66-68; sports.c:444-447 (ledger already binned), :458-461 (poll returns -1 and skips the copy), :475-482,516 (failure after 3 s SD lock wait or I/O error). Failure is uncommon and labelled.
- **Impact:** After ENTRY SAVED the screen can show 0 EAT / 0 mL.
- **Fix:** Fall back to the NVS ledger totals when the query fails.

### G72 — Haptics and Sound preferences (and boot chime) do not apply to alarm, timer, call, find or volume detent

- **Area:** services
- **Nodes:** svc-alarm, [[Call]], [[Find phone]], [[Boot]], [[Settings]], [[Modules]]
- **Votes:** 1/1
- **Evidence:** svc.c:699 (hal_haptic with no haptics check), :701 (only call sfx checks sound), :1188 (vibrate command). sys_boot.c:101 (boot chime unconditional), set_settings.c:2094 (volume detent sfx). Only nos_feedback (nos.c:571-572) checks the prefs; hal_haptic/hal_sfx gate on hardware and volume only. Muting alarms may be a deliberate safety choice.
- **Impact:** Haptics/Sound off still buzzes for alarms and calls and plays the boot chime.
- **Fix:** Apply the prefs in ringer_tick and boot, and define the intended exceptions for alarms.

### G82 — Absent GPS re-probes (12.6 s UART probe) on every enable; workouts and Navigate keep enabling it

- **Area:** hardware
- **Nodes:** hw-gps, svc-track, svc-workout, [[Barometer]], [[Navigate]]
- **Votes:** 1/1
- **Evidence:** ext_gps.c:294-296 (G_ABSENT returns to G_PROBE_START when enabled), :236-262 (3 bauds x 2 rounds, about 12.6 s), :308-315. svc_track.c:106 calls hal_gps_enable(1) every frame while recording; workout.c:389,705,829,850; out_baro.c:603; out_nav.c:469,545. hal_gps_get returns 0 when absent (:319) with no absent text. Only applies with the GPS module unplugged.
- **Impact:** Wasted power and CPU, and a silent dimmed Use GPS or static page.
- **Fix:** Latch absent with back-off unless the user retries.

### G84 — No magnetic declination; Compass heading is magnetic while Navigate subtracts it from a true bearing

- **Area:** hardware
- **Nodes:** [[Compass]], [[Navigate]], hw-mag, hw-gps
- **Votes:** 1/1
- **Evidence:** hal_ext.h:193 ("0 = magnetic north"); ext_mag.c:177-180 (atan2f, no offset); out_nav.c:659-668 (want = home_brg - head, GPS bearing is true); GPS-course fallback (head_src=2) is true-referenced. No declination code anywhere; docs/os-map/systems/Utility apps.md:62 wrongly claims declination.
- **Impact:** Bearings are off by the local declination, many degrees in some regions.
- **Fix:** Apply a GPS-derived declination and show true/magnetic.

### G88 — Phone link is unauthenticated: no bonding, static random address, NUS spoofable

- **Area:** connectivity
- **Nodes:** hw-ble, svc-phone, svc-alarm
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:1558-1560 (sm_bonding=0, sm_sc=0, NO_INPUT_OUTPUT; deliberate per comment), :1485-1487 (same hardcoded static-random address on every unit), :1152-1155 (NUS RX has only WRITE flags), :1133-1145 (bytes fed straight to nus_rx_bytes). svc.c:1148-1191 dispatches notify, call, find, alarm, act, vibrate with no auth. Writing to RX is enough; subscribing to TX is not required. MAX_CONNECTIONS=2 (sdkconfig.defaults:47). Time-set path not confirmed.
- **Impact:** Nearby devices can inject notifications or calls, wipe alarms, and the watch is trackable by its fixed address.
- **Fix:** Enable LE Secure Connections bonding, rotate the address and track the NUS handle.

### G89 — OTA accepts plain HTTP with no signature; /update exposes the session token

- **Area:** connectivity
- **Nodes:** eng-ota, hw-wifi
- **Votes:** 1/1
- **Evidence:** sdkconfig.defaults:65 (ESP_HTTPS_OTA_ALLOW_HTTP=y); sdkconfig:493-494 (secure boot and signed apps off); ota.c:340-343 (url_valid accepts http://); ota.c:455-485 (/update embeds the 32-hex token for any LAN client while a session is live); web_authorized (ota.c:144-155). Session lasts 600 s and starts only when the user opens Phone setup. Only SHA-256 and rollback protect images.
- **Impact:** A LAN attacker could push firmware during a setup session.
- **Fix:** Require https, signed images, and do not render the token.

### G93 — LTE intent not restored at boot; Control Center LTE/GPS tiles, Modules and Settings disagree with the modem

- **Area:** connectivity
- **Nodes:** hw-lte, hw-gps, [[Control center]], [[Modules]], [[Settings]], [[Cellular]]
- **Votes:** 1/1
- **Evidence:** sys_control.c:179,234-235 (tile reads nos_prefs.lte); con_cell.c:96,193-194 (only place prefs are applied to the modem); nos_main.c:953-962 (radio_preferences handles BLE and Wi-Fi only); ext_lte.c:302 (idle power-down), :440-452 (boot probe); set_modules.c:1377-1380 (toggle does not update the pref); out_nav.c:469,474 (overrides the GPS tile). PWRKEY polarity and Settings GPS-row refusal not checked.
- **Impact:** Tile shows ON while the module is off; saved settings are not honoured after reboot.
- **Fix:** Give toggled modules a keep-alive owner and apply saved intent in radio_preferences.

### G94 — Cellular inbox bugs: body OK/ERROR corrupts listing, only 12 messages, stale snapshot when radio off, read state not persisted

- **Area:** connectivity
- **Nodes:** [[Cellular]], hw-lte
- **Votes:** 1/1
- **Evidence:** firmware/main/ext/ext_lte.c:178-186 (exact "OK"/"ERROR" lines tested before the body branch), :375 and :410 (ERROR switches to a CMGL form that may mark messages read), :39 and :224-225 (MAX_SMS 12, first 12 listed kept), :495 (read state in RAM only; SIM still reports REC UNREAD). Radio-off snapshot depends on the boot probe (not traced).
- **Impact:** Hidden or stale texts and read state resetting after reboot.
- **Fix:** Parse by length/state not text, raise the cap and refresh on open.

### G95 — Chat is a launcher tile that does nothing on B, with misleading dead controls and keep-awake

- **Area:** connectivity
- **Nodes:** [[Chat]], [[Launcher]]
- **Votes:** 1/1
- **Evidence:** registry.c:13 (unconditional tile); voice_chat.c:596-615 (non-C stubs, available=0); con_chat.c:131-132 ("needs the C board"), :160 ("ADD YOUR KEY OVER USB"), :234-238 (0%/UNMUTE state), :296 (tier buttons toast "Finish or cancel this turn first"), :311-314 (SCR_KEEPAWAKE); nos_main.c:2080.
- **Impact:** A dead-end app on the active board that drains battery if left open.
- **Fix:** Hide the tile when view.available is 0 or show a clear unavailable page without keep-awake.

## Low severity (103)

### G20 — Do Not Disturb silences only banners; calls, alarms, timers and find rings still ring

- **Area:** services
- **Nodes:** svc-notif, [[Call]], [[Control center]], svc-alarm
- **Votes:** 2/2
- **Evidence:** Only two readers of nos_prefs.dnd: svc.c:230 (banner) and nos.c:519 (notify feedback). svc.c:688-705 ringer_tick never reads it and buzzes every 2000 ms for find, call, alarm and timer. prefs.h:19 documents "banners silent". Alarms/timers ringing through DND is common watch behaviour; the real gap is incoming calls. No schedule or per-app filter.
- **Impact:** With DND on at night a phone call still buzzes the wrist every 2 s.
- **Fix:** Define DND scope (calls/alarms policy), add a schedule and per-app mute.

### G100 — Interactive screens without keep-awake collapse to home after ~15 s (faces picker, world clock, power sheet, Sports session)

- **Area:** shell
- **Nodes:** [[Faces picker]], [[World clock]], [[Power sheet]], [[Sports]], eng-power
- **Votes:** 1/1
- **Evidence:** nos_main.c:2078-2085 (timeout when top screen lacks SCR_KEEPAWAKE), :705 (nos_home_now on AOD/SENSOR), prefs.c:149,159 (sleep_s=15, sensor_idle=1). No keep-awake in sys_faces.c:443-447, time_worldclock.c:280-437, sys_power.c:371 or the workout session screen (fit_workout.c:1058-1066). Recording continues. Faces/world clock/power sheet are ordinary idle behaviour; the live workout is the real gap. Timeout is configurable (5-60 s).
- **Impact:** A live workout session drops to home mid-run and must be re-opened.
- **Fix:** Mark the session screen keep-awake or extend the timeout while interacting.

### G102 — Recorder: no delete/rename/export, only 24 memos listed, minimal playback, no way off the watch

- **Area:** apps
- **Nodes:** [[Recorder]], store-sd
- **Votes:** 1/1
- **Evidence:** con_memo.c:110 (MEMO_MAX 24), :215-218 (extra entries dropped), :1010-1102 (no remove/rename, no long-press), :387-398 (play/pause only); ext_audio_files.inc:61-75 (mono 16-bit WAV only). No BLE or Wi-Fi route reads the folder.
- **Impact:** Older recordings are unreachable and cannot be managed.
- **Fix:** Add paging, delete/share and playback controls.

### G103 — Recorder background recording does not keep display on and can only be stopped from the Recorder

- **Area:** apps
- **Nodes:** [[Recorder]], [[Home pager]], svc-memo
- **Votes:** 1/1
- **Evidence:** svc_memo_stop (svc.h:195) is called only from con_memo.c:578-581; scr_memo has no SCR_KEEPAWAKE (con_memo.c:1104-1105); nos_main.c:740,757 and ota.c:65-66 refuse shutdown/OTA while recording; sys_home.c:150-160 REC chip taps through to the Recorder; a RECORDING toast shows on leaving (con_memo.c:630). Documented as intended. 10-minute cap not found in con_memo.c.
- **Impact:** Users may think the watch is idle while it records and must return to the app to stop.
- **Fix:** Add a stop action on the chip/notification and a status cue.

### G104 — IMU recorder: failed start retries every ~5 s creating header-only CSV and link files; can start by itself; no auto-stop

- **Area:** apps
- **Nodes:** [[IMU]], hw-imu, store-sd
- **Votes:** 1/1
- **Evidence:** imu_rec.c:402-404 (retry every 5 s while intent.active), :213-225 (late failures leave intent.active=1), :250-299 (each retry makes a new NNNN.csv/.link; failures before file creation make none). Resume is by design (persisted intent, boot_resume). No auto-stop, battery or idle check found. imu_hal.c:100, board.c and nos_main.c:2036 not opened.
- **Impact:** File clutter, unexpected recording and battery drain from a forgotten recording.
- **Fix:** Back off and reuse the file, add low-battery and idle stop.

### G105 — Find Phone is optimistic and one-way: RINGING shown regardless of delivery, phone stop never seen, offline refused

- **Area:** apps
- **Nodes:** [[Find phone]], svc-phone
- **Votes:** 1/1
- **Evidence:** svc.c:965-972 (svc_find_phone sets state optimistically), :150-156 (txf drops silently), :1175-1178 (inbound find is the find-watch path), :1224 (cleared only on disconnect); con_find.c:253 (compares local state, never fires), :251 (keeps display awake), :230-233 (leaving sends stop), :249 (refuses offline). No ack, no ring timeout.
- **Impact:** The screen says the phone is ringing when it may not be, and keeps saying so after the phone stops.
- **Fix:** Add acknowledgment/timeout and handle phone-side stop.

### G106 — Music volume is open loop: phone level never read, optimistic play/pause

- **Area:** apps
- **Nodes:** [[Music]], svc-music, svc-phone
- **Votes:** 1/1
- **Evidence:** svc.c:325,331,1162,1172,1226,1286 (volume only ever set to -1); musicstate handler (:1164-1173) never parses volume; con_music.c:348-349,510-511 (assumes 50), :715-723 (steps of 10 units, no read-back); play toggle reverts after 2 s (con_music.c:84,114); position kept on same-source track change (svc.c:1162).
- **Impact:** Phone at 20% can show 50% and presses land at unknown levels.
- **Fix:** Hide the volume pill until reported, reconcile on musicstate.

### G108 — Jobs: init failure is silent and permanent

- **Area:** apps
- **Nodes:** [[Jobs]], eng-nos
- **Votes:** 1/1
- **Evidence:** nos/background.c:117-124 (background_init returns silently on alloc failure; observer never registered), :160-163 (tick returns early), :189; caller is registry.c:70; sys_background.c:85 shows MONITOR UNAVAILABLE with no reason. Alloc is small and fails only if the PSRAM budget is exhausted at boot.
- **Impact:** MONITOR UNAVAILABLE until reboot with no reason; background monitor data stops.
- **Fix:** Log and retry init.

### G109 — Jobs: failed on-demand sensors (heart, GPS, compass) look like normal idle

- **Area:** apps
- **Nodes:** [[Jobs]], hw-strap
- **Votes:** 1/1
- **Evidence:** background.c:29-33 (on_demand jobs), :175-178 (stale on-demand module job maps to IDLE), :197 (120 s window); ext_sys.c:197-199 and the ext modules never clear presence bits (only ext_audio.c:347 does); ext_hr.c:34-35 (40 s per 10 min); HR driver errors never reach BG_ERROR. IDLE for on-demand is partly deliberate.
- **Impact:** A faulted module is indistinguishable from idle and the Heart graph is usually empty.
- **Fix:** Add stale/error mapping and clear presence on failure.

### G110 — Background Jobs is a read-only monitor, not a scheduler: no controls, budget or energy attribution; 2-minute RAM history

- **Area:** apps
- **Nodes:** [[Jobs]], eng-power
- **Votes:** 1/1
- **Evidence:** background.h:1-2,41-48 (observability API only), :17-18 (BG_HISTORY 120, window 120 s, RAM); background.c:19-35 (metadata table, no cost field); sys_background.c:193-258 (inputs only select/switch/freeze/page). The 13 entries are monitored jobs, not 13 tasks. Missing feature, not a defect.
- **Impact:** No central way to bound or inspect background cost.
- **Fix:** Rename to Monitor or add a real budget/scheduler.

### G111 — Quick report writes a false event on open and defaults the label to MISSED

- **Area:** apps
- **Nodes:** [[Energy]], store-sports-journal
- **Votes:** 1/1
- **Evidence:** fit_energy.c:53-60 (sports_report with outcome -1 on open), :188 (display default MISSED, not saved), :261 (SAVE LABEL writes a second record); sports.c:196-206, :319-321,372-382 (retained records replayed into the journal at boot, no writer dedupe); readers dedupe by boot/seq (:424-427). Screen labels the first one "UNLABELLED MARK" (fit_energy.c:174).
- **Impact:** Duplicate and unlabelled marks in the raw journal, mostly hidden by readers.
- **Fix:** Post only on SAVE and dedupe replay.

### G112 — Wrist tuning labels/trials do not match detectors; raise-wake sub-options have no UI

- **Area:** apps
- **Nodes:** [[Energy]], [[Settings]], store-prefs
- **Votes:** 1/1
- **Evidence:** fit_energy.c:195-203 (threshold controls only for features <=2; others show LEARN VIEW), :269-273 (trial only toggles tuning_session); tuning.c:45-48 (only walk_g, knock_g, walk_peaks, quiet_ms adjustable); trial_expected never compared with detected events; wake_service.c:~70-75 pickup/turn flags set only by serial (main.c:856,858) or the legacy app; NOS calls wake_service_set only with "wrist" (set_settings.c:1840, nos_main.c:948).
- **Impact:** Users are led to expect tuning that does not exist.
- **Fix:** Add per-feature help and knobs and show detector enabled state.

### G114 — Reset settings also resets time zone, face, Wi-Fi intent and sports body prefs, leaving sportstune/wake_cfg

- **Area:** settings
- **Nodes:** [[Settings]], store-prefs
- **Votes:** 1/1
- **Evidence:** prefs.c:134-163 (prefs_defaults memsets everything, then face=0, wifi=0, tz_min=330); set_settings.c:1754-1775 (reset_all), :487,1375 (item "Reset settings", pill "TAP AGAIN TO RESET"). sportstune and wake_cfg not checked. Label reads as a full reset.
- **Impact:** Resetting settings silently changes zone, face, Wi-Fi and body values.
- **Fix:** Scope the reset or reset everything consistently with a clear warning.

### G115 — No date setter; Set time edits hour/minute only and fails with an unsynced clock

- **Area:** settings
- **Nodes:** [[Settings]], svc-time, hw-rtc
- **Votes:** 1/1
- **Evidence:** set_settings.c:2355-2371 (edits hour and minute, toast "DATE NOT SET: SYNC PHONE" when year is outside 2000-2099); hal_esp32.c:116-130,156 and board_protocol.c:19-25 (unsynced clock reads 1970); no date picker anywhere. SNTP over Wi-Fi and phone sync are alternatives.
- **Impact:** With a lost RTC and no phone or Wi-Fi the user cannot set the clock.
- **Fix:** Add date drums.

### G116 — 12-hour mode shows no AM/PM outside Alarms

- **Area:** settings
- **Nodes:** [[Settings]], [[Registry]], [[Control center]], [[World clock]]
- **Votes:** 1/1
- **Evidence:** ui.c:901-905 (ui_fmt_time prints %02d:%02d with no suffix); faces.c:79-100; callers include set_settings.c:595, time_worldclock.c:208,382, sys_control.c:349,536, time_timer.c:1066; AM/PM only in time_alarm.c:443-444,760,938; alarm drum is 0-23 (time_alarm.c:708). hour24 defaults to 1 (prefs.c:147). time_timer.c:302 is a bad citation.
- **Impact:** With 12 h on, 07:30 is ambiguous on faces, Set time, header and world clock.
- **Fix:** Add an AM/PM marker and fix the editor.

### G117 — Pedometer persistence skipped while the screen is on; steps regress after an unplanned reset

- **Area:** health
- **Nodes:** task-board-sense, svc-activity, [[Activity]]
- **Votes:** 1/1
- **Evidence:** board.c:45-47,951-956 (60 s save only when background persistence is allowed), :930-934 (shutdown save); nos_main.c:2141 disables persistence every frame while ON and re-enables it at :2098,2110; svc.c:541,558 (nosact saved every 10 min, lowers last_steps). Deliberate to avoid flash hitches during frames. Headline-step display not traced.
- **Impact:** Headline steps can drop after a crash during long active use.
- **Fix:** Allow the small save in ON mode when no DMA is active.

### G118 — Heart session average overwrites shared heart state and clears SpO2; clock-change quirks drop readings

- **Area:** health
- **Nodes:** svc-heart, [[Breathe]], [[Heart rate]]
- **Votes:** 1/1
- **Evidence:** svc.c:625-643 (svc_heart_record_at: spo2_last = 0 outside 70..100 at :642; rejects readings with epoch 0 or older than bpm_last_epoch at :628-631; bpm_last set to the session mean at :632-633); fit_breathe.c:353 calls it with spo2=0. svc_heart_record_at writes no history; the 24 h history comes from heart_tick.
- **Impact:** SpO2 disappears after Breathe and the heart widget can freeze after a backward clock step.
- **Fix:** Update spo2 only when valid, use monotonic sample_ms, append to history with a source flag.

### G119 — Activity screen: 7-day history only, no drill-down, unobserved days drawn as zero, no goal/inactivity feedback

- **Area:** health
- **Nodes:** [[Activity]], svc-activity
- **Votes:** 1/1
- **Evidence:** svc.h:96-98 (week[7], week_valid); svc.c:510-520 (missed days zero-filled); fit_activity.c:421-438 (unobserved days draw empty; only the average skips them), :540 (no tap handling), :76-112,502-510 (ring colour and goal line only). No goal-reached event or sedentary reminder in nos/. Label-format mismatch not verified.
- **Impact:** No trends or feedback beyond one week.
- **Fix:** Mark unknown days, add drill-down and a goal haptic.

### G120 — Heart and Breathe app gaps: resting HR not drawn, page does not refresh, SpO2 caveat missing, fixed breathing pace, no reminders

- **Area:** health
- **Nodes:** [[Heart rate]], [[Breathe]]
- **Votes:** 1/1
- **Evidence:** fit_heart.c:203,222-228 (resting computed, draw_history ~863-871 never shows it; header comment line 11 promises it), :706 and :544 (history read once per visit), :819-824 (SpO2 with no caveat; caveat only in ext_hr.c:19 comment); fit_breathe.c fixed 4/2/6 s pattern, no reminders.
- **Impact:** Missing expected features and a stale history page.
- **Fix:** Display resting, refresh periodically and add a live readout.

### G122 — Power polish: USB plug-in cue, SP_DEEP_SLEEP never emitted, cpu_mode retry spam, stale-age clause, 100 ms BLE callout, heap diag while off

- **Area:** power
- **Nodes:** eng-power, hw-battery, hw-ble
- **Votes:** 1/1
- **Evidence:** sports.h:12 SP_DEEP_SLEEP has no emitter; power_policy.h:9-10 age clause unreachable because hal_esp32.c:385-386 clears sample_valid at 3 s; nos_main.c:634-646 cpu_mode retries every 1 s and emits a record on each failure; hal_esp32.c:1368-1371 BLE callout re-arms forever; deep sleep sets only ext0 with no gpio hold (nos_main.c:~810); no wake or cue on external_power 0 to 1. Heap-diag-while-off not checked.
- **Impact:** Missing plug-in cue and small standby costs.
- **Fix:** Latch failures, stop re-arming, add the cue and measure deep sleep.

### G123 — Battery features missing: time remaining, health, history while charging; BLE battery log dead in NOS

- **Area:** power
- **Nodes:** [[Battery]], [[Power sheet]], hw-battery, [[Energy]]
- **Votes:** 1/1
- **Evidence:** hal_batt_log_tick has a single firmware caller, main.c:1172, which is not built (CMakeLists builds nos_main.c); battdump at main.c:780 also unbuilt; so the 24 h ring (hal_esp32.c:1226-1287) is never fed and the 0xB1-B3 dump is empty. core/hal.h:45-48 comment is stale. set_battery.c shows only level, rail voltage and age. "No navigation" and "blank history on USB" not checked.
- **Impact:** A bare percentage with no runway and a dead offline analysis path.
- **Fix:** Call the log tick or remove it, add an estimate and links.

### G125 — Gadgetbridge protocol edge cases: gps_power always false, only first GB( per line parsed, notification id handling, find-watch not cleared on disconnect

- **Area:** services
- **Nodes:** svc-phone, svc-notif
- **Votes:** 1/1
- **Evidence:** svc.c:1189 (is_gps_active reply hardcoded false), :1190 (calendar/nav/gps/http/actfetch unused), :1221-1227 (disconnect resets s_find_phone but not s_find_watch; it clears by dismissal or a 2*RING_MAX_MS timeout), :1231 (hardcoded hw string); gb.c:103 parses only the first GB( object per line. Duplicate-notification and 192-token cap not verified.
- **Impact:** Missing phone features and a find-watch ring that lingers after disconnect.
- **Fix:** Loop find_call, track tombstones and clear find on disconnect.

### G126 — Utility app polish: timer, stopwatch, alarm editor, world clock limits

- **Area:** services
- **Nodes:** [[Timer]], [[Stopwatch]], [[Alarms]], [[World clock]]
- **Votes:** 1/1
- **Evidence:** Timer: single tm_t, presets (time_timer.c:473), cancel without confirm (:766-770). Stopwatch: FB_ERROR on idle LAP (time_stopwatch.c:673), RESET without confirm; the 30-lap cap gives an FB_LIMIT haptic tick so it is not silent. Alarm: label field exists but no editor UI found; al_save runs twice (time_alarm.c:647,717). World clock: 13 cities, 4 favourites (time_worldclock.c:34-42), digital string computed but not drawn (~436), no reorder.
- **Impact:** Limited and sometimes surprising basic tools.
- **Fix:** Targeted fixes per item.

### G127 — Music remote feature gaps: no seek, album, shuffle/repeat, widget skip; air gestures need an optional module

- **Area:** services
- **Nodes:** [[Music]], hw-light, svc-phone
- **Votes:** 1/1
- **Evidence:** hal_media_seek (hal_esp32.c:1025) has no NOS caller; svc.c:1159-1163 fills album but con_music.c never shows it; no shuffle/repeat in con_music.c; gesture code is the optional APDS-9960 module (firmware/main/ext/ext_light.c). Equaliser, widget-skip and link-gate claims not checked.
- **Impact:** Only a basic remote.
- **Fix:** Add seek/shuffle, hide air controls when absent.

### G128 — Weather app: no refresh control, no sunrise/alerts, icon timing comment, ON WRIST label biased

- **Area:** services
- **Nodes:** [[Weather]], svc-weather
- **Votes:** 1/1
- **Evidence:** out_weather.c:880-905 (no refresh control; tap only replays the icon), :677 (ON WRIST shows the raw strap temperature), rain/UV only in demo data (:309). Not "wait only": weather_service.c:134,140 refetches every 1800 s (60 s on failure), hal_esp32.c:1981 refreshes on got-IP, and Settings > Modules has an RTC resync button (set_modules.c:1374).
- **Impact:** Stale or empty weather cannot be refreshed from the Weather app itself.
- **Fix:** Add pull refresh and correct labels.

### G129 — Nav stack edge cases: push during finger gesture does not reset owner; silent push/pull failures; air gesture overwrite

- **Area:** engine
- **Nodes:** eng-nos
- **Votes:** 1/1
- **Evidence:** nos/nos.c:303-325 (nos_push never touches P.owner; a back-swipe in progress keeps P.owner==1 against the new transition; plausible but not simulated), :305-309 (depth reject is logged to diag), :466 (nos_pull returns silently), :984-985 and :2334-2337 (single s_air slot overwritten).
- **Impact:** Rare dismissal of a new screen, unopened screens, lost gestures.
- **Fix:** Reset owner on programmatic nav and return codes.

### G130 — Clock/dt and timing inconsistencies: hal_millis freeze across tasks, off-screen dt, services 1 Hz alias, frame-stamped stopwatch

- **Area:** engine
- **Nodes:** eng-frame, svc-timer, task-ui
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:65-66,111-146 (non-atomic frozen millis latched per frame, nos.c:2321/2464); nos_main.c:2091-2095 (OFF passes dt=1.0), :2113-2118 (AOD uses measured dt); svc.c:1303-1329 (1000 ms slow gate can stretch to ~2 s); svc.c:917-937 (stopwatch reads frame-latched time). svc_track.c:103 is unrelated.
- **Impact:** Stale timestamps and up to one frame of stopwatch offset.
- **Fix:** Accumulate dt and use per-task clocks.

### G131 — USB/host protocol robustness: stale .part blocks retry, path truncation, spaces, no framing timeout, aborted upload parsed as commands, help/allowlists inconsistent, stale host verbs

- **Area:** engine
- **Nodes:** eng-usb-host, store-sd
- **Votes:** 1/1
- **Evidence:** sd_xfer.c:537-543 (sscanf "%127s" cuts paths at spaces and truncates), :356-362 (existing .part gives "SDPUT ERR staged"), failure paths do not remove the .part; reserved_path blocks .part names (:285-290). Other ranges (nos_main.c:889-997, 1380-1805, watchctl.py, command_input.c:7) not opened.
- **Impact:** Intermittent host errors and odd file names.
- **Fix:** Typed errors, validation and generated help.

### G132 — Sim renders blur (SHARP=0) while the device runs SHARP=1; sim hides device-only paths

- **Area:** render
- **Nodes:** eng-render, eng-blur
- **Votes:** 1/1
- **Evidence:** firmware/main/CMakeLists.txt:73 (NOS_RENDER_SHARP=1 on device); nos/render_config.h:19-23 (default 0, deliberate); nos.c:33-36 and :2301 (blur on only when SHARP=0); nos.c:1441-1445 (V-blur costs UNMEASURED); only SHARP=1 test is a native compile test (tests/test_sharp_render.py:26). Sim faking activity/HR and missing NVS/power models not checked.
- **Impact:** Reviewed visuals and perf budgets differ from the device.
- **Fix:** Add SHARP=1 and device-faithful sim modes.

### G133 — Hardware/driver minor: lux uncalibrated, CSV log writes low-pass snapshot, no TE sync on B, IMU/ENV values approximate

- **Area:** hardware
- **Nodes:** hw-light, hw-imu, task-display
- **Votes:** 1/1
- **Evidence:** firmware/main/ext/ext_light.c:9-11,30-31,211 (lux +-30 % until calibrated); pins.h:45 (no TE routed on B; PIN_LCD_TE only on C); board.c:706-746 (CSV logs one cached IMU sample plus battery mV). Low-pass claim not checked.
- **Impact:** Approximate readings and possible tearing.
- **Fix:** Calibrate and document.

### G134 — Compass/Level polish: false calibration prompt on first open, partial calibration saved on leave, magnetometer runs in Level mode, axis map serial-only

- **Area:** hardware
- **Nodes:** [[Compass]], hw-mag
- **Votes:** 1/1
- **Evidence:** out_compass.c:196-215 (cold open reads calibrated=0 and shows the CAL prompt; leaving calls hal_mag_calibrate(0) and logs CANCELLED), :347 (mag polled in Level mode); ext_mag.c:246-262 (accepts calibration at >=10 of 20 bins and overwrites the old one; save unchecked), :285-298 (axis map serial-only).
- **Impact:** Weaker calibration and wasted power.
- **Fix:** Confirm saves and skip the magnetometer in Level.

### G135 — Connectivity polish: raw error codes, fixed IN regulatory domain, mislabelled diagnostics, hotspot without indicator, BLE accessories not persisted

- **Area:** connectivity
- **Nodes:** [[Connectivity]], hw-wifi, hw-ble
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:643-678 ("Not connected (code N)"), :2289-2293 (country "IN", channels 1-13), no airplane mode, no GATT reconnect; set_connectivity.c:87-109,216,252 (diagnostics logged under fixed NETWORKS name as PENDING). No status indicator for the hotspot found; 10-minute AP timeout not checked.
- **Impact:** Confusing text and limited controls.
- **Fix:** Humanise errors and add indicators.

### G136 — Chat (C only) UX limits: transcript hides during a turn, leaving cancels it, 2 saved turns, non-English replies show ?, Wi-Fi only

- **Area:** connectivity
- **Nodes:** [[Chat]]
- **Votes:** 1/1
- **Evidence:** con_chat.c:30 (non-ASCII becomes ?), :49-57 (transcript drawn only when idle), :183 (LAST 2 TURNS), :201 (leave cancels the turn); voice_history.h:6; nos_main.c:731-757 (shutdown_guard cancel maps to "SAVE RECORDING FIRST"). Other items unchecked.
- **Impact:** Limited assistant behaviour on C.
- **Fix:** Address per item if Chat is kept.

### G137 — Faces polish: date English-only without month, battery shown only at <=20%, unread dot has no count, AOD minimal, saved face index not an ID

- **Area:** shell
- **Nodes:** [[Registry]], [[Dot]], [[Glyph]], [[Life]], [[Matrix]], [[Split]], [[Tape]], [[Faces picker]]
- **Votes:** 1/1
- **Evidence:** faces.c:107-110 (fk_date weekday and day only), :590-593 (status shown when charging or <=20%), :552-575 (plain unread dot); ui.c:914-917; sys_faces.c:92,373 (face index stored, bounds-checked); prefs.c:381-383 (2 s debounce). SPLIT temperature, PSRAM fallbacks and tap handling not checked.
- **Impact:** Reduced glanceability and fragile fallbacks.
- **Fix:** Targeted polish per face.

### G138 — ORRERY minor: events capped at 48, no alerts, SD textures not in firmware, state not persisted, no local sky

- **Area:** shell
- **Nodes:** [[Orrery]]
- **Votes:** 1/1
- **Evidence:** orrery_events.c:17,34-37 (NITEM 48; add() drops silently; ephemeris first, then flybys and comets, sort afterwards, so late rows drop); orrery_card.c:776 (textures read from SD), :1658 (flat circle with "[ PHOTO UNAVAILABLE ]"); face_orrery.c:1141-1146 (wall-clock fallback). Whether 48 is reached in practice not counted.
- **Impact:** Incomplete events and degraded visuals.
- **Fix:** Raise the cap, add retry and texture provisioning.

### G139 — Face and shell odds: TAPE lamp blinks regardless of recording, GLYPH date window never uses 12 o'clock

- **Area:** shell
- **Nodes:** [[Tape]], [[Glyph]]
- **Votes:** 1/1
- **Evidence:** face_tape.c:9-10,251-262 (red REC lamp blinks unconditionally, decorative); face_glyph.c:58-74 (k_at has 4 slots but slot_pick loops k<3, so slot 3 is never returned). No jumpy date effect.
- **Impact:** Misleading indicator; cosmetic.
- **Fix:** Tie the lamp to recording and fix slot hysteresis.

### G140 — Launcher/Home/Boot gaps: fixed list, no badges or long-press, last-app RAM-only, pull gestures only on face, boot not tied to readiness

- **Area:** shell
- **Nodes:** [[Launcher]], [[Home pager]], [[Boot]]
- **Votes:** 1/1
- **Evidence:** registry.c:9-37 (28 static rows, Developer and Diagnostics at :35-36), :40-47 (s_last_app RAM only); sys_launcher.c:1013 (ignores IN_BTN_LONG); sys_home.c:1088-1095 (sheets only from the face page); sys_boot.c (209-line animation, no readiness wiring).
- **Impact:** Slower navigation and no state cues.
- **Fix:** Add favourites, badges and persist the last app.

### G141 — Torch: only two looks, nothing remembered, entry ignores user brightness

- **Area:** apps
- **Nodes:** [[Torch]], store-prefs
- **Votes:** 1/1
- **Evidence:** con_torch.c:69-70 (white level reset to 1.0 on each entry, backlight 255), :44,154-164 (white and red only), :65-83 (nos_prefs.torch written, declared runtime-only at prefs.h:23, no reader found in nos/). Red level is kept within a boot (claim partly wrong). 4-step fade softens entry.
- **Impact:** Blinding first open and repeated setup.
- **Fix:** Persist the level and add modes.

### G142 — Navigate polish: units only for speed, silent save outcome, long tracks lose resolution, no waypoints/pace/elevation, last fix forgotten

- **Area:** apps
- **Nodes:** [[Navigate]], svc-track
- **Votes:** 1/1
- **Evidence:** out_nav.c:246-250 (distance fixed to m/km), :911 (metres), :838-848,531-536 (SAVED / SAVE FAILED word only), :457,549 (state memset on open); svc_track.c:149-157 (full buffer keeps every other point); out_nav.c:9,911 (HDOP called accuracy). Speed unit does follow the pref. No waypoint, pace or elevation code found.
- **Impact:** A recorder with a back arrow rather than a navigator.
- **Fix:** Add summary stats and file listing.

### G143 — Cellular polish: no contact names, five fixed replies, SENT overstates delivery, denied registration shows SEARCHING, PIN dead end, timestamps carrier time

- **Area:** apps
- **Nodes:** [[Cellular]], hw-lte
- **Votes:** 1/1
- **Evidence:** con_cell.c:545-546 (5 fixed replies), :173-178 (denied registration falls to SEARCHING), :335 (SIM LOCKED: remove PIN on a phone), :682 (SENT means SMSC accepted); ext_lte.c:46 (CNMI=2,1,0,0,0, no delivery report), :213-219 (carrier timestamp), :520 (160-char cap), :156 (UCS-2 decode ASCII-only). No contact lookup. NO SIM flash on wake not checked.
- **Impact:** Limited messaging on the optional LTE strap.
- **Fix:** Add contact lookup and a DENIED state.

### G144 — IMU app polish: per-segment counters reset, chain hidden in FILES, orphan .link on delete, scale labels, filtered LIVE

- **Area:** apps
- **Nodes:** [[IMU]]
- **Votes:** 1/1
- **Evidence:** imu_rec.c:314-333 (delete removes only NNNN.csv, not the .link written at :286); out_imu.c:43,179 (rate resets each open), :62-68,150-152 (fixed scale labels, raw axes not rotated); imu_rec.c:120-121,196-198 (counters reset per segment). Numbering from 0000 claim is wrong (id = intent.file+1).
- **Impact:** Confusing long-recording accounting.
- **Fix:** Session totals and link cleanup.

### G145 — Energy/Jobs/Recorder minor: single-step UNDO, mixed offset domains, coarse routing, dB bands uncalibrated, number formatting

- **Area:** apps
- **Nodes:** [[Energy]], [[Jobs]], [[Recorder]]
- **Votes:** 1/1
- **Evidence:** sports.c:352-365 (undo is single-step by design); ext_audio.c:168-169 (SPL = 120 + dBFS from a datasheet -26 dBFS at 94 dB, 30 dB floor); con_memo.c:942-977 (QUIET to HEARING RISK bands, 85 dB threshold, not labelled as an estimate). The offset-domain, frozen-view-label and events-ring claims were not supported (sys_background.c:140 does label frozen views).
- **Impact:** Hearing-risk labels can mislead.
- **Fix:** Calibrate or relabel.

### G146 — Developer/Diagnostics/Modules gaps: trace shows fault slot 0 only, test mark reports SUCCESS before write, overlays have no indicator, Unlimited fps cost, modules status inaccuracies

- **Area:** apps
- **Nodes:** [[Developer tools]], [[Diagnostics]], [[Modules]]
- **Votes:** 1/1
- **Evidence:** set_developer.c:22 (diag_log_fault_get(0) only), :69-71 (SUCCESS reported right after the request). Overlay claim overstated: set_developer.c:114 lists each overlay ON/OFF. Unlimited-fps cost is inferred. set_diag.c and set_modules.c items minor and not each checked.
- **Impact:** Weak on-watch diagnostics.
- **Fix:** Show all slots, report real completion.

### G147 — About/Battery page gaps: hard-coded 30 fps and board name, no MAC/slot/reset reason, uptime wraps at 49.7 days

- **Area:** apps
- **Nodes:** [[About]], [[Battery]]
- **Votes:** 1/1
- **Evidence:** set_about.c:251 ("466 x 466 AMOLED, 30 fps" while NOS_FPS is 40 and fps_cap defaults to 60), :245-248 (board string correct for B), :93 (uptime from 32-bit hal_millis, wraps ~49.7 days); no MAC, OTA slot or reset-reason rows.
- **Impact:** Wrong frame-rate statement; support needs serial.
- **Fix:** Derive from fps_cap and add identity rows.

### G149 — Brightness slider and frame loop use different curves; auto-brightness reasserts level

- **Area:** settings
- **Nodes:** [[Settings]], [[Control center]]
- **Votes:** 1/1
- **Evidence:** set_settings.c:2090, sys_control.c:253, nos.c:580 (v*255/100); nos_main.c:648-656 (6 + pct*2.49, times auto factor), :2226 (set_level every frame unless overridden). Difference is about 6 levels at low percent, none at 100%. Auto factor is labelled.
- **Impact:** Displayed percent does not exactly match panel level.
- **Fix:** Use one curve.

### G150 — Settings polish: pickers commit every detent with diagnostics, step goal row/picker mismatch, prefs only partly range-checked, learn result only while open, common settings absent

- **Area:** settings
- **Nodes:** [[Settings]], store-prefs
- **Votes:** 1/1
- **Evidence:** set_settings.c:1927-1945 (pick_commit logs and saves; pk_tick not opened), :596 (step goal row), pick_enter (2000 + i*500 grid), :688-695 (learn_wait polled only while open); prefs.c:165-191 (normalize covers sleep_s, sports fields, wrist_source, fps_cap, display_te only). Plus-boot backlight and LTE claims unchecked.
- **Impact:** Log flood and minor confusion.
- **Fix:** Confirm each step, clamp loaded values.

### G151 — Dead/legacy code and dead prefs compiled in (core/notif.c, core/battery.c, display_perf, ota_from_sd, face_accent, move_goal_10)

- **Area:** settings
- **Nodes:** eng-nos, store-prefs
- **Votes:** 1/1
- **Evidence:** prefs.h:21 (face_accent) and :27 (move_goal_10) have no readers (prefs.c:34,36 only list them); firmware/main/nos_legacy.c is a 6-line shim for core/settings.c and core/wake_service.c; CMakeLists.txt:18-19 compiles core battery.c, notif.c, settings.c. ota_from_sd and display_perf are not dead (console command at main.c:694; display_bench.c:120-143).
- **Impact:** Flash bloat and maintenance noise.
- **Fix:** Remove or wire the dead items.

### G152 — Documentation drift (PMIC_AUDIT, PHONE.md, SPORT_MODE.md, os-map notes, ui.h comments) contradicts code

- **Area:** settings
- **Nodes:** eng-nos
- **Votes:** 1/1
- **Evidence:** docs/PMIC_AUDIT.md:36-38 cites nos_main.c:1322-1362 and :1340/1359, which are now a command parser and hal_setup_info; docs/SPORT_MODE.md:23 says there are no calorie claims while sports.c:171,391, background.c:155 and svc.c:46,445 publish kcal. PHONE.md frame-rate/storage contradictions not found.
- **Impact:** Maintainers cannot trust the docs.
- **Fix:** Refresh in one pass.

### G17 — Food/water logging refused once the 128-entry ring wraps unless SD is mounted and old slots exported

- **Area:** storage
- **Nodes:** [[Energy]], store-ledger, store-sd
- **Votes:** 1/2
- **Evidence:** sports.c:45 (EN=128), :326-337 (archived_entry), :338-350 (sports_intake refuses to overwrite an unarchived slot); fit_energy.c:238-239 (SAVE FAILED - TRY AGAIN). With a card the writer archives entries automatically (enqueue :134, re-enqueue at init and card insert); hal_sports_sd_lock (board.c:93-97) waits 50 ms. Fails only with no card, unmounted card or a busy writer. Deliberate no-data-loss guard.
- **Impact:** Energy logging can fail with SAVE FAILED after 128 writes without a working card.
- **Fix:** Archive in the background or let the ring overwrite with an explicit warning and queue the archive.

### G31 — Any date/timezone change zeroes today's steps and can wipe the week history

- **Area:** health
- **Nodes:** svc-activity, task-board-sense, svc-time
- **Votes:** 1/1
- **Evidence:** core/pedometer.c:11-14 (any day != today resets steps, no direction check); svc.c:512-519 (gap<0 or >7 clamps to 7, shifting the week to all zero), :1000-1018 (apply_time); hal_esp32.c:822-834 (BLE tz change skips board_calendar_ready). Forward day change is arguably correct; the backward jump wipe is the defect.
- **Impact:** A phone time correction or timezone change across midnight loses today's steps and six days of history.
- **Fix:** Ignore backward/small date steps, key records by date, wrap TZ/clock changes in board_calendar_ready.

### G32 — Persisted max steps shown as live when the pedometer is invalid; no validity flag

- **Area:** health
- **Nodes:** svc-activity, [[Activity]], [[Segment]], [[Inverted]], [[Home pager]]
- **Votes:** 1/1
- **Evidence:** svc.c:532-533 (invalid sample publishes s_as.max_steps), :440-455; svc.h:91-101 (no validity field); board.c:495-506 (invalid when prefs loading, clock not ready or sample >=250 ms old). Deliberate fallback; the invalid window is normally short. Consumers: sys_home.c:469, face_segment.c:260, face_split.c:75, fit_activity.c:46.
- **Impact:** A plausible stale step count appears after boot or sensor stall.
- **Fix:** Add a valid flag and show a placeholder until valid.

### G33 — Step detector rejects racket/vigorous motion, needs 8 coherent peaks and has no wear gating

- **Area:** health
- **Nodes:** svc-activity, task-board-sense, svc-workout
- **Votes:** 1/1
- **Evidence:** core/walk.c:7-13 (ENTRY_PEAKS 8, intervals 300-1500 ms), :59-79 (CV <= 0.25), :76-77 (>3 g or >250 dps ends a bout); workout.c:288 (BADMINTON uses_steps=0, inferred); svc.c:553 (active minute needs >60 steps). No wear gating found. Vehicle overcount is speculative; thresholds are noted as a first-field-trial.
- **Impact:** Undercount for short, fast or racket activity.
- **Fix:** Field-validate, use cadence-aware thresholds and wear gating.

### G34 — Activity goals mostly hard-coded; two active-time definitions; stand hours and move goal unused

- **Area:** health
- **Nodes:** [[Activity]], [[Segment]], svc-activity, store-prefs
- **Votes:** 1/1
- **Evidence:** face_segment.c:266 (goals { step goal, 400, 30 }); svc.c:553 (active minute = >60 steps in a clock minute), :472 (steps/92 fallback); pedometer active_seconds not read by nos/; prefs.h:27 (move_goal_10 reserved, unused); svc.c:451-453 (stand_hours computed, no UI); set_settings.c:1936 (only step goal adjustable).
- **Impact:** Two of three rings cannot reflect user targets; ACTIVE does not match actual walking.
- **Fix:** Pick one active-time source and wire move/energy goals to prefs and the UI.

### G35 — Energy BURN chart is a cumulative staircase; zoomed range shows the running total

- **Area:** health
- **Nodes:** [[Energy]], store-sports-journal
- **Votes:** 1/1
- **Evidence:** sports.c:405-411 (bin burn = latest cumulative kcal), fit_energy.c:94-97,142-145 (header uses the max bin), :164,175 (labelled DAILY ESTIMATE, ACTIVITY CALORIES ONLY). No BMR, balance or target code. SD-history dependency not checked.
- **Impact:** The screen does not show how much was burned per hour or any balance.
- **Fix:** Plot per-bin deltas and add a BMR and balance row.

### G36 — Stale bpm_last shown on faces and sent to the phone as live heart rate

- **Area:** health
- **Nodes:** svc-heart, [[Split]], [[Home pager]], svc-phone, [[Breathe]]
- **Votes:** 1/1
- **Evidence:** svc.c:602-605,629-632 (bpm_last never cleared or expired), :1145 (hrm sent with no age check, only when the phone requested realtime hrm, :1181); sys_home.c:473; face_split.c:76,109,127-128 (red at >=120 or <40). Only fit_heart.c:961-964 shows an "ago" label.
- **Impact:** An hours-old or breathing-session reading appears as the current HR and can trigger false alarm colours.
- **Fix:** Gate on bpm_last_epoch age (<2 min) and show -- otherwise.

### G37 — Heart history, resting and min/max are RAM only

- **Area:** health
- **Nodes:** svc-heart, [[Heart rate]], hw-strap, store-sd
- **Votes:** 1/1
- **Evidence:** ext_hr.c:36,61,317 (144-entry history in a PSRAM struct, never saved), :322 (first background sample 2 min after boot), BG_EVERY_MS 10 min; svc.c:72-73 (day min/max statics), :586-618 (heart_tick rebuilds from the ring every 60 s); s_as has no heart fields.
- **Impact:** LAST 24 H, resting and last reading reset on every restart; no multi-day trend.
- **Fix:** Persist daily HR buckets or rebuild from the SD journal.

### G39 — Strap-dependent features give no explanation when the module is absent (HR, GPS, LTE, rows on launcher)

- **Area:** health
- **Nodes:** [[Heart rate]], [[Sports]], [[Breathe]], [[Navigate]], [[Launcher]], [[Control center]], [[Split]]
- **Votes:** 1/1
- **Evidence:** Mostly handled already: sys_control.c draws absent-strap tiles disabled and explains on tap; fit_heart.c:778 shows "Not connected / Add the strap"; out_compass.c:203, out_nav.c:537 gate on presence; set_modules.c reports presence. Remaining: fit_breathe.c:488-489 prints "HR COVERAGE 0%"; launcher lists Chat, Navigate, Cellular, Recorder unconditionally (native_menu_assets.c); no hal_ext_present call in fit_workout.c; faces show "--".
- **Impact:** On a bare 1.43-B some apps look broken instead of saying they need the strap.
- **Fix:** Gate or mark launcher rows by presence and show explicit needs-strap messages.

### G40 — Breathe: stray swipe/button or idle timer discards the session; nothing is saved

- **Area:** health
- **Nodes:** [[Breathe]], svc-heart, store-prefs
- **Votes:** 1/1
- **Evidence:** fit_breathe.c:574-575 (flags 0, no NOBACK), nos.c:852-856 (right drag is system back), :952 (button goes home); fit_breathe.c:224-230 (leave keeps nothing), :353 (HR average recorded only at natural end), :92 (duration in a static, RAM only), :301,326 (session keeps the screen awake, so the idle-timer claim is false); summary has no keep-awake and follows the sleep_s pref.
- **Impact:** An accidental swipe loses a 1-5 minute session; slow readers can miss the summary.
- **Fix:** Confirm on leave, keep the summary awake and persist session totals.

### G41 — Live BURSTS metric misleads for continuous play; movement time only in DETAILS

- **Area:** sports
- **Nodes:** [[Sports]], svc-workout
- **Votes:** 1/1
- **Evidence:** fit_workout.c:376-386 (live "BURSTS / EST." with "WRIST MOVEMENT / NOT REPS"), :997-1001 (movement time only in DETAILS); docs/SPORTS_RELIABILITY_VALIDATION.md:46-47 (8 episodes in 267.76 s with 223.76 s movement). Already recorded in os-map Sports.md:181.
- **Impact:** Badminton users get a burst count that does not reflect effort or rallies.
- **Fix:** Show movement time/percentage on live and summary and relabel bursts.

### G43 — No link between a workout and its raw sensor data

- **Area:** sports
- **Nodes:** svc-workout, store-sports-journal, [[IMU]]
- **Votes:** 1/1
- **Evidence:** workout.c:207-211 (epoch() returns 0 when the clock is unset); workout.h:72 (started_epoch, ended_epoch only); sports.h:8-11 (no workout start/stop/sport marker); out_imu.c:184 (CSV recorder started manually); imu_rec.c:296 (CSV has utc_ms, so matching by time window works when the RTC is set).
- **Impact:** Raw badminton data cannot be reliably attributed to a session.
- **Fix:** Emit session start/stop records and optionally auto-start the CSV recorder.

### G44 — Workout cannot start without a readable SD history (no RAM-only session)

- **Area:** sports
- **Nodes:** [[Sports]], svc-workout, store-sd
- **Votes:** 1/1
- **Evidence:** workout.c:817 (start refused when !storage.ready), :683 (ready set only after a successful load); fit_workout.c:536-556 (only RETRY offered), :299 ("ORIGINAL SD NEEDED TO SAVE"); tests/workout_test.c:388-392,517-520 assert it. Deliberate design.
- **Impact:** No recording at all on a card-less or card-fault watch.
- **Fix:** Allow RAM-only sessions with an unsaved warning.

### G46 — No phone sync, export or route link for workouts, food/water or the sports journal

- **Area:** sports
- **Nodes:** [[Sports]], [[Energy]], svc-track, svc-phone, store-sd
- **Votes:** 1/1
- **Evidence:** No workout reference in nos/gb.c, host_request.c or ble_xfer.c; workout.c:464 (GPS used only for distance and speed); workout.h:10 and workout.c:873-875 (12-session history, silent eviction); no delete function; svc_track.c:205-255 (GPX writer driven only by out_nav.c, not linked to sessions); only dedicated export is tools/workouts_export.py. Generic sd_xfer (firmware/main/sd_xfer.c) can still pull workouts.N.bin and GPX files. Food/water not checked.
- **Impact:** Users cannot review a route or move data to the phone; old sessions vanish.
- **Fix:** Link tracks to sessions, add delete UI and phone sync.

### G47 — Always-on strap sensors (env, light, HR background, mag, GPS) run with the screen off and no consumer

- **Area:** power
- **Nodes:** hw-env, hw-light, hw-ppg, hw-strap, [[Barometer]]
- **Votes:** 1/1
- **Evidence:** ext_sys.c:144-160 (ext_task services all modules unconditionally); ext_env.c:20,130-132 (every 2 s); ext_light.c:88-89,189 (every 400 ms with proximity); ext_hr.c:34,246-249,301-304 (5-40 s every 10 min); set_modules.c has no enable/disable. "No consumer" is wrong: auto-brightness (nos_main.c:675), baro/weather (out_baro.c:345, out_weather.c:630) and the sports journal read them. Applies only with a strap fitted.
- **Impact:** Hidden battery drain with a strap fitted and no way to disable it.
- **Fix:** Gate sampling by consumer demand, display state and wear.

### G48 — Battery percent is an uncalibrated OCV estimate; calibration serial-only, wobbles, lost on reboot

- **Area:** power
- **Nodes:** hw-battery, [[Battery]], [[Power sheet]]
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:299-308 (piecewise OCV table), :361 (+25/+10 mV load allowance toggled with power state, nos_main.c:701), no hysteresis; nos_main.c:1607 (batcal serial only); hal_esp32.c:2344-2345 and :356 (no s_cali means battery_valid never set). "Lost on reboot" is wrong: bat_cal is saved and reloaded (hal_esp32.c:314,318).
- **Impact:** Percent can jump a few points on wake/sleep, may never appear, and cannot be corrected on the watch.
- **Fix:** Monotone-discharge filter with hysteresis, persist the last percent, surface calibration errors.

### G49 — Low-battery saver state and thresholds invisible and not configurable

- **Area:** power
- **Nodes:** eng-power, [[Battery]], [[Power sheet]], [[Settings]]
- **Votes:** 1/1
- **Evidence:** power_policy.h:6-11 (on at <=15%, off at >=20%, hard-coded); nos_main.c:633,2030-2038,2061-2064 (s_low_battery is file-static; toast on transition and on wake while low); nothing in set_battery.c shows saver state. The saver forces the idle target OFF and disables raise/knock wake, and is bypassed on external power.
- **Impact:** Users cannot see why the display turns off at low charge.
- **Fix:** Show saver status and add a saver setting.

### G51 — Other raw sensor streams (PPG, magnetometer, mic level, env) journaled to SD without disclosure or control

- **Area:** storage
- **Nodes:** hw-ppg, hw-mag, hw-env, [[Recorder]], store-sports-journal
- **Votes:** 1/1
- **Evidence:** Emit sites: ext_hr.c:284,296, ext_mag.c:187,210, ext_audio.c:172-179 (about 31 records/s, dB level only), ext_env.c:174; sports.c:56 (raw types), :119-130, :215-218, :243-272 (hourly sensors.spt files). No pref gate, no clear control; only quiesce at shutdown. A diagnostics tile (background.c:26) shows counts only.
- **Impact:** Undisclosed SD writes and a persisted noise history the user cannot see or clear.
- **Fix:** Gate behind a developer or per-session flag and add retention.

### G52 — Activity buckets (nosact) not flushed at shutdown/OTA; checkpoint status unread

- **Area:** storage
- **Nodes:** svc-activity, store-nvs, eng-power
- **Votes:** 1/1
- **Evidence:** svc.c:496-509,527,558 (act_save every 10 min or at rollover); nos_main.c:763-800 and board.c:931-936 flush prefs, voice, sports and pedometer but not nosact; svc.h:99-100 checkpoint fields have no reader. A failed save emits a diag event (svc.c:502). Pedometer total is saved separately.
- **Impact:** Up to 10 minutes of hourly and active-minute data lost or mis-binned.
- **Fix:** Add a flush to the shutdown/OTA barrier and expose checkpoint errors in diagnostics.

### G53 — Synchronous NVS/SD writes (nosact, alarms, tuning, sports, prefs) run on the UI task

- **Area:** storage
- **Nodes:** task-ui, store-nvs, svc-activity, store-prefs
- **Votes:** 1/1
- **Evidence:** svc.c:496-508,721-728, sports.c:347, tuning.c:40 save inline via hal_store_save_checked; hal_esp32.c:177-257 (nvs_commit in the caller's task). Prefs defer via a dirty flag (prefs.c:260-265, 365-382) but also flush on the UI task and with heavier SD work, so "unlike prefs" is wrong. Stall length unmeasured; saves are infrequent.
- **Impact:** Possible frame hitch of tens to hundreds of ms when a save lands.
- **Fix:** Gate through the board-fs persistence window or a worker with an internal stack.

### G54 — Several saves are unchecked or never retried: alarms, world clock, sea-level ref, compass cal, battery cal, Control Center prefs

- **Area:** storage
- **Nodes:** svc-alarm, [[World clock]], [[Barometer]], hw-mag, [[Control center]], store-nvs
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:256-257 (void hal_store_save drops the result); time_worldclock.c:60, ext_mag.c:260, hal_esp32.c:314 use it; svc.c:721-731 (checked, but failure only traced); out_baro.c:686-692 (SAVED shown unconditionally). Control Center uses prefs_save, which retries every 5 s (prefs.c:260-264, 345-381), so only the user-facing error is missing. These keys are NVS-only.
- **Impact:** An alarm, calibration or reference could silently revert after reboot if the NVS write fails (rare).
- **Fix:** Use hal_store_save_checked with dirty/retry state and surface failure.

### G55 — sportstune and wake_cfg are NVS-only, violating the SD-authoritative settings rule

- **Area:** storage
- **Nodes:** store-prefs, store-nvs, [[Settings]]
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:238 (only nosprefs and wifi_cred route to SD), :253; nos/tuning.c:18,40 and core/wake_service.c:57,63 use hal_store; no reset code touches either key. Saves are checked. Whether the SD-authoritative rule covers every key is not confirmed.
- **Impact:** Tuning is lost on an NVS erase and survives Reset inconsistently.
- **Fix:** Fold into the SD checkpoint or add SD routing.

### G56 — NVS health invisible: init failure ignored, capacity gauge boot-time with wrong denominator

- **Area:** storage
- **Nodes:** store-nvs, [[Modules]]
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:2316-2322 (NO_FREE_PAGES/NEW_VERSION_FOUND only logged, deliberately not erased); board.c:517-533,923 (stats read once at boot); set_modules.c:1307,1314 (percent from that snapshot). partitions.csv:5 gives 756 entries but about 630 are usable, so the percent is optimistic. B's primary settings are on SD.
- **Impact:** NVS-only data can revert to defaults for a boot with no notice; the gauge shows an optimistic stale value.
- **Fix:** Expose an NVS-unavailable flag, refresh the gauge against available entries, keep no-silent-erase.

### G57 — NVS or RTC save failure can block shutdown/restart

- **Area:** storage
- **Nodes:** task-board-fs, store-nvs, hw-rtc, eng-power
- **Votes:** 1/1
- **Evidence:** board.c:903-912 (failed pedometer save returns 0), :935-946, :1150-1152 (sleep_failed on !fs_ok or dirty RTC), :801-828 (rtc_maintain); firmware/main/nos_main.c:798-801 (cancels shutdown, DIAG_SLEEP_BLOCK 16). Other esp_restart paths (nos_main.c:144, 836, ota.c) bypass it. rtc_maintain retries every 5 s. Fails safe by design.
- **Impact:** With NVS full or an RTC fault the watch cannot power off or restart cleanly via the UI.
- **Fix:** Retry once, time-limit the flush and proceed after logging.

### G58 — SD transfer verbs: malformed args silent with ok=1; sdrm can delete txn/backup files

- **Area:** storage
- **Nodes:** eng-usb-host, store-sd
- **Votes:** 1/1
- **Evidence:** sd_xfer.c:536-546 (failed sscanf returns 0 with no reply), nos_main.c:860-865 (DONE ok=1 unless an " ERR " line is printed), host_request.c:19-25; reserved_path (sd_xfer.c:283-289) applied on put but not on sdrm (~600-604). recover_transaction runs before sdrm (~563), so the backup-loss scenario is largely mitigated.
- **Impact:** Hosts believe failed verbs succeeded; leftover .part or .sdxfer-old files can be deleted.
- **Fix:** Return a typed ERR with ok=0 and refuse sdrm of journal files while a transaction is pending.

### G60 — Energy/Battery history re-reads the full day journal every 10 s; drag-zoom can yield an empty range error

- **Area:** storage
- **Nodes:** [[Energy]], store-sports-journal, store-sd
- **Votes:** 1/1
- **Evidence:** fit_energy.c:70 (refresh every 10 s), :32-41; sports.c:465-520 (whole day file re-read, no cache); fit_energy.c:224-226 (integer-division zoom reaches start==end), sports.c:434 (rejects start>=end), fit_energy.c:38 (HISTORY UNAVAILABLE); :71-75 (Sensor logs poll each second). Only while those screens are open; the 180-200 KB size was not verified.
- **Impact:** Sustained SD/CPU load while open and a false error after extreme zoom.
- **Fix:** Cache bins, enforce a minimum zoom width, refresh incrementally.

### G62 — Gadgetbridge alarm message replaces all watch alarms; watch alarms not uploaded

- **Area:** services
- **Nodes:** svc-alarm, svc-phone, [[Alarms]]
- **Votes:** 1/1
- **Evidence:** svc.c:1112-1138 (gb_alarms zeroes the table, rebuilds from the phone list with label Phone, max 8, saves immediately; empty d deletes all), :1179 dispatch, :743 (watch alarms via svc_alarm_add); no upload of watch alarms. Full-list replace is the usual Gadgetbridge convention. Not verified on device.
- **Impact:** Alarms created on the watch vanish after a phone sync.
- **Fix:** Merge by origin or replace only phone-origin alarms.

### G63 — Alarm ring/snooze defects: same-minute alarms lose one, snooze-cancel disables recurring alarm, covering screen stops it

- **Area:** services
- **Nodes:** [[Alarms]], svc-alarm
- **Votes:** 1/1
- **Evidence:** svc.c:803-808,836-840 (alarm_ring overwrites s_ring_alarm; one-shots disabled in the same loop); time_alarm.c:507-512,566-567 (snooze_cancel sets on=0 for the whole row, noted as deliberate). The covering-screen claim is false: ring_screen returns 1 only when the ring screen is out of the whole stack (svc.c:659-676, nos.c:349-354).
- **Impact:** Rarely, a same-minute alarm is missed, and cancelling a snooze can switch off a recurring alarm.
- **Fix:** Queue simultaneous alarms and cancel only the snooze.

### G64 — Timer, stopwatch and snooze state are RAM only and lost on reboot or power off

- **Area:** services
- **Nodes:** svc-timer, [[Timer]], [[Stopwatch]], svc-alarm
- **Votes:** 1/1
- **Evidence:** svc.c:76-79,88-89 (plain statics with hal_millis deadlines), :856-938 (no store call), :1117 (snooze cleared on init); only nosalarm and nosact are stored; nos_main.c:803-811 (restart/deep sleep drops RAM).
- **Impact:** A running 25-minute timer or long stopwatch session disappears after a restart.
- **Fix:** Checkpoint the end epoch and laps with hal_store_save_checked.

### G65 — Notification store is RAM only (20 items, silent eviction); lost on reboot

- **Area:** services
- **Nodes:** svc-notif, [[Notifications]], [[Registry]]
- **Votes:** 1/1
- **Evidence:** svc.c:8,10-11 (only nosalarm and nosact persisted), :206-234 (cap 20, oldest dropped with no trace), :1280-1285 (cleared at svc_init); svc.h:32. No phone resync found in gb.c (Gadgetbridge side not checked).
- **Impact:** Missed messages after restart; the unread dot clears; older items vanish.
- **Fix:** Persist a compact ring and resync unread on reconnect.

### G66 — Notification actions unreliable: OPEN says Opened when a no-op, dismissals offline lost, detail goes dead

- **Area:** services
- **Nodes:** [[Notifications]], svc-notif, svc-phone
- **Votes:** 1/1
- **Evidence:** sys_notifs.c:1026-1030 (OPEN toasts "Opened" on link state only), svc.c:270-274 (silent return for phone=0 companion items, posted at :1259-1260); svc.c:245-258, :150-156, hal_esp32.c:1190+ (dismiss sent once; retried up to 20 times on mbuf exhaustion then dropped; local item removed regardless); sys_notifs.c:~936-945 (stale detail keeps its buttons; STALE return gives no feedback).
- **Impact:** Cards vanish on the watch but stay on the phone; buttons silently do nothing.
- **Fix:** Check item origin, queue dismissals for replay, pop or flag a stale detail.

### G67 — Call screen state gaps: outgoing/active calls untracked, declines read as MISSED, no call log, ring has no timeout

- **Area:** services
- **Nodes:** [[Call]], svc-phone
- **Votes:** 1/1
- **Evidence:** svc.c:1044-1068 (state 2 never opens the screen unless a ringing call was accepted), :977 (ring_screen), RING_MAX_MS (:47) applied to alarms, timers and find only; :1066-1067 (remote end, reject or missed all map to state 0); con_call.c:91 (missed = previous state was incoming); svc.c:1051-1057 (second incoming overwrites an active call), :943-950 (accept is fire-and-forget). A decline made on the watch is not mislabelled (con_call.c:467-469). No call log found in the opened files.
- **Impact:** Phone-side declines read as MISSED CALL, no End or timer for some calls, and an endless ring if the end message never arrives.
- **Fix:** Model call states explicitly, add a ring timeout, call log and acknowledgments.

### G68 — Phone link receive-side loss is silent; ring not flushed on disconnect

- **Area:** services
- **Nodes:** svc-phone, hw-ble
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:1092 (s_nus_dropped incremented at 1114, 1124, 1212, never read), :1130 (overlong lines dropped), ~1421 (disconnect resets assembly only, not the ring); svc.c:1236 (LINES_PER_FRAME=8), :1222-1226 (disconnect clears find/call, but queued lines are handled afterwards). Bad JSON and unknown types are dropped without a log (svc.c:1200-1210).
- **Impact:** Lost messages leave stale state; a phantom call or find ring is possible after link loss.
- **Fix:** Flush the ring on disconnect, count and log drops, raise lines per tick when idle.

### G69 — Phone weather path is thin and misleading (today only, no hourly, defaults to 0 C)

- **Area:** services
- **Nodes:** svc-weather, [[Weather]]
- **Votes:** 1/1
- **Evidence:** svc.c:1073-1110 (memset; ndays=1 at :1098; hi/lo default to temp at :1084-1085; missing temp becomes 273.15 K = 0 C at :1081; Wi-Fi merge skipped at :1102-1103); :416-421 (wx_tick returns early while phone data is under 3 h old); docs/PHONE.md:58 says otherwise. How the UI renders nhours=0 not checked.
- **Impact:** Flat or empty hourly and days pages when weather comes from the phone.
- **Fix:** Mark unknown fields invalid and keep the Wi-Fi forecast beside the phone current.

### G70 — Wi-Fi weather lacks humidity/wind/rain/UV; phone UV/rain/wind-dir never drawn; hourly strip mislabelled

- **Area:** services
- **Nodes:** svc-weather, [[Weather]]
- **Votes:** 1/1
- **Evidence:** weather_service.c:104-106 (request omits those fields); svc.c:1091 (set to -1); out_weather.c:648-664 (HUMID and WIND dashes; FEELS has data), :686-687 (five points at two-hour steps titled "NEXT 5 HOURS"), :691 (second row needs n>6, never renders); weather_parse.c:40-43. Phone rain, UV and wind_dir are never drawn.
- **Impact:** Dashes on HUMID and WIND and a mis-stated hourly span.
- **Fix:** Request the fields, draw them and fix the title.

### G71 — Weather not cached across reboot; phone data disappears after 3 h instead of going stale

- **Area:** services
- **Nodes:** svc-weather, [[Weather]], store-nvs
- **Votes:** 1/1
- **Evidence:** svc.c:70,414-424 (stale flag from 1 h; after 3 h falls back to wx_from_hal); weather_service.c:20,36,159-163 (only weather_loc is persisted, forecast is RAM); svc.c:1109. No store call for forecast data.
- **Impact:** No weather after boot until a fetch or phone push.
- **Fix:** Persist the last snapshot and keep it flagged stale.

### G73 — No unsynced-clock indicator: faces, alarms and apps run from epoch 0 (1970)

- **Area:** services
- **Nodes:** svc-time, [[Registry]], svc-alarm, [[World clock]], hw-rtc
- **Votes:** 1/1
- **Evidence:** board.c:877-882 (clock set to epoch 0 without RTC, retained or phone time); hal_esp32.c:140-153 (hal_get_datetime has no validity flag); svc.c:826-851 (alarm_tick has no epoch guard). hal_epoch_seconds() returns 0 when unset and some screens use it (set_modules.c:777, set_settings.c:1916, set_diag.c:214, fit_workout.c:830), but faces and alarms do not.
- **Impact:** A plausible wrong time with no warning, and alarms may fire on a bogus clock.
- **Fix:** Expose a clock-valid flag, show a not-synced glyph and gate alarms on valid time.

### G74 — Fixed-offset timezone, no DST; phone setTime overrides manual tz/NTP with no source arbitration

- **Area:** services
- **Nodes:** svc-time, [[Settings]], [[World clock]], hw-ble
- **Votes:** 1/1
- **Evidence:** svc.c:1000-1019 (GB setTime overwrites tz_min and saves); hal_esp32.c:822-834 (fixed "UTC+-h:mm"), :881-884 (BLE TIME tz bypasses prefs); nos_main.c:962-966 (calendar_preferences reverts it); prefs.c:154 (default 330); hal_esp32.c:1057-1068 (RESYNC does nothing for GB-only phones); time_worldclock.c:127 fixed offset. No DST handling found.
- **Impact:** Manual zone can silently revert and the clock is an hour off after DST until resync.
- **Fix:** Add auto/manual zone, DST rules, hysteresis and priority between sources.

### G75 — OTA/shutdown read-only scope freezes services, touch and UI during SD transfers

- **Area:** engine
- **Nodes:** eng-ota, store-sd, svc-alarm, svc-workout
- **Votes:** 1/1
- **Evidence:** sd_xfer.c:121-131,547 (sdput/sdput!/sdrm hold OTA owner 2 for the transfer); ota.c:75-79; nos_main.c:1984 (mutate=0), :2072-2075, :2094, :2098, :2110, :2155 (touch, button, prefs_tick, nos_bg_tick skipped; frames run read-only). Deliberate (ota.c:72-74) and lasts only for one transfer. Alarm and track tick timing not traced.
- **Impact:** Large BLE uploads make the watch unresponsive and can gap alarms or recording.
- **Fix:** Use a narrower SD lock and keep a minimal alarm/record tick under read-only.

### G77 — Notification kblur arena (896 KiB) allocated and baked though blur is off under SHARP

- **Area:** render
- **Nodes:** [[Notifications]], eng-blur, eng-render
- **Votes:** 1/1
- **Evidence:** CMakeLists.txt:73 (SHARP=1); ui_blur.c:322-323 (blur level 0), sys_notifs.c:560 (band_lv always 0), :574,617,625 (kblur_draw unreachable); sys_notifs_prewarm has no SHARP guard (sys_notifs.c:505 allocates the arena, :543-548 bakes), called from sys_home.c:645 and sys_notifs.c:568. A card re-bakes only when its signature changes; the arena is released under memory pressure (psram.c:140-155).
- **Impact:** Wasted PSRAM and episodic CPU while notifications exist.
- **Fix:** Skip prewarm when NOS_RENDER_SHARP.

### G78 — Dead blur code compiled in with ~26 KB of OTA slot headroom; cache_packed/native_stage inert

- **Area:** render
- **Nodes:** eng-blur, eng-ota, eng-render
- **Votes:** 1/1
- **Evidence:** firmware/main/CMakeLists.txt:15-16 (globs all nos/*.c), :71-73 (SHARP=1; comment says the normal renderer is kept for reference tests); firmware/build/watchfw.bin 7,837,568 B vs slot 0x780000 = 7,864,320 B (26,752 B headroom); nos.c:2301 forces blur off; watchfw.map lists blur objects; nos_main.c:1397-1404 and :1445-1453 (cache_packed and native_stage only set a flag and print). Real size saving unmeasured.
- **Impact:** Little flash headroom for new features; misleading A/B benchmarks.
- **Fix:** Compile blur out under SHARP, measure the size delta and mark inert commands.

### G79 — Idle UI work runs regardless of need: 60 Hz full pipeline, prewarm every frame, hidden widgets, SEGMENT warm-up from every face

- **Area:** render
- **Nodes:** eng-render, [[Home pager]], [[Segment]], [[Registry]]
- **Votes:** 1/1
- **Evidence:** render_config.h:13 (60 fps), nos_main.c:2155; sys_home.c:635 (all widgets tick every frame); faces.c:61 (every face calls face_segment_warm); face_segment.c:146-147,323-331 (0.43 MB cache kept). Overstated: warm-up is one-shot (:326), prewarm is gated (sys_home.c:640-648), pblur skips valid slots (pblur.c:168-171), idle frames skip paint and DMA, and screen-off runs at 1 Hz. Battery effect not measured.
- **Impact:** Some redundant per-frame CPU while the screen is on and a retained 0.43 MB cache.
- **Fix:** Gate on completion/visibility and warm only when SEGMENT is adjacent.

### G80 — Raw heap_caps allocations bypass the PSRAM budget

- **Area:** render
- **Nodes:** eng-render, hw-strap
- **Votes:** 1/1
- **Evidence:** Raw SPIRAM allocations: voice_chat.c:53, sports.c:303,305, workout.c:772, hal_esp32.c:1524 and :2262, native_menu_assets.c, main.c:1084, nos_main.c:1258; hal_big_alloc (hal_esp32.c:1594-1597) has no internal fallback; psram.c:49-54 checks the 1 MiB floor against free SPIRAM at each nos_mem_alloc call, so NOS callers are still protected. Whether callers degrade cleanly on NULL not verified.
- **Impact:** The 6656 KiB budget undercounts, and some UI caches may fail instead of degrading.
- **Fix:** Route allocations through nos_mem_alloc with fallbacks.

### G81 — Strap presence is latched at boot; no hot-plug, unplug detection or rescan

- **Area:** hardware
- **Nodes:** hw-strap, [[Barometer]], [[Compass]], [[Navigate]], [[Modules]], [[Diagnostics]]
- **Votes:** 1/1
- **Evidence:** firmware/main/ext/ext_sys.c:172-194 (hal_ext_init once, guarded by a static), :153-160 (loop calls services only); presence bits set only at init or probe (ext_env.c:120, ext_mag.c:101, ext_hr.c:324, ext_light.c:113, ext_haptic.c:109, ext_gps.c:251, ext_lte.c:359); the only clear is ext_audio.c:347. GPS re-probes once per enable (ext_gps.c:8).
- **Impact:** Reseating a strap needs a reboot; removed sensors show stale states.
- **Fix:** Add a rescan command or periodic re-probe and clear presence on error.

### G83 — Touch and RTC always reported online; Clock test reads the system clock, not the RTC chip

- **Area:** hardware
- **Nodes:** [[Modules]], [[Diagnostics]], hw-touch, hw-rtc
- **Votes:** 1/1
- **Evidence:** set_modules.c:167-170,274,279 (mod_present returns 1 for parts without strap bits); set_diag.c:200-215 (test_rtc uses hal_get_datetime, i.e. gettimeofday, hal_esp32.c:116-126); board.c:186 board_device_diagnostics is used by nos_main.c:613 and board.c:1089 but not wired into Settings pages. The Diagnostics touch test (set_diag.c:713-735) needs real taps, so touch is overstated.
- **Impact:** A dead RTC passes the clock test once the clock is set by phone or SNTP.
- **Fix:** Wire board diagnostics and read the PCF85063 in the test.

### G85 — Compass heading silently degraded: blank dial while speaker plays, flat-tilt assumption, no interference warning

- **Area:** hardware
- **Nodes:** [[Compass]], hw-mag, hw-speaker
- **Votes:** 1/1
- **Evidence:** ext_mag.c:204 (sampling skipped while haptic or amp is on), :218-226 (valid clears after 500 ms), :157-162 (flat gravity fallback when the IMU read fails); out_compass.c:339,669-672 ("Not connected" is the wrong message), :534 (field strength only in a pill); set_diag.c:379-380 (field range check only in diagnostics). The amp is on only with an external audio module. Not bench-run.
- **Impact:** Heading vanishes or errs with no explanation.
- **Fix:** Hold the last heading, flag stale tilt and warn on abnormal field.

### G86 — Diagnostics strap tests can false-FAIL after a manual screen-off (streams idle off after 8 s)

- **Area:** hardware
- **Nodes:** [[Diagnostics]], hw-gps, hw-ppg, hw-lte
- **Votes:** 1/1
- **Evidence:** nos_main.c:697 (PWR_OFF does not go home, unlike AOD/SENSOR at :705), :2091-2094 (loop only runs nos_bg_tick); the diag screen is not ticked, so last_get goes stale and ext_gps.c:269, ext_hr.c:238, ext_lte.c:302 stop the streams after 8 s (NOS_EXT_IDLE_MS); get functions do not re-enable; set_diag.c:302, 313-315, 395-398 produce the FAILs. Needs a manual screen-off during an unresolved run (set_diag.c:527 keeps the screen awake otherwise).
- **Impact:** A healthy GPS/HR/LTE part can show FAIL; the user can re-run.
- **Fix:** Keep streams alive during a test or re-enable on wake.

### G87 — Diagnostics results are not persisted, timestamped or exportable

- **Area:** hardware
- **Nodes:** [[Diagnostics]], [[Modules]], store-sports-journal
- **Votes:** 1/1
- **Evidence:** set_diag.c:88-92 (static R/T/G), :441-442 (run_start wipes), :127-155 (log_res to hal_log, untimestamped), hal_esp32.c:444-474 (32x80 RAM ring); only reader is legacy core/apps/app_sensors.c:291-302. diag_log itself is used elsewhere but not by set_diag.c. LTE and GPS are tested; no display, Wi-Fi, BLE, flash or PSRAM tests.
- **Impact:** After closing the screen nobody can see what failed or when.
- **Fix:** Post results to diag_log, show the last run and add the missing subsystem tests.

### G90 — Wi-Fi credentials and voice key stored in plaintext on SD and NVS

- **Area:** connectivity
- **Nodes:** store-sd, store-nvs, hw-wifi
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:1987,2004,236-246 (wifi_cred_t raw ssid/pass saved as a file under /sdcard/watch/wifi with NVS fallback); voice_chat.c:524-537 (API key in voice_chat); sdkconfig:2432 and :2744 (NVS and flash encryption off); nos_main.c:1790 (ERR unknown echoes the line, only for unrecognised commands). No normal path logs the password. Needs physical access.
- **Impact:** Card removal or flash dump exposes the network password.
- **Fix:** Encrypt NVS/credentials and echo only the verb token.

### G91 — Bluetooth screen shows no phone link state or enable control; Wi-Fi/BT tiles show intent not state

- **Area:** connectivity
- **Nodes:** [[Connectivity]], [[Control center]], hw-ble
- **Votes:** 1/1
- **Evidence:** set_connectivity.c:47,125 (shows accessory status from hal_ble_device_status), :84-98,143-286 (no BLE enable row); hal_esp32.c:669-680 ("No accessory connected" even when Gadgetbridge is linked); phone link is hal_phone_connected() (:1159-1162), not called here; set_settings.c:17-19 (documented as status-only); sys_control.c:173-174,228-229 (tiles show prefs). Settings root row does show the phone link (set_settings.c:417,586).
- **Impact:** Wrong or empty Bluetooth status and no way to check or reset the link.
- **Fix:** Show phone link state, add an enable row and warn when turning Bluetooth off.

### G92 — Wi-Fi setup UX gaps: one saved network, SSID not shown, no security info, refresh not real, setup leaves Wi-Fi on

- **Area:** connectivity
- **Nodes:** [[Connectivity]], hw-wifi, store-prefs
- **Votes:** 1/1
- **Evidence:** hal_esp32.c:1987,2004,2297 (single wifi_cred key, no forget); set_connectivity.c:56,62-63 (status and rows show no SSID or security); hal_esp32.c:1672-1673 (scan reuses a cache under 8 s old); set_connectivity.c:106-110 (Refresh silently enables Wi-Fi), :242-244 (hotspot start sets nos_prefs.wifi=1). The setup screen states the 10-minute close.
- **Impact:** Stale lists, unexplained radio power draw and no way to forget a network.
- **Fix:** Add known networks, SSID/security display, restore intent after setup.

### G96 — Chat spend limit resets with each conversation (C only)

- **Area:** connectivity
- **Nodes:** [[Chat]]
- **Votes:** 1/1
- **Evidence:** voice_chat.c:136-145 (clear zeroes cost_usd), :420-425 (allowance), :459,478,483 (messages say "start a new chat"); voice_policy.c:21-27 (allowance from that conversation's speech minutes); saved cost reloaded at voice_chat.c:184. No daily or device-wide counter. Deliberate per-conversation design; B has no audio.
- **Impact:** Repeated new chats can overspend the tier target on C.
- **Fix:** Add a daily device-wide cap.

### G97 — Faces lack complications, tap targets and options although steps, HR, weather, next alarm are computed each frame

- **Area:** shell
- **Nodes:** [[Registry]], [[Dot]], [[Glyph]], [[Inverted]], [[Life]], [[Matrix]], [[Segment]], [[Split]], [[Tape]]
- **Votes:** 1/1
- **Evidence:** sys_home.c:466-487 builds steps, bpm, weather, next_alarm_min; face.h:12-27. Only inverted, segment and split read steps; only split reads bpm and weather; no face reads next_alarm_min. Four faces have no tap handler (DOT, GLYPH, LIFE, TAPE). face_accent is inert (prefs.h:21). Only a face index is saved.
- **Impact:** Quick-glance data and shortcuts expected of a modern watch face are missing.
- **Fix:** Add complication slots and tap shortcuts, stop computing unused fields.

### G98 — ORRERY sky pack and status text are frozen at 2026-10-14 with no refresh path

- **Area:** shell
- **Nodes:** [[Orrery]]
- **Votes:** 1/1
- **Evidence:** orrery_data.c:5 (od_pack_jd 2461327.5), :4070 (Juno status text); orrery_events.c:63-64 (BepiColombo JD hard-coded), :65-76 (static flyby and comet tables); orrery_card.c:530,1698 (hard-coded countdown and Venus text); orrery_card_data.inc:1 ("Prototype profile samples and facts"). Planet and event geometry is computed live. Whether the UI labels prototype data not checked.
- **Impact:** Craft, flyby, comet and status entries age or are mistaken for sourced facts.
- **Fix:** Regenerate on release or load from SD and label prototype data.

### G99 — Many screens hold the display awake indefinitely (Compass, Navigate, Music, Stopwatch, Find phone, Chat, Heart, call ring)

- **Area:** shell
- **Nodes:** [[Compass]], [[Navigate]], [[Music]], [[Stopwatch]], [[Find phone]], eng-power
- **Votes:** 1/1
- **Evidence:** nos_main.c:2078-2085 and power_policy.h:18-19 (keep flag disables the timeout); nos.c:585. Unconditional holds: out_compass.c:734, out_nav.c:678,1112, con_chat.c:314, con_find.c:493, con_call.c:500. Conditional: Music only while playing (con_music.c:525), Stopwatch only while running (time_stopwatch.c:343), Heart only while measuring (fit_heart.c:671), Find phone only while active. Documented intent (nos.h:64). Sensors-running claim unverified.
- **Impact:** A forgotten open Compass or Navigate screen drains the battery.
- **Fix:** Add per-screen inactivity caps and wrist-down pause.

## Rejected on verification

These were reported but did not hold up, so they are not in the list above.

- **G45** — Restored running workout keeps counting active time after a reboot. Why: Not supported by the code: segment and clock anchors reset to boot time on recovery. Only a missing RECOVERED banner for a running session remains, which is minor UX.
- **G61** — Alarm rings 60 s then stops silently with no snooze, re-ring or missed-alarm record. Why: Snooze exists in svc.c and the alarm UI, and the timeout is traced as a diagnostic event. Only the lack of an auto re-ring or visible missed-alarm notice holds, at low severity.
- **G76** — ext tasks run at priority 0 and share I2C with IMU/touch without a cross-driver lock. Why: The IDF bus lock covers single transactions, so the missing cross-driver lock is not a defect. Priority 0 is true for ext and extf only, with no harm shown.
- **G121** — Sports UI polish: UTC times, misleading toasts, dead setup rows, transient SAVE FAILED, unreachable HR_STALE, frozen details. Why: UTC times are labelled, SAVE FAILED says RETRY, and HR_STALE is reachable (workout.c:322). MKDIR, wrist-setup and GPS items not verified.
- **G124** — Storage hygiene: orphaned legacy NVS keys, RTC breadcrumbs lost on power loss, SD-OTA unwired, pre-sync records keyed day 0. Why: Only the RTC-breadcrumb and day-0 mechanics are present, and both are intended or narrow. No support for the legacy-key claim, and SD OTA is wired through the "ota sd" console command.
- **G148** — Inert controls: TE sync row N/A on B, GPS auto row always refuses, Sports setup rows do nothing, modules count never 16. Why: GPS shows an N/A label plus a toast, so taps are not silent. TE row shows N/A only. Sports setup rows have no support in the cited code and the modules count is not a control.
