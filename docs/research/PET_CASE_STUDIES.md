# Pip: companion and virtual-pet case studies

Research and design report, 13 September 2026. Primary sources accessed 12–13 September 2026. Prepared for the Waveshare ESP32-S3-Touch-AMOLED-1.43-B port.

Code observations in this study describe the initial audit. The resulting
implementation and later revisions are tracked in [the implementation plan](PET_IMPLEMENTATION_PLAN.md),
[usage guide](../PIP_LIFE.md), and [validation record](PET_VALIDATION.md).

## Purpose and evidence standard

Pip should remain a pair of expressive glowing eyes, with the personality and physical responsiveness of a small robot companion. The expanded brief requires an actual pet simulation: persistent needs, moods, attention, charging care, walks, responses to notifications, and reactions that depend on the relationship. This report evaluates mechanisms that can support that experience on the current watch, then revises proposals against its constraints.

**Documented fact** below means a mechanism described in an official manual, original SDK, creator presentation, or primary research. **Design inference** means a proposed transfer to Pip. **Hypothesis** means an outcome that needs testing with this user and hardware. The sources do not establish that combining their mechanisms will automatically make Pip feel alive. Product descriptions document intended behavior, not controlled evidence of emotional attachment.

The research deliberately emphasizes inspectable mechanisms and repeatable experiments. It does not reproduce commercial character assets or claim to reconstruct Cozmo's proprietary personality engine.

## 1. What the watch can actually perceive

The implementation inspected for this report has a 466 × 466 RGB565 display, FT3168 touch, QMI8658 accelerometer/gyroscope, RTC, ADC voltage readings, USB control, local persistent storage, and a notification store. It has no built-in microphone, speaker, or haptic motor. The software rotates the image and touch coordinates to the case orientation. IMU axes must be interpreted in that same visible frame. The board record and source are the authority for these constraints: [hardware record](../HARDWARE.md), [Pip input model](../../core/pip.h), and [notification store](../../core/notif.h).

| Input | Defensible interpretation | Boundary for the design |
|---|---|---|
| Touch position, motion, duration | Finger attention, boop, stroke, hold, gesture | A hand near the screen is invisible until contact |
| Accelerometer and gyro | Tilt, handling, rocking, sustained movement, strong rotation | Movement alone does not prove walking, outdoor location, or who is wearing it |
| RTC/local time | Time of day and elapsed time when valid | Clock synchronization, timezone changes, and reset require explicit handling |
| ADC/system voltage | Approximate battery condition and a possible external-power clue | USB may raise the measured system rail; voltage is not charge current or precise state of charge |
| New notification event | Something arrived on the connected watch | Arrival is not evidence that the message is happy, urgent, or upsetting |
| Visits and interaction history | Familiarity and preferred interaction patterns | It does not identify faces, voices, or distinct people |

The existing battery model explicitly acknowledges inference and uses a simple voltage-to-percentage map. Care mechanics must not treat that estimate as a precise fuel gauge. The current source also updates needs inside the visible face; a persistent pet needs a separate low-rate service so its history survives changing screens. These are implementation observations, not claims about all hardware revisions. See [battery model](../../core/battery.c) and [current pet model](../../core/pip.c).

## 2. Case study: Tamagotchi's explicit care loop

**Documented facts.** Bandai's Original and Connection manuals expose hunger and happiness, attention calls, feeding, play, sleep-related care, health, and development influenced by care. Connection additionally exposes training and other status information. The pet makes a need legible, the player acts, and its visible state changes. These are deliberately game rules rather than an attempt to estimate a biological animal. [Original manual](https://tamagotchi-official.com/manual/toy/original/Original_webmanual.pdf?20241028113631=), [Connection manual](https://tamagotchi-official.com/manual/toy/connection/connection_web_manual_IS_EN.pdf)

