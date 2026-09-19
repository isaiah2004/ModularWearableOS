# Living with Pip

Pip stays a pair of glowing eyes. It has a continuing mood, remembers care, notices touch and motion, and reacts to the watch's time, qualified power readings, activity, notifications and alarms. Everything needed for its personality runs locally; no SD card or internet service is required.

The animation renderer targets 60 Hz. Measured results, changes and reproduction commands are in [Pip animation performance](PIP_PERFORMANCE.md).

## Interaction

- Tap an eye to boop it. A second quick tap starts a little tilt-controlled ball game.
- Stroke back and forth across the eyes to pet Pip. Hold for a comforting head rub. Petting continues smoothly while the finger moves.
- Tilt the watch to direct its gaze and the play ball. Gentle rocking can soothe it; strong motion can startle it, and sustained shaking/spinning can make it dizzy.
- Leave it still face down to rest. The configured quiet hours also make it sleepy. A fresh touch wakes it.
- Tap the small line below the eyes to open **Pet care**. Swipe through mood/needs, the daily routine, and remembered moments. Tap the bottom of the first page to say hello and return to Pip.
- In the routine page, tap left/right on WAKE or REST to change that hour. The default awake interval is 07:00–22:00 local time. A window crossing midnight is supported.
- Swipes below the eye area still change watch faces. Edge swipes still open the watch's menus.

There is no built-in microphone on this board. Saying hello currently means returning to Pip, tapping it, or using the care screen/USB hello command.

## Its continuing life

Company, play/stimulation, rest and comfort are separate needs. They start comfortably filled and change over hours. Social satisfaction declines by 35 points per awake hour on a 0–1000 scale. Pip becomes lonely below 500 and grumpy below 350. Quiet hours pause social decay and recover rest. Familiarity is separate and survives a temporary bad mood.

A greeting restores 30 social points at most once every five minutes. Petting restores 100 social, 120 comfort and 20 stimulation points; play restores 70, 30 and 120 respectively, with a shared five-minute care reward cooldown. Every touch still gets an immediate animation. Greeting has a separate reward cooldown, so saying hello does not prevent the next head rub from helping.

Pip can briefly enjoy petting while still needing something. If social need remains very low or a charging request is overdue, the happy mood lasts six seconds and then the unresolved need becomes visible again. Otherwise a petting mood lasts a minute and a play mood two minutes. These are initial game parameters, not psychological or biological constants.

Energy reflects rest and a conservative battery estimate when one is available. Low energy changes lid weight, vigor, glow, blinking and play initiative. Touch cannot manufacture battery charge. On external power the board cannot measure actual battery percentage, so the care screen says that the estimate is unknown.

## Charging and walks

The GPIO4 ADC measures a USB-influenced system rail. Charger current/full status is not wired to the MCU. Pip distinguishes an observed external supply from an approximate battery-only reading and from unknown evidence. It never claims that plugging in means a complete charge.

After 60 seconds of qualified readings at or below 20%, Pip can ask for power while visible during awake hours. If that observed request remains unresolved for 30 awake minutes on qualified low readings, it becomes grumpy about power. Unknown readings pause that judgment. External power or sustained qualified recovery resolves the request and produces relief. No missed-charge time is invented during reboot or powered-off gaps. There is no arbitrary daily charge appointment and no demand to reach 100%.

The QMI8658 is sampled by the board service even when another app is open. A conservative cadence detector requires eight coherent peaks and rejects isolated motion, strong spins, implausible magnitudes, discontinuities and invalid samples. This is **estimated walking-like activity**, not a calibrated pedometer. Rhythmic hand motion or transport vibration can still need field tuning.

An outing reward needs 120 qualifying active seconds in the recent three-minute window, with a 30-minute reward cooldown. It restores stimulation and companionship, records a shared outing, and prompts an excited reaction when the face is available. Motion is not sensed during deep sleep. The current activity minute counter is since boot; remembered outing rewards survive reboot.

