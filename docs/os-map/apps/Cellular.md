---
title: "Cellular"
id: app-cellular
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Cellular

Source: `nos/apps/con_cell.c` (718 lines; the file is not valid UTF-8, see Gaps 21). Driver: `firmware/main/ext/ext_lte.c` (603 lines). Scheduler: `firmware/main/ext/ext_sys.c`. Contract: `nos/hal_ext.h`. Read-only source review of the 1.43-B build. Nothing was built, flashed or run on a board, so every timing below is what the code schedules, not a measurement. Evidence is `repo-relative/path:line`.

## Summary

Cellular is the NOS page for the optional Quectel EG800AK LTE strap module. The module sits on the back header and talks on UART0 (TX GPIO43, RX GPIO44, 115200 8N1) with PWRKEY on GPIO18 (`firmware/main/ext/ext_priv.h:41-43`). The page shows radio state, signal, operator and registration. It has a RADIO on/off control, and an SMS button that opens a Messages list of the SIM's texts with a reader and five canned replies.

The page owns no modem logic. It reads a cached `hal_lte` status and a cached `hal_sms` list, and it sends intents: enable the radio, queue a reply, mark a message read. All AT traffic runs in the `ext` task inside `ext_lte.c` ([[Hardware and drivers]], [[Connectivity and voice]]).

Hardware it needs: the strap module, a nano SIM in it, and network coverage. The module is probed once per boot (`firmware/main/ext/ext_lte.c:440-452`). No I2C, SD or data path is used by this app.

Key finding: the modem stays powered only while a getter renews an 8 s idle timer (`firmware/main/ext/ext_lte.c:299-306`, `nos/hal_ext.h:25-27`). Only this page, the Messages page, and a few settings and diagnostics calls renew it. The boot probe reads the inbox once and powers off. So with the screen off the watch does not receive texts and cannot alert. With RADIO off the Messages list is a boot-time snapshot that never refreshes. The page copy "Add the LTE strap to text without your phone" (`nos/apps/con_cell.c:310`) overstates what the code does.

Status: partial. Source implements the signal readout, RADIO toggle, SIM and registration states, inbox while the page is open, and five canned replies. Background reception, alerts, and several data-handling paths are missing or wrong (see Gaps).

## Features

Cellular page (`scr_cell`, `nos/apps/con_cell.c:419`):

- Signal: four rising columns lit from `L.l.bars` (`nos/apps/con_cell.c:259-276`). Bars come from CSQ: 4 at -73 dBm and up, 3 at -83, 2 at -93, 1 at -105 (`firmware/main/ext/ext_lte.c:130-133`). RSSI is `-113 + 2 x CSQ` dBm, or 0 when unknown (`ext_lte.c:194-197`). The page shows "--" when the value is 0 (`nos/apps/con_cell.c:320-321`, `UI_NONE` at `nos/ui.h:187`).
- Operator from `+COPS?` (`ext_lte.c:204-209`). The page shows "Unknown network" when it is empty (`con_cell.c:322`).
- Technology from the COPS access code: LTE, GSM, 3G, CAT-M or NB (`ext_lte.c:210`). Shown with HOME or ROAMING (`con_cell.c:324`).
- States (`lte_state`, `con_cell.c:172-181`):
  - ABSENT: "Not connected", "Add the LTE strap to text without your phone" (`con_cell.c:310`). Buttons are hidden.
  - OFF: "OFF", "TAP RADIO TO TURN ON" (`con_cell.c:327-330`).
  - STARTING: "STARTING", "Waking the modem" (`con_cell.c:331-334`).
  - NO SIM: "Insert a nano SIM" (`con_cell.c:335-338`).
  - SIM LOCKED: "Remove the PIN on a phone" (`con_cell.c:339-342`).
  - SEARCHING: "Looking for a network" (`con_cell.c:343-346`). Any registration value other than home or roaming also lands here (Gaps 11).
  - LIVE: home or roaming registered.
- RADIO (left disc, drawn `con_cell.c:351-357`, tap `:391-401`). A tap flips the saved intent, calls `hal_lte_enable`, sets `nos_prefs.lte` and saves prefs, and fills the disc over 6 frames (`TOGGLE6`, `con_cell.c:70`). The label reads "RADIO ON" or "RADIO".
- SMS (right disc, `con_cell.c:358`, tap `:402-405`). Pushes Messages after the press has shown (`:229-233`). Unread badge shows 1 to 9 (`:359-364`). The disc is active in every state except ABSENT.

