---
title: "Modules"
id: sys-modules
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Modules

## Summary

Modules is the NOS hardware status board and bring-up tool for the 1.43-B watch. One list shows all 16 parts (7 on the board, 9 on the strap), each with a live value refreshed at 1 Hz. Each part has a detail page with a readout and a test action. The screen code is `nos/apps/set_modules.c` (1468 lines). It keeps no state across a reboot, writes nothing to `nos_prefs`, and touches no `svc_*` mutator. Its lasting side effects are the diagnostics trace on SD and the magnetometer calibration in NVS.

- **Purpose:** show which parts answered, show their live values, and let a person exercise each part (touch rings, IMU bubble, RTC resync, haptic and speaker sounds, mic level, GPS satellites, LTE power, environment, light, compass calibration, heart-rate trace).
- **Entry:** not in the app launcher (no entry in `nos/apps/registry.c`). It is reached from Settings > Modules (`nos/apps/set_settings.c:482`, `:1855`) and from the host screen name "modules" (`firmware/main/nos_main.c:930`, `sim/nos_sim.c:82`). Settings > Storage opens the SD detail as a page titled STORAGE (`set_settings.c:1857`, `set_modules.c:631`, `:726`).
- **Screens:** `scr_modules` "MODULES" (`set_modules.c:665`), `scr_mod_detail` "MOD_DETAIL" (`:1402`), `scr_mod_touch` "MOD_TOUCH" (`:1467`).
- **Board parts (7):** Touch FT3168 at I2C 0x38, Motion QMI8658 at 0x6B, Clock PCF85063 at 0x51, SD card one-bit SDMMC, Battery via ADC GPIO4 with a 3:1 divider, Wi-Fi, Bluetooth (`set_modules.c:60-66`). The pins match `firmware/main/pins.h` for B (TOUCH_ADDR 0x38, IMU_ADDR 0x6b, RTC_ADDR 0x51, BAT_DIVIDER 3).
- **Strap parts (9):** Haptics (GPIO16 PWM), Microphone (INMP441 on I2S), Speaker (MAX98357A on I2S), GPS (u-blox M10 on UART1), Cellular (Quectel EG800AK on UART0 with PWRKEY GPIO18), Climate (BME280), Light (APDS-9960 at 0x39), Compass (QMC5883L at 0x0D), Heart rate (MAX30102 at 0x57) (`set_modules.c:67-84`). The strap drivers compile in on the 1.43-B (`firmware/main/ext/ext_priv.h:27-29`) and compile to absent on the 1.43C.
- **Hardware it needs:** the shared I2C bus SDA47/SCL48 for the strap sensors (`firmware/main/board.h`, "Shared I2C master bus"), the back headers J1/J3 (`nos/hal_ext.h:5-18`), the SD slot (CLK41, CMD39, D0=40), IMU INT8, RTC INT15 (`firmware/main/pins.h`).
- **Status: partial.** The UI is complete and every row and page is wired to a HAL getter or command. Several rows are hard-wired "online", two tests cannot prove the part works, and the saved volume and haptic strength never reach the drivers at boot. See Gaps.

Related: [[Services hub]], [[Hardware and drivers]], [[Storage and persistence]], [[Engine and scheduling]], [[Connectivity and voice]], [[Power and battery]], [[Health and activity]], [[Sports and workouts]], [[Shell and faces]], [[Diagnostics]], [[Settings]], [[Cellular]], [[Compass]], [[Heart rate]], [[IMU]], [[Barometer]], [[Recorder]].

## Features

