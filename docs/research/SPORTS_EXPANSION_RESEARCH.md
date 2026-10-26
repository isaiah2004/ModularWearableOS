# Sports expansion: evidence, scope and replay contract

Research checked 20 October 2026. Design recommendations, not a hardware-validation
report. Supplements `SPORT_MODE_RESEARCH.md`; preserve its GNSS validity and session
state rules. No device access or production source changes were made for this note.

## Evidence and limits

* Dominant and non-dominant wrist acceleration have systematically different
  distributions; applying identical activity cut points changes activity-time
  estimates. This study supports placement-aware validation, not a universal
  correction factor for this watch or badminton:
  [Migueles et al., original study](https://www.nature.com/articles/s41598-019-54267-y).
* A badminton recognition study used racket and racket-holding wrist sensors.
  Its recognition results do not establish performance from the opposite wrist:
  [original study](https://www.nature.com/articles/s41598-025-25158-2).
  [MultiSenseBadminton](https://www.nature.com/articles/s41597-024-03144-z)
  provides richer, synchronized body/hand measurements; it is a useful reference
  dataset, not a watch-only performance guarantee.
* [PAMAP2, original dataset documentation](https://archive.ics.uci.edu/ml/machine-learning-databases/00231/readme.pdf)
  uses dominant wrist, chest and ankle IMUs (100 Hz) plus HR. Evaluate wrist-only
  subsets explicitly; results from all sensors do not transfer automatically.
  [REALDISP, original dataset](https://archive.ics.uci.edu/dataset/305/realdisp+activity+recognition+dataset)
  deliberately varies sensor placement, illustrating a relevant validation axis.
* Manufacturer guidance describes movement/fit limitations of optical HR:
  [Garmin accuracy guidance](https://support.garmin.com/en-CA/?faq=xQwjQjzUew4BF1GYcusE59&productID=742133&tab=topics).
  This is qualitative guidance, not measured accuracy for this board.

Engineering inference: one wrist IMU observes that wrist's specific force and
angular velocity, not racket impact, ball trajectory, leg position or tactical
outcome. Integration drifts and arm geometry is unknown. Do not label wrist peak
acceleration as shuttle speed, ball speed, shot power, distance travelled or impact
force. Even playing-wrist shot classification needs labeled validation.

## Placement model and automatic wrist side

Store independent `worn_wrist={unknown,left,right}`, `dominant_hand={unknown,left,right}`,
and `playing_hand={unknown,left,right,both}` per applicable sport; batting and bowling
hands may differ. Record provenance (`user`, `suggested`, `unknown`) and freeze a
session snapshot. Dominant hand never determines worn wrist automatically. Racket
hand, cricket bowling/batting hand and volleyball hitting hand affect interpretation.

Magnitude features are rotation invariant and discard left/right information.
Unconstrained mirrored motion plus unknown strap orientation has no unique wrist
side solution. Gravity alone is insufficient. A guided, geometrically constrained
gesture with known screen orientation might suggest a side; this requires
bilateral data. The implementation now includes an experimental guided detector
using explicitly learned left/right examples, with user confirmation. That
prototype is not evidence of reliable passive anatomical wrist detection.
The uncalibrated or uncertain result is `unknown`; confirmation remains
authoritative. Do not display invented percentage
confidence for a hand-written heuristic. A trained suggestion needs held-out
participant calibration, reliability curves, error among accepted predictions,
coverage and an abstention threshold. Include reversed watch orientation, loose
strap, opposite wrist, missing channels and out-of-distribution gestures; abstain
when sensor quality or calibration is inadequate. Never silently change settings.

Recording context: the user explicitly confirmed BOTH copied `0002.csv` and
`0005.csv` are badminton played with the RIGHT hand while the watch was on the
LEFT wrist. Dominant hand was not stated and remains unknown. These are
non-playing-wrist recordings, not playing-wrist stroke training data. Exact C
replay reports and labeled plots are in
`build/sports-reliability/audit-confirmed-badminton-v2/`; source hashes match the
initial audit and parameters were not fitted to these recordings. Estimated
movement is 68.040 seconds / 15 episodes over 254.040 seconds for 0002, and
223.760 seconds / 8 episodes over 267.760 seconds for 0005. These episode counts
are not shots, rallies or validation accuracy.

## Catalog by shared algorithm family

The following 40 selectable profiles are labels, not automatic sport recognition.
All support recording/paused time, manual laps and quality-gated HR when available.
Unavailable sensors produce missing values, never zeros posing as measurements.

| Family | Profiles | Additional useful outputs / restrictions |
|---|---|---|
| Outdoor locomotion (7) | walking, running, trail running, hiking, trekking, race walking, Nordic walking | accepted GNSS route/distance/pace, session steps; filtered elevation when validated; poles change wrist cadence |
| Indoor locomotion (3) | treadmill, indoor walking, stair climbing | steps/cadence estimates; no distance without calibration; stair count requires validation |
| Cycling (4) | road cycling, mountain biking, gravel cycling, indoor cycling | GNSS speed/distance outdoors; no wrist pedal cadence or watts |
| Court/racket (6) | badminton, tennis, table tennis, squash, pickleball, padel | estimated movement bursts/active time; no automatic shots initially |
| Team/field (7) | football/soccer, cricket, basketball, volleyball, handball, field hockey, rugby | movement bursts; outdoor GNSS route optional; no score, contacts, kicks or ball speed |
| Conditioning (8) | indoor workout, strength training, HIIT, circuit training, aerobics, dance, jump rope, boxing fitness | bursts/interval timing; repetitions/jumps/punches need separate validation |
| Controlled movement (3) | yoga, Pilates, stretching | time and HR; low wrist movement does not mean inactivity or low effort |
| Row/paddle (2) | indoor rowing, kayaking | time/HR, outdoor route for kayaking; stroke rate requires labeled periodicity validation |

Water-related profiles are recording labels only, conditional on device suitability;
this catalog does not establish water resistance. Swimming is deliberately omitted.
Distinct profiles can share a conservative measurement engine; thirty names do not
justify thirty unsupported classifiers.

## Priority outputs

| Sport | Measurements available with appropriate valid sensors | Estimates to label explicitly | Withhold initially |
|---|---|---|---|
| Football | timer, manual halves/laps, valid HR, accepted outdoor GNSS route/distance | wrist movement-active time/bursts; GNSS peak speed only with filtering and suitable sampling | kicks, possession, goals, exact sprints/change-of-direction counts |
| Cricket | timer, manual overs/role laps, valid HR, outdoor route if useful | wrist bursts with batting/bowling hand metadata | deliveries, runs scored, shot type, bowling/ball speed |
| Badminton | timer, manual games, valid HR | wrist movement-active time, movement bursts, burst duration and recovery gaps | rallies, shots, smash speed, precise stroke type; especially on opposite wrist |
| Running | timer, valid HR, GNSS route/distance/pace, session step-counter delta | cadence from accepted steps/time, filtered moving time | stride length/form, VO2max, power without validated algorithms |
| Hiking | timer, valid HR, GNSS route/distance, session steps, raw altitude with source | filtered ascent/descent and moving time only after validation | wrist-derived terrain grade or reliable ascent from noisy unfiltered altitude |

Session recording time (manual pauses removed), movement-active time (detector),
and physiological exertion are different quantities. A badminton-ready stance or
cricket waiting period may be sporting participation without visible wrist motion.

## Conservative burst engine (proposed, needs validation)

Normalize documented units first; check sample rate, gaps, clipping and freshness.
Compute orientation-insensitive acceleration magnitude relative to local gravity
and gyro magnitude, then short-window RMS/dispersion. Use separate on/off thresholds,
minimum duration and refractory/merge intervals. Estimate quiet baseline only
when quiet samples are identified; a recording that begins mid-rally is not a
calibration pause. Bounded adaptation avoids learning vigorous motion as rest.
Use actual timestamps for durations; reset or mark unknown over gaps and pauses.
Count merged movement episodes, not each peak. Do not backfill sensor gaps as rest.
Report valid coverage beside active time. Gyro or acceleration-only fallback must
be documented and evaluated separately, with no mixing of feature units.

Thresholds/windows are engineering parameters, not published sport-specific
constants. Compare against a simple time-only baseline and manually annotated
movement episodes before exposing an estimated metric as useful.

## Energy estimates

Optional broad activity-category MET estimates can use the
[2024 Compendium](https://pacompendium.com/) and its
[standard-versus-corrected MET explanation](https://pacompendium.com/corrected-mets/).
Standard gross estimate: `kcal = MET * 3.5 * mass_kg / 200 * minutes`.
Approximate net energy uses `(MET - 1)` with the same convention. State which is
used, activity category/intensity assumption and mass provenance. Without mass,
omit kcal. These are population reference values, not an individual's measured
calories. Do not infer MET or individual energy accuracy from movement bursts or
the four-minute sample; avoid double-counting pauses or applying competitive-play
MET to all waiting time. Omission is preferable to false precision.

## Offline CSV audit/replay contract

Keep original CSV immutable and record SHA-256. Required normalized columns:
`timestamp_us,ax_mps2,ay_mps2,az_mps2,gx_rad_s,gy_rad_s,gz_rad_s`.
Support explicit mappings for source names/units, never guess silently. Optional
columns: `hr_bpm,hr_valid,latitude_deg,longitude_deg,gps_valid,altitude_m,steps_total`.
Sidecar JSON records source hash, source timestamp/units mapping, sample rate if
known, axis convention, watch orientation, wrist/dominant/playing hand with
provenance, sport, firmware/config version and manual pause intervals. Distinguish
record-receive time from sensor time if both exist. Do not assume every row is a
fresh IMU measurement; record duplication/stale-value detection limits.

Audit output JSON: row count, duration, monotonic violations, duplicates, dt
quantiles, inferred rate, gaps, finite/missing counts per channel, units and
saturation checks, gravity-magnitude distribution and coverage. Replay output CSV:
window start/end, coverage, accel/gyro features, quiet-baseline state, threshold,
movement state, burst ID, cumulative active time and quality flags. Save exact
configuration and version with results. Generate a standalone time plot of raw
magnitudes, thresholds, episodes and missing spans. Run offline with no device,
network, SD settings changes or COM3 dependency.

Tests: equivalent units and irregular timestamps; constant gravity at varied
orientations; zero gyro; no movement; isolated spikes versus sustained motion;
bursts separated by short/long gaps; missing samples; pause/reset boundaries;
NaN/Inf/clipped inputs; timestamp reversal; deterministic replay. Numeric features
may be invariant to coordinate rotations; anatomical left/right need not be.

## Validation and avoiding overfitting

Treat the four-minute recording as an unlabeled engineering/quality case unless
synchronized annotation exists. It can establish signal availability, ranges,
sampling gaps and detector plausibility; it cannot establish shot accuracy or
wrist identification. Do not tune and report accuracy on the same sample.
Collect participants with both dominant hands, both watch wrists, playing and
non-playing wrist, repeated days, varied fit/orientation, skill and intensity;
include walking, cheering, adjusting clothing/watch, waiting and non-sport arm
gestures as negative examples. Obtain synchronized video or observer event/time
labels and retain annotation uncertainty. Cricket must cover batting, bowling,
fielding/waiting; football/drills and badminton warm-up differ from matches.

Split by participant and session before overlapping windows are generated; hold
out a final participant/session set. Tune only training/development data, publish
parameter sweeps and failures, and keep this user's sample as an explicit case
study rather than population evidence. Report episode precision/recall with stated
matching tolerance, active-duration absolute error, coverage and placement-specific
results. Shot models additionally need per-class confusion matrices, unknown-class
rejection, event counts and unseen-player tests. A successful simulator replay is
not hardware, sporting or physiological validation.

## Implemented gross category energy assumptions (20 October 2026)

User-authorized broad category assumptions, not individual energy measurements. 
Every row is an application estimate; `P` additionally means a proxy mapping. 
`E` means the source marks its category as estimated (red activity code); `L` 
means it is not red and the source says published literature supports it. 
These flags describe reference provenance, not validation of this watch. 
No motion, HR, GPS pace or playing-hand feature chooses these METs. 
Values with a decimal preserve the reference lookup, not individual precision.

Sources: [walking](https://pacompendium.com/walking/), 
[running](https://pacompendium.com/running/), [cycling](https://pacompendium.com/bicycling/), 
[sports](https://pacompendium.com/sports/), 
[conditioning](https://pacompendium.com/conditioning-exercise/), 
[dance](https://pacompendium.com/dancing/), 
[water](https://pacompendium.com/water-activities/).

| ID / profile | MET | Reference code | Flags | Assumption |
|---|---:|---|---|---|
| 0 WALK | 3.8 | 17190 | L | Level moderate walk |
| 1 RUN | 7.5 | 12020 | L | Recreational general jogging |
| 2 CYCLE | 7.0 | 01016 | L | Moderate cycling |
| 3 HIKE | 5.3 | 17082 | L | Normal no-load hiking |
| 4 TREADMILL | 3.8 | 17355 | L/P | Level moderate treadmill WALK |
| 5 WORKOUT | 3.8 | 02022 | L/P | Moderate calisthenics |
| 6 FOOTBALL | 7.0 | 15610 | L | Casual soccer |
| 7 CRICKET | 4.8 | 15150 | L | Mixed cricket roles |
| 8 BADMINTON | 5.5 | 15030 | L | Social badminton |
| 9 TRAIL RUN | 7.5 | 12020 | L/P | General jogging; terrain ignored |
| 10 TREKKING | 5.3 | 17082 | L/P | No-load normal hiking proxy |
| 11 RACE WALK | 6.5 | 17110 | E | General race walking |
| 12 NORDIC WALK | 4.3 | 17302 | L | Moderate level poles |
| 13 INDOOR WALK | 3.8 | 17190 | L/P | Level moderate walking proxy |
| 14 STAIRS | 6.8 | 17131 | L | General stair climbing |
| 15 MOUNTAIN BIKE | 8.5 | 01009 | E | General mountain cycling |
| 16 GRAVEL BIKE | 5.8 | 01013 | L/P | Moderate dirt-road cycling proxy |
| 17 INDOOR CYCLE | 6.8 | 01200 | L | General stationary cycling |
| 18 TENNIS | 6.8 | 15675 | L | Moderate tennis |
| 19 TABLE TENNIS | 4.0 | 15660 | L | General table tennis |
| 20 SQUASH | 7.3 | 15652 | L | General squash |
| 21 PICKLEBALL | 4.5 | 15685 | L/P | Recreational tennis-doubles proxy |
| 22 PADEL | 4.5 | 15685 | L/P | Recreational tennis-doubles proxy |
| 23 BASKETBALL | 6.0 | 15050 | L | Non-game basketball |
| 24 VOLLEYBALL | 3.0 | 15720 | L | Recreational team volleyball |
| 25 HANDBALL | 8.0 | 15330 | E | Team handball |
| 26 FIELD HOCKEY | 7.8 | 15350 | L | General field hockey |
| 27 RUGBY | 6.3 | 15562 | L/P | Noncompetitive touch-rugby proxy |
| 28 STRENGTH | 3.5 | 02054 | L | Multiple-exercise resistance |
| 29 HIIT | 7.0 | 02210 | L | Moderate-effort intervals |
| 30 CIRCUIT | 5.0 | 02035 | L | Moderate circuit |
| 31 AEROBICS | 4.8 | 02005 | L | Moderate low-impact aerobics |
| 32 DANCE | 3.8 | 03070 | L/P | General contemporary dance proxy |
| 33 JUMP ROPE | 11.0 | 02068 | L | General rope skipping |
| 34 BOXING FITNESS | 5.8 | 15110 | L/P | Punching-bag fitness proxy |
| 35 YOGA | 2.3 | 02175 | L | General yoga |
| 36 PILATES | 2.8 | 02105 | L | General Pilates |
| 37 STRETCHING | 2.3 | 02101 | L | Mild stretching |
| 38 INDOOR ROWING | 5.0 | 02071 | L | Moderate stationary rowing |
| 39 KAYAK | 5.0 | 18100 | L | Moderate kayaking |

Run and hike use generic recreational assumptions without speed-personalization; 
normal hiking assumes no load, not steep or loaded trekking. Football means 
casual soccer. Cricket averages batting/bowling/fielding roles; it cannot represent 
long inactive waits accurately. Badminton assumes social play. Recreational 
does not necessarily mean the physiological moderate-intensity band.

Frozen session mass (user supplied, zero means unset) and category MET produce 
`gross kcal = MET * 3.5 * mass_kg / 200 * observed_running_minutes`. Manual 
pauses and reboot downtime are excluded. Unpaused waiting, slow portions and 
recovery rests retain the category assumption and may substantially overestimate 
energy; vigorous portions may be underestimated. This is deliberately independent 
of movement-active-time detection and does not measure exertion or calories. 
No entered mass or no MET means unavailable (stored zero, exported null). 
Display gross category estimate with rounded kcal, never an individual precision 
claim. Frozen MET occupies the previously reserved V2 extension byte; V1 
migration leaves mass/MET unset and does not invent historical energy.

## GNSS outlier-bound inheritance

Engineering bounds (not personal performance limits): cycling family 35 m/s;
running, trail running and team-sport family 12 m/s; race walking 8 m/s; remaining
walking/hiking profiles 5 m/s. Outdoor paddling uses a generous 12 m/s bound.
The original walk/hike 5, run 12 and cycle 35 bounds are unchanged. These gates
apply both to module-reported speed and implied coordinate displacement; existing
fix freshness/quality, duplicate timestamps, jitter, gaps and jumps retain their
independent checks. Adding a profile must not silently assign it a walking bound.