**Design inference for Pip.** Preserve the causal loop while replacing redundant chores with activities the watch can observe. Charging can satisfy physical energy; petting satisfies social comfort; play and qualified movement satisfy stimulation; quiet nighttime rest satisfies rest. An eye expression provides the first signal, with a small need indicator or inspectable pet-status screen when its meaning is unclear.

**What does not transfer cleanly.** A virtual hunger bar unrelated to charging would make two competing fuel systems. Minute-scale decay would conflict with ordinary watch use. A user who changes to another face has not necessarily abandoned the pet. Pip should recognize missed care over appropriate elapsed time, then show a short, recoverable sulk when next visited. Do not let routine OS navigation become a care failure.

**Candidate revision.** Start with four needs, not a screen full of meters. Make every negative expression traceable to one of them. Preserve a warm relationship separately from temporary frustration, so a low battery does not erase days of companionship.

## 3. Case study: Cozmo's animation grammar and contextual variation

**Documented facts.** Anki's original Cozmo SDK distinguishes requesting a particular animation from requesting an animation trigger. Trigger selection can choose among animations using mood, emotion, or random weighting. Anki animators Molly Jameson and Daria Jerjomina described a pipeline combining authored animation, physical constraints, programmatic corrections, and tools for quick testing on the robot. [Original SDK](https://github.com/anki/cozmo-python-sdk/blob/master/src/cozmo/anim.py), [Anki's GDC 2017 presentation](https://www.gdcvault.com/play/1024221/Cozmo-Animation-Pipeline-for-a)

**Design inference for Pip.** A semantic event such as `petted` should select a small family of performances rather than one immutable clip. A rested Pip can lift into an exuberant crescent; a tired Pip can close its eyes and lean into the finger; an annoyed Pip can glance back, hesitate, then soften. All three are recognizably positive responses to the same care action.

Build a shared eye rig: position, scale, top/bottom lids, left/right asymmetry, slight lean, glow, and blink closure. Author short trajectories in those parameters. Favor readable silhouettes and timing over ornamental particles. The expression should still work as a grayscale thumbnail on a small display.

**What does not transfer cleanly.** Cozmo also performs through a moving body, sound, and physical objects. Pip cannot compensate for absent channels by making every screen element move constantly. Use a glance before an action and a clear settling phase afterward to supply intent within the eye display.

**Candidate revision.** Keep a stable personality: curious, affectionate, slightly mischievous, briefly grumpy when needs are missed. Variation changes delivery, not the meaning of the action. A boop should not randomly produce an unrelated angry response without context.

## 4. Case study: Vector's separation of intent and performance

**Documented facts.** Vector's original SDK describes coordinated animation tracks for face, body mechanisms, lights, and audio. Its API separates semantic animation triggers from named animations, and notes that the chosen variant can depend on mood, emotion, or weighting. The API recommends triggers for callers that should remain stable as individual animations change. [Original Vector animation module](https://github.com/anki/vector-python-sdk/blob/master/anki_vector/animation.py)

**Design inference for Pip.** Separate `why Pip reacts` from `how Pip performs it`. The event detector produces `new_notification`, `care_received`, `low_energy`, or `walk_completed`; the behavior selector chooses a semantic reaction; the animator blends it into the current pose. USB testing should expose both levels: force a performance for visual review, or inject a synthetic event to test the complete causal chain.

**What does not transfer cleanly.** An SDK animation API does not establish the exact internal decision architecture of the commercial robot. Pip can adopt this interface separation without claiming parity with Vector's autonomous behavior.

**Candidate revision.** Add event IDs, timestamps, confidence, deduplication, cooldowns, and expiry. A repeated notification must not repeatedly restart the same surprise animation. A petting move should prolong pleasure, not reset its sway every few hundred milliseconds.

## 5. Case study: aibo's recognizable identity over time

**Documented facts.** Sony describes aibo's behavior and personality as changing through its life and interactions with its owner. Sony also describes an implementation spanning the device and cloud, using sensors and cameras to adapt to people and surroundings. These capabilities are broader than those of the watch. [Sony help guide](https://helpguide.sony.net/aibo/ers1000/v1/en-us/contents/TP0001970093.html), [Sony's development account](https://www.sony.com/en/SonyInfo/blog/2024/08/06/)

**Research evidence.** Lee, Park, and Song's month-long experiment used 40 participants and compared developing versus already mature AIBO behavior. The reported development condition improved perceived lifelikeness, social presence, and social responses in that study. This supports investigating visible development; it does not determine Pip's correct progression rate. [Original study, 2005](https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1468-2958.2005.tb00882.x)

Kertész and Turunen surveyed 78 long-term AIBO owners. Interaction and autonomous operation were prominent desired capabilities; memory and learning were less strongly prioritized than the researchers expected. The recruited long-term owners are a selected population, so their preferences should not be treated as those of all prospective users. [Original paper in the authors' university repository, 2017](https://trepo.tuni.fi/bitstream/handle/10024/140088/978-952-03-2443-8.pdf?isAllowed=y&sequence=2)

**Design inference for Pip.** Persist a few traits that visibly influence behavior: familiarity, confidence, affection, and preference for gentle petting versus active play. Expose growth through a warmer greeting, a favorite wink, and a wider repertoire. A large hidden memory has little value if the next encounter feels identical.

**Candidate revision.** Learn slowly from distinct sessions and meaningful care events. Do not grant unlimited relationship growth from touch samples at 50 Hz. Keep a baseline temperament stable; persistent variation should make this Pip recognizable across restarts.

## 6. Case study: Kismet's needs influence behavior, not only the face

**Documented facts.** The MIT Kismet framework distinguishes drives from emotions. Drives regulate internal needs toward desired ranges and influence behavior selection; emotions arise from appraisals of beneficial or detrimental stimuli. Affect also influences attention and motor expression. [MIT's original framework](https://groups.csail.mit.edu/lbr/sociable/kismet.html), [Breazeal's original research paper](https://robots.media.mit.edu/wp-content/uploads/sites/7/2015/01/Breazeal-Agents-99.pdf)

**Design inference for Pip.** A mood must change choices as well as eyelids. A bored Pip searches for something to do; a comfortable Pip settles; an overstimulated Pip pauses after a startle; a low-energy Pip reduces play initiative. Temporary joy after petting can coexist with unresolved physical tiredness. The underlying energy need remains until it is actually addressed.

**What does not transfer cleanly.** Kismet's sensing and expressive body differ considerably from two watch eyes. Pip should not infer human emotion from touch pressure, notifications, or motion without an implemented and validated sensor model.

**Candidate revision.** Use a small need model to generate motivations, a continuous mood to shape delivery, and a discrete action to organize the immediate performance. Avoid making a single enum carry all three responsibilities.

## 7. Animation and affect theories that fit the screen

**Gaze and attention.** Disney Research's gaze system selects salient targets and combines attention habituation, saccades, and motions with different response characteristics. Its layered control approach produces complex behavior from simpler motion processes. [Pan and colleagues, 2020](https://la.disneyresearch.com/publication/realistic-and-interactive-robot-gaze/)

For Pip, the design inference is an ordered attention target: active finger, an important event, play object, meaningful tilt, then idle exploration. A target needs a dwell period; a new random gaze every frame looks like noise. Repeated unimportant stimuli should gradually lose influence. Faster gaze and slower expressive settling should remain independently adjustable.

**Continuous affect.** Russell's circumplex work organizes affect using pleasantness and arousal, supported by analyses of affect terms and self-report. It is a descriptive psychological model, not a validated controller for this watch. [Original paper, 1980](https://pdodds.w3.uvm.edu/research/papers/others/1980/russell1980a.pdf)

For Pip, use valence to bias welcoming versus withdrawn posture, and arousal to bias speed and openness. Keep a separate cause tag: high arousal and low valence could mean startled, frustrated, or overstimulated, which need different performances. Two mood numbers alone cannot choose the correct story.

**Animation composition.** Ribeiro and Paiva describe a robot animation pipeline that generates, combines, and blends animation sources during real-time interaction. [Original paper, 2019](https://arxiv.org/abs/1904.02898)

For Pip, blend a long-lived mood posture, action trajectory, target gaze, and breathing. Apply a brief blink after the slower expression filters so it really closes. Preserve the current pose when an action is interrupted; recovery should begin from the face actually on screen.

**Limits of novelty evidence.** A six-week qualitative study of robot kittens describes novelty as involving expectations, technological impressions, and developing relational experiences. The study's circumstances and qualitative method do not establish one universal strategy for all companions. [Original study, 2022](https://pmc.ncbi.nlm.nih.gov/articles/PMC8961975/)

The implication to test here is that daily context and predictable reciprocity may matter more than indefinitely adding animation names. Measure whether the user still understands and enjoys Pip after familiarizing themselves with its repertoire.

## 8. Candidate proposals, criticism, and revision

All proposals and timings from this point are design recommendations, not results established by the cited studies.

| Candidate | Strength | Failure under this brief | Revision |
|---|---|---|---|
| A. Random expressive eye screensaver | Immediate charm; inexpensive | No causal needs, no durable relationship | Retain only as the lowest-priority idle layer |
| B. Strict Tamagotchi care scheduler | Clear responsibility and consequence | Chores can conflict with wearing a watch; absence is misread | Use real charging/rest/movement and long, bounded care windows |
| C. Large discrete emotion state machine | Easy to demonstrate individual reactions | State explosion; difficult to be tired and happy simultaneously | Separate needs, continuous mood, immediate action, and persistent traits |
| D. Cloud language-model character | Could add conversation with future hardware | Current board lacks audio; latency and network dependency complicate every tap | Keep perception and pet life local; future language features may comment on existing state |
| E. Local layered pet with semantic events | Fits present sensors; testable and persistent | Requires careful tuning and clear causes | Recommended basis, with a small initial set of needs and visible debug evidence |

The revised proposal is E, using A as an animation layer and the legible care loop of B. It makes being alive a property of the complete feedback loop: perception, memory, need, choice, expression, and recovery.

## 9. Revised needs, relationship, and reactions

| State | Source and change | Visible effect | Meaningful recovery |
|---|---|---|---|
| Physical energy | Smoothed, confidence-tagged power information | Droop, lower play intensity, brief charge request | Confirmed power recovery; no instant refill from petting |
| Rest | Local day/night context and quiet elapsed time | Longer blinks, slower responses, comfortable sleep | Rest period or explicit wake interaction |
| Social comfort | Time since meaningful care, petting, visits | Curiosity, lonely glance, then brief annoyed side-eye | Petting/hold/greeting; ordinary app use remains possible |
| Stimulation | Play and qualified sustained movement | Exploration, boredom, eager play invitation | A short game or movement session |
| Familiarity | Distinct positive sessions and shared routines | Relaxed boops, greeting variants, confidence | Changes slowly and persists independently of a temporary mood |

### Charging care and getting grumpy

A low-energy expression should first communicate the actionable need. If a trustworthy low-power condition persists through a generous grace period, Pip can give a clearly annoyed look: narrow inward lids, a short glance at the power indicator, then a sulky look away. It should remain touch-responsive. When external power becomes plausible, show relief; call it confirmed charging only if the hardware model supports that claim. A momentary voltage rise should not award a lasting care reward.

“Forgot to charge on time” should mean a real low-power episode that remained unresolved, not an arbitrary daily alarm. If a routine is later learned, base it on several confirmed sessions and give it a broad time window. During development on USB, suppress missed-charge penalties because the measurements may not represent battery operation. Hardware uncertainty is a reason to withhold a negative judgment, not to invent one.

### Ignored, then reunited

Use a progression: quiet self-entertainment → occasional invitation → mild sulk on a later visit. A good first return sequence is averted gaze, one eye checking the user, a softening blink, then acceptance when petted. This allows an annoyed personality while preserving a satisfying way to reconnect. Do not replay every missed invitation after an overnight gap. Consolidate history into one current mood and one greeting.

Initial tuning can use hours for relationship context and seconds for expression. For example, after a long daytime gap, an annoyed look could last 1–3 seconds, while residual low comfort recovers across a short genuine interaction. These values must be tuned; they are not final care requirements.

### Walks and physical play

Reward movement that passes a sustained cadence/continuity check, not every acceleration peak. The first implementation should label it internally as a qualified movement session until wrist-walking tests distinguish walking from shaking, typing, and vehicle movement. Accumulate evidence over multiple seconds, tolerate brief gaps, and give capped progress at minute-scale intervals. Pip can look energized during the session and proud at its completion. Without location input, do not claim it knows that the walk was outdoors.

### Notifications and busy periods

A new notification can trigger a small upward glance toward the notification shade, a brief alert expression, and a return to the previous activity. Arrival, viewing, and clearing are different events. Use a monotonically increasing notification revision/event ID; queue length alone misses arrivals when a bounded store is full. Summarize a burst as one reaction and let active petting finish before a low-priority glance. Notification content should not determine an emotion unless an explicit interpretation system is introduced.

### Reaction priority

Use immediate strong motion as a short reflex, direct touch as an interactive override, then low-energy care and meaningful context events, then autonomous behavior. Expired events are discarded rather than replayed. The pet's emotional expression must not block the launcher, notification shade, or other watch functions. A renderer failure or missing sensor falls back to a stable neutral/curious face rather than a punishment state.

## 10. Eye performance vocabulary

| Performance | Anticipation / notice | Main gesture | Recovery |
|---|---|---|---|
| Boop | Local gaze and squash, 0–80 ms | Spring/open, 80–240 ms | Familiar happy squint by roughly 850 ms |
| Petted | Follow finger for roughly 180 ms | Crescent lids; gentle leaning sway | Sustain while touched; soften over roughly 600 ms after release |
| Annoyed | Pause and narrow inward lids | Look away, small uneven lid lift | Check back with one eye; soften when care arrives |
| Startled | Brief reflex compression | Wide eyes and restrained upward jump | Inspect the source, then curious |
| Dizzy | One surprised beat | Decaying orbit and uneven lids | Stabilize, corrective blink, stretch |
| Wake | One eye opens first | Other eye opens, brief inspection | Stretch and a context-dependent greeting |
| Notification | Glance toward shade | Brief attentive hold | Return toward previous gaze target |
| Movement success | Notice progress cue | Happy lift and asymmetric wink | Comfortable, energetic idle |
| Charge relief | Look toward power cue | Release tense lids; slow contented blink | Rest while physical energy recovers |

Keep the pupils/eye blocks coherent as one character. Use asymmetry deliberately, around 2–4% at neutral and more for readable reactions. Blinks can begin with a 45 ms close, 25 ms hold, and 100 ms reopen, with an occasional contextual double blink. Test these at the measured on-device frame cadence: a visually important phase should survive at least a couple of rendered frames. Preserve contrast against black without using color as the only emotional cue.

## 11. Implementation plan revised for the ESP32

1. **Create a portable pet-life service.** Update needs and event history at a low rate even when another app is visible. Give animation its own frame-rate update. Compute expensive sensor filtering once per real sample, not once per render stripe.
2. **Define an input contract.** Carry sample validity and timestamps; expose notification revision, local time validity, power confidence, activity evidence, and foreground interaction. Keep inferred labels separate from raw observations.
3. **Add bounded persistence.** Store versioned needs, relationship/traits, meaningful timestamps, and a bounded daily history. Check data validity, migrate old Pip memory, coalesce writes, and save on orderly shutdown. Rendering must not write flash.
4. **Handle elapsed time explicitly.** Use monotonic time for gestures and action duration; validated wall time for cross-boot history. On a clock jump or invalid RTC, cap catch-up and avoid synthesizing missed care. Re-entering the face must not integrate a long pause as violent motion.
5. **Add semantic event selection.** Deduplicate, assign priority, set expiry and cooldown, and retain a short recent-performance history. Repeated petting extends one action; it does not reset its oscillator.
6. **Compose the eye rig.** Combine mood posture, short action trajectory, gaze, breath, and a final blink mask. Keep display time visible. New scanline eye geometry should preserve dirty rendering and subpixel motion.
7. **Keep the main experience local.** No network or SD card is required for the pet to function. A future SD diary or richer assets can be optional. USB commands should expose both real state and explicitly synthetic test inputs.

A full 466 × 466 RGB565 frame contains 434,312 bytes. At 40 MHz QSPI on four data lines, the ideal data-only transfer time is about 21.7 ms; at exactly 50 MHz it is about 17.4 ms, before command, software, or scheduling overhead. These are arithmetic lower bounds, not measured frame rates. The requested peripheral clock, actual attainable ESP32 divider, panel limits, and hardware validation must be recorded separately. Tight dirty regions and economical drawing remain useful regardless of the final clock choice.

## 12. Validation and design hypotheses

| Hypothesis | Experiment | Evidence to collect |
|---|---|---|
| Contextual response variants feel more personal than random clips | Repeat boops/pets when rested, tired, and annoyed | User can explain why the delivery differs |
| A recoverable sulk creates character without becoming tedious | Simulate a long gap, then pet Pip | Clear initial annoyance and visible reconciliation without blocking navigation |
| Movement happiness is credible | Record wearing, walking, sitting, typing, shaking, and vehicle sessions | False positives, detection delay, and missed sessions |
| Charging care feels fair | Real battery discharge and USB transitions with logged voltage/confidence | No missed-care penalty from measurement noise or developer USB use |
| Notifications make Pip feel aware | Single arrival, repeated arrival, saturated queue, burst while petted | One timely glance per relevant event; no animation restart storm |
| Needs are causal rather than decorative | Simulate low energy plus petting | Brief pleasure occurs, while the energy deficit remains legible |
| The eye animation is readable | Inspect stills and slow/normal-speed recordings | Confusion between happy, tired, annoyed, and startled; phase visibility |
| The system remains a watch | Exercise launcher and notification gestures in every pet state | OS controls remain available and touch alignment stays correct |

Use deterministic event traces and time acceleration for the life model. Cover a night boundary, DST/timezone or manual clock change, reboot, invalid persisted data, missing/stale IMU, and notification bursts. Compare complete RGB565 frames against the dirty/stripe rendering path rather than relying only on compressed previews. Keep synthetic test mode visibly separate from actual needs so a demo does not silently rewrite the relationship.

The visual investigation during this work found no missing clock pixels in the saved gallery: all 16 original BMPs contained the same 1,034 bright pixels in the clock region. A separate probe compared 320 dirty-rendered frames against fresh full draws at the same time and found zero mismatched pixels. The small eye-outline notches were present in both paths, implicating the joined geometry rather than damage tracking. A replacement rounded shape helper has passed translated/stripe clipping and guard-buffer tests; that is component evidence, not a claim that the entire pet revision has been deployed.

## 13. Decision record

The initial expression proposal emphasized blink timing, independent eyes, touch continuity, and recovery. The expanded virtual-pet brief changes the priority: those improvements become the performance layer of a persistent life model. The recommended design now requires causes that endure between visits, fair interpretation of power and movement, temporary negative moods, and care that visibly changes both the immediate face and the next encounter.

Research supports investigating clear care loops, contextual variation, attention, development, and layered animation. The exact need rates, charge grace periods, walking classifier, balance of affection and grumpiness, and long-term appeal remain design hypotheses. The implementation should make them observable and adjustable so the next revision follows actual use of this watch.