- **MODULES list** (`modules_enter` `set_modules.c:372-402`, `list_values` `:269-329`, `draw_lrow` `:507-558`).
  - Summary: "n/16 ONLINE" with 16 rounded squares in fours. The squares sweep on entry over 18 frames (`SUM_SWEEP_F`, `:476-483`). The count comes from `presence()` (`:227-235`).
  - Two sections: BOARD (Touch, Motion, Clock, SD card, Battery, Wi-Fi, Bluetooth) and STRAP MODULES (Haptics, Microphone, Speaker, GPS, Cellular, Climate, Light, Compass, Heart rate) (`LE[]`, `:334-344`).
  - Each row: a dot (white when present), the name, a bus line, a live value, and a chevron. An absent strap row reads "No module" (`:328`).
  - Values: Touch "x, y" after a touch in this list, else "Ready" (`:273-274`). Motion shows the acceleration magnitude in g, or "No reply". Clock shows HH:MM. SD shows free GB, or "No card". Battery shows volts on the rail. Wi-Fi shows Off, Connected, Joining or Not connected. Bluetooth shows Connected or Not connected. Haptics shows the saved strength %, or "Muted" (`:291-294`). Microphone shows dB or "Ready". Speaker shows the saved volume %. GPS shows "N sats", "N in view" or "Searching". Cellular shows dBm, Off, No SIM or Searching. Climate shows a temperature in °C or °F, or "Warming up". Light shows lux or "Ready". Compass shows "HDG nnn" or "Ready". Heart rate is always "Ready" (`:327`).
  - RUN DIAGNOSTICS pill at the end of the list (`:343`, tap handling `:647-653`) pushes `scr_diag`.
- **Detail pages** (`open_detail` `:716-725`, `detail_values` `:752-879`, `detail_draw` `:1124-1281`, `detail_input` `:1329-1401`).
  - Motion: bubble level driven by the accelerometer, tilt in degrees, X/Y/Z and gyro readings (`:759-773`, `draw_bubble` `:1115-1122`).
  - Clock: HH:MM:SS, the date, and "UTC SYNCED" or "UTC NOT SYNCED" (`:774-778`). RESYNC button (`:1372-1375`).
  - SD card: free GB of total, percent used, and a capacity bar (`:779-790`, `:1175-1182`). From Settings this is the STORAGE page (`storage_page` `:1301-1327`): system image bar, settings-store bar, flash size, card state, and "TAP TO TRY AGAIN" (`:1355-1363`).
  - Battery: volts on the rail, a percent ESTIMATE or "USB POWER", and a bar (`:791-800`, `:1183-1190`).
  - Haptics: seven patterns (Tick, Click, Double, Heavy, Buzz, Alarm, Heartbeat). Tap plays one. Alarm loops until the next tap (`play_tap` `:1288-1296`, stop in `detail_input` `:1341-1347`).
  - Speaker: 14 sound effects (`k_sfx_names`, `:89-91`), tap plays one.
  - Microphone: live dB, peak since entry, and a 32-sample waveform (`:801-805`, `:1191-1196`).
  - GPS: satellites in use, latitude and longitude to 4 decimals, 2D or 3D fix, HDOP, and per-satellite SNR columns. Shows "SEARCHING" with the count in view when there is no fix (`:806-818`, `:1197-1206`).
  - Cellular: RSSI in dBm, signal bars, SIM state, registration (home, roaming, searching), operator. POWER ON or POWER OFF pill (`:819-843`, `:1207-1214`, pill `:1376-1383`).
  - Climate: temperature, humidity, pressure, altitude (`:844-853`).
  - Light: lux, proximity, and the last air gesture name for 2 s (`:854-862`).
  - Compass: 60-dot dial with a north dot and a short trail, heading, field in uT, calibration state. CALIBRATE or DONE pill. Auto-finishes at progress 1.0 with a "CALIBRATED" toast (`:863-871`, `:980-988`, `:1228-1240`).
  - Heart rate: live PPG trace of 48 samples, BPM, SpO2, a signal bar, and MEASURING, NO CONTACT or "SENSOR ON SKIN" states (`:872-875`, `:989-993`, `:1241-1272`).
- **Absent page** (`absent_page` `:1036-1044`): the module's own "Not connected" copy and footer from `MI[]` (`ui_absent`). It adds the bring-up wiring line (for example "I2C 0x39 SDA 47 SCL 48") unless opened from Settings (`:1043`).
- **Touch test** (`touch_draw` `:1421-1441`): five rings at centre and four edges (`k_tgt`, `:1408`). Each turns white when touched. PASS when all five are hit. The finger coordinates are shown while touching.
- **Radio rows:** Wi-Fi and Bluetooth do not open Modules pages. They call `set_settings_open_radio` (`:719`), which opens the Settings radio pages (`set_settings.c:2266-2269`).

