---
title: "Torch"
id: app-torch
tags: [nos, os-map, app]
status: working
updated: 2026-10-23
---

# Torch

Source: `nos/apps/con_torch.c` (218 lines), screen `scr_torch`. Read-only source review on 2026-10-23. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Status comes from source reading only.

## Summary

Torch turns the whole 466 x 466 AMOLED into a light. The 1.43-B has no separate LED (`firmware/main/pins.h`: `PIN_LED` is defined only in the 1.43C branch, line 34). Entry shows white at panel maximum (backlight 255). A tap toggles white and a dim red night-vision field. A vertical drag sets brightness in 10 % detents from 10 % to 100 % (`nos/apps/con_torch.c:3-4`, `:69-70`, `:182-202`).

The screen owns no task, timer or service. All of its state is a RAM `static` struct (`con_torch.c:26-42`). It calls no `svc_*`, `prefs_*` or `hal_*` function directly. Its only effects are engine calls (`nos_brightness_override`, `nos_keep_awake`, `nos_feedback`, `nos_revealed`), diagnostics records, and one prefs byte (`nos_prefs.torch`) that nothing reads.

Launch points: the launcher list (`nos/apps/registry.c:33`) and the Control Center "Torch" tile (`nos/apps/sys_control.c:89`, tap at `:613-615`, long-press at `:701-705`). The tile has no hardware gate (`flags` 0 at `:89`).

Related: [[Utility apps]] (the sibling note with a shorter Torch section), [[Shell and faces]] (launcher, Control Center, back gesture, power sheet), [[Engine and scheduling]] (tick dispatch, sleep policy, cover and reveal), [[Power and battery]] (keep-awake versus low battery), [[Hardware and drivers]] (panel brightness, optional haptic and speaker straps), [[Storage and persistence]] (prefs checkpoint and diagnostics on SD), [[Services hub]] (Torch is not a service consumer). No dependency on [[Health and activity]], [[Sports and workouts]] or [[Connectivity and voice]].

## Features

- **Entry white light.** `torch_enter` sets `mode=0` and `level[0]=1.0` (`nos/apps/con_torch.c:69-70`). `backlight_for(0, 1.0)` gives 255 (`:46-53`). That is panel maximum, not the user's Brightness setting (see Gaps).
- **Tap toggles mode.** `IN_TAP` calls `switch_mode` (`:182-184`, `:154-164`). Mode 0 is white (`C_WHITE`). Mode 1 is the red field (`TORCH_RED` 0x7800, `:19`, `:44`).
- **Night red.** The red mode backlight is `level * 0.55 * 255` (`:48`), so it never exceeds about 140 of 255. The gauge sits on a `TORCH_FIELD` (0x9800) pill with black lit dots, unlit `TORCH_UNLIT` (0x6000) dots and black figures (`:20-21`, `:126-133`).
- **Brightness drag.** `IN_DRAG_START` on the Y axis starts a drag (`:185-190`). `lv = clamp(drag_level0 - dy/260, 0.1, 1.0)` (`:194`). Up is brighter, because `dy` is finger screen Y minus start Y (`nos/nos.c:737`). The value is quantised to 10 % detents (`:195-200`). The feedback is `FB_LIMIT` at 10 % and 100 %, `FB_TICK` between (`:197`).
- **Gauge.** Ten dots at `GAUGE_N` = 10, plus a percentage label (`:24`, `:131-147`). It shows at once while the finger is down and fades out over 6 frames, 0.6 s after release (`:109-110`).
- **Hints.** Tap hint from about 0.35 s to 2.6 s after entry, then drag hint from about 3.05 s to 5.3 s (`:101-108`, `:148-151`). Text: "TAP FOR RED LIGHT" or "TAP FOR WHITE", and "DRAG TO DIM".
- **Fades.** Entry and mode changes take 4 frames (`FADE_F` = 4, `:22`). `M_FPS` is 40, so that is 0.1 s of wall time (`nos/motion.h:25`, `nos/render_config.h:8`). `m_ink` quantises to 1/16 steps, giving 12.5, 37.5, 62.5 and 100 % (`nos/motion.c:42-46`). This matches the header comment at `con_torch.c:5-6`.
- **Restore on cover and reveal.** Leaving releases the override (`torch_leave`, `:80-84`). Coming back after a cover re-applies the saved mode and level (`torch_enter`, revealed branch, `:62-67`).
- **Feedback.** Toggle gives `FB_TOGGLE_ON` or `FB_TOGGLE_OFF` (`:163`). `nos_feedback` maps those to haptic and sound, gated by `nos_prefs.haptics` and `nos_prefs.sound` (`nos/nos.c:557-574`).
- **Diagnostics.** Each mode toggle and each level change writes a DIAG action record (`:154-161`, `:166-173`).

