# Power hardware and firmware audit

Audited 2026-10-20. No electrical settings, firmware behavior, or device flash changed.

## Identity and certainty

The configured target is **Waveshare ESP32-S3-Touch-AMOLED-1.43-B**: `firmware/sdkconfig:602-603` selects B and disables C; `firmware/main/pins.h:36-66` supplies its pin map. This is strong evidence of the intended board, not a physical inspection of the fitted IC markings or resistor values. Confirm the actual PCB revision and component markings before electrical modifications.

This board does **not** have a programmable, register-based PMIC. The [official schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-Touch-AMOLED-1.43.pdf), page 1, separates U6 **ETA6098** charging, U3 **MP1605GTF-Z** conversion, and a discrete USB/battery supply path. The local copy `build/extension-hardware-review/waveshare143.pdf` was also visually inspected. Do not apply AXP/other board PMIC drivers to this hardware. In particular, the C variant has different battery sensing and a charger-status GPIO; its definitions cannot be borrowed for B.

Primary manufacturer references:

- [ETA Semiconductor ETA6098 V1.1 datasheet](https://www.eta-semi.com/wp-content/uploads/2022/03/ETA6098_V1.1.pdf), pages 2-3 and 5-6.
- [Monolithic Power Systems MP1605 datasheet](https://www.monolithicpower.com/en/documentview/productdocument/index/version/2/document_type/Datasheet/lang/en/sku/MP1605/document_id/1049/), and [manufacturer product page](https://www.monolithicpower.com/en/mp1605.html).

## What is available on this board

| Capability | Hardware and firmware disposition |
|---|---|
| Charging | ETA6098 handles precharge, CC/CV, termination and restart autonomously; firmware initialization is unnecessary. R23 is marked 82 kOhm on the schematic, corresponding to 2 A typical fast charging. The datasheet also gives 4.2 V regulation, 200 mA precharge, 130 mA termination and 160 mV recharge hysteresis. These are published values, not measurements on this watch. |
| Charging protection | Input overvoltage protection and thermal regulation/shutdown act in hardware. They are not firmware telemetry. The datasheet does not provide a cell-temperature/NTC input. |
| Current adjustment | ETA6098 supports external ISET resistance and an optional PWM injection circuit. This board wires ISET only through R23 to ground. No MCU control or charging-disable interface is routed. Exploiting PWM needs a hardware redesign and cell-specific validation. |
| Charge status | STAT drives LED2 through R6 from VBAT. It does not connect to an ESP32 GPIO in the B schematic. Firmware cannot read charging/completion, faults or no-battery detection through an I2C register. STAT is low during charging and high impedance after completion; even a future status wire needs supply context. |
| Main converter | MP1605 provides autonomous conversion, light-load power saving, current limiting and thermal protection. Its EN is pulled to VCC through R10, and its feedback voltage is resistor-set. Neither is wired for MCU adjustment. Converter shutdown and voltage scaling are unavailable in software. |
| Supply selection | D2 and Q1 form the discrete USB/battery path. No firmware selection, ship-mode latch or battery-disconnect control is shown. |
| Voltage sensing | BAT_ADC is VCC divided by R12 200 kOhm and R17 100 kOhm, not an independent cell monitor. `pins.h:64-65` correctly uses GPIO4/ADC1 channel 3 and a factor of three. USB can power this rail independently of cell state. |

The autonomous features are already usable without register programming. There is no missing PMIC initialization sequence to unlock them. Published charging-current compatibility remains an electrical issue: see [BATTERY_FIT.md](BATTERY_FIT.md), lines 81-109. No current or voltage change is proposed here.

## Prioritized firmware findings

1. **Respect power validity throughout NOS.** `hal_esp32.c:184-250` already qualifies samples, waits for stable evidence, expires old observations and exposes `battery_valid`/`external_power`. However, `nos/apps/set_battery.c:39-47` always formats a percentage and computes filled cells, and lines 80-90 draw both even when invalid. Its supply-state expression also labels an unknown supply as ON BATTERY. The boot estimate is 50 (`hal_esp32.c:155`), so this can present a placeholder as a measurement. Hide invalid percentages/cells, display USB or unknown as appropriate, and retain an explicit approximation label for valid voltage-based estimates.

2. **Apply the same rule to every battery surface.** `sys_power.c:168-171`, `sys_control.c:317,424,480`, `sys_home.c:464`, and `set_settings.c:589` consume the legacy estimate without checking validity. Audit these together with the Battery screen so headers, faces and the power sheet agree. `nos/svc.c:885-888` carries validity but names external supply `charging`; it cannot establish actual charge flow on B. Preserve that distinction in UI and companion-facing status (`svc.c:147`), preferably with an explicit external-power field/state.

3. **Treat ordinary screen-off as an active standby mode.** `nos_main.c:1322-1362` keeps OFF/AOD housekeeping and motion detection at 50 Hz. `nos_power.c:49` enables 80-240 MHz dynamic frequency scaling but explicitly disables automatic light sleep; the PM locks are released at idle (`nos_main.c:1340,1359`). This already reduces CPU frequency, but does not enter MCU low-power standby. The next substantial power work is a measured IMU interrupt/wake design, coordinating touch polling, radios, SD workers and timekeeping before attempting light/deep sleep. It is not an ETA6098 feature switch.

4. **Define power-off honestly and measure its residual load.** `nos_main.c:532-554` sends panel sleep, drains work, saves settings and enters ESP32 deep sleep with BOOT wake. `board.c:617-640` unmounts SD and disables IMU sensing. Main rails remain energized because there is no software battery disconnect. Sleep current therefore includes board peripherals and divider/converter losses. Measure battery-side current in active, AOD, display-off and explicit deep-sleep states; do not infer watch runtime from the charger's quiescent specification.

5. **Keep gauge estimates bounded by evidence.** `hal_esp32.c:160-175,229` uses a generic LiPo voltage curve, a stored calibration offset and fixed load allowances. These do not measure capacity, battery current, cell health or charging progress. Calibrate against the actual cell and battery-side voltage/current; avoid promising accurate remaining runtime from this implementation. A real fuel gauge or independent battery/status sensing would require additional hardware.

## Validation scope

Read firmware and active configuration, opened the manufacturer references, and visually checked the schematic power/BAT blocks and net connections. No battery, resistor or physical PCB measurement was available; no charge-current, voltage, rail, or flashing operation was performed. The highest-confidence immediate fixes are power-state/percentage presentation. Standby savings and charging compatibility require measurements on the actual board/cell combination.