## Reads

Every service, prefs and HAL read in `set_modules.c`, with the call sites. A full sweep of `svc_*`, `prefs_*`, `hal_*`, `diag_*` and `nos_*` calls in the file matches this list.

**Services (svc):**
- `svc_sys()->phone_connected` (`:289`). Set by `phone_tick` (`nos/svc.c:1217-1220`).
- `svc_track_active()` (`:406`). Returns `T.active` (`nos/svc_track.c:312`). Only used to decide whether leaving the list may turn the GPS off.
- No other `svc_*` call and no `prefs_*` call.

**Prefs (read only, `nos_prefs` fields from `nos/prefs.h`):**
- `wifi` (`:287`), `haptics` (`:292-293`), `haptic_strength` (`:292`), `volume` (`:299`), `units_metric` (`:314`, `:846`, `:848`).

**Board HAL (`core/hal.h`, implemented in `firmware/main/hal_esp32.c` and `firmware/main/board.c`):**
- `hal_imu_read` (`:213`, `:253`). Valid only if the cached sample is under 250 ms old (`firmware/main/board.c:388-395`).
- `hal_storage_get` (`:215`, `:254`), cached SD state (`board.c:534`).
- `hal_flash_info_get` (`:215`), `hal_power_get` (`:216`, `:255`). A power sample older than 3 s is marked invalid (`hal_esp32.c:378-388`).
- `hal_battery_mv` (`:181`), `hal_battery_percent` (`:795`, `:1186`).
- `hal_get_datetime` (`:214`, `:279`), `hal_epoch_seconds` (`:777`). These read the ESP system clock (`hal_esp32.c:116-126`, `:148-152`), not the PCF85063.
- `hal_status(HAL_FEAT_WIFI)` and `hal_status(HAL_FEAT_BLE)` (`:233-234`, `:286`). Read from `hal_esp32.c:897-907`.
- `hal_phone_connected()` (`:289`, `hal_esp32.c:1159`).

**Strap HAL (`nos/hal_ext.h`, implemented in `firmware/main/ext/`):**
- `hal_ext_present()` (`:169`, `:212`). Returns sticky probe bits (`firmware/main/ext/ext_sys.c:197-199`).
- Getters, called at 1 Hz from the list and every frame on the detail page: `hal_gps_get` (`:217`), `hal_lte_get` (`:218`), `hal_env_get` (`:219`), `hal_light_get` (`:220`), `hal_mag_get` (`:221`), `hal_hr_get` (`:222`, detail only), `hal_mic_db` (`:223`), `hal_mic_wave` (`:974`), `hal_light_gesture` (`:977`).
- Freshness rules applied in the drivers: environment valid for 6 s (`hal_ext.h:172`, `ext_env.c:178-184`), light 2 s (`hal_ext.h:184`), GPS fix stale after 3 s (`ext_gps.c:29`, `:317-330`), heart rate blanked after 2 s (`ext_hr.c:343-355`).

**Other reads:**
- `diag_log_name_id("MODULES")` (`:713`), used only to tag the trace.
- Exports read by other screens: `set_modules_online()` (read by `set_settings.c:600`, `:654`, `:679`) and `set_modules_absent_copy()` (read by `nos/apps/set_diag.c:1396`).

## Writes

**Screen-local RAM only:** the sample cache `V` (`:184-202`), the list state `ML` (`:351-363`), the detail state `D` (`:669-707`), and the touch test. None of this is saved.