## Reads

Engine (no service getters, no prefs keys):
- `nos_t` (`nos/nos.h:31`) as the animation clock. `T.t_enter` and the fades use it directly (`con_torch.c:74`, `:93`, `:103`). `nos_enter_t()` is not used.
- `nos_revealed()` (`nos/nos.c:121`, `nos/nos.h:178`). Decides whether `torch_enter` is a fresh entry or a reveal after a cover (`con_torch.c:62`).
- `m_frame_k`, `m_ink`, `m_clamp` (`nos/motion.c:31-46`). Time-based frame steps.
- `in->type`, `in->axis`, `in->dy` (`con_torch.c:174-215`). `IN_CANCEL`, `IN_TAP`, `IN_DRAG_START`, `IN_DRAG`, `IN_DRAG_END`, `IN_UP`. Other input types return 0 and pass to the engine (`:212-213`).

Services and prefs: none read. `grep` for `svc_`, `prefs_`, `hal_` in `con_torch.c` returns nothing. `nos_prefs` is read only in engine code on Torch's behalf: `nos_prefs.brightness` on override release (`nos/nos.c:580`), `haptics` and `sound` in `nos_feedback` (`:570`, `:572`).

HAL (indirect, through engine functions):
- `hal_millis()` through `nos_keep_awake` (`nos/nos.c:585`), called every tick (`con_torch.c:88`).
- `hal_set_backlight()` through `nos_brightness_override` (`nos/nos.c:576-581`).
- `hal_haptic()` and `hal_sfx()` through `nos_feedback` (`nos/nos.c:570`, `:572`). These are optional external-strap drivers (`firmware/main/ext/ext_haptic.c:114-116`, `firmware/main/ext/ext_audio.c:358-360`).

Diagnostics: `diag_log_name_id("torch.mode")`, `"torch.level"`, `"TORCH"` (`con_torch.c:156-157`, `:170-171`). These are name hashes, not reads.

## Writes

- **RAM, Torch-only:** the `static` struct `T` (`con_torch.c:26-42`): `mode`, `level[2]`, fade `from/to/p/t0`, `hint`, `dragging`, `gauge`, `bl`. Reset on fresh entry (`:69-77`).
- **Prefs RAM byte:** `nos_prefs.torch` set to 1 on entry and reveal (`:65`, `:77`) and 0 on leave (`:83`). See Persistence.
- **Panel backlight:** `set_bl()` (`:55-58`) calls `nos_brightness_override(v)` on change. This sets `nos_bright_override` (`nos/nos.c:575-578`) and calls `hal_set_backlight` immediately (`:580`). The frame path then applies it: `set_level(nos_bright_override >= 0 ? override : user_level())` (`firmware/main/nos_main.c:2226`). `display_set_brightness` (`:387-392`) queues `JOB_BRIGHT` (`:187`, `:197`), which writes DCS 0x51 (`:199`).
- **Haptic and sound pulses:** via `nos_feedback` (`con_torch.c:163`, `:197`).
- **Diagnostics:** `diag_action_request`, `diag_action_context`, `diag_action_result` (`nos/diag_log.c:229-241`) from `switch_mode` and `torch_level_trace`. The engine also logs every input event while any screen is on top: `diag_log_event(DIAG_ACTION, ...)` at `nos/nos.c:950`.
- **Diagnostics to SD:** queued records are flushed by `diag_log_service` (`nos/diag_log.c:463`). On ESP this runs from the `sports_sd` writer loop (`nos/sports.c:282-291`, `:289`). The directory is `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`), with `FILE_COUNT` 8 and a 1 MiB file limit (`:47`, `:49`).
- **Prefs checkpoint:** `prefs_save` writes the whole 52-byte `nos_prefs_t` with `hal_store_save_checked("nosprefs", ...)` (`nos/prefs.c:349`). That includes the `torch` byte.

## Background behaviour

Nothing runs in the background. Torch has no task, timer or ISR.