Messages (`scr_cell_sms`, `con_cell.c:527`):

- Cards, newest first, at most 12 (driver cap). Each card has an unread dot, the sender as a raw number or alpha ID, a time (HH:MM if today, else "d MON"), and a one-line preview (`con_cell.c:442-462`).
- Empty list: "No messages", "Texts to your SIM land here" (`con_cell.c:508-512`).
- Tap a card, and after the press shows the reader is pushed (`con_cell.c:498-503`).

Reader (`scr_cell_read`, `con_cell.c:717`):

- Time, sender, body wrapped to 7 lines at 320 px (`con_cell.c:628`, `:700-701`).
- Quick replies: OK, On my way, Call you later, Yes, No (`con_cell.c:534`). Shown only for numeric senders (`can_reply`, `con_cell.c:122-126`). Alpha senders see "REPLIES NOT SUPPORTED" (`:702`).
- Send result card: "SENDING", "SENT" (modem accepted), or "NOT SENT" with the reply text (`con_cell.c:675-687`). SENT pops back to the list after 1.3 s (`:667`).

Not present: compose, new message, reply thread, delete, contacts, delivery reports, SMS alerts, voice calls, data, PIN entry.

## Reads

Services (svc_*): the screen reads no service getter. Its only svc_ reference is `svc_cell_tick`, which it defines and calls to acknowledge send results (`nos/apps/con_cell.c:83`, `:598`, `:645`; see [[Services hub]]).

HAL and driver reads (contract `nos/hal_ext.h`, implemented in `firmware/main/ext/ext_lte.c`):

- `hal_ext_present()`, EXT_LTE bit (`nos/apps/con_cell.c:96`, `:629`; `firmware/main/ext/ext_sys.c:197-199`). The bit is set when the modem answers AT at boot (`ext_lte.c:358-359`).
- `hal_lte_get(&L.l)` on enter (`con_cell.c:195`) and every frame in `cell_tick` (`con_cell.c:208`). Returns a copy of `hal_lte` (`nos/hal_ext.h:144-153`): powered, sim (0 none, 1 ready, 2 PIN locked), registered (0 no, 1 home, 2 roaming, 3 searching), rssi_dbm, bars, op, tech, unread_sms. The copy zeroes the signal fields when unpowered (`ext_lte.c:463-476`). Every call also renews the idle timer (`ext_lte.c:466-468`).
- `hal_lte_sms_list(s_sms, 16)` (`con_cell.c:102`), called from `cell_enter` (`:202`), `inbox_enter` (`:474`) and `inbox_tick` (`:488`). Copies the RAM cache, at most 12 entries (`ext_lte.c:478-487`). Also renews the idle timer.
- `hal_lte_sms_send_status(id)` (`con_cell.c:87`, `:647`). Values: 0 unknown, 1 queued, 2 sending, 3 accepted by CMGS, 4 failed (`nos/hal_ext.h:161`, `ext_lte.c:534-539`).
- `hal_big_alloc` (`con_cell.c:101`) for 16 `hal_sms` entries in PSRAM (`firmware/main/hal_esp32.c:1595-1597`). Never freed.
- `hal_get_datetime` (`con_cell.c:117`). Device clock, used only to decide "today" for the time label.
- `nos_prefs.lte` (`con_cell.c:96`, `:193`). Saved intent.
- NOS runtime: `nos_t`, `nos_revealed()`, `nos_enter_t()`, `nos_transitioning()`, `ui_*`, `dot_*`, `m_*` helpers. No diagnostics reads.

## Writes

Radio and modem (through the driver):

- `hal_lte_enable(on)` at `nos/apps/con_cell.c:194` (enter, when saved intent is on) and `:397` (RADIO tap). Sets `L->enabled` and `L->last_get` (`firmware/main/ext/ext_lte.c:454-460`). Refused while a shutdown is pending (`:455`).
- `hal_lte_sms_send_request(to, text)` at `con_cell.c:603`. Validates the number (digits and an optional leading +, at least 3 characters), replaces Ctrl-Z and ESC with spaces, caps the text at 160 characters, sets `send_pending` (keeps the modem on until sent), and allocates a send-result slot (`ext_lte.c:507-531`). Only one send at a time (`ext_lte.c:509`, `con_cell.c:599-601`).
- `hal_lte_sms_mark_read_id(id)` at `con_cell.c:617`. RAM only (`ext_lte.c:499-505`). No AT command reaches the SIM.

Preferences: `nos_prefs.lte` and `prefs_save()` on each RADIO tap (`con_cell.c:399-400`). Checkpointed by the prefs engine (see Persistence).