**Hardware commands:**
- GPS: `hal_gps_enable(1)` on list enter (`:393`) and on the GPS detail enter (`:921`). `hal_gps_enable(0)` on list leave, unless the list is covered or a track is active (`:404-407`). The driver also resets its idle timer on every getter call (`ext_gps.c:322`).
- Heart rate: `hal_hr_enable(1)` and `hal_hr_enable(0)` on the HR detail enter and leave (`:920`, via `detail_sensors`). These use the legacy owner only (`firmware/main/ext/ext_hr.c:327-339`), so the workout and heart owners are not disturbed.
- Light gestures: `hal_light_gesture_enable(1)` and `(0)` (`:922`).
- Cellular: `hal_lte_enable(on)` from the POWER pill (`:1380`). This starts a PWRKEY sequence and returns at once (`hal_ext.h:154`).
- Compass: `hal_mag_calibrate(1)` on CALIBRATE (`:1388`). `hal_mag_calibrate(0)` on DONE (`:1391`), on detail leave when not covered (`:958-959`), and on auto-finish (`:982`). A finished fit is saved to NVS key `ext_mag` when it passes the checks (`firmware/main/ext/ext_mag.c:252`, `:260`).
- Haptics: `hal_haptic(i)` on a pattern tap (`:1292`). `hal_haptic_stop()` on detail leave (`:963`) and on a tap that stops the alarm (`:1344`).
- Speaker: `hal_sfx(i)` on a sound tap (`:1293`).
- Clock: `hal_time_resync()` on RESYNC (`:1374`). It sends a BLE notify to the companion when one is connected, refreshes weather, and restarts SNTP when Wi-Fi is up (`firmware/main/hal_esp32.c:1057-1068`).
- SD: `hal_storage_retry()` on TAP TO TRY AGAIN, Settings path only (`:1357`). This sets a flag for the board task (`firmware/main/board.c:508`, consumed at `:952`).
- Driver keep-alive side effects: every getter call renews the driver's idle timer. `hal_mic_db` and `hal_mic_wave` set `mic_last_ms` and wake the audio task (`firmware/main/ext/ext_audio.c:406-414`).

**Diagnostics trace (every open, tap, test and result):** `diag_action_request`, `diag_action_context` and `diag_action_result` (`:710-715`, `:721-722`, `:997-998`, `:1290`, `:1294`, `:1343-1345`, `:1357-1358`, `:1373-1381`, `:1386-1395`). Each call writes an integer-only record into the diag ring (`nos/diag_log.c:229-241`). Records reach SD through the flush described under Persistence.

**Not written:** `nos_prefs` (no assignment in the file), any `svc_*` state, any SD file opened directly by this screen.

## Background behaviour

**The screen itself.**
- Its tick runs only while it is the top screen (`nos/nos.c:2101-2110`). A covered page is not ticked, so it stops polling.
- Its own timers are UI timers only: live refresh every 1.0 s (`set_modules.c:47`, `:432-439`, `:1009`), press flash 0.10 s (`:43`), and the LTE request timeout of 20 s (`:996`). The UI frame rate is NOS_FPS 40 (`nos/render_config.h:8`).
- Leaving the list with a pop turns the GPS off (`:404-407`). A covered list leaves it on (the comment at `:403`), but nothing polls the GPS while it is covered.

**Strap drivers** (`firmware/main/ext/`). The "ext" service task (priority 0, 4 KB stack in PSRAM, `ext_sys.c:186`) runs the gps, lte, env, light, mag and hr services in turn. The audio task "exta" runs at priority 3 and "extf" at priority 0 (`ext_audio.c` header).
- Auto-stop when no getter call arrives for `NOS_EXT_IDLE_MS` = 8000 ms (`nos/hal_ext.h:35`):
  - GPS (`firmware/main/ext/ext_gps.c:269-270`)
  - LTE (`ext_lte.c:302`). The modem is switched off after 8 s without a getter.
  - Heart-rate stream (`ext_hr.c:238`)
  - Light gestures (`ext_light.c:180`)
  - Compass, unless it is calibrating (`ext_mag.c:195`)
- Mic I2S channels are deleted 1.5 s after the last need. A mic getter counts as a need for 8 s (`ext_audio.c` header lines 11-13, `need()` `:234-240`).
- Alarm haptic pattern loops until stopped (`ext_haptic.c:32`, `:36`, `:73`, `nos/hal_ext.h:77`).