## Notifications and alarms

New arrivals produce an upward glance while Pip is visible and awake. Bursts coalesce, repeated delivery sequences do not repeatedly restart a reaction, and direct petting/important performances finish before low-priority events. The core notification sequence continues increasing after the eight-entry store fills. Notification contents never enter permanent pet memory or determine whether Pip thinks a message is good or bad.

The alarm scheduler emits a pet wake event at the actual alarm firing point while keeping the watch's alarm callout available. Quiet hours suppress ordinary notification performances.

The firmware accepts incoming companion notifications through these paths:

```text
USB:  notify Messages|A new message
HTTP: POST /notify
      Content-Type: text/plain
      body: Messages|A new message
BLE:  existing TIME characteristic, bytes 0xF2 followed by APP|TITLE
```

The application name must be 1–19 bytes, title 1–39 bytes; empty strings, embedded control characters, and oversized messages are rejected. HTTP body is at most 59 bytes. The BLE extension requires at least seven total bytes to preserve legacy four/six-byte time messages. Longer legacy clock/location packets whose first epoch byte equals 0xF2 must use the separate six-byte clock form. See the hardware report for the precise compatibility boundary.

The transport queue holds eight items and reports failure when full. HTTP uses the watch's existing local Wi-Fi service; it has no new authentication layer, so keep that service on a trusted local network. This protocol makes incoming delivery possible; a phone/PC companion still needs to forward actual notifications. It does not implement native ANCS/Android notification capture. Senders must avoid replaying historical messages because the simple wire format carries no original source event ID.

## Memory and inspection

Needs, routines, charging requests and four remembered care episodes use a validated, versioned record with a field-based checksum. Two alternating NVS slots keep a previous valid generation. Legacy Pip familiarity/seed remain in their own record. Saves happen after settled episodes, periodically around five minutes while dirty, and before planned sleep/power-off. Failed writes retain dirty state and are retried; a failure is visible in diagnostics.

Monotonic time drives running behavior. Trusted UTC supports capped offline advancement, with quiet-hour overlap accounted for. Large/backward clock corrections cannot become instant live neglect. Offline advancement is bounded, and cannot grant walks, notifications or charging events that were never observed. A complete day of use is still needed to tune the rates to this owner.

Useful USB commands with `tools/watchctl.py --port COM3`:

```text
pip                         # action, mood, cause, needs, evidence and memory status
pip care                    # open the care app
pip hello                   # greet Pip
pip pet                     # comfort Pip
pip play                    # start play
pip routine 7 22            # saved awake/rest hours
pip center                  # recenter tilt gaze
pip save                    # checkpoint memory now
pip demo grumpy              # force a performance, without rewriting care needs
pip live                    # return to autonomous behavior
power                       # qualified rail/battery/external-power snapshot
activity                    # estimated cadence activity
notify Test|Hello Pip       # inject a notification into the actual incoming path
prof 1                      # enable USB frame timing while a monitor is reading
prof 0                      # stop profiling (default on boot)
```

The 21 performance names are idle, curious, happy, petted, boop, startled, dizzy, rocked, sleepy, asleep, wake, play, bored, stretch, wink, peek, grumpy, lonely, excited, relieved and notify. A forced demo eventually returns to autonomous behavior; it is not a simulation of hours of care.

## Study and validation

Start with the [research synthesis and revised plan](research/PET_IMPLEMENTATION_PLAN.md), which links all case-study and feasibility reports. [Interactive expression review](research/assets/pip-preview.html) shows actual simulator-rendered clips. The [validation record](research/PET_VALIDATION.md) distinguishes host tests, firmware build results, device observations and wear tests still pending.

QSPI remains configured at **40 MHz**. Requesting 50 MHz does not yield an actual 50 MHz clock on the selected ESP32-S3 clock path. 80 MHz is a controller capability but exceeds the linked panel's 50 MHz specification and is not used by this build.
