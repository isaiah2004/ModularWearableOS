---
title: "Chat"
id: app-chat
tags: [nos, os-map, app]
status: stub
updated: 2026-10-23
---

# Chat

Source: `nos/apps/con_chat.c` (314 lines). Backend: `firmware/main/voice_chat.c`, `nos/voice_history.c`, `nos/voice_policy.c`, `nos/voice_chat.h`. Read-only source review of the 1.43-B build; nothing built, flashed or run on a board. Evidence is `repo-relative/path:line`.

## Summary

Chat is the NOS push-to-talk voice assistant screen. The launcher entry is "Chat" with the mic icon (`nos/apps/registry.c:13`); the screen's internal name is "VOICE CHAT" (`nos/apps/con_chat.c:314`). The screen owns no codec, network or storage. It draws a cached snapshot from the voice service and forwards taps to `voice_chat_*` calls.

The real service is in `firmware/main/voice_chat.c` and is compiled only under `CONFIG_WATCH_BOARD_143C` (`voice_chat.c:7`). Its `#else` branch (`voice_chat.c:596-615`) is a set of stubs. The 1.43-B build leaves `CONFIG_WATCH_BOARD_143C` unset (`firmware/sdkconfig:603`), so on B:

- The launcher lists Chat with no board guard (`registry.c:13`).
- Every voice call returns 0 or an empty snapshot.
- The screen shows "Voice chat needs the C board mic and speaker." (`con_chat.c:131-132`).
- TALK, NEW CHAT, the tier chips and the AUDIO controls do nothing.
- The screen stays keep-awake while open (`con_chat.c:311-314`).

On the C board the same screen is a working OpenRouter push-to-talk client: Whisper transcription, a tier-selected chat model, and Kokoro speech output. It keeps up to four saved text conversations in NVS, and audio stays in RAM. The C behaviour is documented in `docs/VOICE_CHAT.md`. It is described here from source only and was not re-verified on hardware.

Chat does not read the services hub: there is no `svc_*` getter in the file (see [[Services hub]]). Its neighbours are [[Connectivity and voice]] (the OpenRouter engine), [[Engine and scheduling]] (the UI tick, the `voice` worker, navigation-driven cancel), [[Power and battery]] (keep-awake), [[Storage and persistence]] (NVS keys), [[Hardware and drivers]] (the C mic and speaker that B lacks), and [[Shell and faces]] (the launcher that opens it).

Docs versus code, checked:

- `docs/os-map/systems/Connectivity and voice.md:131` says the voice file is compiled "only" on C. The file is built on every board (`firmware/main/CMakeLists.txt:38-39`). Only the implementation is gated. The stub is what runs on B.
- `docs/VOICE_CHAT.md:25` says leaving the Chat stack cancels the active turn. The code cancels only on a real leave. Opening RECENTS, AUDIO or DELETE covers Chat, and a covered screen is not cancelled (`nos/nos.c:206-207`, `con_chat.c:79`).
- `docs/VOICE_CHAT.md:33` says audio and text are sent to OpenRouter when a turn is submitted. This matches the worker (`firmware/main/voice_chat.c:343-351`, `:381-383`).
- `firmware/main/voice_history.h` says the checkpoint is "bounded below one 4 KiB page". The fields sum to 3,892 bytes, which matches `docs/VOICE_CHAT.md`.

## Features

On 1.43-B as built:

- Launcher tile "Chat" (`registry.c:13`), fifth of 28 entries.
- Tier chips LOW, MEDIUM and HIGH, labelled with cents per minute 1c, 5c and 10c from the static table (`nos/voice_policy.c:4-8`, `con_chat.c:123-130`). LOW is highlighted because the tier defaults to 0 (`voice_chat.c:122`).
- "RECENTS 0" opens an empty SAVED CHATS page (`con_chat.c:121-122`, `:170-191`). The page says "Your chats will appear here" (`:185-187`).
- "AUDIO" opens CHAT AUDIO with "0%" volume and UNMUTE. The sensitivity row, the spoken-replies toggle and the slider are drawn but inert (`con_chat.c:232-244`).
- "NEW CHAT" and "TALK" buttons (`con_chat.c:164-166`). Both do nothing.
- Status line "ADD YOUR KEY OVER USB" (`con_chat.c:160`) and cost line "0.000c / TEXT" (`con_chat.c:167`). Both are fixed text.
- Idle progress bar with REC, SEND, THINK and VOICE labels, all unlit (`con_chat.c:101-116`).

