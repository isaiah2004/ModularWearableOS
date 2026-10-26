# Strap modules: drivers, power, testing

Implements `nos/hal_ext.h` on the device (`firmware/main/ext/`) and in the
simulator (`sim/nos_ext_sim.c`). Wiring follows `docs/strap_layout.html` (`const pins`).
Nothing here has run on hardware yet. Every statement about behaviour is what the code
is written to do, and needs confirming on the watch.

## Architecture

| Piece | What it does |
|---|---|
| `hal_ext_init()` | Call once after `board_init()` (I2C bus up, NVS ready). Probes the I2C modules (20 ms timeout each), sets up the motor, amp pin and button, and starts the tasks. GPS and LTE are probed asynchronously. |
| `ext` task | Priority 0 (below the UI main task at 1), no core affinity, 4 KB stack **in PSRAM**. Each loop calls every module's `*_service()`. Each returns how long it can sleep, and the task sleeps for the shortest (50 ms at most). |
| `exta` task | Priority 3, no affinity, 4 KB PSRAM stack. It is the only code that touches I2S and pumps 4 ms blocks. It sleeps on a notification while silent. It is separate from `ext` so slow I2C or SD work cannot make the audio underrun. |
| Haptics | An esp_timer one-shot chain drives LEDC. No task. |
| Memory | About 100 B of static DRAM in total. Each module keeps one state struct in PSRAM. Internal RAM is used only while a stream runs: I2S DMA about 2.1 KB, GPS UART about 0.8 KB, LTE UART about 1.3 KB. |
| Locking | One `portMUX` (`ext_lock`) guards each cached sample during tiny copies. Getters cost a struct copy. The IDF `i2c_master` driver serialises the shared bus against board.c. |
| Flash writes | NVS saves happen in the **caller's** context: `hal_mag_calibrate(0)`, `hal_env_set_sea_level()` and `ext mag map`. The ext/exta stacks are in PSRAM, and a flash write disables the cache behind them. |
| 1.43C | `EXT_BOARD_OK` is 0. That board uses IO5/7/16/17 for its LED, charger and touch, so every module reports absent. |

**Presence:** `hal_ext_present()` reports what answered. Haptic and speaker cannot be
probed. They report present once their driver is up. The mic is probed at boot by
running I2S for 0.65 s and checking for a live data line. GPS appears once a valid
NMEA sentence is seen, up to about 10 s after boot. LTE appears once the modem answers
`AT`, up to about 17 s after boot. Absent modules return 0 or invalid immediately, with
no retries and no logs. The exceptions are GPS and LTE: calling `*_enable(1)` on an
absent GPS or LTE re-runs its probe once, so a late plug-in works.

**Power policy:** GPS, LTE, the HR LED, the mic and gestures run only while requested.
Each auto-stops once its getter has gone `NOS_EXT_IDLE_MS` (8 s) without a call.
Exceptions:
- Recording is explicit and has no idle stop, so a memo keeps going with the screen off. It is capped at 10 min.
- PCM playback runs to EOF.
- An SMS send in flight keeps the modem on until it is done.
- The compass stops by the same rule (standby mode), even though the contract does not require it.

## Serial: `ext`

The lead's `dbg_exec` routes `ext ...` to `hal_ext_debug(line)`.

- `ext` prints one status line per module, plus stack high-water marks for `ext` and `exta` in bytes.
- `ext help` lists the sub-commands:

  `haptic <0-6>`, `sfx <0-13>`, `vol <pct>`, `play <path>`, `rec <path>`, `recstop`,
  `gps on|off`, `lte on|off`, `lte at <cmd>`, `lte sms <num> <text>`, `hr on|off`,
  `gest on|off`, `mag cal|done|map <+x+y+z> [<fwd>]`, `sea <hPa>`.

Streams started with `on` from serial still idle-stop after 8 s unless a getter keeps them
alive. Running `ext` calls the GPS, LTE, HR and compass getters, so repeating it within 8 s keeps
those streams running. Gestures are not kept alive this way (popping them would consume them):
swipe within 8 s of `ext gest on`.

---

## Vibration motor (IO16)

- **Wiring:** J1-8 IO16 to the driver transistor base (motor IN), active high. The motor is fed from VBAT.
- **Driver:** LEDC timer 3 / channel 7 at 20 kHz (inaudible), 10-bit. Patterns are `{duty, ms}` tables:

  | Pattern | Shape |
  |---|---|
  | TICK | 8 ms |
  | CLICK | 15 ms |
  | DOUBLE | 14 on / 70 off / 14 on |
  | HEAVY | 40 ms |
  | BUZZ | 300 ms |
  | ALARM | 3 × 120 ms pulse train, repeating every 1.32 s |
  | HEARTBEAT | 30 ms at 100 %, 110 ms gap, 22 ms at 70 % |

  A new pattern replaces the current one at once. Strength 1-100 % maps to 40-100 % duty, because a coin ERM stalls below about 40 %. 0 % turns it off.
