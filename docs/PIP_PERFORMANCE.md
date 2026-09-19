# Pip animation performance

Measured on the confirmed Waveshare ESP32-S3-Touch-AMOLED-1.43-B, COM3,
MAC `28:84:85:b0:8b:30`, on 13 September 2026. The revised firmware is installed
and packaged in `dist/v1`. Pip's appearance, needs, interactions and saved routine
are retained. This report supersedes the earlier 37–46 FPS boot observation in
`research/PET_VALIDATION.md`.

## Measured result

| Animation | Previous median FPS | New median FPS | New sampled FPS range | New CPU raster time |
|---|---:|---:|---:|---:|
| Idle | 43 | 60 | 60–60 | 7.81 ms |
| Petting | 33.5 | 60 | 60–60 | 9.43 ms |
| Grumpy | 28 | 60 | 60–60 | 8.38 ms |
| Play | 29 | 60 | 60–60 | 8.80 ms |
| Dizzy | 40.5 | 60 | 60–60 | 8.67 ms |

These are one-second firmware counters from the same USB benchmark on the same
watch. Each expression is replayed every two seconds before it expires; the
first two seconds are discarded, then approximately eight seconds are sampled.
This is a short, repeated-expression workload, with live IMU and radio services;
it is not a camera measurement or a guarantee for every interaction. Timing,
pose history and idle randomness can differ between runs. Raw measurements are
in `build/perf-baseline.json` and `build/perf-smooth-final.json`.

The subsequent motion-wake release was checked with the same benchmark:
all five states again had median/minimum/maximum **60 FPS** in
`build/perf-wake-retry.json`. Its first attempt timed out on one USB command
after three completed states; cleanup commands still worked. The complete
repeat with raw USB capture passed, followed by the app/face/overlay tour.
The timeout's cause remains unconfirmed and is retained in
`device-validation.json`; it is not counted as a successful benchmark run.

The legacy `render` counter includes some presentation overhead and excludes
stripe-buffer waits. The new `raster` counter measures CPU drawing calls;
`record`, `present`, and `wait` separate the other stages. `p95_ms` is a
millisecond-bucketed 95th percentile of completed-frame intervals for each log
window. The benchmark reports medians of these window statistics, not a global
95th percentile.

## Changes and revisions

1. Raised the CPU from 160 to its rated 240 MHz. QSPI remains **40 MHz** and
   PSRAM remains 80 MHz. No panel overclock is used.
2. Replaced software rounding and square roots in eye scanline bounds with
   cheaper conservative bounds. Pixel coverage and RGB565 blending retain the
   original equations. Opaque eye interiors use span fills.
3. Replaced the angled lid's per-pixel coverage calculations with an opaque
   column span and at most one antialiased edge pixel.
4. Cached eye geometry, colors and particle trigonometry once per frame, and
   shared the exponential easing coefficient across 14 pose properties.
5. Clipped lid rendering and damage to each eye's halo. Previously black masks
   made empty background below the eyes dirty; this particularly hurt play and
   happy expressions. Stable command slots are retained.
6. Removed Pip's full-screen PSRAM background cache. Black is filled directly;
   clock and care text are skipped outside their stripes. Minute changes now
   damage the clock digits instead of invalidating the entire panel.
7. Replaced rounded scheduler sleeps with a microsecond timer for the software
   60 Hz deadline. Slow frames do not trigger bursts of catch-up frames.
8. Released render buffers immediately after rotation into the separate DMA
   buffer, allowing more rendering/transfer overlap. The DMA buffer remains
   protected until its completion ISR. Every frame boundary drains both input
   buffers and the final DMA; large composed redraws use the same barrier.
9. Made continuous USB profiling opt-in (`prof 1`, stop with `prof 0`). It is
   off on boot and the benchmark disables it on exit, avoiding unnecessary USB
   output during ordinary wear. Fixed the no-damage frame path passing
   uninitialized rectangle coordinates to the frame-begin hook.

The first hardware revision reached roughly 57 FPS idle but only 38 FPS play.
That result led to the damage and geometry revision, which brought play to 56
FPS. Particle caching, the simpler background and improved transfer overlap
provided the remaining headroom. Intermediate evidence is retained in
`build/perf-smooth-pass1.json`, `build/perf-smooth2.json`, and
`build/perf-smooth4.json`.

## Validation

- 5,774 exact eye-shape and 4,500 exact angled-lid comparisons against frozen
  references, including fractional edges, translations, clips and buffer guards.
- 2,520 complete-frame fingerprints unchanged across all 21 performances.
  The final renderer also matched clean full rendering through the actual
  16-row stripe geometry, rather than only a host framebuffer.
- Five minute/hour/midnight clock changes matched clean full rendering and
  did not request a full-screen redraw.
- Gesture checks for sustained petting, face swipes, cancelled edge gestures,
  the care footer and return navigation; existing behavior, care, notification,
  activity, power and board protocol tests; three USB command unit tests.
- The final installed image passed 26 app states, 13 face selections, four
  edge menus, twelve notification arrivals, expression commands and care pages.
- A save/reset/restore check retained care values, familiarity and routine;
  RTC, touch and live IMU responded after reboot. NVS was preserved during
  flashing. Previous firmware is in `build/backups/pre-smooth`.

## Reproduction and limits

```powershell
python tools/pip_perf.py --port COM3 --telemetry --output build/perf-repeat.json
python tools/pip_device_check.py --port COM3 --log build/usb-repeat.jsonl
.\tools\test.ps1
```

The panel has no connected TE synchronization signal. Software pacing and
smaller transfers cannot guarantee tear-free panel scanout. Full-screen UI
transitions are a different workload: even the ideal RGB565 wire time for
466×466 at 40 MHz quad data is about 21.7 ms before command, rotation and render
overhead, so this does not claim 60 FPS full-screen transitions. Flash writes,
radio activity and other occasional work can still cause a longer frame.

240 MHz increases active CPU power demand; battery runtime and wrist temperature
have not been measured. Existing dim, screen-off and AOD behavior remains in
place. Wear, walking and battery calibration limitations from the pet study
still apply. Final image hashes and build-specific observations are in
`device-validation.json`; package test flags are matched to those hashes.
