---
title: "Recorder"
id: app-recorder
tags: [nos, os-map, app]
status: partial
updated: 2026-10-23
---

# Recorder

Source: `nos/apps/con_memo.c` (1106 lines). Read-only source review of the 1.43-B build; nothing built, flashed or run, and the serial port was not touched. Evidence is `repo-relative/path:line`. Where a comment and the code disagree, the code is cited. Strap-module and HAL code is in `firmware/main/ext/`.

## Summary

Recorder is the NOS voice-memo app. It is a three-page horizontal pager, RECORDER, MEMOS and NOISE, over two optional strap modules: an INMP441 microphone and a MAX98357A amplifier with speaker (`nos/apps/con_memo.c:1-18`, `firmware/main/ext/ext_priv.h:32-38`). It records 16 kHz mono 16-bit WAV files to `/sdcard/watch/memos/`, lists the newest 24 of them, plays one at a time through the speaker, and shows a live dB meter.

The screen owns no sensor, no storage driver and no settings. It queues file paths and reads status. The HAL audio task and the file-owner task do the I/O (`firmware/main/ext/ext_audio.c:330-353`, `firmware/main/ext/ext_audio_files.inc:139-211`). A recording belongs to the HAL, so it keeps running after the screen is left (`nos/apps/con_memo.c:16-18`, `:624-632`, `nos/svc.c:1309`).

Status is **partial** from source reading. The record, save, list and play paths are complete in code. One blocker: nothing creates `/sdcard/watch/memos`, so the first recording on a fresh card fails (Gaps 1). Nothing was confirmed on hardware.

Related: [[Services hub]], [[Storage and persistence]], [[Hardware and drivers]], [[Power and battery]], [[Engine and scheduling]], [[Shell and faces]], [[Connectivity and voice]], [[Sports and workouts]].

## Features

RECORDER page (`nos/apps/con_memo.c:866-925`):
- **Monitor waveform.** 23 mirrored bars scroll left, one column every 3 frames (`:264-268`, `:636-641`). Dim while monitoring, white while recording (`:878`). At the default `NOS_FPS` 40 that is 75 ms a column, not the 100 ms the header states (`:4-5`; `nos/render_config.h:8`).
- **Elapsed time.** Dot-matrix, from `ui_fmt_dur` (`:761`, `:894-899`). A dim placeholder shows at rest (`:762`).
- **Input level.** Eight triangles beside the time, one per 5 dB from 41 dB (`:751`, `:272-276`, `:900-906`).
- **Record button.** A 52 px red dot becomes a 34 px rounded square while recording, on a 4-frame table (`:647-651`, `:917-924`). Red is the screen's only red (`:4-9`).
- **Hints and title.** Title swaps RECORDER and RECORDING. Hint lines: TAP TO RECORD, TAP TO STOP, SAVED, SAVING, SAVE FAILED (`:849-858`, `:909-915`).
- **Start and stop.** A tap on the button starts or stops (`:1087-1089`). A stop shows a SAVING toast (`:523`).
- **Ten-minute cap.** Recording stops and saves automatically at 10 minutes (`firmware/main/ext/ext_audio.c:42`, `firmware/main/ext/ext_audio_files.inc:186`). The screen does not show the cap.

MEMOS page (`:927-940`, `:371-427`):
- **List.** Newest-first, up to 24 `.wav` files from `/sdcard/watch/memos` (`:110`, `:179-223`).
- **Row.** Circular play button, name "Memo N", and "length . time" for today or "length . day" for older memos (`:400-408`).
- **Playback.** Tap a row to play it. Tap the playing row to stop it. A new row stops the old one (`:412-427`). Playback also stops on leave (`:631`).
- **Progress.** The playing row shows an 8-segment bar and elapsed time (`:392-398`). On device, progress comes from samples consumed by the HAL (`:774`). In the simulator it is wall time (`:777`).
- **No speaker.** Play buttons go grey and a "NO SPEAKER" footer shows (`:383-385`, `:936-939`). A failed open shows "CAN'T PLAY" (`:421`).
- **Empty state.** "No memos / Your recordings land here" (`:929-932`).

