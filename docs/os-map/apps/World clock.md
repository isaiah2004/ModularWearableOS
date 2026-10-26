---
title: "World clock"
id: app-world-clock
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# World clock

## Summary

World clock is three screens in one file, `nos/apps/time_worldclock.c` (437 lines): the list `scr_worldclock` (`:280`), the city chooser `scr_wc_pick` (`:362`) and the analog city view `scr_wc_detail` (`:437`). It is launcher tile "World clock" with `DOT_ICON_GLOBE` (`nos/apps/registry.c:25`). The screens are declared in `nos/apps/nos_apps.h:48`.

- It keeps up to 4 favourite cities, chosen from a fixed table of 13 (`time_worldclock.c:34-42`, `:42`).
- Each city's time comes from the board clock, its fixed standard offset, and one of six hard-coded DST rules, all computed from the UTC instant (`:97-129`).
- Favourites persist as a 6-byte blob under NVS key `nos_wc`. It is written only when the chooser toggles a city (`:60`, `:334`).
- It owns no task, timer, ISR or service subscription. It runs only as the top screen, once per engine frame (`nos/nos.c:2101-2108`).
- It calls no `svc_*` function and no `prefs_*` function. It reads the `nos_prefs` struct directly, and the `hal_*` calls are listed under Reads.
- Source read only. This card was not built, simulated or run on a 1.43-B, so the status is a reading of the code, not a bench result.

The file header (`time_worldclock.c:1-5`) says "a fixed table of 12". The table holds 13 entries (`:34-40`), so the header is stale.

## Features

- **List (WORLD CLOCK)**: one 344 px card per favourite, 90 px pitch from y 108 (`:184-185`, `:204`). Each card shows:
  - the city name in caps (`:206`);
  - a day/night dot, filled white for day and hollow for night (`:211-212`, `:166`);
  - the offset from this watch in the form `-5:30`, `+3:00` or `SAME TIME` (`:158-165`). When the city is on a different calendar day, a weekday abbreviation is appended (`:163-164`);
  - the city's local time as HH:MM in the dot font, using the 12/24-hour preference (`:208`, `ui.c:903`).
- **Live update**: `wl_tick` recomputes every favourite each frame while the screen is on (`:238-247`). The minute digits change on the minute.
- **Build-in**: the list builds over 12 frames with staggered rows (`:24-29`, `:243`). Returning from a pop does not rebuild it (`:235`).
- **Scroll**: the title tracks the list offset (`:263`). The offset is kept across a re-enter (`:229`, `:232`).
- **EDIT CITIES pill**: always the row after the last favourite (`:194-199`). Tapping it opens the chooser (`:222`).
- **Empty state**: with no favourites, the screen shows a globe, "No cities" and a primary ADD CITIES button (`:251-262`). The button opens the chooser on tap (`:270-275`).
- **City chooser (CITIES n/4)**: the 13 cities, each with its current UTC offset, DST included (`:307-309`). A check disc marks a favourite (`:313-316`). Tapping a city adds it or removes it, with haptic and sound feedback (`:318-335`). A fifth city shows the toast "List full" (`:326-329`).
- **City view (CITY)**: an analog dial of 60 dots with 12 hour marks. The hour and minute hands grow out dot by dot. A red seconds dot grows in last. The city name sits at the top with its day/night dot, and the offset from this watch sits at the bottom (`:369-436`).
- **Button and gesture**: the side button goes home from any depth (`nos/nos.c:952`). A rightward swipe is the system back gesture (`nos/nos.c:841-858`).

## Reads

**Service getters (`svc_*`)**: none. `#include "svc.h"` is at `time_worldclock.c:10`, but no `svc_` call is made.

**Prefs keys and globals**:
- `nos_prefs.tz_min` (`nos/prefs.h:25`, `nos/prefs.h:48`). Read at `time_worldclock.c:127`, `:136` and `:141`. It is the fallback offset when the clock is not trusted, and it is the offset check for a trusted clock (`:141`).
- `nos_prefs.hour24` (`prefs.c:147`). Read through `ui_fmt_time` (`nos/ui.c:903`), called at `time_worldclock.c:208`.
- `nos_prefs.haptics` and `nos_prefs.sound` (`prefs.c:138`, `:140`). Read through `nos_feedback` (`nos/nos.c:557-566`).

