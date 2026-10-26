---
title: "Matrix face"
id: face-matrix
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Matrix face

Source: `nos/faces/face_matrix.c` (387 lines), exported as `face_matrix` with name "MATRIX" (`nos/faces/face_matrix.c:387`) and listed second in the registry (`nos/faces/faces.c:15`). Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from source reading only, not from the device.

## Summary

MATRIX is a full-bleed face (`nos/faces/faces.c:817`) that draws everything into one circular 25 x 25 grid of 14 px dots at 17.6 px pitch (`nos/faces/face_matrix.c:27-32`). It shows the time as two rows of 5 x 7 dot digits, a 60-step seconds ring, a faint ghost grid, a slow background sparkle, a 5-cell battery bar that appears at 20 % or below or while charging, and a red dot for unread notifications. Its only inputs are the clock, the battery and charge flags, the unread flag and the 24 h preference. It writes nothing persistent, opens no screen and has no tap action (`nos/faces/face_matrix.c:385`). It owns no task or timer: home (or the face picker) ticks it from the UI loop, at the frame rate while awake and at 1 Hz in the AOD look.

## Features

- **Time as dot digits.** HH sits on rows 5-11 and MM on rows 13-19, in digit columns 7-11 and 13-17 (`nos/faces/face_matrix.c:143-161`). Digits are 5 x 7 bitmaps (`:44-55`). The default is 24 h (`nos/prefs.c:147`). The global "24-hour clock" toggle switches to 12 h (`nos/apps/set_settings.c:474`, `:534`). In 12 h the leading zero is dropped and a lone hour digit is centred (`nos/faces/face_matrix.c:157`, `nos/faces/faces.c:85-90`).
- **No AM/PM marker in 12 h.** Nothing in the face draws AM or PM (`nos/faces/face_matrix.c:144-161`).
- **Seconds ring.** Ring cells are the inside cells that touch the outside (`nos/faces/face_matrix.c:115-124`), each with a clock angle in 0.1 degree units. At build time (once per boot, `:97-141`) one head cell is assigned per second to the free ring cell nearest i x 6 degrees (`:125-139`). Ring cells the sweep has passed show dim, and the head shows white (`:243-246`). The ring moves only when the second changes (`:207-209`). Spare ring cells light by clock angle, not by adjacency, which is a small difference from the header comment (`:5-6`).
- **Minute change.** At second 0 the whole ring drains backwards over `DRAIN_N` = 6 frames (`:34`, `:210-214`). Changed digit cells step through the levels one row per frame, each line from its own top row (`:233-242`). A change is detected when `hour*60+minute` moves (`:187-197`).
- **Build-in ripple.** A ripple spreads from the centre at `WAVE_PER_F` = 2 cells a frame (`:35`, `:216`). It runs over `FK_BUILD_N` = 12 frames, which is `ENTER_S` = 0.40 s (`nos/faces/face_kit.h:30-32`). Cells ahead of the front are off, and the front smears into a dim edge when `ui_ripple_blur` is set (`:247-260`).
- **Ghost grid.** Every in-circle cell that is not lit draws as a `#181818` dot (`:38-40`, `:299`). The grid is always visible on the glass.
- **Sparkle.** Three slots each light one background cell to dim for the first 1.2 s of every 2.4 s cycle (`:263-275`). Cells are skipped inside the digit zone, the battery row and the ring. It runs only when the build-in is done, the wake is complete and the face is not in AOD (`:263`, `:265`).
- **Battery bar.** Five cells on row 22, columns 10-14 (`:36`, `:173`, `:276-284`). Lit cells are `ceil(percent / 20)`, clamped to 1-5 (`:174`). The top lit cell blinks while charging below 100 % (`:281`). The bar is shown when `fk_status_shown()` is true, which means charging, or 1-20 % (`nos/faces/faces.c:582-586`). There is no percentage readout.
- **Unread dot.** A red 8 px dot sits at top centre, replacing that cell (`:382`, `nos/faces/face_kit.h:122-128`). It fades in and out in four ink steps (`nos/faces/faces.c:552-575`). It is binary: any unread count shows the same dot.
- **AOD variant.** Only the digits draw, as grey 6.5 px dots in rows 5-19 (`:337-356`). The bar appears only below 10 % and not charging (`:353`, `nos/faces/faces.c:584`). The unread dot is a 6 px grey dot (`nos/faces/faces.c:567-569`). The whole face drifts up to +-4 px a minute (`nos/faces/faces.c:20-26`, `:57-64`).
- **Wake from AOD.** The grid fades up from black over six frames (`nos/faces/faces.c:455`, `:482-489`). Digit dots grow from AOD size to full size (`:290-314`). Ring, head and battery cells rise over the last frames (`:308-312`).
- **Picker dim.** When the picker dims the face, the ghost grid fades out in steps (`:295-298`, `nos/faces/face_kit.h:138-146`).
- **REC chip.** While recording, home draws the chip over the face (`nos/apps/sys_home.c:827`). MATRIX's chip line is at y 388 (`nos/apps/sys_home.c:190`). The unread dot's red fades to white while recording (`nos/faces/face_kit.h:135-137`, `:151-155`).