- **Legacy callers:** values above 6 are treated as old `hal_haptic(ms)` durations and mapped to the nearest pattern.
- **Test:** run `ext haptic 0` … `ext haptic 6`. Feel each one; `ext haptic 5` then needs `hal_haptic_stop()` to end. If an 8 ms TICK is not felt, raise the strength floor in `motor_duty()`.
- **Risks:**
  - The transistor needs a flyback diode across the motor.
  - Very short pulses may not be felt through the strap.
  - The motor's magnet and current upset the compass, so the compass skips samples while the motor runs, plus 40 ms after.

## Audio: MAX98357A amp + INMP441 mic (one full-duplex I2S port)

- **Wiring:**
  - IO1 BCLK, shared by mic SCK and amp BCLK.
  - IO2 WS, shared by mic WS and amp LRC.
  - IO3 mic SD (data in).
  - IO5 amp DIN.
  - IO6 amp SD. Low is shutdown, high is the left channel.
  - The mic's L/R pin should be tied to GND (left slot). The probe also detects the right slot.
- **Driver:**
  - 16 kHz, Philips, stereo 32-bit slots carrying 16-bit data. The INMP441 needs 64 BCLK per frame and returns the top 16 of its 24 bits.
  - TX is initialised first and is the clock master; RX joins as the full-duplex slave.
  - DMA is 4 × 64 frames, giving about 16 ms of output latency.
  - The channels are created on demand and deleted 1.5 s after the last need. With BCLK parked low, the mic powers down.
  - The amp SD pin goes high only while samples play, then 60 ms of tail.
  - Mixer: two procedural SFX voices plus one PCM stream, a squared volume curve with 0.45 headroom, and a gentle one-pole low-pass.
- **SFX:** `firmware/main/ext/ext_sfx_synth.h` holds the recipes, and every recipe is documented inline. The device and the sim share this file. All effects are thin pulse waves with sample-and-hold noise transients, millisecond exponential decays and small pitch drops. Most are under 60 ms.
- **Mic:**
  - Loudness is a DC-removed RMS with 125 ms smoothing. dB SPL ≈ 120 + dBFS, from the INMP441's −26 dBFS at 94 dB SPL. It is not A-weighted and has a floor of 30 dB.
  - The wave buffer holds the peak envelope, one value per 32 ms, mapped 40-100 dB to 0-255.
  - The first 300 ms after the clock starts are ignored (start-up mute).
- **Record and play:**
  - `hal_rec_start(path)` opens the file and writes a WAV header in the caller's context. It returns 0 if there is no SD card or no mic.
  - The ext task drains a 1 s PSRAM ring to the card in 4 KB writes and patches the header on stop.
  - `hal_audio_play_file()` accepts 16 kHz mono 16-bit WAV (it parses the chunks) or raw PCM, fed through a 0.5 s ring.
- **Test:**
  1. Run `ext` and check `mic ok=1 slot=0`, and that `audio … running=0` when idle.
  2. Run `ext sfx 0` … `ext sfx 13` at `ext vol 70`. Listen for clicks at the start or end; there should be none, because the amp tail and release ramps prevent them.
  3. Run `ext` while talking and watch `db` move: about 40 quiet, 60-70 speech.
  4. Run `ext rec /sdcard/t.wav`, speak, run `ext recstop`, then `ext play /sdcard/t.wav`. Check `drops=0 underruns=0`.
- **Risks:**
  - Full-duplex sharing of BCLK/WS between the INMP441 and the MAX98357A has not been tested on hardware.
  - IO3 is a strapping pin (JTAG select). The mic drives it only while clocked, so it should be fine at reset, but check boot with the mic fitted.
  - The I2S DMA needs about 2.1 KB of internal RAM while active. If internal heap is exhausted, `port_up()` fails and sounds are silently dropped, with no crash.
  - The 1510 speaker cannot reproduce much below about 800 Hz, so SFX_ERROR relies on harmonics.
  - SD write stalls longer than 1 s would drop recorded samples, counted in `drops`.

## GPS: HGLRC M100 Mini (u-blox M10), UART1

