# Sports simulator validation

All checks use this recovered workspace. No watch, serial port, or flash tool is
used. Synthetic sensors and the known-work timing model establish software
behavior; they do not establish physiological accuracy or physical frame timing.

## Reproduce acceptance checks

```powershell
.\tools\test_workouts.ps1
python tools/test_workout_fuzz.py --seeds 100 --actions 1000
```

For an already built executable, use `-SkipBuild`; to run just simulator
acceptance, use `python tools/test_workout_sim.py --sim build/nos_sports.exe`.

The runner creates a fresh evidence directory under `build/workout-validation/`.
It retains its simulator SHA-256, exact scripts, snapshots, frame estimates,
motion audits, screenshots and damage-comparison logs. `--case NAME` selects a
scenario, `--no-media` omits images, and `--skip-damage` omits pixel comparisons.

## Final verified build

The final `build/nos_sports.exe` passed the complete PowerShell workflow after
the last engine, recording-chip and UI changes. Evidence is retained under
`build/workout-checks/26c6e7da33a148279e3b80f4fabeee37/`, including
`sim/results.json`.

- 26/26 acceptance scenarios passed in 32 normal simulator processes.
- 18,561 frames had zero estimated 30 FPS floor misses; the maximum was
  27.897 ms at the default 180 MHz model setting.
- Seven UI workloads contributed 5,180 frames. All seven damage comparisons
  passed against forced full repaint, and all motion audits passed. Four frames
  exceeded the separate 25 ms target; none exceeded the required 33.333 ms floor.
- The optimized strict C suite, separate-process recovery and nine Python
  checkpoint/export tests passed. Navigation lifecycle regressions also passed.
- A second 100-seed public-API stress run passed all 100,000 actions after the
  final engine fix, including 33 clock-wrap seeds. Its source hash matches the
  delivered recorder. See `build/sport-mode-20261020/final-stress/results.json`.

Final simulator SHA-256:
`1980fcd1f2e00dde8d3906485ed7608e29022b2166a63d62b46324b6c15ec473`.

The final review fixes include preservation of fresh sensor values across rapid
LAP/FINISH calls without counting an acquisition twice, refusal to overwrite a
different SD journal after a card swap, and clear Solar recording-chip placement
and paused AOD labelling. Synthetic replays exercise sensor dropouts, valid-time
averages, save/retry, restart recovery and all six profiles. The actual final
showcase captures are in `build/sport-mode-20261020/showcase-final/`.

A final ESP-IDF build-only check compiled the target ABI and background SD worker.
`firmware/build/watchfw.bin` is 7,747,920 bytes, leaving 116,400 bytes in the
7,864,320-byte application slot. No device was connected or flashed. Target code
size is close to the slot limit and should be checked when adding features.

The acceptance timing claim covers the Sports workloads above. A separate tour
verified the recording overlay on all nine home faces and found no motion audit
failures. Its cold Solar-face preparation still has a roughly 114 ms modeled
spike in unchanged Solar code; this tour is not an all-OS 30 FPS certification.
Physical sensor accuracy, SD latency and display timing remain unmeasured.

## Verified checkpoint: round 4

`build/workout-validation/round4/results.json` records 26/26 passing scenarios:
background recording, pause/resume, bounded laps, missing sensors, stale and
invalid measurements, route gaps, stationary coordinates, dateline geometry,
indoor metrics, RTC corrections, display-off recording, card errors and retries,
history capacity, saved and interrupted restart, corrupted-bank recovery,
all six sport profiles, recording-chip navigation, and metric/imperial display.

The 32 normal simulator processes generated 18,540 frames, with no estimated
frame above the 33.333 ms floor for 30 FPS. The largest estimate was 27.897 ms
at the default 180 MHz model setting. Seven UI workloads contributed 5,159
frames and 121 motion observations, of which 118 were judged, with zero motion
audit failures. Seven independent damage checks found all 5,159 frames identical
to forced full repaint. Four UI frames exceeded the separate 40 FPS target of
25 ms; none exceeded the required 30 FPS floor.

Round 4 simulator SHA-256:
`97d868ac0beb3b92f21b8e60b4ca89a67efe289dbb365569c1ac8106a899b39b`.

The native blur-cache `sharp` diagnostic counts cache readiness and is not a
measurement of final visible blur. An independent A/B run disables that optional
cache and compares temporal fallback enabled against disabled: all 28 moving
Details frames blend 11,311–17,599 pixels with fallback enabled, blend none when
disabled, and have different pixel hashes. Saved individual frames visibly show
the temporal trail. See `round4/blur_fallback_0` and `round4/blur_fallback_1`.

Visual inspection confirmed sport-specific metrics, no indoor GPS distance,
error/retry messages, the six-profile grid, history and details, and conversions
such as 18.0 km/h to 11.2 mph and 3:20 min/km to 5:22 min/mi. Display conversion
does not mutate the SI values in the authoritative session snapshot.

The repository PowerShell entry point also passed the optimized engine build,
separate-process recovery, nine checkpoint/export tests, and the full 26-case
simulator suite. Its evidence is under
`build/workout-checks/0ee86f446b014ce3836f655f85cb2447`.

An ESP-IDF 5.5.4 build from this workspace additionally compiled the target ABI
and background SD worker. The resulting `firmware/build/watchfw.bin` is 7,747,696
bytes; the smallest application slot is 7,864,320 bytes, leaving 116,624 bytes.
This was a build only: no serial port was opened and no firmware was flashed.

## Public-API randomized stress

```powershell
python tools/test_workout_fuzz.py --seeds 100 --actions 1000
```

This compiles `tests/workout_fuzz_test.c` separately with `nos/workout.c`, using
`-O2 -Wall -Wextra -Werror`. It does not include implementation source or access
private state. Each seed runs in a new process with a fresh store and has an
independent model for state transitions, active/total duration, cumulative lap
timestamps, history order, and session IDs. Random actions cover rejected calls,
card removal/retry, absent/invalid/stale sensors, counter resets, long service
gaps, and a 32-bit monotonic-clock wrap for every third seed. Every seed must
still save a subsequent valid session successfully.

The first full stress run passed **100/100 seeds and 100,000 randomized actions**,
including 33 seeds crossing the clock wrap. Evidence is retained in
`build/workout-fuzz/acceptance-100x1000/results.json`, with a separate reproducible
log and alternating checkpoint store for every seed. The product source hash is
recorded alongside the test source hash.
