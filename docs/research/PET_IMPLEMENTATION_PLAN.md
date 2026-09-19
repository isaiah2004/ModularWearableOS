# Pip: research synthesis and revised implementation plan

Prepared September 13, 2026. This is the engineering decision record for the expanded pet requested by the owner. Research and proposals precede the implementation described here; build and device evidence will be recorded separately in `PET_VALIDATION.md`.

**Execution status:** implemented and deployed to the confirmed COM3 watch.
See the [validation record](PET_VALIDATION.md) for final image identity,
actual tests, measured rendering improvement and physical evaluation still
pending. The sequence below is the plan retained as a decision record.

## Reading the study

The full study consists of three complementary reports:

1. [Case studies and expressive design](PET_CASE_STUDIES.md): Tamagotchi, Cozmo, Vector, aibo, robot gaze and affect research; competing visual and behavioral proposals; criticisms and revisions.
2. [Behavior and care proposal](PET_BEHAVIOR_PROPOSAL.md): requirements, theory, persistent needs, priorities, routines, care episodes, memory, clock handling, and 28 proposed acceptance scenarios.
3. [Hardware feasibility](PET_HARDWARE_FEASIBILITY.md): official board/schematic/component evidence, system-rail sensing, movement sensing, transport gaps, memory, display timing, and feasibility revisions.

Together these reports distinguish published findings, product precedents, observed code, design hypotheses, and parameters selected for testing. A shipped pet product is a precedent, not proof that its mechanics will work on a wrist. Study participant counts and limitations are kept with the associated findings. The primary-source inventories are at the end of the reports, rather than treating secondary summaries as independent evidence.

## Selected proposal

Keep Pip as two recognizable glowing eyes. Use a persistent care model to select a mood, and a separate short-action model to acknowledge what just happened. Render the resulting pose procedurally with deliberate anticipation, a readable peak, and recovery. This adopts the useful separation of drives, mood, and expression from the research without implementing a general human-emotion simulator.

Pip is curious, affectionate, slightly mischievous, and capable of a sulk. Its annoyance should have a cause the owner can discover. A short happy expression is not a reset of every need. Conversely, unresolved grumpiness must not prevent petting, play, or ordinary watch navigation. Relationship familiarity remains separate from today's mood.

| Owner requirement | Implementation decision | Visible result |
|---|---|---|
| Keep the eyes, deepen personality | Independent lids, asymmetry, gaze targets, blinks, breathing, phased reactions | Recognizable face with variation rather than disconnected icons |
| Mad when ignored | Slow awake-time social decline and a persistent mood bias | Look-away sulk and checking glances; greeting and petting repair social need |
| Happy when petted | Immediate pointer attention and sustained strokes; spaced care rewards | Soft delighted eyes throughout a stroke, not a repeatedly restarted animation |
| Energy from battery | Qualified power input plus a separate rest reserve | Low physical energy slows and weighs down expressions; sleep also matters |
| Mad after delayed charging | Observe a low-energy request and an unresolved grace period | Concern first, then an attributable low-energy grievance; observed external power brings relief |
| Enjoy walks | Independent cadence detector; reward sustained qualifying activity | Excited shared-outing reaction, stimulation recovery, persistent outing history |
| Notice notifications | Real incoming USB/HTTP/BLE transport plus arrival sequence | Brief upward glance, coalesced during bursts; notification content stays out of pet memory |
| Use clock and time | Local waking/resting routine, trusted UTC persistence, monotonic runtime timers | Quiet nights, morning context, safe return greetings |
| Feel continuous | Versioned bounded needs and episode history, periodic checked saves | A reboot preserves the same pet and does not invent events during the gap |
| Explain its behavior | Care screen, routines, cause history, USB diagnostics | Owner can connect an expression to a need and adjust routines |

## Revisions after investigating difficulties

### Revision 1: charging is an evidence problem

The initial idea was to equate the existing battery percentage with energy and use the charging indicator to judge timely care. The schematic audit showed that GPIO4 measures the **system rail**, influenced by USB, while charger STAT is connected to an LED rather than the MCU. The existing voltage-to-percent conversion is not a fuel gauge. The implementation therefore exposes sample validity, sample age, inferred battery-only state, and observed external power separately. A connected USB host is positive external-power evidence; it does not prove charge current or completion. A plausible stable battery-only rail can provide an explicitly approximate percentage.

Pip requests power only from qualified sustained low readings. It becomes annoyed only after an observed unresolved request. Unknown samples pause this judgment. Reboot/offline time cannot manufacture a missed deadline. Connecting external power resolves the social charging episode without claiming that the battery is full. A fixed daily charging appointment and charge-completion rewards are deferred until they can be supported by actual hardware observations and an owner-selected routine.

### Revision 2: walking cannot reuse a placeholder