**HAL calls**:
- `hal_get_datetime` (`core/hal.h:29`, `firmware/main/hal_esp32.c:142-146`). Called in `city_time` (`time_worldclock.c:146`) and in the chooser row (`:306`). It returns local wall time under the fixed TZ set by `apply_tz` (`hal_esp32.c:822-831`).
- `hal_epoch_seconds` (`core/hal.h:42`, `hal_esp32.c:148-152`). Called at `time_worldclock.c:124` and `:135`. It returns 0 unless the time is between 1704067200 and 4102444800, that is 2024-01-01 to 2100.
- `hal_store_load("nos_wc")` (`core/hal.h:56`, `hal_esp32.c:258-281`). Called once per boot in `fav_load` (`time_worldclock.c:52`).

**Engine state**: `nos_t` (`nos/nos.h:31`), `nos_enter_t` (`:189`), `nos_revealed` (`:178`). `nos_motion_probe` (`:330-332`) is a no-op on device.

## Writes

- **NVS**: `hal_store_save("nos_wc", &F, 6)` in `fav_save` (`time_worldclock.c:60`). It is called only from `pk_tap` (`:334`). See Persistence.
- **Feedback and toast**: `nos_feedback` with `FB_TAP` (`:275`), `FB_TOGGLE_OFF` (`:325`), `FB_TOGGLE_ON` (`:332`) and `FB_ERROR` (`:327`). `nos_toast("List full")` (`:328`).
- **Navigation**: `nos_push` to `scr_wc_pick` (`:222`, `:275`) and to `scr_wc_detail` (`:224`).
- **RAM only**: `F` (favourites, `:44`), `s_loaded` (`:45`), `WL` (list state, `:176-180`), `s_list` (`:175`), `s_detail_city` (`:182`), the chooser state (`:283-286`) and `D` (the city view, `:369`).
- **Not written**: `nos_prefs`, SD, the RTC, the clock and any service state. This app does not change the clock or the zone.

## Background behaviour

- **No owned task**: the file creates no FreeRTOS task, `esp_timer`, ISR or event subscription. A grep of the file finds none.
- **While on screen and ON**: `tick_screens` calls the top screen's tick once per engine frame (`nos/nos.c:2101-2108`, called from `nos/nos.c:2347`). The tick runs `city_time` for each favourite (`time_worldclock.c:240-241`). That is a few `hal_*` reads and DST arithmetic per frame, which is cheap. Presentation is paced at `NOS_PRESENT_FPS` 60 (`nos/render_config.h:13`), with the app loop in `app_main` (`firmware/main/nos_main.c:1899`, `:2116`).
- **Covered or closed**: `tick_screens` ticks only the top screen and the two transition participants (`nos/nos.c:2101-2109`). The app is not ticked when closed. Its RAM state stays, and `wl_enter` runs again on any return, including a pop reveal (`nos/nos.h:71`).
- **Idle timeout**: with no keep-awake flag, the screen times out after `sleep_s`, 15 s by default (`nos/prefs.c:149`, `firmware/main/nos_main.c:2077-2085`). Default `sensor_idle` is 1 (`prefs.c:159`), so the idle target is `PWR_SENSOR` (`nos/power_policy.h:13-14`). Both AOD and SENSOR call `nos_home_now`, which collapses the stack (`nos_main.c:705`, `nos/nos.c:429-437`). The world clock is therefore closed on idle, not kept on screen.
- **Screen off**: the PWR_OFF loop ticks `nos_bg_tick` at 1 Hz (`nos_main.c:2092-2094`). That is background services only and does not tick this app.
- **Clock keeping time**: the ESP32 system clock runs on its own. The RTC is restored at boot by `board_restore_clock` (`firmware/main/board.c:847`). It is written back by `rtc_maintain` (`board.c:801-821`), which runs in the `board-fs` task (priority 1, 100 ms loop, `board.c:919-966`, created at `:1042`), but only when the clock is dirty and trusted.
- **Clock and zone updates from the phone**: a BLE TIME write sets the epoch and the zone (`firmware/main/hal_esp32.c:847-885`, `apply_tz` at `:822-831`). Gadgetbridge `apply_time` also sets the zone and writes `nos_prefs.tz_min` through `prefs_save` (`nos/svc.c:1000-1006`). These run in the connectivity path, not in this app.

## Persistence