- **Wiring:** IO7 is ESP RX (from GPS TX). IO17 is ESP TX (to GPS RX). The TX line is needed for power management.
- **Driver:**
  - Auto-baud: after a wake burst of 0xFF bytes, it listens for one checksum-valid sentence at 9600, then 38400, then 115200 baud, 1.5 s each, two rounds.
  - The parser handles GGA, RMC, GSA and GSV from any talker (GP, GL, GA, GB, GQ, GN). SNRs are merged across constellations and sorted strongest first, up to 16.
  - A fix older than 3 s turns `valid` off. `age_ms` is the time since the last valid fix.
- **Power:** there is no power-gate wire. "Off" means UBX-RXM-PMREQ backup mode (wake on UART RX), and the UART driver is deleted. On enable, the 0xFF burst wakes the module. With backup power on V_BCKP, the M10 hot-starts in about 1-2 s; otherwise it cold-starts in about 30 s.
- **Test:**
  1. Run `ext gps on`, then `ext` every few seconds: `state=run baud=… nmea>0 badsum≈0`.
  2. Outdoors, check that sats and SNR climb and `valid=1` appears.
  3. Run `ext gps off` and check the current drop on the 3V3 rail: tens of mA down to well under 1 mA.
- **Risks:**
  - The module may come preset to UBX-only output, or to a different baud such as 230400 (common for flight controllers). Both would fail the probe. Fix: add the baud to `BAUDS`, or enable NMEA once with u-center.
  - PMREQ needs UBX input enabled on UART1 (the M10 default).
  - The antenna is in a strap pod under the forearm, so fixes will be weak when the wrist faces down.

## LTE: Quectel EG800AK (7Semi Pico), UART0

- **Wiring:**
  - J3-9/J3-11 to UART0: IO43 U0TXD goes to modem RXD; IO44 U0RXD comes from modem TXD; 115200 8N1.
  - IO18 is PWRKEY. VCC is VBAT.
  - UART0 is free because the console is USB-Serial-JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, `CONFIG_ESP_CONSOLE_UART_NUM=-1`).
- **PWRKEY:** the default is open-drain: "press" pulls low, "release" is high-Z. This is safe if IO18 goes straight to the modem pin. If the 7Semi board has an NPN buffer (MCU high = press), build with `-DEXT_LTE_PWRKEY_ACTIVE_HIGH=1`. Before pressing, the driver always tries `AT` three times, so an auto-powered carrier works either way.
- **Driver:** a non-blocking state machine with one command in flight. Responses and URCs are parsed anywhere.
  1. **Power-on:** `AT` ×3. If there is no answer, PWRKEY for 550 ms, then up to 15 s for `AT` to answer.
  2. **Init:** `ATE0`, `CMEE=1`, `CPIN?`, `CMGF=1`, `CSCS="GSM"`, `CNMI=2,1`, `COPS=3,0`.
  3. **Poll:** `CSQ`, `CEREG?`, `CREG?`, `COPS?` in rotation, one every 1.25 s.
  4. **Inbox:** `AT+CMGL="ALL",1` (Quectel: lists without marking messages read) every 60 s and on `+CMTI`. It falls back to `"ALL"`, which marks messages read, if the firmware rejects the mode argument.
  5. **Send:** `CMGS` waits for the `> ` prompt, then sends the text and Ctrl-Z, and waits up to 60 s.
  6. **Power-off:** `AT+QPOWD=1` and wait for `POWERED DOWN`, otherwise PWRKEY for 1 s.
  - **Signal:** `rssi = −113 + 2·CSQ`. Bars: ≥ −73 dBm gives 4, ≥ −83 gives 3, ≥ −93 gives 2, ≥ −105 gives 1.
  - **Inbox cache:** 12 messages, newest first. UCS2 hex bodies are decoded to their ASCII subset.
- **Boot probe:** the modem is powered once at boot to read the SIM state and the inbox, so the unread count is known with LTE off. Then it is powered down again.
- **Test:**
  1. Run `ext lte on`, then `ext` every few seconds: `state=ready sim=1 reg=1 bars>0 op="…" tech=LTE`.
  2. Send any AT command with `ext lte at AT+QENG="servingcell"`; the response lines print as `EXT lte< …`.
  3. Run `ext lte sms +91XXXXXXXXXX hello` and watch `sent=` increment.
- **Risks:**
  - PWRKEY polarity is unknown (see above).
  - At reset the ROM bootloader prints its banner on U0TXD. The modem sees one garbage line, which is harmless.
  - The 7Semi carrier's UART level: the vendor says it is 3.3 V-tolerant; confirm it.
  - LTE transmit bursts reach about 2 A peak on VBAT and can brown out the watch on a weak cell. Keep a large capacitor near the modem.
  - The "only while requested" contract means SMS arrive only while an LTE screen is open (or at the next boot or enable).

