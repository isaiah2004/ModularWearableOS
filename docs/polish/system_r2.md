# NOS critique: system (Settings, Modules, About, power sheet, boot, AOD) + cross-OS consistency, round 2

**Build reviewed:** `build/nos_sim_sys_critic2.exe`, built from HEAD `f04b390` plus the uncommitted
working-tree edits to `sys_control.c`, `sys_faces.c`, `sys_home.c`, `sys_launcher.c`,
`sys_notifs.c` and `sys_power.c`. An earlier exe built just after `2805820`
(`build/nos_sim_sys_critic.exe`) was used for a first pass. Every finding below was re-run and
re-checked on the fresh build, and findings the working tree already fixes were dropped.

**Evidence:** all paths are relative to `build/nos/critic/system/r2/`.
- Scripts are in `scripts/`. Per-script output is in `v2/<script>/`, with per-frame `NAME_NNN.png`
  from `film!`, plus `frames.csv` and `summary.txt`.
- Comparison grids are `g_*.png`, and zooms are `z_*.png`.
- Measuring tools are in `../tools/`: `blob.py` (bbox), `measure.py` (red components, title band),
  `herobox.py` (hero bbox per frame) and `grid.py`.

**Cost:** 0 late frames in all 14 scripts. The worst frame is 24.4 ms, and only full-screen
transitions reach it.

**Damage check:** PASS on 13 scripts, 17,838 frames in total (`v2/damagecheck.txt`).
- a_list 1050, a2_arm 654, b_sub 932, c_sub 1288, d_mod 5603, d2_fix_run 855, e_about 443
- f_power 744, i_tour 3738, l_press 1098, m_launch 336, k_widgets 331, h_aod 766

All analysis was done frame by frame at 30 fps. No source was edited and COM3 was not opened.

## Verdict

Round 1 landed almost in full. Of its 44 items, 37 are fixed (#6, #12, #17 and #36 leave a residue
that is filed below), 3 are partial, 2 are not fixed, 1 is obsolete and 1 could not be reproduced.
The OS now reads as one product: one title band, 88 px buttons, page dots at y 440 on every pager,
one absent layout, one "Phone not connected", one red per screen, case-true units, and the
arm-then-tap-again pattern on every destructive action. The engine work holds up as well. The iris
with its tapped-icon seed and hairline rim, the 6-frame button close, banners that give way to
sheets and toasts that queue behind banners are all clean, damage tracking is pixel-exact and no
frame is late. What is left is a short list of real defects plus a layer of wording drift. Modules
turns the GPS off when you open its detail, so the list and the detail disagree and backing out
throws away a 3D fix. The picker hero now moves under the finger, but as a sawtooth: it sits still
for 7 frames, jumps 15 px against the finger, then snaps back 16 px at each detent, which reads
worse than round 1's static morph. Settings is now the only app that builds late, because its
hard-coded `+0.17 s` predates `nos_enter_t()`. The working-tree power sheet shows two reds while
Restart is armed, long toasts drop onto the content, long values squeeze their labels, and the
same radio state has two or three names across Settings and Modules. Nothing here is a P0. Fix the
two P1s and the Settings start time, and this area is done.

---

## Round-1 verification (`docs/polish/system_r1.md`)