NOISE page (`:953-984`, `:942-951`):
- **Number.** Integer dB, floored and clamped 0 to 140 (`:726-729`), with a "dB" caption (`:961`). It updates once a second, or at once when the band changes (`:716-739`).
- **Meter.** 24 dots at 5 dB each. Dots past 85 dB are red (`:963-977`). Dot i lights at 5i dB (`:967`).
- **Band words.** QUIET below 40 dB, MODERATE below 70, LOUD below 85, HEARING RISK at 85 and above. Subtitles: "Like a library", "Normal conversation", "Busy street traffic", "Limit your exposure" (`:643`, `:944-945`).

Shared:
- **Pager.** Three pages with a pixel spring (`:1058-1080`). Page dots show, and hide while recording (`:1005`).
- **Swipe lock.** While recording, a sideways drag is a rubber-band only, and the pager stays put (`:1032-1035`, `:1046-1050`).
- **Absent states.** RECORDER and NOISE show "Not connected / Add the mic strap..." without a mic (`:101-105`, `:868`, `:955`). Taps on the button are ignored (`:1087`).
- **Feedback.** Haptics and sounds follow the global prefs through `nos_feedback()` (`nos/nos.c:570-572`).

Not offered: delete, rename, share or export, pause, seek, next or previous, loop, a long-press menu (`memo_input` has no `IN_LONG` case, `:1015-1102`), search, titles, transcription, background playback, and any transfer off the watch.

## Reads

Services (`nos/svc.h:191-196`):
- `svc_memo_active()`: is a recording open? Read on enter (`nos/apps/con_memo.c:553`) and leave (`:630`). Defined at `:509`. The Home REC chip reads the same flag (`nos/apps/sys_home.c:152`).
- `svc_memo_elapsed_ms()`: defined at `:510`. Called only by the Home REC chip (`nos/apps/sys_home.c:153`), not by this screen.
- `svc_memo_number()`: defined at `:511`. No caller in the tree.
- No other service getter supplies memo data.

Prefs:
- `nos_prefs.tz_min` (`:160`, `:251`, `:595-596`): offset used for memo stamps and labels. Default 330, which is IST (`nos/prefs.c:154`). Set in Settings (`nos/apps/set_settings.c:1935`).
- `nos_prefs.haptics` and `nos_prefs.sound`, read inside `nos_feedback()` (`nos/nos.c:570-572`).
- Playback loudness follows the global `nos_prefs.volume` through `hal_audio_volume()` (`nos/apps/set_settings.c:1768`, `:2093`; default 60 at `nos/prefs.c:141`). The Recorder does not read it.

HAL (`nos/hal_ext.h:84-119`):
- `hal_ext_present()` for `EXT_MIC` and `EXT_SPEAKER`, on enter and on every tick (`:568`, `:675-677`). Implemented at `firmware/main/ext/ext_sys.c:197`.
- `hal_mic_db()` (`:700`) and `hal_mic_wave()` (`:707`). The screen calls them only on pages 0 and 2, or while recording (`:699`).
- `hal_rec_status()` (`:458-459`, `:467`, `:909`), `hal_rec_result()` (`:460`), `hal_rec_ms()` (`:474`, `:554`, `:759`).
- `hal_audio_playing()` (`:775`) and `hal_audio_play_pos_ms()` (`:774`).
- `hal_get_datetime()` (`:129`, `:403`, `:592`) and `hal_epoch_seconds()` (`:594`).
- `hal_big_alloc()` (`:242`, `:246`) for the list and scan buffers, which are PSRAM (`firmware/main/hal_esp32.c:1595`).

SD, through POSIX calls and the board lock: `opendir`, `readdir`, `stat` and `closedir` on `/sdcard/watch/memos`, under `board_sd_asset_lock()` (`:174-196`). The scan task takes `board_sd_asset_acquire()` (`:231`). Memo audio is read only by the HAL playback path (`firmware/main/ext/ext_audio_files.inc:49-84`).

Engine and shell (timing and input, no data): `nos_t`, `nos_enter_t()` (`:544`), `nos_revealed()` (`:543`), `nos_top()` (`:349`), `nos_leaving_covered()` (`:630`), `nos_subview()` (`:994`), and the `ui_list` input path (`:572`, `:1017`).

Diagnostics reads: `diag_log_name_id("RECORDER")` (`:519`, `:586`).

## Writes

SD, direct from the screen: none. The screen builds the path (`:445-447`) and queues it with `hal_rec_start_exclusive()` (`:448`, `firmware/main/ext/ext_audio.c:417-430`). It never opens a memo file.

