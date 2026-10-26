# Directional menu blur

Launcher and static Settings row caches now store separate upward/downward variants at each of the three exposure lengths. A normalized causal response builds to a delayed peak and decays, making travel direction visible in the cache. This is an authored temporal-response approximation, not a measured individual visual system or a calibrated panel response.

Launcher horizontal placement uses a curve-centroid approximation for each exposure level. It preserves glyph shapes; it does not integrate the full curved trajectory per pixel. Dynamic Settings controls retain their existing symmetric blur treatment.

## Validation and benchmark

The benchmark runs launcher and Settings in ROM and runtime-bake modes, with a 6-second virtual warm-up, held drags and flings in both directions. Each case has 551 virtual frames and three repetitions. `tools/benchmark_directional_menus.py` preserves scripts, frame CSVs, motion CSVs, memory reports, and executable hashes under `build/directional-benchmark/`.

| ROM mode measurement | Before | After |
| --- | ---: | ---: |
| Launcher modeled maximum frame | 25.750 ms | 20.267 ms |
| Settings modeled maximum frame | 34.183 ms | 30.408 ms |
| Launcher measured PC raster median / p95 | 167 / 401 us | 124 / 396 us |
| Settings measured PC raster median / p95 | 155 / 448 us | 116 / 253 us |
| Launcher owner PSRAM | 84,632 B | 89,240 B |
| Settings owner PSRAM | 653,424 B | 656,088 B |
| ROM pixel assets | 2,053,700 B | 2,927,044 B |

No sampled candidate modeled frame exceeded 33.333 ms. Settings still had four modeled frames above 25 ms per run. Allocation failures were zero. These are **simulator estimates and host measurements**, not a device frame-floor or no-drop result. The SDK configuration is 240 MHz; ESP32-S3 supports 80/160/240 MHz settings, so this is not 200 MHz qualification. At the simulator sign-off, no firmware had been flashed or device benchmark performed. The subsequent hardware results are recorded below.

For the final ROM repetition, all 310 motion samples in each app matched before/after exactly (frame, section, pose and step). Painted frame counts were unchanged: launcher 308, Settings 309. Launcher painted pixels changed from 39,833,732 to 39,993,328; Settings from 33,408,272 to 32,840,832. History processing and nonzero blended-pixel work remained active. The CSV does not provide a distinct native-cache hit/fallback/sharp counter, so these checks establish equal trajectories and retained rendering activity, not an independent per-frame proof of native directional coverage. Pixel and source-selection correctness are covered separately by implementation tests and actual cache captures. Work differs with the new blur footprint, so the timing comparison is an app-workload comparison, not an equal-pixel kernel speedup.

The PC timings are sequential cohorts and may be affected by host clock/load drift. History overlaps raster timing and must not be summed with it. Runtime-bake preparation counts depend on host time budgets; no equal-work preparation-speedup claim is made.

Strict simulator compilation passed. ESP-IDF 5.5.4 firmware compilation and final incremental rebuild passed; `firmware/build/watchfw.bin` is 5,928,944 bytes with 25% of the OTA app slot free. This build includes the existing working-tree changes. COM3 was enumerated only.

Detailed evidence: `build/directional-benchmark/comparison.json`, `coverage-check.json`, `REPORT.txt`, `before/results.json`, `after/results.json`, `firmware-build.log`, `firmware-final-build.log`, and `final-hashes.json`.

Final correctness checks passed: all five native-menu motion/launcher-ROM tests; independent 64-bit scalar references for all six directional Settings crops; incremental publication, invalidation and release for 36 entries x seven levels; optional shutter prefix/pixel/lifecycle checks; and kernel impulse polarity, early-peak and flat-field invariants. The production captures at identical poses have identical sharp up/down output and distinct moving up/down output.

Review artifacts: A:/makerspace/smartwatch2.0/build/pov-study/production.html (actual renderer), index.html (weighted full-trajectory reference and four-frame eye response), with reproducible capture_production.py, production_page.py, simulate.py, and page.py in that directory. The retinal response is an illustrative linear gamma kernel, not a nonlinear saturation or calibrated perception model.

## Device verification — 20 October 2026

**The directional build was flashed successfully, but the recorded device tests fail the hard 30 FPS floor.** The simulator timing improvement did not carry over to these hardware measurements.

The ESP-IDF 5.5.4 rebuild passed. COM3 device identity, the previous image and full active slot were checked before the app-only write. Exact image and whole-slot readback passed, and all bytes outside the application slot were unchanged. One captured reset booted the new image; fresh runtime reports confirmed the expected ELF, 240 MHz CPU, compatible native ROM assets (2,927,044 bytes), and enabled history processing.

| Scene | Baseline maximum interval | Directional maximum interval | Baseline >33.333 ms | Directional >33.333 ms |
| --- | ---: | ---: | ---: | ---: |
| Launcher | 30.811 ms | 35.594 ms | 0 / 353 | 3 / 1,048 |
| Settings | 36.497 ms | 40.058 ms | 8 / 345 | 27 / 1,024 |

Each sweep used 290-pixel up/down swipes at 290 and 1,800 px/s, with 1.8 seconds of coast. There was six seconds of preparation after each fresh app entry. The baseline has one sweep per scene (eight windows total); the directional image has three (24 windows total). Per-frame tracing and the detailed pipeline probe were off. These are consecutive painted DMA-completion intervals; idle gaps break the sequence, and optical panel presentation was not measured.

The first directional sweep alone recorded the same maxima, with 3 / 349 launcher intervals and 15 / 342 Settings intervals over the floor. Launcher misses occurred in the first 290 px/s downward window. All 27 Settings misses occurred in 1,800 px/s windows; their counts by sweep were 15, 7, and 5. The runs used the same gesture protocol, with different prior uptime and realized trajectories; the comparison does not isolate a causal kernel-speed change.

The directional run painted 2,126 frames and recorded 2,072 adjacent painted intervals, with 30 floor violations. History processing remained active for all painted frames, and 1,210 frames reported blending. This instrumentation does not independently prove final-visible native directional coverage on every frame. No crash or allocation failure was observed. Existing physical touch-driver errors persisted: 2,347 / 7,420 reads during the baseline and 7,719 / 20,986 during the directional run, with last error 259. Scripted touch injection does not validate physical input reliability. No input-driver change was made here.

The watch was returned to HOME with normal autosleep, tracing/probe disabled, the USB diagnostic guard released, and COM3 closed. Visual quality remains for the user's on-watch review. Broader OS scenes, cold-cache stress, optical presentation, and a 200 MHz budget are outside this measurement.

Flashed ELF SHA256: `77d030c2d9c731f400317b483d6b5524a62ff8731660941bcad331f1515b68e4`. App BIN SHA256: `8c7df1b5bf8bdb374d80fce513e0b001c43f90b0238d9ab4ed8cb888cff63da2`. Exact sources, configuration, binaries, and source hashes are archived in `build/directional-device/artifact/`.

Evidence: `build/directional-device/RESULTS.txt`, `results.json`, `before.json`, `after.json`, `flash.log`, `flash-attempt.json`, `boot.log`, `postflash.jsonl`, and `build/recovery/directional-blur-20261020.json`. The reproducible host measurement and analysis scripts are `build/directional-device/measure.py` and `report.py`.
