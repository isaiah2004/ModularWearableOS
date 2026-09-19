# Battery fit and a 24-hour power budget

Researched 2026-09-13 for **Waveshare ESP32-S3-Touch-AMOLED-1.43-B**, the
metal-case version of the plain 1.43 board. This is not the 1.43C, C6, 1.54,
1.75, or 2.06 board.

## Recommendation

**Do not buy a battery on the assumption that the original metal case has a
usable battery compartment.** I did not find a firsthand report identifying
a battery that fits inside this exact case. The official FAQ actually directs
the case version to connect a battery through the exposed **VBAT and GND**
terminals; it describes the small battery connector for the bare-board version.
[Waveshare FAQ, battery question](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.43/FAQ/)

For a practical wearable, my preferred direction is a **deeper replacement
back or separate strap compartment for a protected 500-800 mAh single-cell
LiPo**, a charger matched to that cell, and a separate standby-power update.
That is a proposed mechanical/electrical design, not a confirmed fit or a
24-hour runtime guarantee. If keeping the case completely unchanged is
essential, the next useful measurement is the actual free space inside it.

There is also a concrete charging issue: the published schematic programs a
much higher charge current than the small cells considered below allow.
Resolving that matters as much as finding the right dimensions.

## What the enclosure evidence establishes

The official B outline drawing gives:

| Measurement | Published value | Meaning |
|---|---:|---|
| Outer diameter | 48.00 mm | Complete outside envelope |
| Total thickness | 12.30 mm | Includes the display assembly |
| Case thickness shown in side view | 11.60 mm | Outside dimension, not free depth |
| Rear screw-centre spacing | 31.50 x 23.10 mm | Hole centres, not a battery pocket |

The official 3D archive contains a STEP model and drawings for the **bare
board**, but the **B folder contains only an outside-outline PDF**. It does not
give an internal case cross-section, free battery depth, or usable pocket
dimensions. Subtracting the bare-board thickness from the outside case thickness
would not account for the backplate, components, connector, or insulation and
would not establish a safe fit. The archive and both drawings were downloaded
and inspected. [Official resource page](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.43/Resources-And-Documents/),
[official CAD archive](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-Touch-AMOLED-1.43-3DFile.zip)

To select a pack, measure a clear **length x width x depth with the back closed**,
including the SD card, screw posts, connector and cable bend. Compare with the
finished protected pack's maximum dimensions, not just its pouch code. Allow
the cell maker's required expansion clearance and insulation; the backplate
must not squeeze the pouch.

## What people online have actually demonstrated

Searches covered the exact plain/B model name, battery/LiPo/fit/case/runtime
terms, common pouch sizes, Reddit, GitHub, Arduino Forum and Hackaday. This is
a bounded search of accessible public material, not proof that nobody has
ever fitted a battery.