- **Key and namespace**: NVS namespace `wos`, key `nos_wc`. The blob is `{ uint8_t magic = 0x57; uint8_t n; uint8_t idx[4]; }`, 6 bytes (`time_worldclock.c:44`, `:46`).
- **Write path**: `hal_store_save` (`firmware/main/hal_esp32.c:256-257`) calls `hal_store_save_checked` (`:228`). For keys other than `nosprefs` and `wifi_cred` it calls `nvs_save` (`:236`), which does `nvs_set_blob` and `nvs_commit` (`:177-190`). It does not read the value back. `hal_prefs_overlay_save_checked` does read back, but this key does not use that path.
- **Not on SD**: on the 1.43-B, SD is used only for `nosprefs` and `wifi_cred` (`hal_esp32.c:236`). The world clock list is not in the `/watch/settings.*.bin` set named in CLAUDE.md.
- **Read path**: `hal_store_load` for this key skips the SD branch and reads NVS directly (`hal_esp32.c:258-281`). It returns 0 on any NVS error, and `fav_load` then uses the defaults (`time_worldclock.c:52-57`).
- **Defaults**: London, New York, Tokyo and Sydney (`time_worldclock.c:57-58`). They are not saved until the first chooser edit.
- **Failure handling**: a failed save is silent. The return value is dropped at `hal_esp32.c:256-257` and at `time_worldclock.c:60`. RAM keeps the new list and it is not retried.
- **What is not persisted**: scroll position, the chooser's selection state and the city view. Scroll is kept only in RAM while the watch stays up (`time_worldclock.c:229`).

## Navigation

- **Opened from**: the launcher only. The tile is `registry.c:25`, opened with `nos_open`'s iris transition (`nos/apps/nos_apps.h:76-77`). No home widget or complication opens it. The widget list (`nos/apps/nos_apps.h:69`) has no world-clock entry.
- **Opens**:
  - the chooser `scr_wc_pick`, from the EDIT CITIES row (`time_worldclock.c:222`) or the empty-state ADD button (`:275`);
  - the city view `scr_wc_detail`, from a city row (`:224`).
- **Back**: a rightward swipe is the system back gesture. None of the three screens sets `SCR_NOBACK` (`nos/nos.h:62`), and the city view has no handler (`:437`). The side button goes home (`nos/nos.c:952`).
- **Inside the city view**: taps do nothing, because the screen has no input handler (`:437`). Nothing on it can be pressed.

## Gaps