## BME280 (7Semi Nano), I2C 0x76/0x77

- **Driver:**
  - Chip ID 0x60 (a BMP280, 0x58, also works, with humidity 0).
  - Forced mode every 2 s: humidity ×1, temperature ×2, pressure ×8, IIR filter 4. The result is read 50 ms after the trigger.
  - Compensation is Bosch's reference integer code: 32-bit temperature, 64-bit pressure, 32-bit humidity.
  - Altitude uses the international barometric formula against the sea-level reference, which is persisted in NVS as `ext_sea`.
  - History holds one pressure sample per 10 min, 144 deep, oldest first. It starts empty at boot and is not persisted.
- **Power:** the chip sleeps between conversions, well under 10 µA on average.
- **Calibration:** `hal_env_set_sea_level(QNH)` or `ext sea 1012.5`. This is a single NVS write.
- **Test:** run `ext` and compare against a phone or weather report. Pressure should agree within about 1 hPa. Temperature reads high on the wrist.
- **Risks:** skin and battery heat bias temperature and humidity. The vent should face away from the skin, as the layout note says. Pressure is unaffected.

## APDS-9960, I2C 0x39

- **Driver:**
  - Accepted IDs: 0xAB (genuine) and 0xA8 / 0x9C (clones).
  - Always-on low-rate cycle: proximity, then ALS (103 ms integration), then a 206 ms wait. It is read every 400 ms.
  - ALS gain auto-ranges 1/4/16/64×.
  - Lux = (G − IR) · DF · GA / (t_int · gain), where IR = (R+G+B−C)/2, DF = 310 and GA = 1. Override with `EXT_APDS_LUX_DF` and `EXT_APDS_WINDOW_GA`.
  - Proximity is raw PDATA minus a floor-tracking baseline, which cancels window crosstalk.
- **Gestures:** only while enabled; they idle-stop after 8 s.
  - The gesture FIFO is polled every 15 ms.
  - The first and last datasets of a session give the U/D and L/R ratio deltas; a delta above 20 % is a swipe. The sign convention follows the SparkFun reference driver.
  - A hover of 600 ms without a swipe gives `GEST_NEAR`, and its end gives `GEST_FAR`.
  - `EXT_APDS_ROTATE` (quarter turns) fixes the directions for the strap orientation.
- **Calibration:** compare `lux` against a lux meter through the real lid window and set `EXT_APDS_WINDOW_GA`. For gestures, swipe toward 12 o'clock with `ext gest on` running. If `EXT light … gestures=` counts but the direction is wrong, set `EXT_APDS_ROTATE`.
- **Risks:**
  - Lux accuracy is about ±30 % even after calibration.
  - The window and lid can reflect the IR LED into the sensor (crosstalk); the baseline only partly hides this.
  - The sensor is mounted on its side in the strap, so the gesture axes are rotated.
  - At 100 mA LED drive, current spikes on the 3V3 rail during gestures.

## Compass: GY-271 (QMC5883L 0x0D, or HMC5883L 0x1E)

- **Driver:**
  - QMC: 50 Hz continuous, ±8 G (30 LSB/µT), OSR 512.
  - HMC: 15 Hz, 8-average, ±1.9 Ga.
  - Read at 20 Hz while `hal_mag_get()` is being polled; otherwise standby.
  - Samples taken while the motor or amp is active are discarded.
  - Tilt compensation is vector-based, in the IMU frame:
    - D = −accel/|accel| (down)
    - E = norm(D × m) (east)
    - N = E × D (north)
    - heading = atan2(F·E, F·N), for the watch's 12 o'clock axis F
  - Heading is smoothed on the unit circle.
- **Calibration:**
  - `hal_mag_calibrate(1)`, then figure-8s, then `hal_mag_calibrate(0)`.
  - Per-axis min/max give the hard-iron offset (centre) and a diagonal soft-iron scale.
  - Progress is coverage of 26 direction bins; 20 bins count as 100 %.
  - The calibration is saved only if at least 10 bins were covered and every axis swung by at least 10 µT.
  - It is stored in NVS as `ext_mag`, together with the axis map.
