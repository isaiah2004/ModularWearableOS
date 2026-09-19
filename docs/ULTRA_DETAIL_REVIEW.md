# Ultra reference detail correction

2026-09-15. Uses the newly attached close-up reference and the earlier saved
source/render history. Neither this workspace nor the original smartwatch
directory contains Git metadata; originals for this pass are retained under
`build/ultra-detail/before`.

[Before](assets/ultra-review/pass-6.png) ·
[After](assets/ultra-detail.png) ·
[Enlarged inspection grid](assets/ultra-detail-grid.png) ·
[3,457 steps / 100% battery / day 28](assets/ultra-detail-reference-values.png)

## Changes

- **Dial markers:** square-ended batons with four length/width combinations:
  quarter-hours 7.2/3.0, other hours 5.8/2.4, neighboring minute ticks 4.6/1.15,
  and shorter minute ticks 3.5/0.85, in the existing 240-unit coordinate system.
  A fine offset contact shadow lifts the marks off the dial.
- **Compass:** N/E/S/W occupy the central tick lane, with three ticks omitted
  around each letter. Letters face inward, following the reference. Remaining
  ticks vary in length, with stronger five-minute marks.
- **Hands:** ten-corner frames have narrow heels, flared shoulders, tapering
  rails and clipped tips. Only the rails and short hub stems are drawn: there
  is no fill inside the frame. The forecast, digits, gauges and intersecting
  hand remain visible through each aperture. A fine shaded side under the
  white rail gives the frame a bevel.
- **Weather:** the semicircle expands from radius 68 to 70 toward 12. Its
  label baseline moves from 58 to 60; icons move from 43 to 46. A charcoal
  inner fan contrasts with the black time-label band, with a recessed rim and
  small cloud shadows. The final placement retains clearance below 12.
- **Steps:** removed STEPS, shifted the count toward the top of its lane and
  placed the shoe just before the arc at the same radius, 90.5. The first
  inspection caught a shoe/9 collision; the final shoe angle is -1.30 and the
  arc spans -1.18 to -0.42 radians, preserving a visible gap above 9.
- **Battery/depth:** a chamfered orange battery with an attached terminal and
  narrow highlight replaces the capsule/dot symbol. Small contact shadows
  also lift the large digits, step track and score well.

## Validation

- Inspected native 466×466 renders and the enlarged nine-tile grid. Generated
  four second-hand positions for default, reference, zero and maximum values.
  Zero and 100,000 steps, 0/100% battery and days 1/31 remain readable.
- `tools/test.ps1` passes, including exact one-step updates, cache/latching,
  full-frame versus 16-row stripe equality, translated composition, font
  metrics and the existing damage-command limit below 64.
- Added an aperture regression in `tests/ultra_test.c`: sample both openings
  at 10:09, 02:42 and 07:18 and compare the pixels to the cached dial. It checks
  preservation of contrasting artwork, not merely a uniform dark interior.
- `sim/build.ps1` and `sim.exe --face 4` passed; the full OS capture is saved
  in `build/ultra-detail/sim/ultra.png`. The simulator still reports pre-existing
  unused-code warnings in `app_music.c` and `app_wifi.c`.
- `tools/build.ps1` passed with ESP-IDF 5.5.4. `watchfw.bin` is 0x625f50 bytes,
  leaving 59% of the application partition free. Logs are retained under
  `build/ultra-detail`.

Static detail remains in the existing cached dial; no new framebuffer or
per-frame font work is added. On-watch redraw latency, AMOLED appearance and
power are unmeasured. Time, date, battery estimate and steps retain their
existing HAL sources; weather, health, score and compass remain the documented
demonstration values.

## Approved device deployment

After inspecting the visible simulator, the user approved flashing on
2026-09-15. The exact reviewed firmware was written to COM3, confirmed as
ESP32-S3-Touch-AMOLED-1.43-B, MAC `28:84:85:b0:8b:30`, 16 MB flash / 8 MB PSRAM.
The bootloader, partition table and application transfer hashes verified;
NVS was preserved. Application file SHA-256:
`87e36868594abec3f93d41ac9d7398b1b53a4473b0d13419cb2602d4e574deda`.

Ultra was selected and saved as face 4. The clock had been unset, so it was
synchronized to the PC's local time and IST offset (+330 minutes). An explicit
reboot reached `watch os running (466x466)` without a panic/assert/reset loop
in the 18-second capture. Post-reboot USB queries confirmed face 4, watchface
app 0, a valid RTC and working touch/IMU detection. Home was opened for viewing.

Evidence: `build/ultra-detail/flash.log`, `ultra-boot.log`,
`device-validation.jsonl` and `pre-flash.jsonl`. `dist/v1` remains the prior
package; this deployment used the verified `firmware/build` segments directly.

Reproduce the main preview with:

```powershell
./tools/preview_ultra.ps1 -OutputDirectory build/ultra-detail/final
python tools/ultra_review.py build/ultra-detail/final/preview2.png build/ultra-detail/final/tiles
```
