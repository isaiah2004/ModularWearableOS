# TE routing investigation — 2026-09-19

Target: Waveshare ESP32-S3-Touch-AMOLED-1.43 / 1.43-B, not 1.43C.

## Conclusion

The published schematic provides no TE connection from display connector J9
to the MCU or expansion headers. There is no documented TE GPIO or test pad.
GPIO8 is IMU_INT, not LCD_TE. The current firmware's lack of TE synchronization
matches the documented board wiring, rather than merely an omitted pin setting.
This is a documentation conclusion, not a continuity measurement of this unit.

The schematic shows several unconnected J9 contacts, but does not name their
panel-side functions. A controller die pinout does not identify the corresponding
display-module flex contact. The linked resources do not supply that module
pinout, so no specific solder point can be recommended from these documents.

## Sources inspected

- [Official resources](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/Resources-And-Documents).
- [Linked schematic, page 1](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-Touch-AMOLED-1.43-Schematic.pdf):
  Display/J9, MCU, OUTPUT/J1/J3 and GPIO table. Display signals are QSPI,
  reset, enable and shared touch I2C; no TE net is shown. J9's unused contacts
  have no-connect markers. R9 is an unpopulated OLED_EN link, not a TE jumper.
- [SH8601Z datasheet](https://files.waveshare.com/wiki/common/SH8601Z0_V0.0.pdf):
  TE output pin page 16; timing pages 32–33; TEON page 121; current scan-line
  read RDSCAN (45h) page 132.
- [CO5300 manufacturer datasheet hosted by Espressif](https://dl.espressif.com/AE/esp-iot-solution/CO5300_Datasheet_V0.00.pdf):
  QSPI read protocol page 28; TE pages 31–33; TEON page 170; tear scan-line
  selection page 180; current scan-line read GSL (45h) page 182.
- [Official Demo V3 archive](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-AMOLED-1.43-Demo-V3.zip),
  cross-checked against the user's extracted copy under Downloads.

Downloaded PDFs, extracted text and schematic preview are retained locally in
`build/te-research/`. The CO5300 PDF was already in
`build/display-optimization/CO5300_Datasheet_V0.00.pdf`.

## What the vendor examples actually do

`ESP-IDF/09_FactoryProgram/main/example_qspi_with_ram.c`:

- Lines 47–54 assign CS 9, clock 10, data 11–14, reset 21, enable 42.
- SH8601 init lines 72–73 send `44h = 01 D1` (scan line 465) and
  `35h = 00` (TE output enabled, mode 0).
- CO5300 equivalents at lines 84–85 are commented out, including `TE ON`.
- No TE GPIO input, ISR, edge wait or transfer synchronization is implemented.

ESP-IDF 07_LVGL_Test and 08_LVGL_SDIMG, and Arduino 07_LVGL_Test, have the
same distinction. Enabling controller TE output alone cannot synchronize the
host when that signal is not connected and consumed.

## Feasible next steps

1. **Without board modification:** investigate register readback. Both chips
   document command `45h` returning the current scan line. CO5300's QSPI read
   framing is opcode `03h`, address `{00h, command, 00h}`, followed by data
   returned on SIO0. A successful changing scan counter could support polling
   or phase estimation, with overhead and transfer timing still to be measured.
   Earlier all-FF ID reads do not establish a usable read path; this option is
   unverified and is not equivalent to a working TE interrupt.
2. **For hardware TE:** obtain the exact installed display-module flex pinout
   or manufacturer confirmation of its TE contact/test pad. Then verify signal
   voltage, physical accessibility and continuity before choosing an unused
   MCU GPIO. No wire or GPIO assignment is established by this investigation.
3. **Meanwhile:** coherent transition state and improved update ordering/pacing
   remain useful software work, but cannot be advertised as guaranteed tear-free.

No firmware settings were changed or flashed during this investigation.

## Module-pinout follow-up — 2026-09-21

Found a strong candidate for the missing TE contact: **J9 pin 27**. This is a
pinout-correspondence inference, not identification of the installed module.

The DWO manufacturer specifications for DO0143FMST05 (SH8601/FT3168) and
DO0143FMST10 (CO5300/FT3168) both assign TE output to connector pin 27.
The latter's [official product page](https://www.dwo.net.cn/cn/h-pd-12011.html?fromColId=104)
links its [module datasheet](https://18746902.s21i.faimallusr.com/61/1/ABUIABA9GAAg1tbEvgYox4O_qQc.pdf);
the pin table is on pages 7–8. The
[FMST05 specification](https://18746902.s21i.faimallusr.com/61/1/ABUIABA9GAAgkejBrgYogNP71wQ.pdf)
provides the same positions for these signals:

| Connector contacts | DWO module function | Waveshare J9 evidence |
| --- | --- | --- |
| 3 / 5 | Touch SCL / SDA | Same signals |
| 4 / 6 | Display clock / CS | Same signals |
| 8 / 10 / 12 / 14 | QSPI D3 / D2 / D1 / D0 | Same signals |
| 7 / 9 | Touch interrupt / reset | Unconnected |
| 13 | VPP, leave open | Unconnected |
| 15 / 17 | IM1 / IM0 | High / low selection straps |
| 19 | Display reset | OLED_RESET |
| 21 | Power enable | Pull-up; unpopulated R9 to OLED_EN |
| 25 | I/O supply | 3V3 |
| 27 | **TE output** | **Unconnected** |
| 29 / 30 | VBAT supply | 3V3 |

This correspondence is substantially stronger than matching screen size or
controller alone. There are module-specific differences: FMST10 describes pin 2
as unused and pin 23 as internally unused, whereas Waveshare connects these to
ground and 3V3 respectively. FMST05 uses pin 2 for DC and pin 23 for VCI.
Do not treat either specification as confirmation that a replacement module
is electrically interchangeable.

No Waveshare source located names either DWO part as its installed module.
The public product photos inspected do not reveal a readable module part number.
LilyGO documents use of FMST10 on its own board, which does not establish
Waveshare's supplier or the revision in this watch.

Next confirmation: read the part number printed on this watch's display flex,
or obtain Waveshare's confirmation of J9-27's panel-side function. If matched,
pin 27 becomes a specific place to verify TE with a scope after TEON; it still
has no existing route to the ESP32 and would require a physical connection.
No probe or solder connection has been performed or prescribed as confirmed.

Local evidence: `build/te-research/dwo-a.pdf` (FMST10), `dwo-b.pdf` (FMST05),
their extracted text, `dwo-a-pinout.png`, and Waveshare product photos.

## C-board implementation — 2026-10-22

The sections above describe the B board. The **1.43C routes display TE to
GPIO8**, confirmed by its [official schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43C/ESP32-S3-Touch-AMOLED-1.43C-Schematic.pdf)
and live GPIO interrupts. The initial implementation enabled this by default;
the follow-up below changes the default to off and adds a saved control.

`nos_display_te.c` consumes rising edges with `35h=00` and explicit `44h=0000`
(vertical blank, CO5300 datasheet pages 31–33, 170 and 180). The flush owner
prepares its first DMA chunk, waits for a fresh edge, then streams the frame.
Subsequent stripes and exposure fences do not wait again. A static semaphore
avoids a new heap allocation. A missing edge falls back after 25 ms. Panel
sleep disables the interrupt; wake discards old edges and restores it.
The B build has no TE GPIO configuration or wait.

USB `te` reports readiness, edge count/period, synchronized frames, timeouts
and ISR-to-worker wake latency without waking the screen. `te 0` / `te 1`
disable/enable synchronization. These commands now update the saved preference,
just like the Developer Tools control. `info` also includes `NOSTE`, with
`saved=0|1` alongside the effective `enabled` state.

COM4 validation: approximately **60.6 Hz**, **421 waits / 421 synchronized /
0 timeouts**, maximum measured ISR-to-worker wake latency **244 us**.
Sleep/wake, microphone/speaker, saved Wi-Fi and API key, and preference
checkpoint hash all passed. Both board builds succeeded; 68 host tests passed.

**Performance limit:** the same twelve scrolling gestures averaged 54.87 UI
frames/s with TE disabled and 32.22 with TE enabled. With TE enabled, 71 of
156 painted intervals exceeded the 33.3 ms floor and p95 was 36 ms. The strict
30 fps interval requirement is therefore **not consistently met**. The QSPI
clock remains 40 MHz; full-screen RGB565 payload alone takes at least 21.7 ms,
longer than a panel refresh. Aligning frame starts does not guarantee tear-free
presentation, and no optical tearing measurement was performed.

See [structured verification](performance/C143_TE_SYNC_20261022.json) for build
identities, timing captures, checks and evidence hashes. That initial verification
ended with TE enabled; the follow-up below leaves it disabled.

## Saved TE control and 60 fps investigation — 2026-10-22

**TE is now off on the watch.** Developer Tools has a **TE sync** ON/OFF row,
between Frame rate and Turn all off. Unsupported boards/simulators show N/A.
The choice survives reboot. Existing installations migrate to OFF; new installs
also default to OFF. Turning off the developer overlays does not change TE or
the frame-rate preference.

The preference occupies byte 50 of the existing 52-byte V4 payload, with field
revision 5. It retains the alternating verified SD checkpoint and the existing
NVS/card-absent fallback, dirty retry, delayed-load merge and explicit edit
overlay. Existing overlay bit assignments and wrist provenance are preserved.
The UI owner applies changed TE intent only between completed display frames;
unchanged settings do not reset the measured edge period every frame. An
unavailable GPIO cannot enable synchronization. The controller's TE generator
remains configured for an eventual enable, while OFF disables the GPIO IRQ and
all frame-start waiting.

Both board builds and 61 targeted tests passed. Simulator captures verify the
reflowed page and access to Frame rate. The connected C board identified itself
on COM4 (the B-board workspace instructions' COM3 is absent). The checked flash
used this workspace's C build, verified all required runtime features and
excluded NVS. On-watch taps enabled and disabled TE; a controlled ROM read/reset
after each verified checkpoint proved both choices survive reboot. The final
52-byte preference hash matches the post-migration OFF checkpoint, with the
original brightness, face, radios and 60 fps cap retained. No display faults or
allocation failures were observed.

Repeated twelve-swipe Settings windows reproduced **55.07 UI loop fps with TE
off** and **32.22 with TE on**. Painted-interval p95 was **20 ms / 36 ms**. The
disabled window had zero TE waits. These windows contain settling/idle frames;
loop fps is not a count of optical refreshes. These measurements use the current
sharp renderer (`NOS_RENDER_SHARP=1`), not the motion-blurred reference build.

Further bounded profiles explain the remaining gap:

- Settings uses about 71–75 thousand pixels and 62–64 DMA chunks per painted
  frame. End-to-end work averages 17.95–18.17 ms; the display API averages about
  12–13 ms and C-board byte swapping/copying about 2.2 ms. Raster includes producer
  waiting; these overlapping stage times must not be added together as CPU costs.
- The existing damage-band diagnostic improved Settings from about 54 to 57 loop
  fps in two comparisons (painted p95 24/22 ms to 20/19 ms). The app list improved
  from 47.26 to 57.37 loop fps in one pair (p95 27 to 19 ms). This is a promising
  measured direction, still short of sustained 60 fps and not an optical
  qualification. The diagnostic was restored to OFF after profiling.
- Unlimited produced 67 loop calls/s but only 51.85 painted frames/s across the
  whole window; painted-interval p95 was 22 ms. Removing the cap does not solve
  the display cost. The saved cap was restored to 60.

The next focused driver optimization is to reduce small-transfer overhead.
Each 8-row `esp_lcd_panel_draw_bitmap` call currently sends CASET, RASET and a
color transfer. The C board needs no rotation, and each 16-row source stripe is
already split across two 8-row DMA buffers. Setting one window for that source
stripe and sending its two buffers with RAMWR/RAMWRC could remove half those
repeated address commands while retaining DMA ownership and the final fence.
The controller documents RAMWRC (`3Ch`); this driver change has **not** been
implemented or measured. Damage-band tuning and a paired byte-swap kernel are
other candidates. All need pixel comparisons and timing measurements before
changing defaults.

There is also a hard payload limit. At 40 MHz quad SPI, RGB565 transfers at most
20 MB/s before overhead: 466 × 466 × 2 bytes takes **21.716 ms**, or at most
46.05 full rectangular writes/s. Even an ideal circular mask would take about
17.1 ms. A full rectangular RGB565 frame at 60 Hz needs at least 52.12 MHz
before overhead. The ESP32-S3's current 80 MHz source/divider cannot supply an
intermediate 50 MHz setting; its next faster setting is 80 MHz. The
[CO5300 timing table, page 107](https://dl.espressif.com/AE/esp-iot-solution/CO5300_Datasheet_V0.00.pdf)
specifies a minimum 20 ns write cycle, so 80 MHz exceeds that rating. The C-board
vendor examples also use 40 MHz. No overclock trial was performed.

The same datasheet documents RGB332 QSPI on pages 22 and 25: 8-bit transfer
could halve full-screen payload to 10.86 ms at 40 MHz, at the cost of 256-color
quantization and a driver/packing change. This is a possible quality tradeoff,
not a performance result or a change made to the watch. Full-color partial
updates and fewer commands are the first candidates to pursue.

See [verification and profiles](performance/C143_TE_SETTING_60FPS_20261022.json)
for identities, exact per-frame measurements, restored state and evidence hashes.
