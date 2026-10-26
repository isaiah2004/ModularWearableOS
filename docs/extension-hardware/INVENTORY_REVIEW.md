# Extension inventory for ESP32-S3-Touch-AMOLED-1.43-B

Reviewed 2026-10-17. This inventory maps the proposed strap functions to the
hardware assemblies and OS support they need. Nine extension capabilities have
written drivers; the expanded design adds features that still need hardware
and software integration. The existing strap demo remains unchanged.

The intended existing demo is **`A:\makerspace\smartwatch2.0\docs\strap_layout.html`**, currently previewed at [the strap layout](http://127.0.0.1:8766/strap_layout.html). It is a modified tracked file in the original workspace, SHA256 `3ab43a4908e115adf54d99341318ac40e7b58ad9e109097342ac52674089adc1`. Preserve that newer version. The optimization worktree also contains a copy, but the original is the source for the expanded design below.

The HTML title names the correct B board. Its interactive Three.js view switches between worn and flat layouts, adjusts wrist circumference, toggles audio and a separate GPS module, and includes pod geometry, fit checks, pin tables, component lists and estimated runtime/cost. It is a concept and packing demo, not fabrication CAD or a verified schematic. Its default wrist circumference is 230 mm; strap width is 26 mm, the proposed under-watch cradle adds 5.5 mm, and many part dimensions are explicitly approximate. Runtime and historic prices are not qualified measurements or current quotations.

## Scope and evidence

Three independent local sources were checked:

- **Existing driver scope:** `docs/MODULES.md`, `nos/hal_ext.h`, `firmware/main/ext/ext_priv.h` and the nine extension presence bits in `hal_ext_present()`.
- **OS integration:** `nos/apps/registry.c`, `nos/apps/nos_apps.h`, `nos/apps/set_modules.c`, `nos/svc.h`, `firmware/main/nos_main.c`, `firmware/main/CMakeLists.txt`, `docs/PHONE.md` and `docs/LIVE_DATA.md`.
- **Expanded design:** the newer original `docs/strap_layout.html`, its `PARTS`, `PODS`, `pins`, and unpriced-component tables; `docs/HARDWARE.md` and `docs/BATTERY_FIT.md` for the board and power history.

“Implemented” below means real driver/API/app code exists, not that the strap hardware has been assembled or qualified. MODULES.md explicitly says the extension drivers have not run on hardware. Simulator samples are not hardware presence or sensor validation. Haptic and speaker presence cannot be physically probed and may report driver readiness even with no part fitted.

The repository establishes the functions listed below. Earlier conversation outside the available repository/history cannot be reconstructed; this report does not invent additional promised functions. Solar-system watchfaces are software astronomy views, not evidence that solar charging was requested.

## Already on the B board

Do not buy duplicates for these functions: ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 466×466 touch AMOLED, FT3168 touch, QMI8658 motion sensor, PCF85063 RTC, microSD slot, battery/system-voltage ADC, USB-C, BOOT/reset buttons, Wi-Fi and BLE. A microSD **card** and main battery are separate items. An RTC backup cell is optional. The board is not the audio-equipped 1.43C. [Waveshare board reference](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43)

Consequently time/alarms/world clock, touchscreen UI, activity/steps, wrist wake, IMU display, breathing guidance, display torch, astronomy/Pip graphics and phone notifications do not require the new sensor pods. Weather can come from phone/Wi-Fi. Phone music/call controls use BLE; they do not establish streamed phone-call or music audio on the watch. ESP32-S3 is an LE device; this design does not provide a Classic Bluetooth HFP/A2DP audio path. [Espressif ESP32-S3](https://www.espressif.com/products/socs/esp32-s3), local `docs/PHONE.md`.

## Pod/PCB and component matrix

All rows are optional for the basic OS. A dependency becomes necessary when its associated function is selected. Do not order both cellular variants or both GNSS solutions merely because both are listed.

The matrix contains **22 functional/assembly blocks**, including alternative cellular implementations and conditional remote trackers. This is not a requirement for 22 PCBs: the HTML proposes bundling them into navigation, interface, audio, cellular, hub, health and power pods. Several blocks can share a custom PCB; some are separate off-the-shelf modules; electrodes, antennas, speakers, harnesses and remote trackers are physical assemblies of their own. The [editable CSV](./COMPONENT_INVENTORY.csv) mirrors this matrix and includes the already-onboard functions. It is a design inventory, not a priced or approved procurement BOM.

| Pod / PCB | Functions | Actual added assembly and supporting parts | Interface and power | Mechanical requirements | Existing OS status |
|---|---|---|---|---|---|
| Watch interposer / wiring harness | Connect the extension to the B case | Two mating 2×7, 1.27 mm header assemblies, flex or strain-relieved wire harness, common ground, local bypass capacitors, test points; selected rail switching/protection | J1/J3; shared 3.3 V I2C, battery net, UARTs, I2S and GPIO | Clear back-header windows and sensor cradle; no wire load on pads | Board pin map exists; no finished interposer PCB established |
| Health core under watch | Heart rate, PPG trace, indicative SpO2, contact detection | MAX30102 **breakout with the correct regulators/logic interface**, not a bare IC wired to VBAT | I2C 0x57 on GPIO47/48; verify breakout at 3.3 V. Bare MAX30102 needs 1.8 V core and separate LED supply | Skin-facing optical window, controlled contact/pressure and optical isolation; allow sensor height | EXT_HR driver, periodic history and Heart app implemented; wrist accuracy not qualified |
| Health core under watch | Skin/contact temperature | MAX30205 breakout or custom sensor PCB, bypassing, address straps | I2C, design proposes 0x48; bare sensor supply 2.7–3.3 V | Thermal path to skin, isolation from MCU/radio/charger heat | HTML plan only; no MAX30205 HAL/driver/app found |
| Health electrodes + hub analogue front end | EDA/GSR | Two skin electrodes, an explicitly designed excitation/sense and protection/filter front end, ADC input(s); HTML currently sketches only a resistor divider | Analogue to hub; reference/excitation/ADC budget still unspecified | Stable contact, separated electrodes, cleanable skin surface | Planned only. A pair of discs is not a complete calibrated EDA module |
| ECG/front-end PCB, finger pad and back electrode | Single-lead ECG, R-R intervals, optional BioZ/respiration | ProtoCentral tinyECG/MAX30001 assembly, its required electrode wiring, finger/back contacts and front-end support; verify the selected board's electrode topology rather than assuming two custom discs are a drop-in replacement for its cable | SPI to hub, separate select/interrupt as required, board-specific regulated supply | Finger pad on outer pod and skin contact under watch; route low-level electrode wires away from RF/motor | MAX30001/tinyECG appears in HTML only; no HAL/driver/UI or acquisition validation found |
| Navigation pod | Magnetic heading / tilt-compensated compass | GY-271 with **QMC5883L**; HMC5883L is an implemented alternative. Verify actual IC, not just seller's “GY-271” label | I2C 0x0D or supported HMC 0x1E, shared 3.3 V interface | Away from speaker/motor magnets and high-current paths; calibration in final assembled position | EXT_MAG and Compass app implemented. QMC5883P 0x2C is explicitly unsupported in MODULES.md |
| Navigation pod | Pressure, barometric altitude/trend, ambient temperature/humidity | BME280 breakout; BMP280 is a pressure/temperature-only supported alternative, not a humidity replacement | I2C 0x76/0x77; select one address | Vent to ambient, away from skin/charger heat; protect vent without sealing pressure out | EXT_ENV, QNH calibration/history and Barometer app implemented |
| Navigation pod, optional if modem GNSS is qualified | Independent position, speed, route/track inputs | HGLRC M100 Mini complete GNSS module with patch antenna and cable | UART1: ESP RX7 from GPS TX, ESP TX17 to GPS RX; manufacturer specifies 3.3–5 V input | Manufacturer size 15×15×5.2 mm; outward antenna view, separation from RF/noisy supplies | EXT_GPS NMEA/UBX power handling and Navigate app implemented. Actual module configuration/fix still needs validation |
| Interface pod | Ambient light/auto brightness, proximity, air gestures/music controls | APDS-9960 breakout | I2C 0x39, 3.3 V-compatible assembly | Outward optical window; window attenuation/gesture orientation need calibration | EXT_LIGHT driver and gesture consumers implemented |
| Interface pod | Vibration alerts, timers, feedback | Coin ERM motor **plus transistor/MOSFET driver**, control resistor/pulldown as appropriate, flyback protection and local supply bypass | GPIO16 PWM control; motor supply matched to its rating, not driven from GPIO | Secure coupling to strap; isolate from compass/PPG | EXT_HAPTIC patterns implemented; raw motor electrical/physical response not qualified |
| Interface pod | UV exposure / light | LTR390 UV/ALS breakout, optically suitable lid window | I2C 0x53, selected breakout at verified rail | Ordinary tinted/glass lids may change UV response; use suitable window and calibrate | HTML plan only; APDS does not provide this UV channel |
| Interface antenna plus hub-side NFC circuit | Phone-tappable NFC tag / dynamic payload | ST25DV04K tag IC or module with tuned 13.56 MHz loop, matching/support parts of selected implementation | Tag's own isolated I2C branch via hub or mux; its 0x53 and 0x57 addresses collide with LTR390/MAX30102 | Loop clear of metal case, electrodes and battery; final installed tuning | Planned only. This is a **tag**, not an NFC reader or payment module |
| Optional watch audio pod | UI sounds, audible alarms, memo playback | MAX98357A I2S amplifier breakout, matched speaker (layout proposes 8 Ω 1510), bypass/bulk capacitors, SD control | BCLK1/WS2, data out5, shutdown6; amp supply verified for selected speaker/load | Acoustic grille/cavity, secure speaker mounting, separation from mic and compass | EXT_SPEAKER mixer and memo playback implemented. This is distinct from the modem voice path |
| Same audio pod | Voice memos, sound-level estimate | INMP441 I2S microphone breakout, correct L/R strap and bypass; microSD card for persistent WAV recording | Shared BCLK1/WS2, data in3; mic 1.8–3.3 V, not raw VBAT | Unblocked bottom sound port and acoustic isolation; preserve GPIO3 reset/strapping behaviour | EXT_MIC and Recorder implemented; mic/amp shared-I2S hardware unqualified |
| Cellular pod — existing implementation variant | Independent SMS and signal/SIM status | Compatible Quectel EG800AK carrier, SIM holder/card, LTE antenna, carrier power/control support | UART0 ESP TX43/RX44, PWRKEY18; verify carrier voltage levels and supply specification | Antenna keepout and burst-current supply path | EXT_LTE/SMS app implemented for Quectel AT dialect; not evidence of standalone voice calls |
| Cellular pod — newer HTML alternative | Standalone VoLTE calls, SMS; optional integrated GNSS | Exact SIMCom A7672S-FASE SKU/firmware on a designed carrier: SIM socket, antenna connectors/antennas, level translation if needed, PWRKEY circuit, bypass/bulk power network, optional RI/DTR | UART0 43/44, KEY18; SIMCom family supply 3.4–4.2 V; I/O voltage of selected carrier must be confirmed | Bare module is 24×24×2.4 mm; proposed ~26×28×5.4 mm carrier is an estimate, not verified layout | Planned substitution, not drop-in software support. Add SIMCom AT/backend, calls, ring events and power policy. Integrated GNSS/BLE are variant options |
| Cellular audio, if voice calls selected | Actual call audio | Modem-compatible electret mic/bias/filter network and earpiece/output circuitry; layout proposes separate 1510 call speaker. Verify the modem output's impedance/power; “8 Ω 1 W” on a speaker does not prove the earpiece pins can drive it | Modem analogue MIC/EAR paths, or a deliberately designed alternative codec path | Separate mic/speaker ports and acoustic/echo control | No current HAL for this path. The watch's INMP441/MAX98357A I2S path is not automatically connected to analogue modem voice |
| Hub PCB | Bridge planned extra features to ESP32 | XIAO nRF52840 or redesigned/custom hub, programmer/SWD access, regulated power, I2C target protocol; additional ADC/GPIO or revised pin routing if needed | Watch I2C target plus peripheral buses. The stock board/LoRa stack has a concrete pin conflict described below | Account for headers, stack height, antenna and cable bend radii | HTML concept only; no hub protocol/firmware/driver in OS |
| Hub radio stack | LoRa/mesh communication | Wio-SX1262 add-on and antenna for the final selected region; local bypass and correct RF-switch wiring | SPI plus NSS/RST/BUSY/DIO1/RF-switch signals, 3.3 V module side | Antenna keepout away from electrodes/case; final frequency plan is a separate design decision | Planned only. Meshtastic-capable hardware does not establish OS integration or a combined hub image |
| Remote trackers, if motion capture is selected | Smol/SlimeVR-style multi-body tracking | Compatible nRF-based tracker(s), IMU(s), battery/switch per tracker, receiver function and host/PC/server bridge | nRF ESB receiver/forwarding, not an ESP BLE notification feature | Remote wearable mounts and their independent batteries | HTML mentions receiver only. Combined hub receiver/LoRa firmware and watch-to-host transport remain unimplemented |
| Two removable power sleds | Hot-swap while one pack remains | Two exact protected 1S LiPo packs, two reverse-blocking/ORing power branches, keyed/polarized pogo connectors, wiring rated for worst single-pack load, selected filtering/hold-up, pack measurement dividers/ADC and appropriate protection | Shared VBAT after reverse-blocking branches; pack-side sensing before branches. Nominal 420 mAh model is a planning candidate | Finished pack dimensions incl. PCM/leads/swelling clearance; retention and touch-safe connectors; layout estimates 6×20×42 mm | Only combined board rail measurement is implemented. Per-pack presence/percentage/health and swap UI require hub telemetry/new services |
| External charging dock | Charge removed packs at qualified current | Cell-matched single-cell charger(s), charge connector/polarity/protection and a deliberately isolated path. HTML's TP4056 and ~210 mA are candidates pending exact pack/board specification | Keep removed-pack charging separate from the watch's published ~2 A charge setting | Off-wrist removable sled/dock | No dock or per-pack charge control implemented |

## Verified electrical/assembly blockers and decisions

1. **Stock XIAO + Wio add-on conflicts with the proposed watch I2C connection.** Seeed's Wio schematic uses XIAO D4 for LoRa NSS and D5 for RF_SW1; XIAO's usual SDA/SCL are D4/D5. Radio DIO1/RST/BUSY also occupy D1/D2/D3, and SPI uses D8–D10. Do not stack it and wire the watch I2C bus as assumed. Remap and isolate the conflicting lines, choose a different exposed pin pair/board, or design a custom hub. Then allocate NFC's separate bus, ECG select/interrupt, EDA and **two** pack ADC channels, plus optional modem RI/DTR. No complete allocation is present. [Wio schematic](https://files.seeedstudio.com/products/113010003/Wio-SX1262%20for%20XIAO%20V1.0.pdf), [XIAO pin map](https://wiki.seeedstudio.com/XIAO_BLE/).

2. **Charging is a real circuit constraint.** Published Waveshare schematic U6 is ETA6098, R23 is 82 kΩ. The ETA datasheet explicitly specifies 2 A typical at 82 kΩ, plus 200 mA precharge and 130 mA termination. This confirms the published design, not this particular fitted board or measured current. A generic protected 420 mAh pack does not establish compatibility. Qualify the exact cell and a proper charge/reverse-blocking circuit before using onboard USB charging. [Waveshare schematic, BAT block](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.43/ESP32-S3-Touch-AMOLED-1.43.pdf), [ETA6098 V1.1, page 2](https://www.eta-semi.com/wp-content/uploads/2022/03/ETA6098_V1.1.pdf).

3. **One-pack modem supply remains unqualified.** LM66100 is 1.5 A continuous; its higher pulsed absolute maximum is conditional, not a general modem supply rating. A dual ideal-diode design does not guarantee equal current sharing: either branch/remaining cell must carry the whole actual load when the other pack is removed. Freeze the modem SKU, worst burst waveform, cell/PCM/connector limits and rail droop/thermal budget before selecting the final power-path parts. The HTML's LTE runtime figures mix assumptions from different modems and are not measurements. [TI LM66100 datasheet](https://www.ti.com/lit/ds/symlink/lm66100.pdf).

4. **Modem choice changes software and RF/audio hardware.** Existing code names Quectel EG800AK and uses Quectel SMS/power commands. SIMCom A7672S is a different backend. SIMCom lists GNSS/BLE as options; the exact FASE order code and firmware must be confirmed before removing the separate GNSS receiver. VoLTE must be proven with the chosen network/SIM and voice interface. [SIMCom A7672 family](https://en.simcom.com/product/A7672.html), [Quectel EG800AK](https://www.quectel.com/product/lte-cat-1-bis-eg800ak/).

5. **The NFC bus collision is genuine.** ST25DV04K device-select bytes A6/AE correspond to seven-bit addresses 0x53/0x57; these are its user/system spaces, not simply two freely selectable device addresses. Keep it off the same physical I2C segment as LTR390 0x53 and MAX30102 0x57, using the hub's genuinely separate bus or a mux. [ST datasheet, Appendix B device-select table](https://www.st.com/resource/en/datasheet/st25dv04k.pdf).

## Pin contract to keep

`firmware/main/pins.h` and `firmware/main/ext/ext_priv.h` establish the current B-board allocation:

| Extension signal | ESP GPIO / header |
|---|---|
| I2C SDA / SCL | 47 / 48; J3-5 / J3-3 |
| Motor control | 16; J1-8 |
| I2S BCLK / WS | 1 / 2; J1-13 / J1-11 |
| Mic data / amplifier data / shutdown | 3 / 5 / 6; J1-9 / J1-7 / J1-5 |
| Separate GNSS ESP RX / TX | 7 / 17; J1-12 / J1-10 |
| Modem ESP RX / TX | 44 / 43; J3-9 / J3-11 |
| Modem PWRKEY | 18; J3-2 |
| VBAT / GND | J1-2 / J1-6; other grounds as schematic |

Header orientation must match the actual case/back label. USB19/20, display9–14/21/42, IMU interrupt8, RTC interrupt15 and SD39–41 are already allocated. GPIO38 is routed to the SD socket even though the current one-bit SD mode does not use it. Do not copy the 1.43C I2S mapping. Confirm logic thresholds, pull-up population and total strap bus capacitance for each actual breakout; a listing saying “5 V module” does not establish 3.3 V I2C pull-ups. The source uses a shared 400 kHz I2C bus.

## OS work corresponding to the inventory

Existing board configuration is `CONFIG_WATCH_BOARD_143B=y`. `app_main` calls `hal_ext_init()`, and real extension sources are compiled; there is no reason to duplicate the board IMU/RTC or force absent devices to appear present. Keep optional hardware presence and invalid/unavailable states explicit.

The nine existing extension capabilities are haptic, mic, speaker, GPS, LTE, environment, light, magnetometer and HR. The Modules dashboard and relevant current apps consume those APIs. Hardware bring-up should validate actual device IDs, missing-device behaviour, pin levels, power policy and streaming allocations before treating those features as available. Keep `EXT_BOARD_OK` safeguards for the different C board.

The expanded HTML features need new work: a framed/versioned hub protocol with presence/validity/timestamps; MAX30205 and LTR390 drivers; an ECG/BioZ/EDA acquisition API and UI; NFC tag configuration/payload handling; LoRa/mesh messaging; motion-capture receiver/host forwarding; SIMCom voice/GNSS backend; per-pack sensing/swap telemetry; and matching presence diagnostics. Names/bit positions are not allocated here. These are proposals, not currently enabled OS features. A stock Meshtastic image and a stock Smol receiver image do not automatically become one combined hub application.

Keep sensor and radio acquisition outside synchronous UI drawing. A hub can publish cached snapshots with validity/age and use interrupts or scheduled polling; the main UI must not wait for a new sensor or radio transaction to draw a frame. Audio needs a dedicated buffered stream with explicit ownership and bounded queues. These are integration constraints from the ongoing timing work, not a newly specified hub protocol or a claim that the present drivers already satisfy every constraint.

## Primary component references

- [MAX30102](https://www.analog.com/en/products/MAX30102.html): separate core/LED supplies; use a suitable actual breakout.
- [MAX30205](https://www.analog.com/en/products/MAX30205.html): 2.7–3.3 V I2C sensor; skin temperature is not automatically core-body temperature.
- [BME280](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf): pressure/humidity/temperature, I2C/SPI.
- [APDS-9960](https://www.broadcom.com/products/optical-sensors/integrated-ambient-light-and-proximity-sensors/apds-9960): RGB/ambient/proximity/gesture.
- [LTR390 UV/ALS](https://optoelectronics.liteon.com/upload/download/DS86-2015-0004/LTR-390UV-01_Final_%20DS_V1.7.PDF): UV is a separate sensor function.
- [HGLRC M100 Mini](https://www.hglrc.com/products/hglrc-m100_mini-gps): module dimensions/input/interface, not a bare GNSS IC.
- [MAX98357A](https://www.analog.com/en/products/max98357a.html) and [INMP441](https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf): different amplifier and mic voltage/clock requirements.
- [ProtoCentral tinyECG](https://protocentral.com/product/protocentral-tinyecg-max30001-ecg-respiration-module-for-qt-py-xiao/): actual SPI ECG/BioZ module, not just a MAX30001 chip.
- [Smol tracker architecture](https://docs.slimevr.dev/smol-slimes/index.html): remote tracker/receiver dependencies, distinct from the watch's existing QMI8658.

Primary schematic and datasheet PDFs used for visual checks, plus rendered
detail images, are preserved in `build/extension-hardware-review/` in the
optimization worktree. This inventory does not change production OS support.
