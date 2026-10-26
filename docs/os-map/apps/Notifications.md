---
title: "Notifications"
id: sys-notifications
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Notifications

Source: `nos/apps/sys_notifs.c` (1064 lines). Two screens: `scr_notifs` (the list, `nos/apps/sys_notifs.c:855`) and `scr_notif_detail` (`nos/apps/sys_notifs.c:1063`). The data store and the phone protocol live in `nos/svc.c` and `nos/gb.c`. The queue and transports are in `firmware/main/hal_esp32.c`. Read-only source review for the 1.43-B target. Nothing was built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Where a comment and the code disagree, the code is cited.

## Summary

Notifications is the NOS feed of alerts. An upward drag from the face pulls up a sheet (`TX_RISE`, `nos/apps/sys_home.c:1088-1091`) that lists up to 20 cards, newest first. A tap opens a detail screen with OPEN and DISMISS. A left swipe dismisses one card, and CLEAR ALL empties the list.

The screen owns no data. It reads the RAM store kept by the service layer (`svc_notif_*`, `nos/svc.h:32-52`, `nos/svc.c:52-58`). That store is filled from two inputs, plus simulator seeds:
- Gadgetbridge over NUS (`notify`, `notify~`, `notify-`, `nos/svc.c:1152-1154`).
- The companion queue, which takes BLE F2, Wi-Fi `/notify` and the USB console `notify` (`firmware/main/hal_esp32.c:857-860`, `:1771-1797`, `firmware/main/main.c:755`).
- Simulator seeds. They are compiled out on the device (`nos/svc.c:1264`, `:1290`).

Each new item also posts the top banner and an alert buzz (`nos/svc.c:230-232`, `nos/nos.c:509-521`).

Status is **partial**, from source only. The list, swipe, CLEAR ALL, detail, phone DISMISS and OPEN requests, the banner and the host tests are in place. Four things are missing or unverified on a bare B. A new item does not wake a dark panel. The buzz and chime depend on a strap that is not verified for B. Nothing survives a reboot. No delivery acknowledgement comes back from the phone.

Doc and comment drift (code is authoritative):
- `nos/svc.h:47` says dismiss "syncs DISMISS to the phone". It does so only for items that came from Gadgetbridge, and only while the link is up (`nos/svc.c:245-248`, `:150-156`).
- `nos/svc.h:51-52` says OPEN asks the phone. Only phone-origin items send that request (`nos/svc.c:269-272`). The detail draws OPEN for every item (`nos/apps/sys_notifs.c:984`).
- `nos/apps/sys_notifs.c:1043-1046` says the list closes the card's slot "while the detail slides away". That holds only when the detail was opened from the list. A banner tap pushes the detail with no list beneath it (`nos/svc.c:199-203`, `nos/apps/sys_notifs.c:915`).
- `nos/apps/sys_notifs.c:14-16` says the detail "tracks its notification by id". It does, until the item is removed. After that it keeps the last copy and its buttons do nothing (Gaps 4).
- `nos/apps/sys_notifs.c:9-12` says everything is marked read on entry. That is true. Items that arrive while the list is open stay unread until the next entry (`nos/svc.c:206-233`, `nos/apps/sys_notifs.c:292`).

## Features