| Finding | What it does and does not prove |
|---|---|
| A Reddit user confirms a 400 mAh battery fits a Waveshare **1.54-inch LCD** device; another comment mentions support suggesting 103030/1000 mAh. | A real fit report for another enclosure. It does **not** verify the 1.43-B. [Discussion](https://www.reddit.com/r/esp32/comments/1tfnvwx/trying_to_identify_battery_for_waveshare_s3_154/) |
| PasswordVault uses the exact non-C 1.43 board. | Its hardware list explicitly says USB powered, no battery. [Project](https://github.com/pdhruv93/PasswordVault) |
| Capsule Radar has a 1.43 port with battery-related software. | The README does not identify a battery pack fitted to the B case. Its linked printed enclosure is for the original 1.75 board. [Project](https://github.com/ON6PH/capsule-radar-1.43) |
| Other exact-model GitHub projects include numrow-touchpad, esp32-gallery, Civic boost gauge and FlightDot. | The accessible READMEs did not provide a verified B-case battery model, internal dimensions, or measured watch runtime. FlightDot mentions battery operation without identifying a fitted pack. |

**Result: no verified internal battery fit or 24-hour wear test for this exact
board/case was found.** The similar-looking 1.43C is especially easy to confuse
with it because that product is sold in a battery-included variant.

## Electrical compatibility: verify before attaching a small cell

Waveshare specifies a normal **3.7 V nominal / 4.2 V fully charged** lithium
cell. Its product page calls the bare-board connector **MX1.25**, while the FAQ
calls it **PH1.25**. These labels are inconsistent; do not substitute a common
2.0 mm JST-PH plug on the strength of the name. The case version's documented
connection is VBAT/GND. The separate RTC connector is not the main battery
input. [Product specification](https://www.waveshare.com/product/esp32-s3-touch-amoled-1.43.htm),
[Waveshare FAQ](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.43/FAQ/)

I visually checked the schematic's BAT block at enlarged resolution:

- **U6 is ETA6098**.
- **R23 is 82 kOhm**, connected between U6 pin 8 **ISET** and ground.
- The main battery connector **J7 pin 1 is VBAT; pin 2 is GND**.
- Charge status drives an LED; no I2C charge-current setting or MCU connection
  to ISET is shown. Exposed VBAT is the same battery net, so using those
  terminals does not bypass the charger.

[Official schematic, page 1 BAT block](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-Touch-AMOLED-1.43.pdf)

The **manufacturer's ETA6098 V1.1 datasheet, page 2**, explicitly specifies
**82 kOhm -> 2 A typical fast charge**; page 5 repeats this in the Fast Charge
description. This is a stated datasheet value, not an equation inferred from
another charger. It also specifies **4.2 V** regulation, **200 mA** precharge
and **130 mA** charge termination. Those latter parameters mean simply
extrapolating a larger ISET resistor for a tiny cell is not a complete charging
design. [ETA6098 manufacturer datasheet](https://www.eta-semi.com/wp-content/uploads/2022/03/ETA6098_V1.1.pdf)

**This describes the published circuit, not measured current on this watch.**
The physical board revision and fitted R23 have not been checked. Actual
current depends on cell voltage, USB supply and thermal regulation. A normal
500 mAh cell rated for 500 mA charging must not be assumed compatible with
that published 2 A setting; a protection board does not make it a 2 A cell.

Before using onboard USB charging, verify the fitted circuit and charge
current against the chosen cell's data sheet. Otherwise use a cell-appropriate
external charger with the cell disconnected from the watch while charging, or
design a proper isolated replacement charging path. Adding another charger
in parallel with the existing one is not a solution. Verify polarity at the
actual connector/terminal, rather than relying on red/black wire positions.

## Plausible cells to dimension a design around

These are **sourcing and enclosure-design candidates**, not drop-in fit
recommendations. All need the charging issue above resolved. No purchase,
seller contact, shipping quote or physical fit test was performed.

| Candidate | Published dimensions and capacity | Assessment |
|---|---|---|
| [THOR 402030](https://thorbattery.com/product/thor-402030/) | 200 mAh; bare cell 4 x 20 x 30 mm; protected pack approximately 4 x 20 x 32 mm, plus specified tolerances | Thin exploratory option, but a demanding 24-hour power budget. Maker lists maximum charging 200 mA and maximum continuous discharge 200 mA; radio peaks also need checking. |
| [Koosay 603030](https://koosay.com/products/603030-500mah-3-7v-premium-grade-raw-materials-lithium-ion-ncm-battery-pack-for-toys-boats-multiple-certifications) | 500 mAh; finished pack listed as 6 x 30 x 32 mm; final PCM/wire arrangement may change dimensions | More useful capacity for a deeper back. Maker lists 250 mA standard and 500 mA maximum charging. Connector/protection configuration and final dimensions require confirmation. |
| [Adafruit 1578](https://www.adafruit.com/product/1578) | Protected 500 mAh; 4.75 x 29 x 36 mm; JST-PH lead | Documented retail option, maximum charging 500 mA. Its approximately 46.2 mm rectangular diagonal leaves very little room inside a 48 mm outer shell; suitable for evaluating a new enclosure, not evidence of original-case fit. |

For India, [ArrowTechCart lists a 402030/200 mAh pack](https://arrowtechcart.com/products/402030-3-7v-200mah-lithium-polymer-rechargeable-battery)
and [Hubtronics has a corresponding listing](https://hubtronics.in/402030-3.7v-200mah-lipo-battery?tag=Battery).
These are retailer leads, not the same manufacturer-qualified pack above.
ArrowTechCart displayed contradictory stock labels and Hubtronics showed zero
stock at retrieval. Neither establishes a B-case fit. An accessible Robu
500 mAh listing did not expose enough specification detail to qualify it.
I cannot identify a verified, immediately purchasable Indian drop-in battery
from this evidence.

A size code also does not guarantee capacity: an [EEMB LP503030-PCM-LD
manufacturer specification](https://macrogroup.ru/upload/iblock/806/l9ka6axlc8lwl5b0dbf2xov43s2g5l33/LP503030.pdf)
lists **350 mAh nominal**, whereas another manufacturer's similarly named
pack lists a different capacity. Buy by an exact finished-pack specification.

## What a day of power requires

Use **average current measured at the battery**, with USB disconnected:

`runtime_hours = usable_capacity_mAh / average_battery_current_mA`

For initial planning, using 80% of nameplate capacity reserves 20% for cutoff,
aging and uncertainty. This is an engineering allowance, not a measured
capacity or guaranteed aging model. If measuring current on the 3.3 V rail
instead, account for battery voltage and regulator efficiency rather than
directly dividing battery mAh by that rail's current.

| Battery | Planning capacity | Maximum average battery current for 24 h |
|---|---:|---:|
| 200 mAh | 160 mAh | 6.7 mA |
| 350 mAh | 280 mAh | 11.7 mA |
| 500 mAh | 400 mAh | 16.7 mA |
| 800 mAh | 640 mAh | 26.7 mA |
| 1000 mAh | 800 mAh | 33.3 mA |

The inspected firmware runs the CPU at **240 MHz**, dims after 30 seconds and
switches the display off after 120 seconds. Screen-off is not MCU sleep:
motion sampling and other tasks continue. The pickup/wrist-wake work uses the
existing cached motion sampling; it does not itself implement an IMU interrupt
and CPU sleep strategy. These are source-code findings in
`firmware/main/main.c`, `firmware/main/hal_esp32.c` and `firmware/sdkconfig`.

For scale, Espressif lists **32.9-47.6 mA at 240 MHz WAITI**, depending on
peripheral clocks, even in modem sleep. Those are chip measurements, not this
watch's battery current; the datasheet warns that embedded-PSRAM variants can
consume more. Display, sensors, SD, RF activity and conversion losses add
other loads. This is why a 200-500 mAh battery should **not** be expected to
last a day with the present always-running architecture.
[ESP32-S3 datasheet, section 5.6.2](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf)

For example, a 500 mAh pack with the 80% allowance lasts 8 hours at 50 mA or
4 hours at 100 mA. These are conditional calculations, **not measured runtime**.
A future 24-hour target could budget 1 hour at 120 mA and 23 hours at 8 mA:
304 mAh/day, within the 400 mAh planning budget. Achieving that 8 mA standby
state requires implementation and measurement; it is not a claim about the
current device.

## Work needed to qualify an all-day build

1. Measure the case clearance and choose an exact protected pack with a
   suitable charge/discharge rating. Use a deeper back if the stock case
   cannot accommodate it without compression.
2. Verify the physical charger revision and match charging to the cell.
3. Measure battery-side current with radios off, BLE connected, Wi-Fi active,
   screen off, Pip animated and SD inserted. USB-connected tests cannot
   substitute for this because USB changes the power path.
4. Add automatic CPU frequency scaling/light sleep, motion-interrupt wake and
   appropriate sensor/radio duty cycling. Preserve responsive 60 FPS rendering
   during interaction. Verify display/DMA clocks and wake latency after adding
   power-management locks. [Espressif power-management guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/power_management.html)
5. Run an actual 24-hour wear test with a recorded use pattern, notifications,
   wrist wakes and walking. Report observed runtime and remaining capacity.

The current evidence supports a feasible development direction, but does not
yet support promising an internal drop-in battery or a day's runtime.