## Reads

Face context, built once per home frame by `build_ctx()` (`nos/apps/sys_home.c:465-490`). The face picker uses the same builder through `sys_home_ctx()` (`nos/apps/sys_faces.c:136-137`, `nos/apps/sys_home.c:493-497`).

- `c->dt` (hour, minute, second) from `hal_get_datetime()` (`nos/apps/sys_home.c:468`, `firmware/main/hal_esp32.c:142-146`). Read at `nos/faces/face_matrix.c:187`, `:207`.
- `c->t`, the monotonic animation clock `nos_t` (`nos/apps/sys_home.c:483`). Read at `face_matrix.c:196`, `:210`, `:266-268`.
- `c->battery` from `svc_sys()->battery_pct`, falling back to `hal_battery_percent()` (`nos/apps/sys_home.c:474`). Read at `face_matrix.c:278`, `:319`.
- `c->charging` from `svc_sys()->charging` (`nos/apps/sys_home.c:475`). Read at `face_matrix.c:281`.
- `c->unread` from `svc_notif_unread()` (`nos/apps/sys_home.c:476`, `nos/svc.c:238-243`). Only the "greater than zero" test is used (`nos/faces/faces.c:552-554`).
- `c->aod` (`nos/apps/sys_home.c:484`), `c->enter` (`:485`), `c->lite` (`nos/apps/sys_faces.c:146-151`), `c->rec` (`nos/apps/sys_home.c:488`). The face reads `c->aod` at `face_matrix.c:206`, `:337`. It reads `c->enter` only through `fk_build()` (`:205`, `nos/faces/faces.c:478-481`). It reads `c->lite` through `fk_ghost()` (`nos/faces/face_kit.h:141-146`, used at `face_matrix.c:357`), and `c->rec` through `fk_red()` (`face_kit.h:151-155`).

Built into the context but never read by this face: steps and goal (`nos/apps/sys_home.c:471-472`), `bpm` (`:473`), weather (`:477-480`), next alarm (`:481-482`), and the date fields `dt.weekday` and `dt.day`. None of them appear in `face_matrix.c`.

Preferences:
- `nos_prefs.hour24` (`nos/faces/face_matrix.c:188`, `:199`, `nos/faces/faces.c:81`, `:89`). Written by the Settings toggle (`nos/apps/set_settings.c:534`).

Services, getters and globals:
- `svc_sys()` (`nos/svc.c:986`), written by `sys_tick()` once a second (`nos/svc.c:987-997`, `:1315-1317`).
- `svc_notif_unread()` (`nos/svc.c:238-243`).
- `ui_ripple_blur` (`nos/ui.c:842`, `nos/faces/face_matrix.c:252`, `:255`). On the watch it stays at its default of 1, because only the simulator writes it (`sim/nos_sim.c:821`).
- Face kit helpers from `nos/faces/face_kit.h`, `nos/faces/faces.c`: `fk_aod_update`, `fk_wake_tick`, `fk_wake_k`, `fk_wake_off`, `fk_wake_stale`, `fk_build`, `fk_build_k`, `fk_frames`, `fk_unread_tick`, `fk_unread`, `fk_status_shown`, `fk_blink`, `fk_hh`, `fk_mm`, `fk_red`, `fk_ghost`, `fk_view_of`, `fk_clock_fallback`.