Diagnostics: `diag_action_request/context/result` on DIAG_DOMAIN_CONTROL "cell.power" (`con_cell.c:393-398`), and on DIAG_DOMAIN_SMS for each send (`con_cell.c:596-606`). The power result is left PENDING (`con_cell.c:398`).

UI and scheduling state: `nos_keep_awake()` while STARTING or SEARCHING (`con_cell.c:228`, body at `nos/nos.c:585`). `nos_feedback` taps and toggles (`con_cell.c:401`, `:403`, `:609`, `:650`). `nos_push` and `nos_pop` (`con_cell.c:232`, `:502`, `:667`, `:711`). Module RAM: `s_sms`, `s_nsms`, `s_selected`, `s_open`, `s_send_backend`, `s_send_trace` (`con_cell.c:74-82`), and the state blocks `L`, `I`, `R` (`con_cell.c:150-168`, `:426-436`, `:536-549`).

Nothing is written to SD, NVS or the SIM directly by this screen.

## Background behaviour

Who runs what, with the screen on or off:

| Runner | Period and priority | Cellular work | Evidence |
|---|---|---|---|
| `ext` task | Priority 0, 4096 B stack in PSRAM, no core affinity. Sleeps min(50 ms, service hints). | Calls `lte_service()` each loop: UART pump, AT state machine, inbox poll, send engine, power control. | `firmware/main/ext/ext_sys.c:144-166` (call at :155), created at :186 |
| UI frame, screen on | Each display frame | `cell_tick` (`hal_lte_get`, keep-awake while STARTING or SEARCHING); inbox and reader ticks call `sms_keepalive` | `nos/nos.c:2101-2108`, `nos/apps/con_cell.c:205-244` |
| Services tick | Each UI frame; 1 Hz when the screen is off | `svc_cell_tick` acknowledges send results | `nos/apps/registry.c:62-65`, `nos/svc.c:1303-1310`, `firmware/main/nos_main.c:2091-2094` |
| Boot probe | Once per boot | Powers the modem, reads SIM and one inbox listing, then QPOWD unless enabled | `firmware/main/ext/ext_lte.c:446-450` (`probing = 1`), `:392-393` |

Modem power rules. The modem is wanted while `enabled`, while the boot probe runs, or while a send is pending (`want_on`, `firmware/main/ext/ext_lte.c:299-306`).

- `enabled` clears when no getter has renewed `last_get` for NOS_EXT_IDLE_MS = 8000 ms (`ext_lte.c:302`, `nos/hal_ext.h:35`). Getters never start the modem. Only `hal_lte_enable(1)` does.
- Power-on: AT probe x3, then PWRKEY held 550 ms if no answer, then wait up to 15 s for AT (`ext_lte.c:338-357`). Init script: ATE0, CMEE=1, CPIN?, CMGF=1, CSCS="GSM", CNMI=2,1,0,0,0, COPS=3,0 (`ext_lte.c:45-47`).
- While on: CSQ, CEREG?, CREG?, COPS? round robin, one every 1.25 s, so each value refreshes about every 5 s (`ext_lte.c:419-423`). CPIN? every 30 s while the SIM is not ready (`:414-417`). CMGL "ALL",1 every 60 s and on +CMTI (`:408-411`, `:214-215`).
- Power-off: AT+QPOWD=1 (`:392-393`), then a 1 s PWRKEY pulse if POWERED DOWN is not seen within 8 s (`:425-433`).
- While off: `lte_service` checks `want_on` every 1 s (`ext_lte.c:333`), with the UART closed.

Consequences:

- With the screen off nothing renews the idle timer, so the modem powers down within about 8 s of the last getter call. Getter callers are this page, the Messages page, the Settings Cellular row (`nos/apps/set_settings.c:589`) and diagnostics. The services tick and the screen tick do not run in the screen-off loop, which runs only `nos_bg_tick` at 1 Hz (`firmware/main/nos_main.c:2091-2094`; `nos/nos.c:2101-2108`).
- A pending send keeps the modem on until it is sent or fails (`ext_lte.c:299-306`). Its outcome is acknowledged at 1 Hz with the screen off (`nos/svc.c:1310`).
- Incoming texts: CNMI=2,1 reports only while the modem is powered and registered (`ext_lte.c:46`). With the modem off the watch is not registered on the network. This is an inference from source, not tested on hardware: the carrier holds texts until the module registers, and the list is read when the page next opens (first listing 2 s after the modem reaches READY, `ext_lte.c:369`).
- Nothing applies the saved intent at boot. `radio_preferences()` pushes only BLE and Wi-Fi (`firmware/main/nos_main.c:953-962`). No other caller enables the LTE module (see Navigation). The systems note that says LTE is pushed every pass is wrong for LTE (Gaps 7).
- Display sleep is PWR_OFF, not deep sleep (`firmware/main/nos_main.c:749-752`, mode 0). Deep sleep (mode 2) runs `hal_ext_shutdown_begin` (`nos_main.c:779`) and `lte_shutdown_poll`, which sends QPOWD and the PWRKEY pulse before sleep (`firmware/main/ext/ext_sys.c:91`, `firmware/main/ext/ext_lte.c:541-562`).
- Cost at every boot: the boot probe powers the modem even when the radio is saved as off (`ext_lte.c:446-450`). With no module, the probe waits up to 15 s (`ext_lte.c:350-353`) before it gives up.

## Persistence

- `nos_prefs.lte`: uint8 in the prefs struct (`nos/prefs.h:24`, default 0 at `nos/prefs.c:152`, overlay field at `nos/prefs.c:35`). `prefs_save()` marks it dirty (`nos/prefs.c:260`). `prefs_tick()` flushes 2 s after the last edit or 10 s after the first (`nos/prefs.c:366-383`). `prefs_flush()` calls `hal_store_save_checked("nosprefs")` (`nos/prefs.c:349`).
- On the 1.43-B build that save writes the checksummed alternating SD checkpoint `/sdcard/watch/settings.{0,1}.bin` (`nos/prefs_disk.c:30-32`). It also mirrors to NVS namespace `wos`, key `nosprefs` (`firmware/main/hal_esp32.c:228-251`, NVS helper at `:177-181`). If the card is absent the NVS copy is the load fallback (`hal_esp32.c:277`). A failed save stays dirty and retries every 5 s (`nos/prefs.c:349-352`, `:378-381`).
- SMS inbox: RAM only, in the driver's `sms[]` and `tmp[]` arrays of `MAX_SMS` = 12 (`firmware/main/ext/ext_lte.c:39`, `:61-66`). The SIM is the real store. The watch never deletes or rewrites messages (no CMGD, CMGR, CSMP or CDS in the driver).
- Unread: set from the "REC UNREAD" status on each listing (`ext_lte.c:236`). Local read marks are RAM only (`ext_lte.c:499-505`), so a reboot shows read messages as unread again.
- Sent replies: not stored. The send-result ring holds 8 ids in RAM (`ext_lte.c:124-125`, `:219`). The screen's send latch is RAM (`con_cell.c:82`).
- Diagnostics: power and send actions go to the in-memory diagnostics queue (`nos/diag_log.c:218-228`, `QUEUE_CAP` 128 at `:42`). They drain to `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`) through `diag_log_tick()` in the main loop (`firmware/main/nos_main.c:1982`, `nos/diag_log.c:348`).
- No NVS key or SD path of its own, apart from the shared prefs record above.

## Navigation

Opened from:

- Launcher: `nos/apps/registry.c:30` ("Cellular", DOT_ICON_SIGNAL, `&scr_cell`).
- Settings, Cellular row: `nos/apps/set_settings.c:471` (row), `:1853` (push). The row reads "No module" when absent, or the operator name (`:589-591`).
- Simulator screen table: `sim/nos_sim.c:81` (name "cell").

Opens:

- Messages (`scr_cell_sms`, `nos/apps/con_cell.c:527`) from the SMS disc (`con_cell.c:232`).
- Reader (`scr_cell_read`, `con_cell.c:717`) from a card (`con_cell.c:502`). The reader pops after a SENT result (`con_cell.c:667`) or a tap on that result (`:711`).

Other screens that drive the same module without opening this page:

- Control Center LTE tile: `nos/apps/sys_control.c:93` (shown only when EXT_LTE is present). Reads `nos_prefs.lte` (`:179`). Writes it and calls `hal_lte_enable` (`:234`).
- Modules screen: row `nos/apps/set_modules.c:75`, status `:305-307`, toggle `:1376-1380` (calls `hal_lte_enable`, leaves `nos_prefs.lte` unchanged).
- Diagnostics: `nos/apps/set_diag.c:71` (D_LTE test, "ANSWERS AT IN 25 s"). Enables and disables the radio at `:148`, `:169` and `:1220`.
- Serial: `lte on`, `lte off`, `lte at ...`, `lte sms ...` (`firmware/main/ext/ext_debug_commands.inc:29`, `:49`, `firmware/main/ext/ext_lte.c:564`).