**Always on, independent of Modules:**
- BME280 forced measurement every 2 s (`firmware/main/ext/ext_env.c:22`, `:123-135`).
- APDS-9960 ALS and proximity read every 400 ms. The proximity engine is enabled at init (`ext_light.c:88`, `:94-108`, `:192`). The LED drive is 100 mA (`:92`).
- Heart-rate background sample every 10 min (`ext_hr.c:34`, `:238-251`). The first one comes 2 min after boot (`:322`).
- LTE boot probe powers the modem once and then leaves it off unless enabled (`ext_lte.c:450`).
- Board sensing task, a 20 ms cycle that reads the IMU and the pedometer, and samples power once per second (`firmware/main/board.c:967-1025`).
- Diagnostics SD flush from the `sports_sd` writer task, priority 1. It runs about once a second when records are queued (`nos/sports.c:282-289`, `:323`, `nos/diag_log.c:463`). Device only.

**While the list is visible**, the 1 Hz sample keeps the GPS enabled (`:393`) and renews the idle timers of the GPS, LTE, environment, light, compass and mic (`:223`, `:433`). It does not poll the heart rate (`poll_hr` is 0 at `:395` and `:434`).

## Persistence

- **Screen state** (`V`, `ML`, `D`, touch test) is RAM only. It is rebuilt on each entry (`modules_enter` `:372-402`, `detail_enter` `:924-953`) and lost at reboot.
- **NVS:** the magnetometer calibration under key `ext_mag`. It is written only when the fit passes its checks (`firmware/main/ext/ext_mag.c:252`, `:260`) and loaded at boot (`ext_mag.c:97`).
- **SD:** diagnostics records in `/sdcard/watch/diagnostics/diag-NN.bin` (device; `nos/diag_log.c:24`, flush `:463`). Fixed-size DLG1 records, integers only, no credentials (`nos/diag_log.h:28-30`). Each open, tap, test and result adds records. The host build writes to `build/nos_diagnostics` (`nos/diag_log.c:36`).
- **Not persisted:** GPS, LTE, heart-rate, light-gesture and compass run state. The LTE POWER ON request, haptic and speaker test results, the touch test, and the strap presence bits (set again at every boot).
- **Read here but stored elsewhere:** the saved volume and haptic strength live in `nos_prefs` (defaults in `nos/prefs.c:138-141`). See Gaps for what the drivers actually use.

## Navigation

**Opens from Modules:**
- Each board or strap row opens its detail page through `open_detail` (`:716-725`). Touch opens `MOD_TOUCH`.
- Wi-Fi and Bluetooth rows call `set_settings_open_radio` (`:719`), which opens the Settings radio pages (`nos/apps/set_connectivity.c:144`).
- RUN DIAGNOSTICS pushes `scr_diag` (`:652`). The Diagnostics app is covered in [[Diagnostics]].

**Opened from:**
- Settings > Modules (`nos/apps/set_settings.c:482`, `:1855`), with the count "n/16" read from `set_modules_online` (`:600`).
- Settings > Storage calls `set_modules_open_sd()` (`set_settings.c:1857`, `set_modules.c:631`), which pushes the SD detail titled STORAGE (`:726`).
- Host screen name "modules" (`firmware/main/nos_main.c:930`, `sim/nos_sim.c:82`).
- Settings > Cellular does not open this screen. It opens the Cellular app `scr_cell` (`set_settings.c:1853`). See [[Cellular]].
- Diagnostics reuses the absent copy from this file (`nos/apps/set_diag.c:1396`).

## Gaps

Ranked by severity. Each item cites the code that shows the gap.

