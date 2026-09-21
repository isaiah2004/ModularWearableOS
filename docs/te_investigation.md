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
