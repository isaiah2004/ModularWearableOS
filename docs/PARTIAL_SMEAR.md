# Partial smear simulator experiment

2026-10-20. Active worktree: A:/makerspace/optimizations. No device flash.

Replaces normalized native menu cache convolution with a directional maximum envelope: head 100%, first trailing sample 95%, shelf 90%, last sample 80%. This interpretation preserves bright detail but can merge text strokes; it is not a physical exposure model. Existing temporal history remains active. Dynamic Settings controls keep their prior blur.

Cache exposure lengths are now 15, 30, 60 instead of 4, 16, 48, in both directions. The existing half-width support convention is retained, so maximum trail reaches 7, 15, 30 pixels, bounded by row crops. A continuous speed mapping reaches those levels at 75, 150, 300 px/s; there is no discontinuous threshold. Launcher head remains on its original curve without the old normalized-kernel centroid correction.

Solar-system hold ring radii: 44 -> 79.2, 52 -> 93.6. Dot diameters: 4 -> 7.2, 3 -> 5.4. Background, text offset and stripe culling bounds expanded too.

Validation: strict simulator build passed; source-generated ROM asset fingerprints verified; four native menu motion/carry/control tests passed; focused smear impulse, polarity, head/shelf, constant-field, speed mapping and interpolation-bound checks passed. Matched launcher/Settings captures at 75/150/300 px/s are in build/smear-study/{before,after}; capture.py reproduces them. Hold-ring screenshots use the same touch position/time. build/nos_sim_smear.exe is the interactive candidate. Tests were updated for the deliberately changed kernel.

Authored reference pixels increased to 3,935,620 and deduplicated pixel ROM to 3,393,598 bytes. Simulator timing is not hardware acceptance. This is a visual experiment pending user review, not evidence that the hard 30 FPS hardware floor passes.

## Revised profile: 85 / 90 / 95 / 100 / 95
The current experiment replaces the flat trailing shelf with a ramp from 85% at the tail through 90% and 95% to the 100% head, plus a one-pixel 95% leading edge. The ramp scales across each existing cache support. Lengths and speed mapping are unchanged. Exact five-sample impulse and scaled-ramp checks pass. Regenerated ROM assets, strict simulator build, and native menu motion tests pass. Current executable: build/nos_sim_smear859095.exe; captures and prior comparison executable: build/smear-profile-859095. The previous simulator was closed and this executable launched. No firmware flashed.

## CPU dial and revised 45 / 60 / 75 / 100 / 30 profile

The current smear uses a piecewise-linear trailing ramp through 45%, 60%, 75% to the 100% head, with a one-pixel 30% leading edge. Cache lengths remain 15/30/60. Speed mapping now explicitly uses M_FPS, preserving 75/150/300 px/s at any configured cadence.

The simulator Cost HUD has a rotary effective-CPU dial spanning 20–480 MHz in 1 MHz increments. Drag upward to increase, downward to decrease; the wheel adjusts 1 MHz per notch, or 10 with Shift. Click Reset for 240 MHz. This is a model multiplier, not a request to change the PC's clock or a list of supported ESP32 clock modes.

Pace to model (default ON in interactive mode) schedules each frame at max(configured frame period, estimated frame work). The existing message-aware timer handles input while waiting. CPU class costs scale with reference_MHz / dial_MHz; QSPI transfer and unassigned empirical overhead stay fixed. These simplifications do not model memory contention, cache misses, interrupts or real hardware clock scaling exactly. Use comparable hardware gestures to fit an effective rate, not to claim device qualification.

The branch currently targets 40 FPS / 25 ms with a 30 FPS acceptance floor. Scripted runs remain virtual-clock runs; --cpu-mhz changes their cost estimates, not scripted wall-clock pacing. --cost-hud opens the panel; --no-cpu-pace disables modeled pacing. Changing the dial clears the rolling history/session counts so rates are not mixed.

Evidence: build/smear-cpu-dial. tools/test_sim_cpu_dial.py checks 80 vs 240 MHz CPU scaling, unchanged bus time, and invalid CLI arguments. Native UI capture confirms the rotary panel is visible. The bundled native-automation transport was unavailable, so the window was inspected using Win32 PrintWindow. No device firmware was flashed.

## Developer Tools and current simulator defaults

Current smear profile: 15 / 30 / 65 / 100 / 30. The simulator now starts and resets its effective CPU dial at 180 MHz. Cache lengths and the 75/150/300 px/s mapping are unchanged.

Developer Tools is available in the app launcher and through the simulator's bottom-row Developer tools button. Session-only toggles: loop FPS, frame statistics, touch position, and screen name; Turn all off clears all four. The simulator also has direct FPS overlay, Frame stats and Touch overlay buttons. Defaults are off, including after restart. Overlays incur rendering work; disable them for clean performance comparisons. Raster statistics in the simulator are modeled CPU costs; on hardware they are host-reported raster duration, which can include pipeline waits.

The HUD shows measured live engine-loop FPS, painted frames per second, and the model-predicted cadence separately. Idle scenes can have a high loop rate and low painted-frame rate. Neither is an optical panel-refresh measurement. Developer readouts refresh twice per second and exclude temporal history in their text bounds for legibility.