As built for the C board (`CONFIG_WATCH_BOARD_143C`):

- Push-to-talk. TALK starts a turn (`con_chat.c:304`, `voice_chat.c:523`). Capture runs for up to 20 s (`voice_chat.c:29`), then SEND or CANCEL (`con_chat.c:297-302`).
- Stage bar REC, SEND, THINK, VOICE with byte or sample progress (`con_chat.c:101-116`, `voice_chat.c:65-69`).
- Transcript of 5 lines at a time with vertical drag scroll (`con_chat.c:135-139`, `:291-292`).
- Tier selection, which changes the model used for the next turn (`voice_chat.c:129-133`).
- Speaker volume: slider, minus, plus and mute (`con_chat.c:237-239`, `:257-258`, `voice_chat.c:146-155`).
- Microphone sensitivity LOW, NORMAL and HIGH at 24, 30 and 36 dB (`con_chat.c:240-242`, `voice_chat.c:156-167`).
- Spoken-replies toggle. Turning it off stops speech in progress (`con_chat.c:243`, `voice_chat.c:156-167`).
- Saved chats: up to 4, newest first. Tap a row to resume it, or DEL then Delete or Keep (`con_chat.c:170-231`, `voice_history.h:5`, `voice_history.c:40-64`).
- Cost meter in cents for the session, with "~" when the figure is an estimate (`con_chat.c:167`, `voice_chat.c:71-76`).

## Reads

On 1.43-B, what actually runs:

- `voice_chat_get()` is the stub at `voice_chat.c:600`. It zero-fills the snapshot and sets a status string nobody displays. The screen calls it from `refresh()` (`con_chat.c:62-73`, `:67`) at most every 80 ms (`:65`), and on enter (`:77`).
- `voice_chat_history_list()` returns 0 (`voice_chat.c:609`). `transcript()` then sets `saved_count` to 0 (`con_chat.c:49-51`).
- `voice_profile_get(i)` is the real policy table (`nos/voice_policy.c:9-10`). It feeds the tier chips (`con_chat.c:124`) and history rows (`:181`).
- `hal_millis()` for the refresh throttle (`con_chat.c:65`, `:86`). `nos_t` is engine time for the entry animation and bar cell (`con_chat.c:86-88`).
- `nos_leaving_covered()` decides whether leaving cancels (`con_chat.c:79`).
- No `svc_*`, `prefs_*` or `hal_store_*` identifier appears in the file. The only other calls are `ui_*`, `dot_icon_anim`, `nos_*` engine calls, and `voice_*`.

On the C board, the getters the stub replaces:

- `voice_chat_get` copies the session view, then `nos_prefs.volume` (`voice_chat.c:125`), the archive's `speak`, `mic_gain` and `last_volume` (`:125-126`), and the storage flags (`:127`).
- `voice_chat_history_list` copies archive chats (`voice_chat.c:168-173`).
- Init reads NVS namespace `wos` key `voice_hist` directly (`voice_chat.c:98-105`) and loads the config blob `voice_chat` (`:109-112`).
- A turn checks `hal_status(HAL_FEAT_WIFI)` (`voice_chat.c:510`) and reads `nos_prefs.volume` (`:516-517`). Codec I/O goes through `firmware/main/voice_audio.c`.

## Writes

On 1.43-B, nothing is persisted:

- Taps call stubs that return 0 (`voice_chat.c:601-612`). The only effects are the screen's RAM state (`s->pressed`, `s->offset`), the navigation stack (`nos_push` and `nos_pop`, `con_chat.c:202`, `:209`, `:303`, `:306-307`), a haptic tick (`con_chat.c:277`, `:309`), and toasts (`con_chat.c:204`, `:208`, `:296`, `:299`).
- The screen's own struct `s` (`con_chat.c:12-21`) is allocated on first enter with `nos_mem_alloc(..., "chat.ui")` (`con_chat.c:76`, `nos/psram.c:29`). It is never freed, so it is held for the session.

On the C board:

- NVS namespace `wos`, key `voice_hist`: the 3,892-byte checkpoint, written by `voice_chat_maintenance()` (`voice_chat.c:199-211`).
- The shared watch volume `nos_prefs.volume` through `prefs_save()` (`voice_chat.c:146-152`), which writes the `nosprefs` blob.
- Archive fields `speak`, `mic_gain` and `last_volume` (`voice_chat.c:151-153`, `:159-161`), which set `history_dirty` for the checkpoint.
- New saved chat and turn append (`voice_chat.c:426-437`, `nos/voice_history.c:40-64`). Resume (`voice_chat.c:174-190`). Delete (`voice_chat.c:191-197`).
- Outbound HTTPS to `openrouter.ai`: the recorded WAV as base64 for Whisper (`voice_chat.c:338-351`), the chat completion (`:354-376`), and speech synthesis (`:377-418`).
- The OpenRouter key and model config are written only by USB `chat <field> <value>` (`voice_chat.c:524-538`, `:539-595`). The screen cannot write them. USB `chat wifi` also writes the Wi-Fi preference and starts a connection (`voice_chat.c:583`).

## Background behaviour

On 1.43-B:

- The screen has no task and no timer. `voice_chat_init()` is a no-op (`voice_chat.c:599`), `voice_chat_maintenance()` returns at once (`voice_chat.c:613`), and cancel does nothing (`:603`). Nothing from Chat runs with the screen closed or off.
- Keep-awake. `scr_chat` and its three child screens set `SCR_KEEPAWAKE` (`con_chat.c:311-314`). While one is on top, the idle sleep timeout is suppressed (`firmware/main/nos_main.c:2080-2084`, `nos/power_policy.h:19`). A display-off request is also reversed on the next loop, because the PWR_OFF branch sees a keep-awake top and calls `go_state(PWR_ON)` (`nos_main.c:2091-2096`). The same holds for the AOD branch (`:2121`). The display therefore stays lit while Chat is open, even though B's Chat does nothing.

On the C board:

- The `voice` worker task (`voice_chat.c:447-502`) is created on the first request at priority 3 with a 10,240-byte stack in PSRAM (`voice_chat.c:519`). It is never deleted and blocks on `ulTaskNotifyTake` between turns (`:450`). The engine's priority table lists it with the other workers (`docs/os-map/systems/Engine and scheduling.md`).
- Per turn: a 120 s deadline (`voice_chat.c:451`), a 60 s deadline per HTTP request (`:232`), and read timeouts of 5 s and 10 s (`:245`, `:253`). Cancellation is cooperative: `cancelled()` (`:56`) is checked between blocking calls, so a stalled header wait can delay cancel by up to its timeout.
- The codec is opened at the start of each turn and closed at the end (`voice_chat.c:452`, `:491`). The copied API key in `s->session` is zeroed after each turn (`:491`).
- A real leave of the Chat screen calls `voice_chat_cancel()` (`con_chat.c:79`), which sets a flag. The turn ends with status "Cancelled" (`voice_chat.c:492`). A covering child screen (RECENTS, AUDIO, DELETE) does not cancel (`nos/nos.c:206-207`, `:316`, `:468`).
- Screen-off: the keep-awake rule above means the display cannot go off while Chat is top. A turn therefore runs to completion or is cancelled by a real leave.
- Restart and deep-sleep requests (power sheet modes 1 and 2) go through `shutdown_guard()`, which cancels the turn and refuses the request (`firmware/main/nos_main.c:731-734`, `:754-757`; the toast at `:757` is the misleading one listed in Gaps). Display sleep (mode 0) does not check voice at all (`:754`).
- OTA is refused while voice is active (`firmware/main/ota.c:59`). Preference writes are deferred while voice is active (`nos_main.c:2098`, `:2110`).
- `voice_chat_maintenance()` runs inside the main loop's `mutate` block (`nos_main.c:1986`), which comes before the PWR_OFF branch. So a history checkpoint can commit while the display is off, when the loop is not transitioning and not touched.

## Persistence

On 1.43-B: nothing is written from Chat. The stub returns before any store call.

On the C board, namespace `wos` (`firmware/main/hal_esp32.c:177-186`, `:277`):