SD, by the HAL file owner `extf` on the screen's behalf (`firmware/main/ext/ext_audio_files.inc`):
- Creates `M_YYYYMMDD_HHMMSS_NNNN.wav.part` with `O_EXCL` (`:93-95`), writes a 44-byte header with zero length (`:99`, `:38-48`), then streams 16-bit samples in 2048-sample chunks (`:183-198`).
- On stop, rewrites the header, flushes, fsyncs, closes, and renames to `.wav` (`:117-133`, `:112`). The host build uses `link()` (`:105-110`).
- Nothing in the Recorder or the HAL deletes a memo or a `.part`.

RAM: the screen state `R` (`:281-327`); the PSRAM list `s_memos`, 24 entries (`:110-113`); the scan result (`:163-168`); and the session fields `s_rec_*` (`:430-440`).

Sports journal (`nos/sports.c`, `nos/sports.h`):
- `SP_ROUTINE` {2,1} on enter (`con_memo.c:558`) and {2,0} when a recording finishes (`:473`). These are not raw types. They go to `/sdcard/watch/sports/<day>-events.spt` (`nos/sports.c:217-218`, `raw_type` at `:56`).
- `SP_MIC`: every 32 ms while the mic port is warm, the HAL emits the dB estimate, the peak and an envelope value (`firmware/main/ext/ext_audio.c:168-179`). `SP_MIC` is a raw type (`nos/sports.h:8-11`, `nos/sports.c:56`), so it is written to `/sdcard/watch/sports/<day>-<hour>-sensors.spt` by the 1 Hz drain (`nos/sports.c:287`, `:215-217`). Each record is 64 bytes (`nos/sports.h:13-18`). At about 31 records a second, that is roughly 2 KB/s, or 7 MB an hour, while the mic runs. This is not audio, but it is not disclosed on screen.

Diagnostics: `diag_action_request`, `diag_action_context` and `diag_action_result` for START and STOP (`:518-522`, `:585-617`, `:470-471`; helpers `nos/diag_log.c:229-240`). The log is written to `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`, `:484`). The code comment says paths and audio are never logged (`:439`).

Feedback and toasts: `nos_feedback()` at `:421`, `:482`, `:608`, `:621`, `:696`, `:1034`. Toasts: LIBRARY ERROR (`:349`), NO SPEAKER (`:418`), CAN'T PLAY (`:421`), SAVE FAILED (`:482`), the saved toast (`:499-507`), SAVING (`:523`, `:589`), RECORDING (`:630`).

Sleep timer: `nos_keep_awake()` while a recording is open (`:759`) or playing (`:780`). Both calls sit inside `memo_tick`, which runs only while the Recorder is the top screen (`nos/nos.c:2101-2109`).

Audio out: `hal_audio_play_file()` (`:419`) and `hal_audio_play_stop()` (`:335`). Recording control: `hal_rec_start_exclusive()` (`:448`, `:602`) and `hal_rec_stop()` (`:520`).

Settings and NVS: none. The Recorder writes no prefs key and no NVS key.

## Background behaviour

Who runs what while the screen is closed or off:

- **Screen code.** Not ticked. `tick_screens()` ticks only the top screen (`nos/nos.c:2101-2109`).
- **`exta` (`audio_task`).** Priority 3, PSRAM stack of 4096 bytes, unpinned (`firmware/main/ext/ext_audio.c:344-345`). It pumps I2S in 4 ms blocks while `need()` is true. `need()` is true while `A->recording` is set (`:234-240`).
- **`extf` (`audio_file_task`).** Priority 0, PSRAM stack of 4096 bytes (`:350-351`). It writes WAV chunks and finalizes the file, waking every 20 ms or on notify (`ext_audio_files.inc:139-211`). The 10-minute stop and the card-full stop both run here (`:186-196`).
- **Mic warm-up.** Samples are captured only after 300 ms of warm-up (`ext_audio.c:44`, `:164`, `:184`).
- **Service tick.** `svc_memo_tick()` runs inside `svc_tick()` (`nos/svc.c:1303-1309`). `svc_tick` runs once per frame with the display on (`nos/nos.c:2341`), once per second with the display off (`firmware/main/nos_main.c:2094`), and at about 1 Hz in AOD (`:2104-2116`). So the "SAVED" toast and the diagnostics result can lag by up to a second with the screen off. The file itself is finalized by `extf` regardless.
- **Sports writer.** `sports_sd`, priority 1, PSRAM stack, unpinned (`nos/sports.c:323`), drains the journal once a second (`:287`).
- **Display sleep is not held off.** The sleep check reads only the top screen's `SCR_KEEPAWAKE` flag and the awake timer (`firmware/main/nos_main.c:2079-2084`). The Recorder's flags are `SCR_HDRAG` only (`nos/apps/con_memo.c:1104-1105`). `nos/power_policy.h:15-19` calls recording a background service and never a wake source. So the display can time out during a background recording.
- **Mic stays on for the recording.** The port stays up while `A->recording` (`ext_audio.c:234-240`), independent of the screen.
- **No stop from outside.** `svc_memo_stop()` is called only by the Recorder's own button (`con_memo.c:580`). The REC chip and the Control Centre row only open the screen (`nos/apps/sys_home.c:519`, `nos/apps/sys_control.c:357`).
- **Shutdown and OTA refused.** Power-off, restart and deep sleep are refused with "SAVE RECORDING FIRST" (`firmware/main/nos_main.c:740`, `:757`). OTA is blocked while a recording or playback runs (`firmware/main/ota.c:65-66`).
- **Leaving.** Leaving while recording shows "RECORDING" once (`con_memo.c:630`). It is silent if a call or alarm covers the screen (`nos/nos.c:206`).
- **Playback.** Stops on leave (`con_memo.c:631`). While the Recorder is on top, playback keeps the screen awake (`:780`). If the display sleeps while the Recorder is still on top, the HAL keeps playing, and `R.playing` catches up on the next frame (`:771-778`).
- **Mic idle timeout.** The mic port stays up for 8 s after the last poll (`nos/hal_ext.h:35`, `ext_audio.c:234-240`). Recorder page 0 and page 2 poll every tick (`con_memo.c:699-707`). After leaving, the port parks and the INMP441 powers down (`ext_audio.c:12-14`, `:131-135`).

## Persistence

- **Memo files.** `/sdcard/watch/memos/M_YYYYMMDD_HHMMSS_NNNN.wav` (`con_memo.c:445-447`). Format is 16 kHz mono 16-bit PCM in a 44-byte WAV header (`ext_audio_files.inc:38-48`). The playback reader accepts only that format (`:73`).
- **Name stamp.** The stamp is UTC (`con_memo.c:597-600`). The list converts it to local time with `nos_prefs.tz_min` (`:203`, `:206`).
- **Duration.** `(size - 44) / 32` ms (`con_memo.c:198`). No header read is needed.
- **Collisions.** The suffix counts from 0000 to 9999 (`:444-452`, `:461-463`).
- **In-progress file.** `<name>.wav.part` (`ext_audio_files.inc:93`). The list ignores it because the name must end in `.wav` (`con_memo.c:189`).
- **Publication.** A rename on the device (`ext_audio_files.inc:112`). A failed save leaves the `.part` on the card, and the comment says so on purpose (`:107-108`, `:133-136`). Nothing in the tree removes it. The SD transfer recovery covers only its own transaction files (`firmware/main/sd_xfer.c:203-259`).
- **Directory.** `/sdcard/watch/memos` is never created. Board mount creates only `/sdcard/watch` and `/sdcard/watch/logs` (`firmware/main/board.c:592`).
- **"Memo N" numbers.** Count-based, rebuilt on every scan (`con_memo.c:223`, `:347`). Nothing is saved. Numbers change when files are removed or the card is swapped.
- **List.** RAM only, rebuilt on enter (`:571`) and after each save (`:486`). Every rebuild walks the whole folder and stats each WAV (`:175-219`). Only the 24 newest are kept.
- **Sports journal.** `/sdcard/watch/sports/` (`nos/sports.c:25`, `:215-219`), as above.
- **Diagnostics.** `/sdcard/watch/diagnostics` (`nos/diag_log.c:24`), as above.
- **Survives reboot.** The memo WAV files and the sports and diagnostic journals. Does not survive: an unfinished recording, which is never recovered, and the list, which is rebuilt.

## Navigation

Opens from:
- Launcher entry "Recorder" (`nos/apps/registry.c:31`), opened by `nos_open()` with the iris transition (`:26-34`).
- Home REC chip, shown while a memo records (`nos/apps/sys_home.c:152-158`; tap at `:502-519`; `sys_rec_screen` kind 2 at `:174`).
- Control Centre REC row (`nos/apps/sys_control.c:284`, `:352-357`).
- Last-app double press (`nos/apps/registry.c:23-24`).