Sheet and list (`scr_notifs`, `TX_RISE`):
- **Entry.** An upward drag on the face at page 0, within 40 px of centre, pulls the sheet (`nos/apps/sys_home.c:1088-1091`). Each entry marks every item read (`nos/apps/sys_notifs.c:276`, `:292`).
- **Cards.** One flat card per item, 344 px wide, radius 40 (`nos/apps/sys_notifs.c:31-33`, `:386`). It shows the app icon (`dot_icon`, default bell), the app name or "Phone" (`:389`), an age label, the title (ellipsis fit, `:395`) and two body lines (`:396-397`). The card height follows the wrapped body (`:116-123`).
- **Age label.** `NOW` under 60 s, then `nM AGO`, `nH AGO`, `nD AGO`. The label is blank when the clock is unset (`:49-57`, `:391-392`).
- **Header.** The count in 5x7 dot numerals, which morph when the count changes, and NOTIFICATION or NOTIFICATIONS (`:415-437`).
- **Scrolling.** Momentum scroll with rubber-band bounds (`:197-203`, `:765-833`). A downward drag at the top hands the gesture to the engine, which closes the sheet with the finger (`:789`, `:804-809`).
- **Swipe to dismiss.** The card follows the finger. Past -110 px, or on a flick faster than -600 px/s, it dismisses. Otherwise it springs back (`:797`, `:816-827`). A disc with a cross, white when armed, shows the action (`:636-650`). The dismissal commits when the card is half off the glass (`:333`, `:249-266`).
- **CLEAR ALL.** A button after the last card. The visible cards leave to the left, top first, 49 px over 7 frames each, two frames apart. The button leaves last. The store is cleared once the animation ends (`:306-315`, `:665-686`, `:737-758`, `:338-346`). Input is ignored while it runs (`:763`).
- **Empty states.** "No notifications" with "ALL CAUGHT UP" when the phone is connected. "Phone not connected" with "Pair in Gadgetbridge as Bangle.js NOS" when it is not (`:401-411`).

Detail (`scr_notif_detail`, `TX_PUSH`):
- **Open.** A tap on a card (`:834-844`) or a banner tap (`nos/svc.c:199-203`) pushes the detail (`:904-916`).
- **Content.** The header carries the icon, app name and age (`:963-988`). The title wraps to three lines, the body to twelve. The view scrolls (`:890-902`, `:963-988`).
- **OPEN.** Sends OPEN to the phone for phone-origin items (`:1037-1040`, `nos/svc.c:269-274`). The detail stays on screen.
- **DISMISS.** Closes the card's slot, dismisses the item and pops back (`:1041-1051`).

Banner (posted for each new item, drawn by the engine):
- Shown for 4 s, then hidden (`nos/nos.c:493`, `:2222`). A tap or a pull-down opens the detail. A flick up dismisses it (`nos/nos.c:905-920`).
- Silent under Do Not Disturb (`nos/svc.c:230`, `nos/nos.c:519`).

Alert:
- Haptic BUZZ and SFX_NOTIFY on each new banner (`nos/nos.c:519`, `:557-573`). Gated by the haptics and sound prefs, and by DND.

Phone sync:
- `notify` adds an item and posts a banner. `notify~` updates title and body in place with no banner (`nos/svc.c:1023-1032`). `notify-` removes the item (`nos/svc.c:1154`).

Blur for glides:
- Each card has a blurred copy, baked in idle frames and cached in PSRAM (`nos/apps/sys_notifs.c:439-610`). The list is fast to scroll because of this, not because the data is special.

Not present: wake on notification, reply, quick actions, snooze, per-app mute, grouping, unread marks per card, history across reboot. See Gaps.

## Reads

Service getters (`nos/svc.h`, implemented in `nos/svc.c:236-274`):
- `svc_notif_count()`: list length, in `sync_cards` (`nos/apps/sys_notifs.c:216-218`), `n_enter` (`:271`, `:279`), `dismiss_flying` (`:262`), `prewarm` (`:495`).
- `svc_notif_get(i)`: each row (`:166`, `:172`), `band_tick` (`:565`), `n_draw` (`:624`), `n_input` (`:774`), `sys_notif_open` (`:906`), the detail copy (`:914`, `:947`).
- `svc_notif_revision()`: the layout and sync key (`:178`, `:215`, `:940-941`).
- `svc_sys()->phone_connected` and `hal_phone_connected()`: `N.phone` (`:348`) and `D.phone` (`:950`). `N.phone` only changes the empty-state copy (`:404-405`). `D.phone` decides the OPEN branch (`:1038-1040`).
- Not read here: `svc_notif_unread()`. The face reads it (`nos/apps/sys_home.c:476`).