- **[medium] An untrusted clock is not flagged.** Evidence: `time_worldclock.c:122-141` (falls back to `tz_min` and to the 1970 wall clock when the epoch is 0); `firmware/main/board.c:882-885` (sets the system time to 0 when the RTC is unset); `firmware/main/hal_esp32.c:148-152` (epoch 0 below 2024). Detail: by reading the code, with epoch 0 the list shows each city at its UTC offset from midnight (London 00:00, Tokyo 09:00) and nothing says so. The app never calls `hal_epoch_seconds` for display. Settings > Modules does show "UTC NOT SYNCED" (`nos/apps/set_modules.c:777`). User impact: wrong times with no warning until a phone or USB sync.
- **[medium] The home zone has no DST.** Evidence: `hal_esp32.c:822-831` (fixed POSIX offset "UTC±hh:mm"); `firmware/main/board.c:830-839`; `nos/svc.c:1000-1006` (the zone changes only when the phone sends TIME); `time_worldclock.c:133-156` (relative offsets use the fixed "here"). Detail: the 13 city rows follow DST, but "here" does not. After a home DST change the "here" time and every relative offset are an hour off until the next phone sync. User impact: the list disagrees with itself across a DST change.
- **[medium] Favourite saves are fire-and-forget.** Evidence: `time_worldclock.c:60` (result ignored); `hal_esp32.c:256-257` (`hal_store_save` drops the result); `hal_esp32.c:228-236` (the NVS failure is returned to the caller and then dropped). Detail: the new list is shown and kept in RAM, is not retried, and is lost on the next reboot if the write failed. User impact: a city added on the watch can silently disappear.
- **[medium] The city view shows no digital time.** Evidence: `time_worldclock.c:382` (`ui_fmt_time(D.tm, ...)` computes the text each tick); `:408-436` (`dt_draw` never draws `D.tm`). The only time cues are the analog hands and a seconds dot (`:431-435`). The list also shows no seconds. User impact: reading an exact minute needs the dial.
- **[medium] Fixed catalogue, four slots.** Evidence: `time_worldclock.c:34-42` (13 fixed cities); `:42` (`NFAV 4`); `:326-329` (fifth add refused). There is no city search, no custom city, and no use of the phone-supplied location text that the BLE TIME write stores (`hal_esp32.c:884-890`). User impact: a city outside the 13 cannot be added, and only four are visible.
- **[medium] The screen does not stay on.** Evidence: `time_worldclock.c:280`, `:362`, `:437` (flags 0, no `SCR_KEEPAWAKE`); `nos/prefs.c:149` (15 s); `firmware/main/nos_main.c:2077-2085` and `nos/power_policy.h:13-14` (the idle target is SENSOR by default); `nos_main.c:705` (AOD and SENSOR call `nos_home_now`). User impact: a glance lasts about 15 s without a touch, and then the watch goes to home. There is no always-on world clock.
- **[low] A corrupt or unreadable store resets the list.** Evidence: `time_worldclock.c:52-57` (any load failure gives the defaults; the range check at `:54` does not reject duplicate indices); `hal_esp32.c:258-281` (returns 0 for every NVS error). Detail: the next edit writes the defaults plus the change over the saved list (`:334`). A duplicated index would show one city twice. User impact: a transient NVS read error loses the saved list.
- **[low] Cities cannot be reordered or removed from the list.** Evidence: `time_worldclock.c:266-279` (no long-press, swipe or reorder handling); removal exists only through the chooser toggle (`:318-335`). User impact: order is insertion order, and removing one means a trip to the chooser.
- **[low] The city view takes no input.** Evidence: `time_worldclock.c:437` (input handler is 0). User impact: no way to step to the next favourite or to act on the city from the view.
- **[low] 12-hour mode has no AM/PM.** Evidence: `nos/ui.c:903` (changes the number only); `time_worldclock.c:208` (prints HH:MM). The alarms app adds AM/PM (`nos/apps/time_alarm.c:936`). User impact: 9:05 is ambiguous in 12-hour mode.
- **[low] The relative offset has no label.** Evidence: `time_worldclock.c:158-165` (prints `-5:30` or `+3:00`, with no "HRS" or "FROM HERE" text). User impact: the direction of the difference is implied, not stated.
- **[low] Day and night use the clock, not the sun.** Evidence: `time_worldclock.c:166` (day is 06:00 to 18:00 in the city's clock). The dot is also drawn in the city view title (`:419-420`). User impact: the dot can be wrong near dawn and dusk, and in polar or unusual zones.
- **[low] No home widget.** Evidence: `nos/apps/nos_apps.h:69` (the widget list has no world clock). User impact: the time is one launcher tap away and is not on the home pager.
- **[low] Feedback is silent without the strap.** Evidence: `nos/nos.c:557-566` (calls `hal_haptic` and `hal_sfx`); `firmware/main/ext/ext_haptic.c:116` (returns if the driver is not up); `firmware/main/ext/ext_priv.h:33` (motor on IO16 of the back-header strap); `firmware/main/ext/ext_sys.c:169-185` (init gated on `EXT_BOARD_OK`). User impact: on a bare board the taps are visual only.
- **[low] Missing comparison features.** Evidence: the file has no meeting planner, no "time in city X when it is Y here" conversion, and no alarm or notification tied to a city. The alarms live in `nos/apps/time_alarm.c`. User impact: the app shows times, but does not help schedule across cities.
- **[low] Stale header.** Evidence: `time_worldclock.c:1-5` ("a fixed table of 12"), against 13 entries at `:34-40`. The polish note on DST (`docs/polish/healthtime_r1.md:65`) is out of date; `docs/polish/healthtime_r2.md:358` reflects the current DST code for the city rows only, not for the home zone. Documentation only; no user impact.

## Related notes

- [[Services hub]]: no service getters are called by this app. The list is reached without a service.
- [[Storage and persistence]]: the `nos_wc` NVS key and the SD-versus-NVS split.
- [[Hardware and drivers]]: display, touch, the B RTC at 0x51 and the strap motor at IO16.
- [[Power and battery]]: the idle timeout, PWR_SENSOR and the home collapse.
- [[Engine and scheduling]]: `tick_screens`, transitions and the frame loop.
- [[Shell and faces]]: the launcher tile, home and the AOD path.
- [[Connectivity and voice]]: the BLE TIME write and Gadgetbridge zone updates that set the clock this app reads.