- **Axis map:** the module sits in a strap pod, not on the PCB, so its axes must be mapped into the QMI8658 frame. `ext mag map +x-y-z +x` sets the mag-to-IMU signed permutation and the IMU axis that points to 12 o'clock. The default is identity / +x.
- **Test:**
  1. Run `ext mag cal`, wave figure-8s for about 20 s, then `ext mag done` and check `cal=1`.
  2. Hold the watch flat with 12 o'clock pointing north: `hdg` should be about 0. Turn clockwise: about 90 facing east.
  3. Tilt the wrist ±30° and check the heading moves less than 5°.
  4. If the heading runs the wrong way or jumps by 90/180°, fix the map.
  5. Check `field` reads 25-65 µT in open space.
- **Risks:**
  - The motor, speaker magnet, LTE PA currents and the battery near the pod all disturb it. Recalibrate after any change to the strap.
  - A wrong axis map produces a heading that looks plausible but is wrong. Verify it with the test above.
  - Newer GY-271 boards ship a QMC5883P at 0x2C, which is not supported.

## Heart rate: MAX30102, I2C 0x57

- **Wiring:** I2C plus 3V3. Some boards need a separate 1.8 V/5 V for the LEDs: use the "black" board, which takes 3V3.
- **Acquisition:** SpO2 mode, 400 sps ÷ 4 averaging = 100 Hz, 411 µs pulses, 18-bit, 4096 nA range. The 32-deep FIFO is drained every 120 ms. LED current starts at 6.2 mA each and auto-ranges once a second to keep IR DC within 50k-200k counts.
- **DSP:**
  - DC is an EMA, giving a 0.3 Hz high-pass.
  - A 4 Hz Butterworth low-pass follows.
  - PPG = −IR AC.
- **Beats:**
  - A beat is a local maximum above 40 % of the decaying envelope, with a 330 ms refractory period.
  - Intervals between 40 and 180 bpm are kept in an 8-deep median buffer.
  - Lock needs at least 4 intervals with spread under 15 %.
  - Quality = regularity × perfusion index.
- **SpO2:** ratio of ratios over 4 s, using Maxim's quadratic. It is reported only when locked with quality ≥ 0.5. **Not a medical measurement.**
- **On-wrist:** IR DC above 25k for 0.5 s.
- **Background:** every 10 min (the first 2 min after boot), a measurement of up to 40 s runs. It ends after 8 s of good lock and appends the bpm, or 0, to the 144-entry history.
- **Power:** the chip is in SHDN (about 0.7 µA) whenever neither the user stream nor a background measurement runs.
- **Test:**
  1. Run `ext hr on` and hold a finger on the sensor, then run `ext` every second.
  2. Check `wrist=1`, `dc_ir` within 50k-200k, `beats` climbing, `bpm` within ±3 of a pulse oximeter within about 10 s, and `q>0.6`.
  3. Repeat on the wrist through the strap window.
  4. History: leave it 10 min and check `hist=1`.
- **Risks:**
  - Through a strap window on the side of the wrist, the signal is far weaker than at a fingertip. Expect low quality, motion artefacts and frequent unlocks.
  - Window reflections raise DC without adding signal.
  - Clones sometimes swap the red and IR FIFO order; if `dc_red ≫ dc_ir` on skin, swap them in `hr_service()`.
  - SpO2 through a window is indicative only.
  - Haptic pulses during a measurement add artefacts.

## System

- **`hal_button_down()`:** reads GPIO0 (BOOT), active low, with the board pull-up.
- **`hal_mem_info()`:** reports PSRAM and internal free heap.
- **`hal_display_power()`:** a weak no-op that records the mode. The main firmware overrides it.

## Simulator (`sim/nos_ext_sim.c`)

- Data is driven by `hal_millis()` and the sim RTC only, so it is deterministic under the scripted virtual clock. `hal_ext_sim_present` (default 0x1FF) clears modules.
- Behaviour:
  - GPS fix about 4 s after enable, 3D at 5.5 s, walking loop around Cubbon Park.
  - HR locks about 3 s after enable at about 72 bpm, with a real PPG shape; SpO2 from 6 s.
  - LTE: "Jio 4G", 3 bars, 4 SMS of which 2 are unread.
  - Light about 300 lux.
  - The BME 24 h history is full from boot, with a front.
  - Mic: speech-like phrases.
- Hooks:
  - `hal_ext_sim_inject_gesture(g)`.
  - `set gesture|bpm|lux|heading|db|nofix|magcal <v>` via `nos_ext_sim_set`.
  - `hal_ext_sim_last_haptic`, `_last_haptic_ms`, `_last_sfx` and `_last_sfx_ms` for the HUD.
- In interactive mode, SFX and PCM play through waveOut, synthesised from the same recipes as the watch.