HAL and transport calls:
- `hal_epoch_seconds()` for the age (`:347`, `:513`, `:949`). It returns 0 before 2024 (`firmware/main/hal_esp32.c:148-151`), which blanks the label.
- `hal_big_alloc` and `hal_big_free` for the table and copies (`:180`, `:254`, `:460`, `:913`). These map to SPIRAM (`firmware/main/hal_esp32.c:1595-1597`).
- `hal_phone_connected()` (`:348`, `:950`). Its definition is at `firmware/main/hal_esp32.c:1159-1161`.
- `hal_haptic` and `hal_sfx` are not called directly. They run through `nos_feedback` (`nos/nos.c:557-573`).

Engine and shell (timing, navigation and drawing, not data): `nos_t`, `nos_touch_travel()`, `nos_enter_t()`, `nos_entered_by_finger()`, `nos_revealed()`, `nos_top()`, `nos_transitioning()`, `nos_contains()`, `nos_mem_available()` (`:501`, under `NOS_PSRAM_TRACKING`), `nos_subview()`, `nos_ink_dim()`, `nos_mblur_v_fallback()`, `nos_blur_note()`, `nos_motion_probe()` (sim audit only).

Diagnostics: `diag_log_name_id("NOTIFICATIONS")` and `diag_log_name_id("NOTIFICATION")` (`:740`, `:822`, `:910`, `:1034`) produce name hashes for the action records.

Prefs: the screen itself reads none. The notification path reads `nos_prefs.dnd` (`nos/svc.c:230`, `nos/nos.c:519`). Feedback reads `nos_prefs.haptics` and `nos_prefs.sound` (`nos/nos.c:570-572`).

## Writes

Notification store, through the service API only:
- `svc_notif_mark_all_read()` on entry (`nos/apps/sys_notifs.c:276`, `:292`). Sets `read` and bumps the revision when something changed (`nos/svc.c:259-264`).
- `svc_notif_dismiss(i)` on a swipe commit (`:258`) and on the detail DISMISS (`:1047`). Sends DISMISS for phone-origin items (`nos/svc.c:248`), then removes the card and bumps the revision (`nos/svc.c:191-198`, `:245-250`).
- `svc_notif_clear()` after CLEAR ALL (`:340`). Sends one DISMISS per phone-origin item, then empties the store (`nos/svc.c:251-258`).
- `svc_notif_open_on_phone(D.idx)` on the detail OPEN (`:1039`). Sends OPEN for phone-origin items and marks the item read (`nos/svc.c:269-274`).

Outbound phone lines: `txf` sends DISMISS and OPEN lines, and does nothing when the link is down (`nos/svc.c:150-156`). The bytes go to `hal_phone_write_line` (`firmware/main/hal_esp32.c:1190-1222`). Nothing is acknowledged. The OPEN diagnostic is set to PENDING and never resolved (`nos/apps/sys_notifs.c:1038`).

Diagnostics (every swipe, clear, open and rejection): `diag_action_request`, `diag_action_context` and `diag_action_result` in domain NOTIFICATION (`nos/apps/sys_notifs.c:739-741`, `:821-822`, `:908-910`, `:1033-1036`, `:1048`). The records go to the bounded diag ring (`nos/diag_log.c:229-236`), which writes to `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`). They are developer records, not user data.

PSRAM (retained, sized once):
- `NL`, the card-height and y table, 244 bytes (`nos/apps/sys_notifs.c:159`, `:180`).
- `s_fly`, the dismissed-card copy (`:160`, `:254`).
- `s_dn`, the detail copy (`:873`, `:913`).
- `s_kb`, the blur arena of 896 KiB (`:455`, `:505`), with bakes at `:544` and `:548`.