HUD width increased to 360 pixels; long status messages wrap inside the panel. Minimum HUD height and section spacing prevent the readouts from colliding. Verified the reported Settings status message wraps completely in the native window.

Validation: strict 105-source simulator build; regenerated launcher ROM assets with the new app; four native menu motion tests; focused smear checks; developer toggle/all-off/hit-bound/touch/cadence checks; CPU scaling tests. Captured developer states (all off, FPS only, all on, cleared) under build/developer-tools/review. Active executable: build/nos_sim_devtools_final.exe. No firmware flashed.

## Larger segmented solar-system hold loader

The hold loader now uses PROG_SEGMENTS, the same primitive as the zoom dial. Both radii and thicknesses increased another 75%: main 79.2 -> 138.6 px, thickness 7.2 -> 12.6 px; outer 93.6 -> 163.8 px, thickness 5.4 -> 9.45 px. Both stages have 32 segments with a growing leading segment. Expanded background, label offset and stripe-culling bounds cover the larger footprint. Hold thresholds/actions are unchanged.

Strict simulator build passed. Visually checked the initial ring, outer-stage progress, and release cleanup in build/hold-ring-large/review. Simulator restarted as build/nos_sim_hold_large.exe with the solar-system face and cost HUD. This ring revision has not been flashed.

The subsequent size adjustment halves the segmented loader's radii and thicknesses: main 69.3 px / 6.3 px, outer 81.9 px / 4.725 px. Background, label placement and culling bounds were reduced accordingly. Strict build and hold-stage/release captures passed; simulator restarted as nos_sim_hold_half.exe. Not flashed.

Current smear revision: 10 / 20 / 40 / 100 from tail to head, with no leading sample. Cached trail generation now samples ages 0..support only. Cache lengths, speed mapping, 180 MHz simulator default, Developer Tools and half-size segmented hold loader are retained. Rebuilt ROM assets and strict simulator build passed, as did the focused impulse/polarity/leading-edge checks and four native menu motion tests. Simulator restarted as build/nos_sim_tail_only.exe. This revision has not been flashed.


## Five-level simulator experiment (2026-10-20)

Profile: 10 -> 20 -> 40 -> 50 -> 100 -> 20. Four equal trailing segments retain the 100% head; one leading pixel is 20%. Cache exposure widths are 15, 30, 60, 90, 120, in both directions; actual trailing support remains half the labelled width, bounded by the existing row canvas. Corresponding speeds are 75, 150, 300, 450, 600 px/s with continuous interpolation.

Simulator: `build/nos_sim_five_levels.exe`, restarted with cost HUD at default 180 MHz, launcher open. Actual C renderer comparison captures: `build/five-level-study/production_*.png`; reproducible harness in the same directory. No device flash.

Validation: strict 105-source simulator build and generated asset fingerprints pass; standalone profile/ladder test passes; fractional/prepared tiles, carry kernels, and actual Settings carry tests pass. Expanded immutable bundle contains 1,107 descriptors and 6,470,169 deduplicated pixel bytes. Exporter frees completed rows after writing them to disk to respect its memory budget.

Known experimental limitation: two all-resident mutable-fallback tests fail at existing memory ceilings (Settings controls full runtime cache and launcher nonstandard-font prewarm). Default ROM-backed rendering works; memory limits have not been relaxed to mask this. This experiment is not yet validated for device flashing.


## Extended halos and two-pixel leading edge (2026-10-20)

Profile now 10 -> 20 -> 40 -> 50 -> 100 -> 40 -> 20. Settings blur output adds 60 pixels on either side of the source row without moving sharp artwork. Launcher bake canvas expands from 92 to 212 rows, with matching scratch capacity. Draw bounds admit the halos; overlapping prepared sprites merge component-wise ink, preserving adjacent trails instead of overwriting them with blank pixels. Inverted palettes use the opposite envelope.

Strict simulator build passes; simulator restarted as `build/nos_sim_halo.exe` with Settings open. Profile tests, 9.4 million independent native pixel checks, overlapping-halo preservation, and every Settings padded level against the independent reference pass. Captures and comparison harness are under `build/halo-study`. Known all-resident fallback memory limitations from the preceding experiment still apply; memory budgets unchanged.

Device build reached partition-size validation and failed: image exceeds the existing 0x780000 app slot by 0x1ebec0 bytes (2,014,912 bytes). No flash performed, no partition changes. A smaller on-device asset representation is required before this five-level padded-cache experiment can be flashed.


## Four-level bounded-row fit check (2026-10-20)

At user request removed 120 px and extended halos; levels now 15/30/60/90 with the 10/20/40/50/100/40/20 profile unchanged. Source row clipping is restored. Strict simulator build, profile tests, and independent native pixel tests pass. Restarted simulator as `build/nos_sim_four_levels.exe`. Actual device image is 8,092,688 bytes, exceeding the 7,864,320-byte app slot by 228,368 bytes. Conditional flash was not performed because this variant still does not fit. No partition or watch changes.