The existing step function returned zero. The official demo contains a hardware pedometer path, but its configuration depends on undocumented/revision-sensitive behavior. The first implementation uses a bounded, tested cadence classifier over the already configured QMI8658 stream. It requires a series of plausible periodic peaks, rejects strong shocks/spins and discontinuities, and accumulates activity only while cadence remains credible. The board task runs independently of the visible face.

The UI and reports call this an estimate. Synthetic traces establish algorithm behavior; they do not establish real-world fitness accuracy. Rhythmic hand motion and vehicle vibration remain cases for physical calibration. A walk episode requires sustained accumulated activity rather than one gesture or a single peak. Deep sleep turns sensing off, so offline walks cannot be credited.

### Revision 3: notifications need an actual source

The eight-entry notification store had no firmware producer. Watching its count would also fail after the eighth item. Add a monotonic arrival sequence and a bounded HAL queue. USB commands, a local HTTP endpoint, and a documented extension to the companion BLE time characteristic enqueue incoming items; only the UI thread changes the notification store. No native phone notification forwarding is claimed merely because a test command works. A phone or PC companion must send its notifications using this protocol. Protocol deliveries are fresh events; a sender must avoid replaying history because this minimal format has no universal source event ID.

### Revision 4: care must survive changes in foreground app

Animation previously advanced only when Pip was drawn. Move slow care and incoming event processing into a periodic service outside rendering. Keep the existing once-per-frame pose latch so striped drawing cannot advance time multiple times. Movement sampling remains in the board service. Use monotonic elapsed time for live episodes and trusted UTC only for bounded absence advancement. Unknown time, backward adjustments, or large clock corrections must not create a sudden care penalty.

### Revision 5: more expressive eyes must still fit the renderer

An initial eye built from intersecting circles and rectangles had visible seams. Replace it with a continuous rounded-rectangle raster primitive. Keep drawing command counts bounded, apply blink closure after pose smoothing, and preserve separate startle cooldowns and sustained-stroke phases. Add opposing angled lids for grumpiness and phased lonely, excited, relieved, and notification actions. Each expression must be previewed and checked under the existing rotated stripe renderer.

### Revision 6: 50 MHz is not a selectable improvement here

The configured QSPI clock is **40 MHz**. With this ESP32-S3/IDF clock path, requesting 50 MHz still selects 40 MHz; the next higher divider result is 80 MHz. The linked SH8601 write timing allows 50 MHz, so retain 40 MHz. The exact board controller was not positively identified by the current readback; it follows the official fallback initialization. A 466 × 466 RGB565 full transfer requires about 21.7 ms of wire time at 40 MHz quad data rate, before software overhead. Improve bounded rendering and partial updates rather than claiming a 50 MHz speed increase.

## Implementation sequence

1. **Evidence contracts and input services.** Add cached, single-owner ADC sampling, qualified power snapshots, UTC access, cadence estimates, and queued incoming notifications. Preserve existing HAL APIs where other apps rely on them.
2. **Pure care simulation.** Implement validated fixed-width memory, bounded needs, local routine, observed charging episodes, outing rewards, mood/reason selection, event priorities, and checked snapshots. Exercise time and sensor failures with deterministic scenarios.
3. **Expression model.** Add slow context targets to the existing action model, new expressions, and explicit anger geometry. Preserve touch and motion regression coverage.
4. **Watch integration.** Connect background care, physical touch episodes, notification arrivals and alarms. Add a care screen with mood, needs, evidence, routines and remembered events. Add USB controls for inspectable scenarios without silently rewriting live needs.
5. **Audit and revise from failures.** Compile with warnings treated as errors in pure modules, run OS gesture scenarios, compare stripe/full renders, review expression images, and build the actual ESP-IDF target. Any failure or unsupported input changes the implementation and validation record before flashing.
6. **Deploy and verify.** Flash the existing connected watch while preserving NVS and the already saved factory recovery image. Verify boot, command round trips, bounded memory, notification arrival, care persistence after reset, apps and edge navigation. Return to Pip. Record what is physically observed, instrumented, simulated, or still awaiting wear testing.

## Limits of the first deployed version

The board has no local microphone, speaker, or motor, so no voice greeting, emotional speech analysis, or audio-only behavior is part of this implementation. Saying hello means opening/returning to Pip and touching it. Battery percentage and walking remain estimates rather than calibrated measurements. No software can infer an unseen charging failure or activity while the processor is asleep. User-facing explanations should communicate the current need rather than expose implementation terminology, while diagnostics retain the evidence details.

This is a tunable first implementation of the selected proposal. The full reports include additional experiments and acceptance scenarios; an item appearing in the study is not automatically a passed test or a deployed feature. The accompanying validation record must identify the actual coverage and unresolved physical evaluation.