## Gaps

Ordered by severity. Every entry is from source reading; none was reproduced on a board.

1. **high. No background text reception.** The modem listens only while this page or Messages renews the idle timer (`firmware/main/ext/ext_lte.c:299-306`, `:302`, `nos/hal_ext.h:25-27`). With the screen off nothing does, so the modem powers down (`ext_lte.c:392-393`) and the screen tick does not run (`firmware/main/nos_main.c:2091-2116`). Impact: texts show up only when Cellular or Messages is opened. The copy "Add the LTE strap to text without your phone" (`nos/apps/con_cell.c:310`, `nos/apps/set_modules.c:75`) promises more.

2. **high. No alert for an incoming text.** `unread_sms` feeds only the Cellular badge (`nos/apps/con_cell.c:218`). Nothing in `nos/svc.c`, the notification store or `nos/apps/sys_notifs.c` reads it (grep of `unread_sms` and `sms` over `nos/` and `firmware/main/`). `nos/svc.c:175` is the phone-notification keyword list, not the modem. Impact: no vibration or notification when a text arrives.

3. **high. The list is a boot-time snapshot while RADIO is OFF.** `sms_keepalive` enables the modem only when `nos_prefs.lte` is set (`nos/apps/con_cell.c:96`). `inbox_enter` and `inbox_tick` call it (`:473`, `:486`) but do nothing with the radio off. The list reads the RAM cache (`ext_lte.c:478-487`), which only the boot probe fills. The SMS disc stays active on the OFF page (`con_cell.c:402-405`, drawn in LS_OFF). Impact: a user with the radio off sees old texts, with no stale indicator and no refresh.

4. **high (source-level). A text whose body is exactly "OK" or "ERROR" corrupts the inbox listing.**
   - In `line()` the bare OK result is tested before the SMS body branch (`firmware/main/ext/ext_lte.c:180`, body branch at `:186`). A body line that reads OK therefore closes the CMGL command.
   - `lte_service` then clears `cmd` (`ext_lte.c:327-328`) and publishes the partial list (`:373`). The rest of the listing is dropped, because later +CMGL headers are parsed only while `cmd` is CMGL (`:224`). The next 60 s refresh hits the same line.
   - A body exactly "ERROR" sets R_ERR (`ext_lte.c:181-184`). The refresh is skipped and the previous snapshot stays (`:373`). The failure also sets `cmgl_mode1_bad` (`:374`), which switches every later listing to the plain `AT+CMGL="ALL"` form for the rest of the boot (`:410`). The driver's own header says the ",1" form exists to list without marking read (`ext_lte.c:20`), so the plain form may mark messages read on the SIM. Not verified.
   - Impact: one common reply text can hide later messages until reboot.

5. **medium. Only the first 12 listed messages are kept.** `MAX_SMS` is 12 (`ext_lte.c:39`). The parser stops adding entries after 12 in modem order (`ext_lte.c:225`), and only then sorts newest first (`ext_lte.c:266-295`). Newer messages in higher SIM slots are dropped. The UI allows 16 (`nos/apps/con_cell.c:73`) and never fills them.

6. **medium. Read state does not survive reboot.** `hal_lte_sms_mark_read_id` changes RAM only (`ext_lte.c:499-505`). The listing form never marks read on the modem (`ext_lte.c:409-411`, comment at `:20`). "REC UNREAD" comes back after reboot (`ext_lte.c:236`).

7. **medium. Saved RADIO intent is not applied at boot or in the background.** `nos_prefs.lte` is read on Cellular enter (`con_cell.c:193-194`) and on Messages entry (`con_cell.c:96`). `radio_preferences()` handles BLE and Wi-Fi only (`firmware/main/nos_main.c:953-962`). The systems note `docs/os-map/systems/Connectivity and voice.md:15` says LTE intent is pushed every main-loop pass. That is wrong for LTE.

8. **medium. Control Center and Modules disagree with the modem.** The LTE tile shows `nos_prefs.lte` (`nos/apps/sys_control.c:179`), so it reads ON after the modem has idle-powered off (`ext_lte.c:302`). The Modules toggle changes the modem but not the saved intent (`nos/apps/set_modules.c:1380`). The Cellular page keeps its own `L.want_on`, loaded only on enter (`con_cell.c:193`).

