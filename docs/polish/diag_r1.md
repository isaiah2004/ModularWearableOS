# Diagnostics — critique round 1

Sim: `build/nos_sim_crit_diag.exe`. Scripts and outputs: `build/nos/critic/diag/` (c_all, c_fail, c_flow, c_done, c_abs2, c_mod, c_push).

## Verdict
The engineering is strong. Tests run in the background while the person does the cards, the thresholds are real, there are 0 late frames in every script (worst frame 24.4 ms, a full-screen bus transfer), and `--damagecheck` passes (c_all 1086, c_fail 558, c_flow 2738 frames identical). The results hero build is clean. The main promise does not hold yet: "tell in 2 seconds what's broken". With 4 fails the first screen says `9/13 PASSED · 4 FAILED`, and the fails themselves sit in fixed list order, up to 5 scrolls down. Every card change and RE-RUN also has a visible glitch: the outgoing card is reset before it fades, and RE-RUN cuts to a blank screen. Two flow problems: an unattended card keeps the screen on forever, and three stray touches silently fail the touch panel.

## Issues

1. **[P1] Results: failures are not surfaced.** fail/l_done.png shows the hero `9/13`, `PASSED ●4 FAILED`, an unlabelled 15-dot strip, and then BOARD › Button PASS. The 4 FAIL rows (Touch, Haptics, Compass, Heart rate) are at list positions 2, 7, 14 and 15: s1.png, s4.png, 4 drags of 300 px. The ring-vs-disc difference in the 8 px strip dots can't be read at arm's length. **Fix:** when `nfail > 0`, insert a `Failed` header plus the failed rows straight after the summary (same row component), then Board / Strap modules with the fails left out. The first fail row then lands at y≈336 on the first screen.
2. **[P1] Card hand-off resets the outgoing card before it fades.** `card_done → go_view → card_reset()` clears `C` while the old view still draws for 0.15 s. Touch: on the final hit the 2nd target turns back into a hollow ring and target 1 re-lights white (all/touch_to_haptic.png f60–f64). Felt/Heard: the pressed YES (C_SEC) snaps back to white for a frame, then fades (haptic_to_spk.png f84→f86, spk_to_list.png f108→f110). The lit bell disc and ring also cut out on the same frame. **Fix:** keep a copy of `C` for `pview` (or call `card_reset()` only once `nos_t ≥ G.build_t`). Hold the flash (`flash_b`) and touch state through the fade.
3. **[P1] RE-RUN cuts to blank.** flow/rerun.png: at f2610 the whole list disappears in one frame, and f2611–f2616 are empty apart from the title, which also pops in at full at y 70 because it had been scrolled off. Cause: `go_view` moves `G.build_t` into the future, so `lbuild()` hides every row of the fading list. **Fix:** draw the outgoing list with build progress 1 (skip `lbuild` when `k < 1`). Reset the scroll after the fade, and let the title arrive with the card build.
4. **[P1] An ignored card keeps the screen on forever.** `diag_step` calls `nos_keep_awake()` whenever `G.running`, and a card waits with no timeout (set_diag.c:449, 577). flow/ignore_60s.png is still on card 1 after 60 s. **Fix:** keep awake only while some row is `ST_RUN`. If the screen sleeps on a card, abort, as for leaving. Optionally, after 30 s a card resolves as `NO ANSWER` (FAIL, why = "NO ANSWER").
5. **[P1] The touch test fails people, not panels.** Every `IN_DOWN` that isn't on the target counts as a miss: taps on the text, and the down of a back swipe (flow/backtouch.png, grey miss dot at x≈20). After 3 misses the card moves on to card 3 with no word (fail/c_touch_after3.png) and logs `TOUCH FAIL MISSED BY 292 PX`. **Fix:** don't count downs at x < 40 (the back-swipe edge) or more than 120 px from the target. Show a miss as the target ring stepping to C_PRI for 0.2 s plus `MISSED` at y 330. Fail only on 3 misses within 120 px (a real offset), with why = `OFF BY 37 PX` in the offset's direction.
6. **[P2] Two builds back to back when the tests are already done.** In the healthy run, every test resolves during the cards (the last one, Light, about 0.2 s after the list arrives). The list builds `[ TESTING ] 14 OF 15`, fades it, then builds the hero (all/spk_to_list.png frames 7–23, about 0.5 s). **Fix:** if no test is still running when the list's build starts, build the hero directly.
7. **[P2] Counter hard cut, and two different denominators.** `14 OF 15` → `15 OF 15` swaps in one frame (done/done.png f241→f242). The running count includes absent parts (out of 15), but the result leaves them out (`9/13`). **Fix:** count only tested parts (`8 OF 13`) and change the value with `dot_text_morph`, or a 2-level step.
8. **[P2] No feedback when a run or card resolves.** `run_finish` and `card_done` call no `nos_feedback`. **Fix:** FB_TICK on every card resolve; when the run finishes, FB_SUCCESS (0 fails) or FB_ERROR (any fail).
9. **[P2] Long waits don't say what they're waiting for.** HR takes 8 s and LTE up to 25 s. All that moves is one 5 px dot blinking at the end of the strip (fail/fail_done.png), and `14 OF 15` stays still. **Fix:** under `[ TESTING ]`, show the label of the part still running (`HEART RATE`) in C_DIS at y+108 in place of the count.
10. **[P2] All-absent summary hides 9 missing modules.** abs2/list_done.png says only `6/6 PASSED`. **Fix:** add `· 9 NOT CONNECTED` (C_DIS) after PASSED when absent > 0.
11. **[P2] The absent row page isn't the standard absent state.** abs2/detail_absent.png shows `[ NO MODULE ]`, READING `--`, and the CHECK thresholds for hardware that isn't there. Modules uses `ui_absent(icon, "Not connected", body, "<NAME> MODULE")` (set_modules.c:813). **Fix:** use the same call here.
12. **[P2] Sub-line copy.** `+1 s IN 2.2 s` (a PASS that reads like a fault) → `ON TIME`. `OFF BY 0 PX AT MOST` → `0 PX OFF`. Row `26.0°  915 hPa` → `26.0°C  915 hPa` (the row page says °C). `SKIPPED` → `SKIPPED, NOT TESTED`.
13. **[P2] PASS and FAIL rows look nearly the same.** Same font and size; FAIL is C_PRI against PASS in C_SEC, and the status dot is 10 px (fail/s4.png). **Fix:** PASS value in C_DIS, and a 12 px status ring for fails. Most of the fix comes from #1.