- **Tick scope.** `tick_screens` ticks only the top screen when idle, or the under and over screens during a transition (`nos/nos.c:2101-2109`). A covered Torch is already left, so it is not ticked.
- **Sleep blocked.** The sleep policy passes `keep = top->flags & SCR_KEEPAWAKE` (`firmware/main/nos_main.c:2080-2081`). `power_timeout_due` returns false when `keep` is set (`nos/power_policy.h:18-19`). So Torch blocks the screen-off timeout and AOD for as long as it is on top. Torch's `nos_keep_awake()` every tick (`con_torch.c:88`) is redundant with the flag. The `Utility apps` note attributes this to `nos_keep_awake`; the decisive part is the flag.
- **Re-wake.** If the panel is already off or in AOD and the top screen has `SCR_KEEPAWAKE`, the loop forces `PWR_ON` (`nos_main.c:2096`, `:2121`).
- **Low battery is ignored.** `power_low_update` sets `s_low_battery` (`nos/power_policy.h:6-11`, `nos_main.c:2031`), and a toast is shown (`:2034`). The only forced transition is AOD or sensor state to OFF (`:2036`). With Torch on top the state stays `PWR_ON`, and the override still wins (`:2226`). Torch never reads battery (`con_torch.c` has no `hal_power_get`).
- **Knock lock.** While locked, new presses are dropped (`nos/nos.c:666`), so Torch cannot be tapped or dragged. The side button still works (`nos.c:952`). The light stays on.
- **Covers.** A power sheet or any other screen pushed over Torch calls `call_cover` (`nos/nos.c:316`, `:468`), which runs `torch_leave`. The override is released while the cover is up. Closing it calls `torch_enter` in the revealed branch.
- **Diagnostics flush** keeps running on the `sports_sd` writer (`nos/sports.c:282-291`). Its wait is 1000 ms when idle and 20 ms when pending (`:291`).

## Persistence

- **Mode, level, fade and hint state:** RAM only (`con_torch.c:26-42`). Lost on reboot or OTA. There is no NVS key or SD path for them.
- **White level:** always reset to 100 % on fresh entry (`con_torch.c:70`).
- **Red level:** `level[1]` is 0 at boot and defaults to 0.5 on first entry (`:71`). It is kept only in RAM for the rest of the boot. It is not in `nos_prefs_t`.
- **Mode:** always starts white on a fresh entry (`:69`).
- **`nos_prefs.torch`:** written in RAM, carried in the whole-struct checkpoint `nosprefs` (`nos/prefs.c:349`), and stored on SD as `/watch/settings.{0,1}.bin` (`nos/prefs_disk.c:32`, `CLAUDE.md`). It is absent from `EDIT_FIELDS` (`nos/prefs.c:32-40`), so it is not a user-edit field. Nothing reads it (`grep` for `.torch` and `->torch` finds only `con_torch.c:65`, `:77`, `:83`). The `prefs.h` comment (line 23) calls it "runtime only, not persisted meaningfully".
- **Boot:** the Torch does not come back after reboot.
- **Diagnostics:** `/sdcard/watch/diagnostics/diag-NN.bin` (`nos/diag_log.c:24`). Evidence only, not read back by Torch.

## Navigation

Opens from:
- Launcher list entry "Torch" (`nos/apps/registry.c:33`), declared `nos/apps/nos_apps.h:53`.
- Control Center tile `T_TORCH`: tap (`nos/apps/sys_control.c:613-615`) and long-press (`:701-705`). Both call `nos_open_icon(&scr_torch, ...)` with `TILE_R`. The tile is drawn outlined as a screen opener (`:415`).
- Re-entry after a cover (`torch_enter` revealed branch, `con_torch.c:62-67`). Not a navigation.

Exits to:
- **Side button:** Torch returns 0 for `IN_BTN` (`con_torch.c:212-213`), so the engine goes home (`nos/nos.c:952`, `nos_home` at `:403`).
- **Right swipe:** Torch sets neither `SCR_NOBACK` nor `SCR_HDRAG` (`con_torch.c:217-218`). The engine treats a rightward drag as back (`nos/nos.c:852-858`), from any point on the screen.
- **Long side button:** pushes the power sheet `scr_power` as `TX_RISE` (`nos/nos.c:953-955`; `nos/apps/sys_power.c:371`). The sheet covers Torch, which then leaves.

Opens other screens: none. `con_torch.c` contains no `nos_push`, `nos_pop` or `nos_open` call. There is no on-screen close control.

