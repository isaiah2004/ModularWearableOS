# Hardware and source record

Target: **Waveshare ESP32-S3-Touch-AMOLED-1.43-B**. Waveshare documents B as the
metal-case option of the plain 1.43, with the same board mapping. This is not
the 1.43C AI/audio board used by the older firmware.

## Sources used

Retrieved 2026-09-12:

1. [Board documentation and pin table](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43?variant=ESP32-S3-Touch-AMOLED-1.43-B).
2. [Official resources](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/Resources-And-Documents)
   and its [Demo V3 archive](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-AMOLED-1.43-Demo-V3.zip).
3. [Waveshare ESP-IDF guide](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/ESP-IDF).
4. [Official Waveshare component repository](https://github.com/waveshareteam/Waveshare-ESP32-components),
   `display/lcd/esp_lcd_sh8601`, version **2.0.0**, component metadata commit
   `9daffb7168b3c093a330da700988a22092294eef`. Its license is distributed with
   the managed component. `idf_component.yml` pins it exactly.

For this board the resources page links its first-party application examples
as a ZIP, rather than a dedicated B-board GitHub repository. The community
repositories listed on that page were not used as the hardware reference.

Archive SHA-256:
`0d407137e944059426dd44b34e9c153f058bf6d00d540faf912f7805a33cd67e`.
The downloaded original and extracted examples are under ignored `reference/`.
They are reference material, not build dependencies.

Files consulted within `ESP32-S3-AMOLED-1.43-Demo/ESP-IDF`:

- `09_FactoryProgram/main/example_qspi_with_ram.c`: both panel init tables,
  display enable, controller selection, pixel alignment, CO5300 six-column gap.
- `09_FactoryProgram/components/read_lcd_id_bsp/read_lcd_id_bsp.c`: pre-QSPI
  single-line D0 read of register 0xDA. Port adds explicit CS framing and releases
  GPIO state before assigning the pins to SPI.
- `09_FactoryProgram/components/touch_bsp/touch_bsp.c`: FT3168 address 0x38,
  normal-mode write, touch count/coordinates and unrouted reset/interrupt.
- `09_FactoryProgram/components/qmi8658c/`: WHO_AM_I, auto-increment, range/ODR,
  output register locations. Port configures +/-8g and +/-512 dps at 125 Hz;
  cached readings are sampled at approximately 50 Hz.
- `09_FactoryProgram/components/pcf85063/`: RTC address and BCD calendar layout.
  Port adds calendar/oscillator validation, UTC storage, and deferred sync writes.
- `09_FactoryProgram/components/adc_bsp/`: ADC1 channel 3 and **3:1** divider.
- `04_SD_Card/components/sd_card_bsp/` and factory SD component: one-bit SDMMC.
  Port disables the example's auto-format and uses 20 MHz instead of high-speed.

## Pin mapping

| Function | GPIO / setting |
|---|---|
| QSPI CS / CLK | 9 / 10 |
| QSPI D0 / D1 / D2 / D3 | 11 / 12 / 13 / 14 |
| AMOLED reset / enable | 21 / 42 |
| LCD TE | Not routed; GPIO8 is the IMU interrupt |
| Shared I2C SDA / SCL | 47 / 48 |
| FT3168 touch | 0x38; no reset/interrupt GPIO |
| QMI8658 IMU | 0x6B; INT GPIO8 |
| PCF85063 RTC | 0x51; INT GPIO15 |
| SDMMC CLK / CMD / D0 | 41 / 39 / 40 |
| SD D3 / SPI CS | 38; not used in one-bit SDMMC mode |
| System voltage | GPIO4, ADC1 channel 3, 3:1 divider |
| BOOT / RESET | GPIO0 active low / EN |
| Native USB D- / D+ | 19 / 20 |
| Flash / PSRAM | 16 MB / 8 MB octal |
| Resolution / pixel format | 466 x 466 / RGB565 |

`firmware/main/pins.h` is the authoritative mapping for this port. Flash is
configured for 16 MB; the 0xEF0000-byte factory app occupies 0x10000..0xEFFFFF.
NVS and PHY precede it, and 1 MiB at 0xF00000 is reserved (not mounted). SD
capacity is independent of flash. No OTA partition or updater is enabled.

## First physical inspection

Verify the silkscreen/product is the B/plain model before flashing. Record the
`PANEL` boot line. Waveshare chooses SH8601 for ID 0x86 and CO5300 otherwise;
the fallback needs confirmation if an unexpected ID such as 0x00 is read.
Check colours, circular bounds, and a touch at the top/right/bottom/left of the
display. The first physical report showed the UI 90 degrees clockwise. Output
now rotates 90 degrees counterclockwise in software: UI `(x,y)` maps to native
`(y,465-x)`. FT3168 coordinates use the inverse `(465-y,x)`; USB taps remain in
upright UI coordinates. The native CO5300 gap stays on the controller's x axis.
The pinned driver does not implement `swap_xy`, so this uses a DMA-safe rotated
stripe buffer rather than an unsupported controller setting.

An external mic/codec must use a separately verified pin assignment. In
particular GPIO38..42 are used by this board's SD/display hardware and cannot
inherit the old C-board I2S wiring.