P3:
- Lit bell/speaker disc (96 px C_SURF2) cuts off in one frame at 0.6 s: step it off over 2 levels.
- Button card: the white 96 px pressed disc disappears on release before the fade (btn_to_touch.png f31→f32). Hold it through the fade.
- Row page: `[ PASS ]` is the same size as the readings, `308 LUX` repeats the READING line, and all 9 lines are the same caps size (all/row_detail.png).
- Card hints (`Tap the bell to buzz again`) are sentence-case body text; §8 says hints are `ui_label` caps.
- `1 OF 4` → `2 OF 4` fades out and back in; morph the digit instead.
- Engine (not this app): the push dim tints the pressed C_SURF2 pill to (24,20,24) then (16,20,16) (push/push_003.png).

## Already excellent (keep)
- Background tests during the cards; test clocks advance only on screen; streams power down when their test resolves; the `DIAG …` log lines.
- One red only (the dot by `N FAILED`, plus one on a FAIL row page); fails are white rings, not red.
- Results hero: TESTING fades over 5 frames, then the hero builds with `DOT_REVEAL_RANDOM` 0.4 s (done/done.png f242–f257). Rows stagger 34 ms.
- The held side button shows a white disc with black icon; the list's pressed pill lasts ≥ 3 frames; a half back-swipe cancels without aborting.
- 0 late frames everywhere, still screens cost 1.5 ms, damagecheck PASS.
