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