| # | Item | Status | Evidence (round 2) |
|---|---|---|---|
| 1 | Settings 9/9 vs Modules 16/16 | **Fixed**: both say `16/16` | `v2/a_list/l6.png`, `v2/d_mod/mod_l0.png` |
| 2 | LTE "0 dBm" while searching | **Fixed**: hero `--`, `SEARCHING`, `SIM READY` | `v2/d_mod/d_lte_on.png` |
| 3 | About board row clipped | **Fixed**: `ESP32-S3-Touch-` / `AMOLED-1.43-B` | `v2/i_tour/t_about.png`, `v2/e_about/ab_1.png` |
| 4 | Picker taps late | **Fixed**: `pk_step` sets idx, ticks and commits on the tap (set_settings.c:206) | `v2/b_sub/pick_tapnext.png` |
| 5 | Picker hero static under the finger | **Partial**: it moves now, but as a sawtooth (new #2) | `v2/b_sub/pick_drag_006..029.png` |
| 6 | Module-absent state in 3 wordings | **Fixed in apps**: one `ui_absent` layout. Modules footers and copy still differ (new #12) | `g_v2_hints.png` |
| 7 | Barometer 7 reds | **Fixed**: one red "now" dot | `v2/i_tour/t_baro.png` |
| 8 | Heart widget hides a missing module | **Fixed** | `v2/i_tour/a_widget_2.png` |
| 9 | "No phone" said 4 ways | **Fixed**: `Phone not connected` / "Pair in Gadgetbridge as Bangle.js NOS" on Find, Music and Bluetooth; the row says `NOT CONNECTED` | `v2/i_tour/np_find.png`, `v2/c_sub/bt_nophone.png`, `v2/c_sub/set_absent_l3.png` |
| 10 | Values pop at the bezel | **Partial**: never hidden now, but long values squeeze the label (new #7) | `v2/a_list/l6.png` |
| 11 | Rows drift sideways | **Fixed**: labels at x 62 through the middle band; only the edge rows inset | `v2/a_list/l0..l8.png` |
| 12 | Quick taps show no pressed state | **Fixed in Settings** (4 frames). Alarms ADD ALARM shows only 2 (new #5) | `v2/a_list/quicktap_bright.png`, `v2/l_press/p_alarm_000..002.png` |
| 13 | Settings build hidden under the iris | **Not fixed, now late instead**: content first appears at f5 (new #3) | `v2/i_tour/e_settings_000..005.png` |
| 14 | Wi-Fi tile flashes on release | **Fixed**: monotonic `#E6E2E6 → #CDCACD → … → #181818` | `v2/c_sub/wifi_tap_000..010.png` |
| 15 | Bluetooth tile looks like a switch | **Fixed**: bare icon over the state | `v2/c_sub/bt_0.png` |
| 16 | Auto-brightness absent label | **Fixed**: `[ NO LIGHT MODULE ]` | `v2/c_sub/bright_absent.png` |
| 17 | Sliders look live with no hardware | **Fixed for sliders** (grey fill, `[ NO SPEAKER MODULE ]`). Toggles are still live (new #11) | `v2/c_sub/vol_absent.png` |
| 18 | Flip rows: no affordance, ragged edge | **Fixed**: two-way dot glyph, values end at x 384 | `v2/a_list/l6.png` |
| 19 | Same value, different formats | **Fixed**: `8000` in the list and the picker; `29` GB everywhere | `v2/a_list/l5.png`, `v2/b_sub/pick_goal.png`, `v2/d_mod/d_sd.png` |
| 20 | Units corrupted by caps | **Fixed**: `4.00` VOLTS, `dB SPL`, `hPa`, `m`, `g`, `°/s`. Lux is still spelled 3 ways (new #10) | `v2/d_mod/d_batt.png`, `d_mic.png`, `d_env.png`, `d_imu.png` |
| 21 | Storage duplicates Modules > SD card | **Fixed**: one page. Its title is `SD CARD` from the Storage row (new #16) | `v2/c_sub/storage_0.png`, `v2/d_mod/d_sd.png` |
| 22 | Partial progress segments | **Fixed**: whole segments only | `v2/d_mod/d_sd.png`, `d_batt.png` |
| 23 | TZ/Step picker type sizes | **Fixed**: `UTC OFFSET` / `10:09 LOCAL`, `STEPS A DAY` / `6.0 km` as labels | `v2/b_sub/pick_tz.png`, `pick_goal.png` |
| 24 | Motion axis line on the glass | **Fixed**: split into 3 lines | `v2/d_mod/d_imu.png` |
| 25 | Modules rows have no chevron | **Fixed** | `v2/d_mod/mod_l4.png` |
| 26 | Modules hero pitch varies | **Fixed**: Motion `2°` is about 80 px tall, the same as SD `29`. It now crowds `TILT` (new #17) | `v2/d_mod/d_imu.png` |
| 27 | Touch test: no title, stale coordinate | **Fixed** | `v2/d_mod/d_touch.png` |
| 28 | Reset icon reads as a spinner | **Obsolete**: Reset arms in the list, and the confirm page is gone | `v2/a_list/reset_armed.png` |
| 29 | Destructive actions confirmed 3 ways | **Fixed** (the power sheet only in the working tree): Settings Restart/Reset, power sheet Restart/Power off and Alarm DELETE all arm. Two reds on the sheet (new #4) | `v2/a2_arm/restart_armed.png`, `v2/f_power/power_restart_005.png` |
| 30 | Title placement on 10 screens | **Fixed**: Launcher, Alarm ring, Timer ring (C_SEC), widgets Weather/Timer/Heart and Music at 58–69. About still has none (new #18) | `g_v2_misc.png`, `g_v2_widgets.png` |
| 31 | 7 button heights | **Fixed**: every action button checked is 88 tall. The Timer ring and Stopwatch pairs are off the y/width standard (new #19) | `g_v2_misc.png` |
| 32 | Page dots at 5 heights | **Fixed**: 440 on the pager, Baro, Navigate and widgets | `v2/i_tour/t_baro.png`, `e_nav_005.png`, `t_widget_*.png` |
| 33 | No face shows unread | **Fixed**: the face draws a 10 px white dot at (233, 24). Widgets draw an 8 px red one (new #21) | `v2/i_tour/t_face_unread.png`, `t_widget1_unread.png` |
| 34 | Two code paths with two reds | **Fixed in code**: the Activity widget has no red (fit_activity.c:81–99), and icon-less banners get a white bell (ui.c:689) | – |
| 35 | Hints in two styles | **Fixed**: `TAP TO RING`, `TAP TO STOP` as caps labels | `v2/i_tour/t_find.png`, `t_find_watch.png` |
| 36 | Entry builds 0–560 ms | **Fixed except Settings**: Activity builds from f2 with no resting `0`; Navigate builds its rings; Music fades with no scale-up; Torch dithers from f1 | `g_v2_ent2.png` |
| 37 | Copy widows | **Fixed** for all three. A new one in Modules Cellular (new #12) | `v2/d_mod/d_haptic_absent.png` |
| 38 | Reset button ignores the bezel dim | **Fixed**: `arm_pill` takes `k` (set_settings.c:129–146) | `v2/a_list/l7.png` |
| 39 | About rows before the logo | **Partial**: rows start at 0.20 s, but the tagline belongs to the logo row and shows at full strength at f2, before the dots (new #18) | `v2/i_tour/e_about_002.png` |
| 40 | About mixes 3 value styles | **Fixed**: memory in grotesk; only uptime and rail in dot matrix | `v2/e_about/ab_1.png` |
| 41 | Slider tap snaps 5 % | **Fixed**: 10 % (set_settings.c:1037) | – |
| 42 | Small polish | **Fixed**: Watch face row shows `DOT`; Modules `bottom_pad 120`; Cellular row opens a radio page | `v2/a_list/l1.png`, `v2/d_mod/mod_l5.png`, `v2/c_sub/cell_from_settings.png` |
| 43 | `-` vs `--` placeholders | **Not reproduced**: Weather keeps cached values with no phone; the Light gesture shows `--` | `v2/i_tour/np_weather.png`, `v2/d_mod/d_light_gesture.png` |
| 44 | Two brightness sliders | **Not fixed**: the Control centre uses 10 segments, Settings a pill slider (new #20) | `v2/i_tour/t_control.png`, `v2/b_sub/bright_0.png` |

---

## Issues (most severe first)

### P1

1. **[P1] Modules: opening the GPS detail turns the GPS off, so the list and the detail
   disagree, and backing out loses the fix.**
   - The sequence:
     - The list row says `8 IN VIEW` (`v2/d2_fix_run/list_gps.png`).
     - Tap it, and the detail opens on `0 IN VIEW` / `NO FIX YET` (`d_gps_05.png`, 0.5 s in).
     - It climbs back to `8 IN VIEW` at 3 s (`d_gps_3.png`) and gets `9` / `3D FIX` at 10 s
       (`d_gps_10.png`).
     - Back on the list, the row says `SEARCHING` (`list_after_gps.png`).
   - Cause:
     - `modules_leave()` calls `hal_gps_enable(0)` (set_modules.c:354) even when the detail is
       only pushed over it.
     - `detail_enter` switches it back on (line 692).
     - `detail_leave` switches it off again (line 708), and the list's re-enter
       (`hal_gps_enable(1)`, line 344) starts cold.
   - On hardware, every round trip costs a time-to-first-fix.
   - Fix:
     - `static void modules_leave(void) { if (!nos_leaving_covered()) hal_gps_enable(0); }`
     - In `detail_leave`, don't disable the GPS when popping back to the Modules list.
     - Simplest: a reference count, `gps_hold(+1/-1)` in svc, which the Navigate app needs anyway.

2. **[P1] Picker drag is a sawtooth, not a drum.**
   - Settings > Screen timeout, a slow upward drag. `herobox.py` hero top y per frame over
     `v2/b_sub/pick_drag_006..029.png`:
     - 194 for 7 frames (the finger is already moving)
     - then 209, i.e. +15 px down while the finger moves up
     - then 202, 193, 181
     - then 197, i.e. +16 px back at the next detent
     - then 189, 184, 181, 178 …
   - Two causes:
     - `pk_off()` is clamped to ±40 px, but `PK_STEP` is 30 drum units while the slots are about
       95 px apart. The hero goes back to centre every 30 units of finger travel, so it jumps at
       every detent.
     - `idx` and the morph trail the drum position by a frame, which gives the first reverse step.
   - Rule §1 (1:1 under the finger) is now visibly broken, which is worse than round 1's static
     hero.
   - Fix (either works):
     - (a) A real drum. Values sit `SLOT` px apart (≈ 95, the hero-to-neighbour pitch). The
       finger maps `y` 1:1 to `x * SLOT / PK_STEP`. Draw the hero and both neighbours at
       `slot_y(i) - off`, clipped to y 110–350, and morph only on settle.
     - (b) Keep the hero still and let only the rulers and neighbours move.
   - Either way, compute `idx` from the same `p->x` that is drawn this frame.

### P2

3. **[P2] Settings is the only app whose entry build starts late.**
   - At f0 the iris is already at 40 %. From f0 to f4 only `SETTINGS` is visible on a black
     disc. `DISPLAY` first shows at f5, faint (`v2/i_tour/e_settings_000..005.png`, `g_v2_entries.png`).
   - By comparison, Modules shows its hero dots at f2 and About its logo dots at f2
     (`e_modules_002.png`, `e_about_002.png`). Activity, Music, Navigate and Torch build from
     f1–f2 (`g_v2_ent2.png`).
   - Cause: `S.build_t0 = nos_t + 0.17f` (set_settings.c:452). That was round 1's "40 % of a
     0.42 s iris". The engine now reports that point itself, and the 0.17 s is added on top of it.
   - Fix: `S.build_t0 = nos_enter_t();` (keep 0 for push entries and reveals).

4. **[P2] The power sheet (working tree) shows two reds while Restart is armed.**
   - `TAP AGAIN TO RESTART` (red outline, red text, 3 red dots) sits above a still-red
     `POWER OFF` outline and label (`v2/f_power/power_restart_005.png`).
   - Settings already solves this with `other` → grey (set_settings.c:520).
   - Fix: while one pill is armed, draw the other with `C_LINE2`/`C_DIS` and cross-fade back over
     150 ms on expiry. Arming Power off greys Restart the same way.

5. **[P2] Alarms > ADD ALARM: a blank frame, then a half-built list, under the push.**
   - `v2/l_press/p_alarm_000..004.png`:
     - The pressed fill shows for 2 frames (000–001, below the ≥ 3 standard).
     - At 002 the screen is black apart from `ALARMS`.
     - At 003–004 a half-built list with the new `ONCE` alarm and `+ ADD` slides out while the
       editor pushes in.
   - The tap creates the alarm, rebuilds the list and pushes, all at once.
   - Fix:
     - Push the editor straight from the empty state, and insert the alarm on save (Cancel then
       leaves no stray row).
     - Hold the pressed fill until the push covers it (≥ 3 frames, as the other time-app presses
       do).

6. **[P2] Long toasts drop onto the content.**
   - `ui_draw_toast` (ui.c:721) moves the pill down from y 30 to y 72 until it fits. At y 30 it
     fits about 229 px, i.e. about 9 characters of copy inside `[  ]`.
   - Two cases:
     - `[ RESYNC REQUESTED ]` is a 318×42 pill at y 68–109. It covers the Compass `30` numeral
       and the north marker (`v2/i_tour/t_toast_long.png`).
     - Modules > Clock: the pill covers `CLOCK` and touches the top of the hero
       (`v2/d_mod/d_rtc_toast.png`).
   - Toasts that drop the same way:
     - con_memo.c:234/237/375: "No speaker module", "Can't play this memo", "Still recording"
     - set_modules.c:730/958/991: "Compass calibrated", "Tap again to stop", "Resync requested"
     - set_settings.c:634: "Settings reset"
     - sys_notifs.c:638/639: "Opened on phone", "Phone not connected"
   - Fix:
     - Keep copy to 9 characters or fewer: `RESYNCING`, `CALIBRATED`, `RESET`, `OPENED`,
       `NO PHONE`, `RECORDING`, `NO SPEAKER`, `CAN'T PLAY`.
     - Have the sim print a warning when a toast lands below y 40.

7. **[P2] Long values squeeze their labels.** The round-1 fix stopped values vanishing, but the
   row now packs the label against the value.
   - Settings: `Storage29 GB FREE` with zero gap when the row sits at the bottom edge
     (`v2/a_list/l6.png`).
   - Settings, no phone: `Bluetooth NOT CONNECTED` with an 18 px gap
     (`v2/c_sub/set_absent_l3.png`).
   - Modules: `Environment 26.0°C 58%` pushes its name and status dot out to x 59 / 39, against
     x 86 / 66 for the rows below (`v2/d_mod/mod_l4.png`). `Bluetooth PHONE LINKED` has a 14 px
     gap (`mod_l2.png`).
   - Fix:
     - Set a minimum gap of 24 px. When it fails, move the value to the sub-line (Modules
       already has one).
     - Shorten values: `26 °C` (drop humidity from the row), `LINKED`, `29 GB`.
     - The bottom-edge Storage row needs the same inset as the label.

8. **[P2] The same radio state has two or three names, on two different layouts**
   (`g_v2_radio.png`).
   - Wi-Fi:
     - Settings row `CONNECTED`, Settings page `[ CONNECTED ]` with a white tile and a body
       sentence (`v2/c_sub/wifi_0.png`).
     - Modules row and detail `ONLINE`, with a bare icon and `2.4 GHZ STATION / ENABLED IN
       SETTINGS` (`v2/d_mod/d_wifi.png`).
   - Bluetooth:
     - Settings row `CONNECTED`, Settings page `[ PHONE CONNECTED ]` (`v2/c_sub/bt_0.png`).
     - Modules row and detail `PHONE LINKED`, with `SIM: NO RADIO / NORDIC UART GADGETBRIDGE`
       (`v2/d_mod/d_ble.png`).
   - Fix:
     - One word per state: `CONNECTED` / `NOT CONNECTED` for both radios everywhere.
     - One radio page shared by Settings and Modules, as Storage and SD card now are (round-1
       #21). Modules adds its bus line under it.

9. **[P2] The Cellular page from Settings uses the word "OFF" as a dot-matrix hero and shows
   wiring.**
   - Settings > Cellular opens the Modules LTE detail: `OFF` in TALL dots, then
     `MODEM POWERED DOWN` and `PWRKEY ON IO18` (`v2/c_sub/cell_from_settings.png`,
     `v2/d_mod/d_lte.png`).
   - Words in dot matrix break §8, and the GPIO line is bring-up detail in a user page. The
     Cellular app shows the same state as bars plus `[ OFF ]` (`v2/i_tour/t_cell.png`).
   - Fix: hero `--` (or the bars icon from the app), with `[ OFF ]` as `ui_state`. Show
     `PWRKEY ON IO18` only when the page is opened from Modules.

10. **[P2] Lux is spelled three ways.**
    - Settings > Brightness: `320 lux now` (sentence case, `v2/b_sub/bright_0.png`).
    - Modules list: `320 LUX` (`v2/d_mod/mod_l4.png`).
    - Modules > Light: hero `320` over the label `LUX` (`v2/d_mod/d_light_gesture.png`).
    - Round 1 fixed hPa, dB, V, g and m with case-true unit tokens, but lux was missed.
    - Fix: `lx` (SI symbol) or `LUX` everywhere, through the same unit helper. The Brightness
      sub-line becomes the label `320 LX NOW`, matching the other caps labels.

11. **[P2] With modules absent, Settings greys the sliders but leaves toggles live.**
    - With `present 0`, `GPS auto` is a white, on, tappable toggle with no GPS module
      (`v2/c_sub/set_absent_l4.png`). Meanwhile Volume and Haptic strength grey their fill
      (`v2/c_sub/vol_absent.png`).
    - The `UI sounds` and `Haptics` toggle rows (set_settings.c:314–316) have no presence check
      either.
    - Fix: with the module absent, draw the toggle in `C_SURF2`/`C_DIS` (the value still saves),
      and on tap toast `NO GPS` / `NO SPEAKER` / `NO MOTOR`, as the brightness Auto row does.

12. **[P2] The Modules absent pages don't match the app absent pages** (`g_v2_hints.png`).
    - Same layout, different footer and body for the same module:
      - Cellular app: "Add the LTE strap to text without your phone", footer `LTE MODULE`
        (`v2/i_tour/a_cell.png`).
      - Modules > Cellular: "Calls and SMS without the / phone", a one-word widow, and the
        footer `EG800AK MODULE` (`v2/d_mod/d_lte_absent.png`).
    - Footers in Modules use part numbers (`QMC5883L`, `EG800AK`, `BME280`, `INMP441`), except
      Haptics, which says `HAPTIC MODULE` (`v2/d_mod/d_haptic_absent.png`).
    - Heart rate's body ends with a full stop ("…BPM and SpO2."); the others don't
      (`v2/i_tour/a_heart.png`).
    - Fix:
      - One string table per module (`body`, `footer`) shared by the app and Modules.
      - Footer: the user name (`LTE MODULE`), with the part number on the Modules wiring line
        above it.
      - No trailing full stops.
      - Body width 340, which ends the widow.

### P3

13. **[P3] The Settings arm pill cuts its width and colour.**
    - `RESET SETTINGS` jumps 260 → 320 px wide in one frame on arm (`v2/a2_arm/reset_do.png`,
      f470 → f471) and back 320 → 260 on expiry (`arm_expire.png`).
    - When Restart arms, the Reset pill cuts from red to grey in one frame
      (`v2/a2_arm/arm_restart.png`, f216).
    - Fix: spring the width (SNAPPY, ~0.16 s, the same `at` as the Restart row) and cross-fade
      the red over 150 ms.

14. **[P3] A slow drag in Settings flashes the pressed pill.**
    - A finger that starts still and then scrolls shows the SURF2 pill for 5 frames before the
      list takes the drag (`v2/a_list/slow_up.png`).
    - Fix: drop the pill as soon as the finger moves more than 8 px (the press already ends the
      tap there).

15. **[P3] The Wi-Fi and Bluetooth pages sit 10 px apart, and the Wi-Fi tile is off-white.**
    - `[ CONNECTED ]` is at y 294 and `[ PHONE CONNECTED ]` at y 284 (`g_v2_radio.png`).
    - The Wi-Fi tile settles at `#FFFAFF`, not `#FFFFFF` (`v2/c_sub/wifi_tap2_011..015.png`).
      A green-channel rounding in the mix.
    - Fix: one state y for both. End the tile mix with `on >= 1 ? C_WHITE : mix`.

16. **[P3] The Storage row opens a page titled `SD CARD`.**
    - (`v2/c_sub/storage_0.png`.) The shared page is the right fix, but the title should follow
      the row.
    - Fix: pass the title (`STORAGE`) when it is opened from Settings, or rename the row
      `SD card`.

17. **[P3] The Motion hero crowds its label.**
    - The `2°` hero ends 7 px above `TILT` (`v2/d_mod/d_imu.png`). The other details leave
      about 20 px (`v2/d_mod/d_sd.png`: `29` → `GB FREE OF 32`).
    - Fix: move the label to hero bottom + 20, the same rule as the other detail pages.

18. **[P3] About: no title, tagline before logo, a glint that reads as a dropout, and a word in
    dot matrix.**
    - About is the only pushed screen with no `ui_title`.
    - `NOTHING-INSPIRED WATCH OS` is drawn inside the logo row at full strength. It is complete
      at f2 while only 8 logo dots exist (`v2/i_tour/e_about_002.png`, `e_about_004..007.png`).
    - The 5 s glint dims dots to C_SEC and shrinks them by 28 %, so a band of smaller grey dots
      crosses the logo. Against white dots it reads as dead pixels, not light (`z_about_logo.png`).
    - The rail value `4.00 V` sets the unit `V` in dot matrix (`z_about_uptime.png`).
    - Fix:
      - `ABOUT` title, or let the logo count as the title and say so in §8.
      - Fade the tagline in with the first info row (`k` = row 1's `bk`).
      - Make the glint grow the dots +15 % at full white, or drop it.
      - Draw `4.00` in dots and `V` as a label, as Modules > Battery does (`VOLTS ON THE RAIL`).

19. **[P3] The Timer ring and Stopwatch pairs are off the pair standard.**
    - Timer ring: `STOP` / `+1 MIN` are 124×88 centred at y 316 (`v2/i_tour/t_timer_ring.png`).
    - Stopwatch: `LAP` / `START` are 140×88 at y 354 (`t_stopwatch.png`).
    - §8 says a pair is 140 at y 372.
    - Fix: Stopwatch to y 372. The Timer ring keeps its height, since the ring owns the bottom,
      but goes to 140 wide.

20. **[P3] The two brightness controls still differ** (round-1 #44).
    - The Control centre has 10 square segments with a small sun; Settings has a pill slider
      with the sun inside the fill (`v2/i_tour/t_control.png` vs `v2/b_sub/bright_0.png`).
    - Fix: share the pill slider (235×50) in the Control centre, or move Settings to segments.
      One look per control.

21. **[P3] The unread dot changes colour and size on a pager swipe.**
    - The face draws a 10 px white dot at (233, 24); every widget page draws an 8 px red dot at
      the same spot (`v2/i_tour/t_face_unread.png`, `t_widget1_unread.png`).
    - Swiping from the face to a widget turns the dot red and shrinks it.
    - Fix: one dot. Red at 8 px on both (the Dot face's colon is the face's own red, so a face
      that uses red shows white), or white on both.

22. **[P3] AOD entry is a 1-frame cut.**
    - Face to AOD in one frame (`v2/h_aod/aod_enter.png`). The wake grow on the way back is
      lovely (`aod_wake.png`), but the way in breaks §8 ("never a hard cut").
    - Fix: run the existing ink-dim backdrop ramp over 150–200 ms before `nos_aod = 1`, or
      cross-dissolve to the AOD face in 4 frames.

23. **[P3] Weather says `STRAP 26°`.**
    - Users don't know the sensor lives on a strap (`v2/i_tour/np_weather.png`, `t_weather.png`).
    - Fix: `ON WRIST 26°` or `HERE 26°`.

24. **[P3] Notification detail mixes a centred header with a left-aligned body.**
    - The icon + `MESSAGES 2M AGO` header is centred (x 105–360), while `Maya` and the body
      start at x 68 (`v2/m_launch/notif_detail.png`). The list cards left-align everything.
    - Fix: left-align the header at x 68, as on the card.

---

## Consistency audit (round 2)

**Titles (`ui_title`, cap band y 58–69, C_SEC).**
- On the standard:
  - Settings and all sub-pages, Modules and all details
  - Launcher `APPS`, Alarm ring, Timer ring (now C_SEC)
  - Heart, Weather, Timer and Music widgets, Music app, Stopwatch, Alarms, Barometer, Navigate,
    Cellular, Find phone, Find watch, power sheet `BATTERY`
- Touch test: `TOUCH` at y about 114, inside the ring layout. This is intended.
- About: none (#18).
- Exempt: faces, Compass, Torch, Activity app and widget, sheets.

**Action buttons (UI_BTN_H 88).**
- On the standard:
  - Single, 200×88 at y 372: Alarms `ADD ALARM`, Modules `RESYNC` / `POWER ON` / `POWER OFF`,
    Cellular from Settings.
  - Pair 140×88 at y 372: notification `OPEN` / `DISMISS`.
  - Power sheet 88 tall: armed pill at y 272, `POWER OFF` at y 372.
  - Alarm ring `SNOOZE` 200×88.
- Off the standard: Stopwatch pair at y 354, Timer ring pair 124 wide at y 316 (#19).

**Pressed states (≥ 3 frames).**
- Meet it:
  - Settings rows: 4 frames (`v2/a_list/quicktap_bright.png`).
  - Power sheet: 0.09 s hold then a 0.10 s release (sys_power.c).
  - Time apps' held presses: Timer, Stopwatch, World clock (`v2/l_press/`).
  - Modules rows, Control tiles, Find, Cellular.
- Misses it: Alarms `ADD ALARM` at 2 frames (#5). The arm pills use arming as their feedback,
  which is fine.

**Absent states.**
- One `ui_absent` layout everywhere: icon at 170, head at 270, body at 306, footer at 398.
- The words still differ between the apps and Modules (#12).
- Phone absent is unified.

**Units.**
- Case-true everywhere: `V`, `dB`, `hPa`, `m`, `g`, `°/s`, `km`, `km/h`, `°C` / `CELSIUS`.
- Exceptions: lux (#10), and About's dot-matrix `V` (#18).

**Toasts.**
- Good:
  - White pill, h 42, bracketed caps, drops in at full colour.
  - Queues behind a banner (`v2/i_tour/t_banner_toast_wait.png`, then `t_after_banner.png`).
- Long copy lands on the content (#6).

**Entry builds.**
- All apps start their build within f0–f2 of the 40 % iris point, with no scale-ups, no grey
  fade-ups and no resting zero.
- Exception: Settings at f5 (#3).

**Red count.**
- One red per screen on every screen captured in `i_tour` (`measure.py`).
- Exception: the working-tree power sheet while Restart is armed (#4).

**Page dots.** y 440 on every pager.

---

## What is already excellent (don't break it)

- **Performance and correctness.**
  - 17,838 damage-checked frames are pixel-identical to full repaints.
  - 0 late frames across 14 scripts, including the whole-OS tour (3,738 frames) and the
    Modules sweep (5,603 frames).
- **The new iris.**
  - From the launcher it grows from the tapped icon as a seed, with a 1 px hairline rim and a
    hard edge. It looks like the same object opening, not a wipe (`v2/m_launch/launch_app.png`).
  - The button close is a clean 0.22 s exit, 6 frames with the ink dim ramping, and no tail
    (`v2/f_power/power_close_btn.png`).
- **Banner manners.** A banner yields the moment a sheet starts (`v2/i_tour/t_banner_sheet.png`),
  and a toast waits for it.
- **The arm-then-tap-again pattern.**
  - Settings Restart/Reset, the power sheet and Alarm DELETE now share it: red outline,
    `TAP AGAIN TO …`, and three red dots that count the seconds down.
  - Settings greys the other pill while one is armed, which is the model for #4.
- **One OS-wide absent state.**
  - Compass, Barometer, Navigate, Heart, Cellular and Recorder share the icon / head / body /
    footer stack at fixed y.
  - "Phone not connected" is the same on Find, Music and Bluetooth.
  - The heart widget shows `--` with no module.
- **Settings detail work.**
  - Rows hold x 62 through the middle band, and flip rows have a two-way dot glyph.
  - Values end at x 384.
  - Picker taps commit on the tap.
  - Slider taps snap to the 10 % detents.
  - The Wi-Fi tile release is monotonic.
- **Modules.**
  - The `16/16` summary agrees with Settings.
  - LTE reads `--` / `SEARCHING` until it registers.
  - Units keep their case.
  - Heroes share one pitch.
  - Every row has a chevron.
  - The Touch test has a title.
- **About.** It splits the board name cleanly, sets static values in grotesk and morphs only the
  live numbers.
- **Boot and AOD wake.** The particle logo boot (`v2/g_boot/boot.png`) and the AOD wake grow
  (`v2/h_aod/aod_wake.png`) are the most "Nothing" moments in the OS.
- **Red discipline.**
  - The Barometer's single "now" dot and the Activity widget's white overflow lap.
  - The white bell on icon-less banners.
  - The OS keeps to one red everywhere it was measured.