1. **[high] Touch and RTC are always "online", and Clock reads the system clock.** `set_modules.c:169-171` returns 1 for any part with no strap bit, and `presence()` (`:227-235`) never sets `ok[M_TOUCH]` or `ok[M_RTC]`. The Touch value is a fixed "Ready" (`:274`). Clock reads `hal_get_datetime` (`:214`, `:279`), which is `gettimeofday` plus `localtime` (`firmware/main/hal_esp32.c:116-126`). No PCF85063 read happens on that path, and `board.c:755-795` writes the RTC but nothing reports its health here. `board_device_diagnostics` and `board_touch_diagnostics` exist (`firmware/main/board.h:21-33`) but no screen in `nos/apps` calls them. The bring-up page cannot show a dead RTC or touch controller. RESYNC still shows "RESYNCING" with a dead chip. The Diagnostics note makes the same point about its Clock card.
2. **[high] Speaker and haptic presence is assumed, and their tests can never pass.** The speaker bit is set with the comment "cannot be probed; default on" (`firmware/main/ext/ext_audio.c:343`). The haptic bit is set once the LEDC driver starts (`ext_haptic.c:109`). `hal_haptic` and `hal_sfx` return void, and the trace records PENDING, "void HAL command, no physical ACK" (`set_modules.c:1292-1294`). The list shows an online speaker with a volume percent even when the amplifier is not wired. No on-screen test can confirm sound or vibration. Diagnostics has a guided "Felt it?" card for haptics. Modules has none.
3. **[medium] Saved volume and haptic strength are not applied at boot.** The list shows the prefs values (`set_modules.c:292`, `:299`). The drivers start at 70 (`ext_audio.c:337`) and 80 (`ext_haptic.c:43`), while prefs default to 70 haptic and 60 volume (`nos/prefs.c:139-141`). The only callers that push the values are the Settings reset and slider, and the Diagnostics run (`nos/apps/set_settings.c:1768-1769`, `:2093-2096`; `set_diag.c:634-643`). No boot path was found. After a reboot the real output can differ from the displayed percent until the slider is touched. Alert buzz strength can be 80 when 70 is shown.
4. **[medium] Presence is sticky, so a strap part removed after boot is never seen.** Each driver sets its presence bit once: GPS (`ext_gps.c:251`), LTE (`ext_lte.c:359`), HR (`ext_hr.c:324`), light (`ext_light.c:113`), compass (`ext_mag.c:101`), env (`ext_env.c:120`). Only the audio driver clears bits, and only when its task fails to start (`ext_audio.c:347`). The microphone is probed once, when the audio task starts (`ext_audio.c:296`). A removed GPS reads "Searching" forever (`set_modules.c:303`). A removed sensor keeps its online dot.
5. **[medium] No device-side retry for absent strap parts.** GPS and LTE re-probe when something enables them (`ext_gps.c:294-295`, `ext_lte.c:333-336`). Every other strap driver probes only at boot (`ext_sys.c:169-195`). The SD card has a retry only on the Settings path (`set_modules.c:1355`). The board-level retry is serial-only (`firmware/main/nos_main.c:1205`). A strap module seated loosely at boot shows "No module" until reboot. The SD bring-up page has no retry at all (`:1355` requires `from_set`).
6. **[medium] Always-on sensors cost power that this screen neither shows nor controls.** BME280 is triggered every 2 s (`ext_env.c:22`, `:123-135`). APDS-9960 is read every 400 ms with proximity enabled since init (`ext_light.c:88`, `:94-108`, `:192`). Neither depends on which app is open. See [[Power and battery]].
7. **[medium] Wiring text is hard-coded, and the B strap map is unverified.** The wiring strings in `MI[]` (`set_modules.c:60-84`) repeat the pin map in `firmware/main/ext/ext_priv.h:33-44` by hand. The literal "SDMMC 1-BIT, 20 MHZ" (`:788`) is not read from the driver, which uses `SDMMC_FREQ_DEFAULT` (`firmware/main/board.c:574`). "GPIO4 ADC1 CH3 3:1" (`:798`) and "PWRKEY GPIO 18" (`:841`) are literals too. The repo's `CLAUDE.md` says the 1.43-B external strap "needs its own verified mapping" and that built-in audio is absent. The page can state a wiring that has not been checked on a B watch.
8. **[low-medium] Test actions ignore the mute preferences.** `play_tap` calls `hal_haptic(i)` and `hal_sfx(i)` with no check of `nos_prefs.haptics` or `nos_prefs.sound` (`set_modules.c:1288-1296`). The Haptics row shows "Muted" when haptics are off (`:292-293`). Other callers gate on the same pref (`nos/apps/fit_breathe.c:105`, `nos/nos.c:570`). A tap buzzes while the row says Muted. The speaker still stops at volume 0 (`ext_audio.c:360`).
9. **[low-medium] The alarm test can keep vibrating under a covering screen.** `detail_leave` returns early when covered (`set_modules.c:956`), which skips the stop at `:963`. The Alarm pattern loops with no timeout (`ext_haptic.c:32`, `:36`, `:73`). Nothing else stops it on screen change. Compare the 60 s cap on the timer's alarm haptic (`nos/apps/time_timer.c:914`). It stops only on the next haptic call or tap.
10. **[low-medium] RESYNC reports nothing about its outcome.** The toast "RESYNCING" and the PENDING trace are shown regardless (`set_modules.c:1373-1374`). `hal_time_resync` returns void (`core/hal.h:279`), and it notifies the companion only when a BLE client is connected (`firmware/main/hal_esp32.c:1057-1068`). With no phone and no Wi-Fi the user sees RESYNCING and nothing happens.
11. **[low-medium] Compass success and cancel do not match what the driver did.** Auto-finish toasts CALIBRATED and records SUCCESS at progress 1.0 (`set_modules.c:981-987`). The driver keeps the previous calibration when the new fit fails its range check (`ext_mag.c:252`). Leaving the page or pressing DONE calls `hal_mag_calibrate(0)`, which saves any fit that passes (`ext_mag.c:260`). The trace still records CANCELLED (`set_modules.c:960`, `:1392`).
12. **[low] LTE POWER ON does not stay on.** The driver switches the modem off 8 s after the last getter call (`ext_lte.c:302`). This screen calls `hal_lte_get` only while it is visible (`set_modules.c:395`, `:434`, `:971`). `detail_leave` never disables LTE (`:954-964`). Nothing persists the request, and nothing tells the user it stopped.
13. **[low] "n/16 ONLINE" mixes radio state and hardware.** Wi-Fi counts as online only when connected (`set_modules.c:233`), but Bluetooth counts whenever the radio is not off (`:234`). An idle Bluetooth radio counts as online while its row says "Not connected". Touch and RTC always count (gap 1).
14. **[low] Stale header claims.** The header says Settings > Cellular opens a Cellular page here (`set_modules.c:20-21`). Settings opens the Cellular app instead (`set_settings.c:1853`). The header says the radio pages get a bus line (`:21-22`). `set_settings_open_radio` ignores `from_modules` (`set_settings.c:2266-2269`), and `set_connectivity_open` takes only `bt` (`nos/apps/set_connectivity.c:144`). No bus line appears on those pages.
15. **[low] Battery percent is shown without checking validity.** `set_modules.c:795-797` and `:1186` use `battery_percent` (or the fallback) without `battery_valid`. `core/hal.h:34-36` calls the value "evidence, not a fuel gauge". The charge state is not shown at all.
16. **[low] Some rows never update.** The heart-rate row is always "Ready" (`set_modules.c:327`). The Touch row changes only on a touch inside this list (`:637-638`). The Haptics and Speaker rows show prefs values, not driver state, because no getter reports the driver's level (`nos/hal_ext.h:82-95`). This compounds gap 3.
17. **[low] Expected features missing.** There is no re-probe or rescan (gap 5), no hot-unplug detection (gap 4), and no pass or fail history. Diagnostics results are RAM only, and the only SD record is the developer trace. There is no per-module power switch for the always-on sensors (gap 6). There is no chip identity or firmware version, no calibration reset (only recalibrate), no charge state, and no heart-rate history. The touch test has no fail state or timeout (`set_modules.c:1421-1441`).