Opens: nothing. `con_memo.c` makes no `nos_push`, `nos_open` or `nos_pop` call. Leaving is the generic shell pop.

Internal: the pager moves between RECORDER, MEMOS and NOISE (`:1015-1102`). A tap on a memo row only plays it.

Settings: no Recorder section. Settings > Modules shows the mic row ("Add the mic strap to record voice memos", pins "BCLK 1 WS 2 SD 3") and the speaker row (pins "BCLK 1 LRC 2 DIN 5 SD 6") (`nos/apps/set_modules.c:63`, `:69-72`).

Notifications: none.

## Gaps

Ordered by severity. Each gap is from static reading only.

1. **High. The memos folder is never created.** The HAL opens `.part` files in `/sdcard/watch/memos` with `O_EXCL` and no mkdir (`firmware/main/ext/ext_audio_files.inc:93-98`). Board mount makes only `/sdcard/watch` and `/sdcard/watch/logs` (`firmware/main/board.c:592`). Nothing else in `firmware/` or `nos/` creates it. On a fresh card, `open` returns ENOENT, the result is HAL_REC_ERROR (`ext_audio_files.inc:98`), and the status latches at 4 (`:180`). The screen then shows SAVE FAILED with an error buzz (`nos/apps/con_memo.c:456-483`). Opening the screen shows LIBRARY ERROR, because `opendir` fails (`:175-177`, `:349`). A card where someone created the folder by hand would work. User impact: the feature looks broken on any new card.

2. **High. Failed saves strand audio that nothing shows or cleans up.** On a write error, a full card, or a failed close, `record_finish` skips publication and keeps the `.part` (`firmware/main/ext/ext_audio_files.inc:133-136`, comment `:107-108`). The library ignores `.part` files (`nos/apps/con_memo.c:189`). Nothing deletes them. The screen says only "SAVE FAILED" (`:482`). Captured audio is gone from the user's view and keeps using card space.

3. **Medium. No free-space check before recording.** Recording starts without checking free space. The screen makes no statvfs or storage-status call (grep of `con_memo.c`). A full card fails mid-recording: the writer sets `rec_err` and stops (`firmware/main/ext/ext_audio_files.inc:190-196`). The user sees SAVE FAILED (Gap 2).

4. **Medium. Background recording does not keep the display on.** The sleep check does not know about recording (`firmware/main/nos_main.c:2079-2084`). The Recorder has no keep-awake flag (`nos/apps/con_memo.c:1104-1105`). The power policy calls recording a background service (`nos/power_policy.h:15-19`). So the screen can time out while the mic records, and the only sign is the REC chip on wake. This may be intended, but no UI says so.

5. **Medium. Stopping a background recording needs the Recorder.** `svc_memo_stop()` is declared as a service API (`nos/svc.h:195`) but called only from the Recorder's own button (`nos/apps/con_memo.c:580`). The REC chip and Control Centre only open the screen (`nos/apps/sys_home.c:519`, `nos/apps/sys_control.c:357`). Shutdown and OTA are refused until the user stops it (`firmware/main/nos_main.c:740`, `:757`; `firmware/main/ota.c:65-66`). The 10-minute cap is the only automatic end.

6. **Medium. No way to get memos off the watch.** A case-insensitive grep for "memos" in `firmware/main/` and `nos/` finds no reader other than the Recorder (`nos/apps/con_memo.c`). No BLE, Wi-Fi or Gadgetbridge route in those trees reads the folder, so a memo leaves the watch only by pulling the SD card.

7. **Medium. Only 24 memos are visible, with no paging.** `MEMO_MAX` is 24 (`nos/apps/con_memo.c:110`). The scan keeps the newest 24 (`:212-218`). There is no paging, count or "more" marker. Older files stay on the card and in no UI.

8. **Medium. No delete, rename, share or export.** `memo_input` has no `IN_LONG` case, so a long press does nothing (`nos/apps/con_memo.c:1015-1102`; `IN_LONG` is defined at `nos/nos.h:40`). The only row action is play.

9. **Medium. Playback is minimal.** There is no pause (a second tap stops), no seek (the progress bar is display-only, `con_memo.c:385-398`), no next or previous, no loop, and no background playback (`:631`). The reader accepts only 16 kHz mono 16-bit WAV (`firmware/main/ext/ext_audio_files.inc:61-75`). Other files show "CAN'T PLAY" (`con_memo.c:421`).