## Gaps

| # | Severity | Gap | Evidence |
|---|---|---|---|
| 1 | high | No auto-off timer. `SCR_KEEPAWAKE` disables the screen-off timeout and AOD while Torch is open, so a forgotten Torch stays lit at panel max until the user leaves or the battery dies. | `nos/apps/con_torch.c:218`; `firmware/main/nos_main.c:2080-2085`; `nos/power_policy.h:18-19` |
| 2 | high | Low battery does not dim or close Torch. The screen runs at 255 with `s_low_battery` set. Only AOD and sensor states are forced off. | `firmware/main/nos_main.c:2031-2036`, `:2226`; `nos/power_policy.h:6-11`; no battery read in `con_torch.c` |
| 3 | medium | Entry ignores the user's Brightness and auto-brightness. It forces panel maximum. The header calls this "White at full brightness", and it is by design, but there is no cap. | `con_torch.c:3`, `:69-70`, `:46-53`; `nos/nos.c:576-578`; `firmware/main/nos_main.c:648-655` (`user_level` is not used while the override is set) |
| 4 | medium | Only two looks. No strobe, SOS, warm or cool white, or colour choice. | `con_torch.c:19`, `:44`, `:126`. The only modes are `mode_color` 0 and 1. |
| 5 | medium | Nothing is remembered between sessions. White level and mode reset on each fresh open. Red level is RAM only and resets to 50 % after reboot. | `con_torch.c:69-71`; `nos/prefs.h` (`nos_prefs_t`; no torch level field) |
| 6 | low | The dimmest setting is 10 %. The gauge has no off step. Going dark requires leaving the screen. | `con_torch.c:194` (clamp 0.1) |
| 7 | low | Dead persisted flag. `nos_prefs.torch` is written and checkpointed but never read. The `prefs.h` comment says it is "runtime only". | `nos/prefs.h:23`; `nos/prefs.c:349`; writes at `con_torch.c:65`, `:77`, `:83`; no readers |
| 8 | low | A cancelled drag keeps the level. `IN_CANCEL` logs `DIAG_RESULT_CANCELLED`, but the level already written in `IN_DRAG` is not reverted. The diagnostic and the panel disagree. | `con_torch.c:176-179`, `:200`, `:166-173` |
| 9 | low (plausible, not seen on hardware) | A second tap during the 4-frame fade restarts from the mode colour, not the displayed mix. The colour sweep reverses for up to about 0.1 s. | `con_torch.c:158-162` (`T.from = mode_color(T.mode)`, `T.p = 0`), `:122-123` (`ui_mix(T.from, T.to, T.p)`) |
| 10 | low | Release after leaving uses a different curve than the frame path. Override release writes `brightness*255/100`. The frame path uses `6 + 2.49*pct` plus auto-brightness. Corrected on the next frame. The header says "restored" without noting this. | `nos/nos.c:580`; `firmware/main/nos_main.c:648-655`, `:2226`; `con_torch.c:7` |
| 11 | low | Colour comments do not match the stored values. `#780000`, `#9A0000` and `#600000` are labels. The RGB565 values 0x7800, 0x9800 and 0x6000 expand to about #7B0000, #9C0000 and #630000 in 8-bit. The polish review measured #9C0000 on the pill. | `con_torch.c:19-21`; `docs/polish/outconn_r4.md:44` |
| 12 | low | No visible exit. The only hints are tap and drag. Exit is side button, right swipe or long-press sheet, and none is shown on the screen. | `con_torch.c:148-151`; `nos/nos.c:852-858`, `:952` |
| 13 | low | Haptic and sound feedback are optional straps. On a bare 1.43-B they silently do nothing, so mode changes and detents are visual only. | `nos/hal_ext.h` (`EXT_HAPTIC`, `EXT_SPEAKER`); `firmware/main/ext/ext_haptic.c:116`; `firmware/main/ext/ext_audio.c:360`; `nos/apps/sys_control.c:91-92` |

Not gaps, to avoid over-reporting: no dead buttons (Torch has no buttons); no placeholder data (the percentage is the real level); no service or SD dependency for the light itself.

Doc drift to fix: `docs/os-map/systems/Utility apps.md:101` attributes the sleep block to `nos_keep_awake` every tick. The decisive mechanism is the `SCR_KEEPAWAKE` flag in the sleep policy (`nos_main.c:2080-2085`). The Utility note also does not mention the low-battery gap.