Engine and shell: pushes the detail (`:915`), pops it (`:1050`), and closes the sheet through `nos_sheet_take` (`:808`). Feedback calls (`:798`, `:826`, `:841-842`, `:1039-1042`). Toasts "Opened" and "No phone" (`:1039-1040`).

Not written: `nos_prefs`, NVS, and any SD settings file. The notification store is never saved.

## Background behaviour

Who runs it: there is no notification task. Ingest runs in `svc_tick()` (`nos/svc.c:1303-1322`) on the UI loop. The loop calls `nos_bg_tick` each frame (`nos/nos.c:2341`), which is `services_tick` (`nos/apps/registry.c:62-71`). The UI runs in `app_main` on CPU 0 (`firmware/sdkconfig:1706-1707`). The main task priority is not overridden in `firmware/sdkconfig`.

Cadence:
- 40 Hz while the panel is on (`nos/render_config.h:8`, `NOS_FPS 40`). Each frame runs `svc_tick`.
- 1 Hz with the panel off (`firmware/main/nos_main.c:2091-2094`), which runs `nos_bg_tick(1.0f)` once per second and then `continue`s.
- 1 Hz in AOD (`firmware/main/nos_main.c:2111-2117`), where `nos_frame_run` is called once per second.

Parsing:
- `phone_tick` reads at most 8 NUS lines per call (`nos/svc.c:49`, `:1236-1241`).
- `companion_tick` takes at most 4 queue items per call (`nos/svc.c:1256-1259`).

BLE:
- The NimBLE host task is pinned to core 0 at `configMAX_PRIORITIES-4` with a 4096-byte stack (`firmware/main/hal_esp32.c:1531-1533`, `firmware/sdkconfig:709-712`). It fills an 8192-byte PSRAM ring with lines up to 2048 bytes (`firmware/main/hal_esp32.c:1082-1083`).

HTTP and queue:
- The `/notify` handler runs in the esp_http_server task. That task uses the library defaults, except a 6144-byte stack and PSRAM task caps (`firmware/main/hal_esp32.c:1800-1806`). The server starts when the station gets an IP (`firmware/main/hal_esp32.c:1979`) or in setup AP mode (`:1929`).
- The companion queue is a FreeRTOS queue of 8 items (`firmware/main/hal_esp32.c:2324`).

Screen off:
- The store still fills, at 1 Hz. Every new item calls `nos_banner` (`nos/svc.c:230-232`). That calls `nos_feedback(FB_NOTIFY)` (`nos/nos.c:519`), which runs whatever the power state is (`nos/nos.c:557-572`). So the buzz is not gated by the panel.
- No wake happens. Nothing draws. The banner is set but not drawn. Wake sources are touch, button, raise and knock only (`nos/power_policy.h:16-17`, `firmware/main/nos_main.c:2061`).
- The panel timeout runs only in the PWR_ON state (`firmware/main/nos_main.c:2079-2085`). A banner calls `nos_keep_awake()` (`nos/nos.c:520`, `:585`), which restarts that timer while the panel is on.

Screen on, another app on top:
- Only the top screen ticks (`nos/nos.c:2104-2108`). The list's own sync and layout do not run while the detail or another app is on top (`nos/apps/sys_notifs.c:216-236`, `:347-351`). The store is still current. The list catches up in `n_enter` (`:279-283`).

Idle prewarm:
- `sys_notifs_prewarm()` bakes one missing card blur per frame. It runs from the home pager's idle frames (`nos/apps/sys_home.c:645-647`, idle block `:636-648`) and from the list's idle band tick (`nos/apps/sys_notifs.c:568`).
- It creates the 896 KiB arena only when at least 1408 KiB of PSRAM is free (`nos/apps/sys_notifs.c:499-505`). The budget is recorded in `docs/PSRAM_BUDGET.md:105`.
- Under memory pressure, `sys_notifs_blur_release()` is called from `nos/psram.c:140-155` and from `nos/apps/set_settings.c:1234-1235`.