- `voice_hist`: the `voice_history` struct, 3,892 bytes, with magic `0x56434831`, a version field and an FNV-1a checksum (`nos/voice_history.c:6-34`). It holds up to 4 chats of 2 turns each. Input is kept to 128 bytes and reply to 320 bytes per turn, titles to 40 bytes, plus tier, cost, speak, mic gain and last volume. It holds no audio. Writes are sealed, saved, read back, and compared (`voice_chat.c:203-208`). A failed save stays dirty and retries every 5,000 ms (`:210`).
- `voice_chat`: the `voice_config` struct, about 292 bytes, holding the OpenRouter key, the STT and TTS model names, and the voice name. Written only over USB, with read-back (`voice_chat.c:534-535`). The key is never echoed (`:592`).
- `nosprefs`: the shared watch preferences, which carry the volume. On C this is NVS only, because `BOARD_HAS_SD` is 0 (`firmware/main/hal_esp32.c:231-236`). On 1.43-B the same key routes to SD (`hal_esp32.c:262-269`), but the Chat stub never writes it.

Not persisted: the transcript (`voice_chat.c:28`), the four session turns (`:37`), the tier (`:40`; it is not in the archive, so it resets to LOW at boot), the session cost meter (`:514-515`), and audio (PSRAM buffers freed per turn, `:453-491`). The screen's `s` struct is RAM only (`con_chat.c:76`).

The chat checkpoint is in NVS, not on SD. `hal_esp32.c:236` sends only `nosprefs` and `wifi_cred` to the SD path. The CLAUDE.md note that B settings live on SD therefore does not cover voice history.

## Navigation

Opened from:

- Launcher grid tap, which calls `nos_open` with the iris transition (`nos/apps/sys_launcher.c:968-969`). The list entry is `registry.c:13`.
- Double press reopens the last app through `nos_last_app` (`nos/apps/registry.c:40-47`).
- Serial `open chat`, matched case-insensitively by name (`firmware/main/nos_main.c:924-930`, `:1557-1559`).

From Chat:

- RECENTS pushes SAVED CHATS (`con_chat.c:305-306`, `:311`), unless a turn is running, in which case it toasts.
- AUDIO pushes CHAT AUDIO (`con_chat.c:303`, `:307`, `:312`). The audio screen has the horizontal drag flag for its slider.
- TALK, SEND and CANCEL stay on Chat.
- Back: the button pops any child (`con_chat.c:193-194`). Chat itself has no `SCR_NOBACK`, so the edge swipe-back applies.

From SAVED CHATS:

- Tap a row to resume it. This calls `voice_chat_history_select` and pops back to Chat (`con_chat.c:199-203`). On B the list is always empty.
- DEL on a row pushes DELETE CHAT (`con_chat.c:183`, `:201-202`).
- NEW CHAT calls `voice_chat_clear()` and pops (`con_chat.c:207-210`).

From DELETE CHAT:

- KEEP pops (`con_chat.c:220`, `:227`). DELETE deletes and pops (`con_chat.c:221`, `:228`).

Leaving Chat for home or the launcher calls `leave()`, which cancels the active turn on C (`con_chat.c:79`; `nos/nos.c:416-418` for the home path).

## Gaps

The 1.43-B items come first. The C items apply only if the voice service is built.