HAL calls:
- Direct: `hal_big_alloc()` once, for the PSRAM state (`nos/faces/face_matrix.c:100`). That is `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` (`firmware/main/hal_esp32.c:1595-1597`).
- Indirect, through the context and the kit: `hal_get_datetime()` (`firmware/main/hal_esp32.c:142-146`, `:116-127`), `hal_millis()` (`nos/faces/faces.c:460`), `hal_power_get()` (`nos/svc.c:990`, `firmware/main/hal_esp32.c:378-390`), `hal_battery_percent()` (`firmware/main/hal_esp32.c:392`).
- Libraries and drawing: `gfx_fast_sqrt`, `m_clamp`, `ui_mix`, `dot_disc` (`nos/dot.h:64`), `gfx_rec_cmd`, `gfx_hash_u`, `gfx_hash_f`, and `ui_text` through the fallback (`nos/faces/faces.c:105`). Probe calls are compiled only under `GFX_COST_MODEL` (`nos/faces/face_matrix.c:217-224`).

Hardware the face needs:
- The display (466 x 466 RGB565), reached through the stripe framebuffer and `dot_disc` damage commands (`nos/faces/face_matrix.c:323`, `:346`, `:370`).
- PSRAM for one state block of about 4.5 KB (`nos/faces/face_matrix.c:57-65`). Without PSRAM the draw falls back (see Gaps).
- A system clock. On the B board the PCF85063 RTC restores it at boot (`firmware/main/board.c:869-874`). The phone, USB or SNTP can set it.
- Battery. B samples GPIO4 through ADC1 channel 3, divider 3 (`firmware/main/pins.h:65-66`). The board-sense task samples once a second (`firmware/main/board.c:1021`, task at `:967`, created at `:1036`).
- Charging. B has no charge-status pin (`PIN_CHG_STAT` is defined only in the 1.43C block, `firmware/main/pins.h:33`). Charging is inferred (see Gaps).
- Not needed: IMU, SD, Wi-Fi, BLE, audio, haptics, touch. Touch and the button belong to the shell. The unread count depends on notifications from the phone or the local queue (`nos/svc.c:1041`, `:267-268`).

## Writes

Persistent writes: none. The face never calls `prefs_save()`, `hal_store_*`, SD or NVS. The face choice (`nos_prefs.face`) is written by the picker, not by this face (`nos/apps/sys_faces.c:374-375`).

RAM writes, all in `face_matrix.c`:
- The state block `M` (`:57-65`, allocated at `:100`, never freed). It holds `lvl`, `mask`, `ang`, `head`, `now`, `prev`, `used` and `built`. `tick()` writes `now`, `prev` and `lvl` (`:189-198`, `:261`, `:274`, `:282`).
- Statics: `s_change_t`, `s_last_hm`, `s_last_hour24`, `s_wake_k`, `s_lo`, `s_hi`, `s_w`, `s_bat_aod` (`:67-74`).

Shared face-kit state, written on every tick of this face:
- `fk_wake_tick()` updates its own `fk_wake` (`nos/faces/faces.c:458-477`).
- `fk_aod_update()` bumps the shared dot-list epoch `s_dc_epoch` on every tick, which invalidates other faces' dot caches. It also updates `fk_aod_dx` and `fk_aod_dy` (`nos/faces/faces.c:57-64`).
- `fk_unread_tick()` updates `s_unread_k`, which all faces share (`nos/faces/faces.c:552-560`).
- `sec_tick()` updates `s_sk`, which `fk_blink` and `fk_sec_k` read (`nos/faces/faces.c:41-55`, `:801-811`).

Framebuffer writes: dot discs, the battery bar, the unread dot, the ROM-font fallback clock, and `gfx_rec_cmd` damage records (`nos/faces/face_matrix.c:321-383`).

## Background behaviour

Nothing runs for this face on its own. The face is a callback in the UI loop.