## Persistence

Nothing about notifications is persisted.
- The store is the `svc_big` block in PSRAM (`nos/svc.c:52-58`). `svc_init` clears it at boot (`nos/svc.c:1280-1282`). A reboot or a power cycle empties the feed.
- The service header lists only the persisted keys, `nosalarm` and `nosact` (`nos/svc.c:7-8`). The tree has no notification NVS key and no SD path for notifications.
- Read and unread flags are RAM only (`nos/svc.c:206-233`).

The one related persisted value is `nos_prefs.dnd` (`nos/prefs.h:19`). The prefs layer checkpoints it to SD through `hal_store` key `nosprefs`, with an NVS fallback (`nos/prefs.h:1-2`). The Control Centre toggle writes it (`nos/apps/sys_control.c:230`).

Developer diagnostics: the action records are bounded (8 files, `nos/diag_log.c:331`) and go to `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`). They are not user data.

Simulator only: `seed_sim` adds four fake items (`nos/svc.c:1264-1272`). It is compiled out on the device (`#ifndef ESP_PLATFORM`, `:1264`, `:1290`).

## Navigation

Opens from:
- The face: an upward drag at page 0 pulls the sheet (`nos/apps/sys_home.c:1088-1091`).
- The banner: a tap calls `banner_open` (`nos/svc.c:199-203`) and then `sys_notif_open` (`nos/apps/sys_notifs.c:904-916`), which pushes the detail. The banner tap path is `nos/nos.c:914-920`.
- Simulator: `nos/sim/nos_sim.c:70-71`, `:1878`. The path is `sim/nos_sim.c`.

Opens:
- The detail, from a tap on a card (`nos/apps/sys_notifs.c:842`). Pushed with `TX_PUSH` (`:915`).

Exits:
- OPEN keeps the detail on screen (`:1037-1040`).
- DISMISS pops the detail with `nos_pop` (`:1041-1051`, `nos/nos.c:382`). It returns to the screen below, which is the list if it was opened from the list, and the face if it was opened from a banner.
- Back gestures: the list is a sheet, so the system back gesture is skipped (`nos/nos.c:846-866`). It has `SCR_HDRAG` (`nos/apps/sys_notifs.c:855`, flag at `nos/nos.h:60-61`), so horizontal drags are its own and back only starts from the left edge. The detail has flags 0 (`:1063`), so a rightward swipe backs out from anywhere.
- The side button on a pushed screen goes home (`nos/nos.c:952`).
- The list closes with a downward drag at the top (`nos/apps/sys_notifs.c:789`, `:808`).

Related system notes: [[Services hub]], [[Connectivity and voice]], [[Shell and faces]], [[Engine and scheduling]], [[Power and battery]], [[Storage and persistence]], [[Hardware and drivers]].

## Gaps

1. **High. A new item does not wake the panel.** Evidence: `nos/power_policy.h:16-17` (wake = touch, button, raise, knock), `firmware/main/nos_main.c:2061` (the wake call takes only these four; a top screen flagged `SCR_KEEPAWAKE` also wakes, `:2096`, and no notification screen sets that flag, `nos/apps/sys_notifs.c:855`, `:1063`), no notification path in `nos/svc.c:206-233` or `nos/nos.c:509-521` that wakes the panel. `docs/os-map/systems/Power and battery.md:15` agrees. User impact: a watch with the panel dark shows nothing when a message arrives. The banner lives 4 s (`nos/nos.c:493`, `:2222`), so it is seen only by chance.

2. **High. The arrival buzz and chime are unverified on a bare B, and probably silent.** Evidence: the motor is on GPIO16 of the external strap (`firmware/main/ext/ext_priv.h:33`), and the speaker pins are on the strap too (`firmware/main/ext/ext_priv.h:34-38`). The haptic driver reports itself present once LEDC config succeeds, with no probe (`firmware/main/ext/ext_haptic.c:107-109`). `hal_haptic` (`:114-124`) and `hal_sfx` (`firmware/main/ext/ext_audio.c:358-365`) act on that. `CLAUDE.md` lists built-in audio as absent on B and the strap mapping as unverified. User impact: with no strap, arrivals give no buzz and no chime. The screen is the only alert, and it is dark (Gap 1).