1. [high] Chat is a launcher tile with no working function on 1.43-B. Evidence: `nos/apps/registry.c:13` has no board guard; `firmware/main/voice_chat.c:596-615` are stubs; `nos/apps/con_chat.c:131-132` shows the notice. User impact: an assistant entry that cannot be used.
2. [medium] Dead controls give a haptic tick but no reason. TALK (`con_chat.c:304`), NEW CHAT (`:300`) and the AUDIO controls (`:270-277`) all call stubs and then `nos_feedback(FB_TICK)` (`:277`, `:309`). The tier chips instead toast "Finish or cancel this turn first" (`con_chat.c:296`), which describes a turn that does not exist. User impact: taps look accepted and the cause is hidden.
3. [medium] Copy asks for an action that cannot succeed. The status line says "ADD YOUR KEY OVER USB" (`con_chat.c:160`, because `configured` is 0). USB `chat` on B returns "ERR chat requires C board" (`voice_chat.c:614`, `firmware/main/nos_main.c:1595`). The notice uses internal board jargon, "C board" (`con_chat.c:132`).
4. [medium] Placeholder zeros look like real state. AUDIO shows "0%" and UNMUTE (`con_chat.c:234-238`), but the real speaker level is `nos_prefs.volume`, which defaults to 60 (`nos/prefs.c:141`). The stub never reads it. LOW is highlighted (`con_chat.c:124-125`). RECENTS shows 0 and "Your chats will appear here" (`con_chat.c:121`, `:185-187`). The cost line reads "0.000c / TEXT" (`:167`). User impact: a wearer may read the watch volume as muted, or think chats are being saved.
5. [medium] Keep-awake on a screen that does nothing. `con_chat.c:311-314` sets `SCR_KEEPAWAKE` on Chat and its children. The sleep timeout is suppressed (`nos_main.c:2080-2084`, `nos/power_policy.h:19`), and a display-off request is reversed (`nos_main.c:2091-2096`). User impact: parking on this tile drains battery with no output.
6. [medium] No usable fallback on B. There is no on-device typed path. The typed-prompt route (`voice_chat.c:574`, `chat ask`) exists only on C, and the OpenRouter client is compiled only under `CONFIG_WATCH_BOARD_143C` (`voice_chat.c:7`). B has no mic or speaker (`firmware/main/voice_audio.c:114-121` stubs; CLAUDE.md B hardware table). User impact: a Wi-Fi watch with a touch screen still gets no assistant.
7. [low] The simulator shows a fully working Chat on every target. `sim/nos_voice_sim.inc:20` sets `available=1`, and the canned transcript and reply are at `:52` and `:56`. The file is labelled an offline preview (`:1`). Host screenshots of Chat therefore do not show the 1.43-B unavailable state.
8. [medium, C only] The spend limit resets with each conversation. The allowance is computed from session totals (`voice_chat.c:419-425`) that `voice_chat_clear()` zeroes (`:136-145`). NEW CHAT calls it (`con_chat.c:209`, `:300`). Each fresh conversation gets a one-minute allowance at its tier rate (`nos/voice_policy.c:21-27`). The block message tells the wearer to start a new chat (`voice_chat.c:478`, `:483`). There is no daily or device-wide cap.
9. [medium, C only] Non-English replies lose characters. `con_chat.c:30` replaces every non-ASCII UTF-8 character with "?" in the transcript, although the system prompt asks for the user's language (`voice_chat.c:360`). Saved titles skip this filter (`nos/voice_history.c:52-53`) and are drawn byte by byte (`con_chat.c:180`, `nos/ui.c:103-111`). The exact glyphs depend on the font, which was not checked.
10. [low, C only] The transcript disappears while a turn runs. `transcript()` reads earlier turns only when not busy (`con_chat.c:49-54`). On TALK, `refresh(1)` (`:309`) rebuilds the transcript with no history, and earlier turns return only when the reply lands.
11. [low, C only] Leaving the screen loses the turn. `con_chat.c:79` cancels on any real leave, and the home path does the same (`nos/nos.c:416-418`). The turn is not finished in the background and no notification is sent. A question asked before pressing Home is lost.
12. [low, C only] A reopened chat keeps two turns of context. `VOICE_SAVED_TURNS` is 2 (`nos/voice_history.h:6`). Resume copies only those (`voice_chat.c:178-182`), and the transcript shows only those (`con_chat.c:54-57`). Earlier turns are gone and the model forgets them.
13. [low, C only] The shutdown message is misleading. `shutdown_guard()` cancels the turn (`nos_main.c:734`) and returns 4, which maps to "SAVE RECORDING FIRST" (`nos_main.c:757`).
14. [low, C only] Wi-Fi only, with no phone relay and no offline answer. Requests need station Wi-Fi up (`voice_chat.c:510-511`). The error "Connect Wi-Fi in Settings first" has no path from Chat, so the wearer must leave and fix it.
15. [low, C only] Saved chats cannot be renamed, searched, exported or cleared as a group. Titles are the first 39 bytes of the first question (`nos/voice_history.c:52`, `nos/voice_history.h:16`). The only management is DEL on each row (`con_chat.c:183`, `:201-202`).
16. [low, C only] Push-to-talk only. There is no streamed reply (`voice_chat.c:359`, `stream` 0), no wake or raise-to-talk, no typed input on the watch, and a 20 s cap per question (`voice_chat.c:29`).