- **Awake (PWR_ON).** The UI loop runs in the `app_main` task (`firmware/main/nos_main.c:1899`), calling `nos_frame_run()` at `:2155`. It is paced by `nos_frame_cap_period(nos_prefs.fps_cap)` (`:2132`). The default cap is 60 fps (`nos/frame_cap.h:5`, `nos/prefs.c:161`). While home is the top screen, each frame runs `home_tick()`, which ticks the current face (`nos/apps/sys_home.c:612-626`). `tick_screens()` ticks only the top screen when idle (`nos/nos.c:2101-2110`).
- **Picker open.** The picker ticks the page under the finger and the next one, `s0` to `s0 + 1` (`nos/apps/sys_faces.c:151-155`). MATRIX is ticked there only when it is one of those two pages.
- **An app is open.** Home is not the top screen, so the face is not ticked (`nos/nos.c:2101-2110`). Services still run (`nos/nos.c:2341`). On return, `fk_wake_stale()` suppresses the replay of a minute change that happened off the glass (`nos/faces/faces.c:458-461`, `nos/faces/face_matrix.c:200-203`).
- **AOD (nos_aod = 1).** The loop runs `nos_frame_run()` once a second (`firmware/main/nos_main.c:2106-2131`). The face ticks and draws once a second in its AOD look. Ring, sparkle, ghost grid and the normal bar are off. `nos_aod` is set in `go_state()` (`firmware/main/nos_main.c:705`).
- **Sensor idle (PWR_SENSOR).** It uses the same 1 Hz AOD path (`firmware/main/nos_main.c:705`, `:2106`). The defaults are `sensor_idle = 1` and `aod = 0` (`nos/prefs.c:159`, `:144`). With `sleep_s` = 15 (`nos/prefs.c:149`), the idle target is SENSOR unless the battery is low (`nos/power_policy.h:13-14`, `firmware/main/nos_main.c:2084-2085`). So with the shipped defaults, the idle screen is this face's AOD look at 1 Hz, not a blank panel. A user's saved settings can change this.
- **Off (PWR_OFF).** The face is not ticked or drawn. The loop only calls `nos_bg_tick(1.0f)` once a second, which runs `svc_tick()` and `background_tick()` (`firmware/main/nos_main.c:2091-2103`). This is reached when the battery is low, from the serial `off` command (`firmware/main/nos_main.c:1616`), or from the sleep policy when both `sensor_idle` and `aod` are off (`nos/power_policy.h:13-14`).
- **Battery sampling.** `board-sense` samples the battery once a second whatever the screen state (`firmware/main/board.c:1021`).
- **Services.** `svc_tick()` runs from `services_tick()` (`nos/apps/registry.c:62-66`). Battery and charge refresh in `sys_tick()` once a second (`nos/svc.c:1315-1317`).

## Persistence

- The face persists nothing and keeps no state across reboots. The state block is about 4.5 KB in PSRAM and is rebuilt after boot (`nos/faces/face_matrix.c:57-65`, `:97-141`).
- The only saved input is `nos_prefs.hour24`. It lives in the `nosprefs` blob, checkpointed to SD at `/sdcard/watch/settings.0.bin` and `settings.1.bin` (`firmware/main/hal_esp32.c:243`, `firmware/main/sd_xfer.c:294`). It is mirrored to NVS under `nosprefs` (`firmware/main/hal_esp32.c:228-262`). Saves are debounced: two seconds after the last edit, or ten seconds at most (`nos/prefs.c:366-383`).
- The unread count is RAM only. Notifications live in the `svc_big` block in PSRAM (`nos/svc.c:57-58`, `:1281`). No store key holds them, so after a reboot the dot is gone until new notifications arrive.
- The clock comes from the RTC on B (`firmware/main/board.c:869-874`).

## Navigation

- **Opened from:** home page 0 is the face (`nos/apps/sys_home.c:271-275`, `:895-903`). It becomes the face when the user picks it in the picker (`nos/apps/sys_faces.c:374`). The picker opens from a long press on home's face (`nos/apps/sys_home.c:1135-1140`). It also opens from the Settings "Watch face" row (`nos/apps/set_settings.c:1852`), or the serial `face N` bench command (`firmware/main/nos_main.c:1389`). That command sets `nos_prefs.face` in RAM without calling `prefs_save()`, so it persists only if another save happens later.
- **Opens:** nothing. `tap()` returns 0 (`nos/faces/face_matrix.c:385`). `nos_face_press_rect()` also returns 0 for this face (`nos/faces/faces.c:830-840`), so no press highlight shows. Home's tap handler checks only the REC chip before offering the tap to the face (`nos/apps/sys_home.c:1119-1126`).
- **Not interactive:** `nos_face_input()` returns 0 for every face except ORRERY (`nos/faces/faces.c:826-828`).
- **Notifications:** the unread dot clears when the notification list is entered (`nos/apps/sys_notifs.c:276`, `:292`, `nos/svc.c:259-263`). The face does not open the list.

## Gaps