3. **Medium. OPEN misreports for items that cannot be opened on the phone.** Evidence: OPEN is always drawn (`nos/apps/sys_notifs.c:984`). It shows "Opened" whenever `D.phone` is true, even when the item is local (`:1037-1040`). `svc_notif_open_on_phone` then returns early because `B->phone[i]` is 0 (`nos/svc.c:269-272`). Companion, USB, Wi-Fi and F2 items all have phone 0 (`nos/svc.c:1256-1260`). User impact: OPEN says "Opened" and does nothing. With the link down, the same button says "No phone" for an item the phone could never open.

4. **Medium. The detail goes dead when the phone removes its item.** Evidence: `notify-` removes the card (`nos/svc.c:1154`, `:191-198`). The detail keeps the last copy (`nos/apps/sys_notifs.c:947`) and still draws OPEN and DISMISS (`:984-985`). A tap sets `D.idx` to -1 and then returns at `:1036` with a STALE diagnostic only. It gives no toast, no feedback and no pop. User impact: the screen stays on a card whose buttons do nothing, with no message, until the user swipes back or presses the side button.

5. **Medium. Dismissals and clears are lost when the link is down, and nothing reports it.** Evidence: `txf` returns silently when the link is down (`nos/svc.c:150-156`). `svc_notif_dismiss` removes the card anyway (`:245-250`). CLEAR ALL sends one DISMISS per phone item with no retry or result (`:251-258`). `hal_phone_write_line` drops a line after 20 ENOMEM retries (`firmware/main/hal_esp32.c:1211`). OPEN never gets a result (`nos/apps/sys_notifs.c:1038`). The offline list also shows no offline marker while items exist; `N.phone` only drives the empty state (`:404-405`). User impact: the card vanishes on the watch and stays on the phone, and the user is not told.

6. **Medium. The store holds 20 RAM-only items, silently overwritten, with no history.** Evidence: `nos/svc.h:32`. When full, the oldest item is dropped silently (`nos/svc.c:214`). The store is PSRAM with no persistence (`nos/svc.c:52-58`, `:1280-1282`, `:8`). User impact: a busy phone pushes older items out with no count and no "more" marker. A reboot empties the feed, and there is no history screen.

7. **Medium. No per-app control and no schedule for Do Not Disturb.** Evidence: DND is one global bit (`nos/prefs.h:19`). It is toggled from the Control Centre (`nos/apps/sys_control.c:230`) and checked once in `notif_post` and `nos_banner` (`nos/svc.c:230`, `nos/nos.c:519`). No per-app filter exists in `nos/svc.c`. User impact: one chatty app fills the 20 slots and buzzes every time. DND has to be switched on by hand.

8. **Medium. No reply, no quick actions, no snooze.** Evidence: the detail has two buttons, OPEN and DISMISS (`nos/apps/sys_notifs.c:984-985`). The protocol comment says quick reply is unsupported (`nos/svc.h:51`). No reply path exists in `nos/svc.c` or `nos/gb.c`. User impact: a user cannot answer from the watch or defer an item.

9. **Medium (security). Unauthenticated `/notify` on Wi-Fi.** Evidence: `web_notify` has no auth check (`firmware/main/hal_esp32.c:1771-1797`). The route is registered next to `/cmd` and `/media` (`:1817`). The server starts on station IP (`:1979`) and in setup AP mode (`:1929`). `docs/os-map/systems/Connectivity and voice.md:20` and `:212` flag the same issue. User impact: any client on the LAN can post cards, which buzz the watch and fill the store.