9. **medium. Replies are five fixed strings, and "SENT" overstates delivery.** The list is `k_replies` (`con_cell.c:534`). There is no compose, no new message, no thread, and sent replies are not stored. "SENT" means the modem accepted the text (`ext_lte.c:219` sets state 3 on +CMGS), not that it was delivered. No delivery report is configured (no CDS or CSMP in `INIT_CMDS`, `ext_lte.c:45-47`).

10. **medium. No contact names.** The sender is shown as the raw number or alpha ID (`con_cell.c:460`, `:698`). There is no phonebook read (no AT+CPBR).

11. **medium. Denied registration reads as "SEARCHING" forever.** `reg_publish` maps CREG or CEREG 3 (denied) and 4 to 0 (`firmware/main/ext/ext_lte.c:135-139`). `lte_state` maps 0 to LS_SEARCHING (`nos/apps/con_cell.c:179-180`). No DENIED state exists. SEARCHING also keeps the watch awake (`con_cell.c:228`), so a denied SIM holds the display on while the page is open.

12. **medium. A module attached after boot is not found from this page.** Presence is probed at boot (`ext_lte.c:440-452`). A failed probe goes to L_ABSENT (`:350-353`). `hal_lte_get` returns 0 while the presence bit is clear (`:465`). The page ignores input when absent (`con_cell.c:382`). Only `hal_lte_enable(1)`, from the Modules or diagnostics screens or serial, restarts the probe (`ext_lte.c:332-333`).

13. **low. Keep-awake while searching has no timeout.** `nos/apps/con_cell.c:228` blocks auto-sleep while STARTING or SEARCHING. Combined with Gap 11 it can hold the display on as long as the page is open.

14. **low. "NO SIM" flashes on wake.** L_INIT sets `powered = 1` (`ext_lte.c:361`) before CPIN is answered (`:45-47`). `sim` stays 0 until `+CPIN:` arrives (`:213`). The page shows "NO SIM, Insert a nano SIM" (`con_cell.c:177`, `:335-338`) until then.

15. **low. Send timeout counts from READY entry, not from the request.** `ext_lte.c:398` compares against `L->t_state`, which `go()` sets when a state begins (`ext_lte.c:111`, `:368`). A reply queued more than 20 s after READY, with no usable SIM, fails at once rather than waiting.

16. **low. The power action never completes in diagnostics.** `nos/apps/con_cell.c:398` logs PENDING. The request id is not stored and nothing reports the outcome.

17. **low. SIM PIN lock is a dead end.** "Remove the PIN on a phone" (`con_cell.c:341`). There is no PIN entry. CPIN value 2 covers both PIN and PUK (`ext_lte.c:213`).

18. **low. Timestamps are in the carrier's wall time.** The +zz offset is dropped (`ext_lte.c:235`, `%.17s`). The time is compared with the watch date (`con_cell.c:112-119`) and is not converted with `nos_prefs.tz_min`.

19. **low. Text limits.** Stored bodies are at most 159 characters (`nos/hal_ext.h:156`, append at `ext_lte.c:186-190`). UCS-2 bodies keep only ASCII and other characters become "?" (`ext_lte.c:157-172`). Multi-part messages are not joined. The reader shows 7 lines (`con_cell.c:700-701`).

20. **low. Not built.** Voice calls, data, SIM identity or balance, and signal history are absent. `docs/os-map/systems/Connectivity and voice.md:119` says the same.

21. **low. Maintenance.** `nos/apps/con_cell.c` is not valid UTF-8. The byte at line 63 is a legacy-encoded symbol, and the editor tool refuses to write the file. The header comment says replies go through `hal_lte_sms_send` (`con_cell.c:9`), but the code calls `hal_lte_sms_send_request` (`con_cell.c:603`).

Docs versus code, for the owner of the systems notes: `docs/os-map/systems/Connectivity and voice.md:15` (LTE pushed every pass, wrong; see Gap 7), `:118` (correct: applied only when Cellular, Control Center or Modules open), `:161` (correct: LTE effectively sleeps with the screen off after the boot probe). `docs/os-map/systems/Hardware and drivers.md:98` (correct about the 8 s idle expiry).

Related: [[Services hub]] (no service getter; svc_cell_tick only), [[Connectivity and voice]] (the module driver and its radio rules), [[Engine and scheduling]] (ext task cadence and screen tick dispatch), [[Power and battery]] (idle timer, keep-awake, boot probe cost), [[Storage and persistence]] (prefs checkpoint on SD and NVS), [[Hardware and drivers]] (strap pins and presence probe), [[Shell and faces]] (launcher entry, Settings row, power states).