- **No date or weekday.** The face never calls `fk_date()` (`nos/faces/faces.c:107-110`), though DOT and others do (`nos/faces/face_dot.c:66`). The date fields are in the context and unused (`nos/apps/sys_home.c:468`). Severity medium.
- **No battery percentage.** The bar appears only at 20 % or below, or while charging (`nos/faces/faces.c:582-586`), with 20 % steps (`nos/faces/face_matrix.c:174`). Severity medium.
- **Charging can read true on USB without a charger.** B has no charge-status pin (`firmware/main/pins.h:33`). `usb` is set when a USB serial-JTAG host is attached (`firmware/main/hal_esp32.c:323`). The code's own comment says that is a host signal, not charge current. `external_power` is then 1 (`:337`, `:350`), and the bar shows the charging blink (`face_matrix.c:281`). A rail reading above 4400 mV for three samples also counts (`hal_esp32.c:347-350`). Severity medium.
- **Stale battery estimate.** On USB or during unknown intervals the percentage is held at the last estimate (`firmware/main/hal_esp32.c:390-391`). `hal_power_get()` clears `battery_valid` and `external_power` after 3 s without a sample (`firmware/main/hal_esp32.c:378-390`), but keeps the percent. So the bar can show a stale level. Severity low.
- **Unused context data.** Steps and step goal, heart rate, weather and the next alarm are built every frame (`nos/apps/sys_home.c:469-482`) and ignored by this face (`face_matrix.c` has no reads of them). Other faces show them (`nos/faces/face_split.c:76-79`, `nos/faces/face_inverted.c:76`). Severity medium.
- **Unread is a dot, not a count.** Any count shows the same dot (`nos/faces/faces.c:552-554`, `:568`, `:573`). Severity low.
- **Unread count is not persisted.** Notifications are RAM only (`nos/svc.c:57-58`, `:1281`), so a reboot clears the dot. Severity low.
- **Unset clock shows 1970.** If no source has set the clock, `board.c` sets the epoch to zero (`firmware/main/board.c:883-886`). The face then shows a 1970 time with no warning. The `HAL_FEAT_SYNC` status (`firmware/main/hal_esp32.c:905`) has no caller in `nos/`. Severity medium.
- **No AM/PM in 12 h.** The 12 h mode omits any AM/PM marker (`nos/faces/face_matrix.c:144-161`). Severity low.
- **No tap targets.** Taps do nothing except on the REC chip (`face_matrix.c:385`; `nos/apps/sys_home.c:1119-1126`). There is no route from the time, the battery or the unread dot to Alarms, Battery or Notifications. Severity low.
- **No per-face options.** The only setting that touches the face is the global 12/24 h toggle (`nos/apps/set_settings.c:474`). There is no ring-off or accent option. The `face_accent` preference is stored (`nos/prefs.c:34`), but no code reads it, so MATRIX's red stays fixed (`nos/faces/face_kit.h:151-155`). Severity low.
- **Fallback loses the unread dot.** If PSRAM allocation fails, `draw()` shows only the ROM-font clock, and the unread dot is not drawn (`nos/faces/face_matrix.c:331-334`, `:382`). `build()` also retries the allocation on every tick (`:97-101`, `:180`). Severity low.
- **Header comments that do not match the code.** The header says "no anti-aliasing" (`face_matrix.c:9`). Dots are anti-aliased discs from a coverage mask (`nos/dot.h:4`, `:58`, `nos/dot.c:673`), and fades use interpolated colours (`face_matrix.c:297`, `:304`, `:310-312`). The header's "~11 ms full redraw" figure (`:12-14`) is not verified here, and the probes behind it are simulator-only (`:217-224`). Severity low.
- **Simulator seeds fake notifications.** `seed_sim()` posts sample notifications only in non-ESP builds (`nos/svc.c:1264-1273`). On the watch the count comes from real sources only.

## Related

- [[Shell and faces]], [[Face picker]], [[Home pager]], [[Notifications]], [[Settings]].
- [[Services hub]] for the `svc_sys` and notification getters.
- [[Health and activity]] and [[Sports and workouts]] for the steps, heart rate and workout values the face does not show.
- [[Power and battery]] for the AOD, sensor-idle and off states, and the battery sampling.
- [[Storage and persistence]] for the `nosprefs` checkpoint and its SD and NVS paths.
- [[Hardware and drivers]] for the RTC restore, the PSRAM allocator and the battery ADC.
- [[Connectivity and voice]] for where notifications come from.
- [[Engine and scheduling]] for the UI loop, frame pacing and the screen tick order.