10. **Low. A repeated `notify` with a known id re-alerts.** Evidence: `notif_post` keeps the instance and moves the card to the top when the id is known (`nos/svc.c:206-214`). It still posts the banner and feedback (`:230-232`), with no check on the existing index. User impact: a phone that re-sends an item buzzes again.

11. **Low. Stale list geometry while the detail is open.** Evidence: only the top screen ticks (`nos/nos.c:2104-2108`). The list's sync and layout run only in its own tick (`nos/apps/sys_notifs.c:216-236`, `:347`). DISMISS sizes the closing gap from `card_hi(D.idx)` (`:1046`, `:162`) and skips it when `D.idx >= N.n` (`:1046`). User impact: if a card arrived while the detail was open, the closing animation may use the wrong height or be skipped. This is cosmetic.

12. **Low. Intake is 1 Hz with the panel off.** Evidence: `firmware/main/nos_main.c:2091-2094`. `phone_tick` takes 8 lines per call (`nos/svc.c:49`, `:1236`) and `companion_tick` takes 4 (`:1256-1259`). The NUS ring holds 8192 bytes and drops lines over 2048 (`firmware/main/hal_esp32.c:1082-1083`). User impact: a burst of more than 8 lines per second queues, and an alert can lag by up to a second with the panel off.

13. **Low. Unread state is not shown on cards.** Evidence: `draw_card` has no read indicator (`nos/apps/sys_notifs.c:373-398`). Every item is marked read on entry (`:276`, `:292`). Arrivals while the list is open are stored with `read` 0 (`nos/svc.c:206-233`), and the face counts them (`nos/apps/sys_home.c:476`) until the next entry. User impact: the list never says what is new.

14. **Low. CLEAR ALL is immediate and mirrors to the phone.** Evidence: no confirmation (`nos/apps/sys_notifs.c:737-758`). The store clears after the animation (`:338-346`). Each phone item gets a DISMISS (`nos/svc.c:251-258`). User impact: a mis-tap removes everything from the phone too, and cannot be undone.

15. **Low. Text is folded to ASCII and fixed-length.** Evidence: text goes through `gb_ascii` (`nos/svc.c:108`, `nos/gb.c:524-529`). Accents are folded through a Latin table, and emoji and pictographs are dropped (`nos/gb.c:460-484`). Title is 40 bytes and body is 160 bytes, truncated at copy (`nos/svc.h:36-40`). User impact: emoji vanish and non-Latin text is lost.

16. **Low. App icons come from a substring match.** Evidence: `icon_for` checks app names for fragments, and the first match wins (`nos/svc.c:173-189`). User impact: an app whose name contains "call" or "chat" takes that icon, whatever it is.

17. **Low. No absolute time, and no age without a clock.** Evidence: the label is relative only (`nos/apps/sys_notifs.c:49-57`). `hal_epoch_seconds` returns 0 before 2024 (`firmware/main/hal_esp32.c:148-151`), and the label is then blank. User impact: a card from yesterday reads "1D AGO" with no time of day.

18. **Low. Dead API on the device.** Evidence: `svc_notif_add_local` (`nos/svc.c:265-268`) has callers only in the simulator (`sim/nos_sim.c:1013`) and in tests. User impact: none. It is code to review.

19. **Missing features a user would expect.** Each is a concrete gap, not a placeholder: wake on notification (Gap 1); audible or tactile alerts on bare B (Gap 2); reply and quick actions (Gap 8); per-app mute and grouping (Gap 7); history across reboot (Gap 6); a phone-missing marker on the list (Gap 5); a per-card unread mark (Gap 13); a scheduled DND (Gap 7). No placeholder or fake data reaches the device. The seeds are simulator-only (`nos/svc.c:1264-1272`).

Related: [[Services hub]], [[Connectivity and voice]], [[Shell and faces]], [[Engine and scheduling]], [[Power and battery]], [[Storage and persistence]], [[Hardware and drivers]].