10. **Medium. Speaker presence is assumed, not probed.** `audio_init()` marks the speaker present with "cannot be probed; default on" (`firmware/main/ext/ext_audio.c:343`). Only the mic is probed (`:207-231`). With no amplifier wired, the play buttons stay live, the progress runs (`nos/apps/con_memo.c:777-778`), and "NO SPEAKER" never appears (`:418`, `:936`). The user sees playback with no sound.

11. **Medium. The dB figures are uncalibrated, but the screen reads them as a health warning.** The HAL assumes -26 dBFS at 94 dB SPL and applies no A-weighting (`firmware/main/ext/ext_audio.c:168-169`; `nos/hal_ext.h:104-105`). The Noise page still names bands and shows "HEARING RISK / Limit your exposure" at 85 dB (`nos/apps/con_memo.c:942-951`, `:964-977`). Treat the figures as relative only.

12. **Medium. The mic and speaker strap wiring is unverified on the B.** CLAUDE.md says the B external strap "needs its own verified mapping". The pins used here, I2S BCLK 1, WS 2, mic SD 3, amp DIN 5 and amp SD 6 (`firmware/main/ext/ext_priv.h:34-38`), cite `docs/strap_layout.html`. `firmware/main/pins.h` has no strap entries, and sets `BOARD_HAS_AUDIO` 0 for the B. Without the strap, the RECORDER and NOISE pages show "Not connected" (`nos/apps/con_memo.c:868`), so the feature depends on hardware the base board does not carry.

13. **Medium. Every noise sample is journaled to the SD card.** The HAL emits an `SP_MIC` record every 32 ms (`firmware/main/ext/ext_audio.c:172-179`). It is a raw type (`nos/sports.c:56`), so it goes to the `*-sensors.spt` hourly file (`:215-217`, `:287`). That is about 2 KB/s, or 7 MB an hour, while the mic runs, including while the Noise page is shown and during recording. The screen does not say so.

14. **Medium-low. Error copy and empty states conflate causes.** The code comment says the hint shows "SAVED (or NO SD CARD)" (`nos/apps/con_memo.c:880`), but the code draws "SAVE FAILED" for every failure (`:853`, `:482`). The MEMOS empty state reads "No memos" whether the card is missing, the library failed, or the folder is empty (`:929-932`). The only distinction is a transient LIBRARY ERROR toast (`:349`).

15. **Low. Comments give the wrong cadence.** `NOS_FPS` defaults to 40 (`nos/render_config.h:7-8`), and nothing in the tree overrides it. The header says a waveform column every 100 ms (`nos/apps/con_memo.c:4-5`). The code uses 3 frames, which is 75 ms (`:267-268`). The meter comment says 10 Hz (`:298`), but the code samples every 3 frames, about 13.3 Hz (`:714-715`). A comment at `:55-56` says 30 fps.

16. **Low. The 10-minute cap is silent.** `REC_MAX_MS` is 10 minutes (`firmware/main/ext/ext_audio.c:42`). It stops and saves the file (`ext_audio_files.inc:186`). The screen shows no cap or countdown, and the save shows only "SAVED" (`nos/apps/con_memo.c:499-507`).

17. **Low. Some service API has no UI path.** `svc_memo_number()` has no caller (`nos/apps/con_memo.c:511`). `svc_memo_stop()` is used only by the Recorder itself (`:580`), so the background-stop API has no entry point outside the screen.

18. **Low. Every open rescans the folder.** Each enter and each save walks the whole folder and stats each WAV (`nos/apps/con_memo.c:175-219`, `:571`, `:486`). Cost grows with file count, and only 24 entries are kept.

19. **Low. No daylight saving.** Labels use the manual `tz_min` offset (`nos/apps/con_memo.c:203`; `nos/prefs.c:35`, default 330 at `:154`). The file stamps are UTC (`con_memo.c:597-600`).

20. **Low. Placeholder memos in the simulator only.** `memo_load` seeds three fake memos only in the host build (`nos/apps/con_memo.c:256-259`). The device starts with the real folder.

21. **Low. Volume 0 is silent with no notice.** The amplifier turns on only when volume is above 0 (`firmware/main/ext/ext_audio.c:284-289`). Playback still shows progress (`nos/apps/con_memo.c:777`). No message explains the silence.
