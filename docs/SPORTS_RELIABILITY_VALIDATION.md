# Sports reliability validation

Status: 21 October 2026. Built and flashed from the recovered `smartwatch2.0`
workspace. Flashing verified the SD preferences checkpoint and excluded NVS.
Host checks, device checks and remaining physical validation are distinguished
below; none establishes sporting accuracy or unplugged battery endurance.

| Check | Result and scope |
|---|---|
| Strict firmware build | PASS; image `0x769d80` (7,773,568 bytes), remaining partition space 90,752 bytes. Compile-time workout V2 layout assertions passed on ESP target. |
| Simulator acceptance | 30/30 PASS; additional six targeted GPS/profile checks PASS after the family-bound correction. |
| Workout engine | Strict C checks PASS: session boundaries, sensor validity/gaps, independent/frozen placement, movement invariance/spikes, energy/mass/pause semantics, V1 migration, recovery, storage errors/readback and card lineage. |
| Workout export | 10 tests PASS, including actual C-bank cross-language reads, V1/V2 validation and frozen placement/MET fields. |
| Public workout API stress | 10,000 randomized actions PASS in the focused expansion run. |
| Raw recorder endurance/faults | PASS: two synthetic hours at 100 Hz, 720,000 samples, 3,064 deliberately small test segments; strict parsing and fault scenarios below. |
| Diagnostics | Eight checks PASS for queue/retention/export/storage behavior. |
| Preferences | PASS; existing settings provenance and checkpoint behavior retained. |
| Frame pacing fault handling | PASS; injected arm failure, missing/early callback and invalid deadlines return in bounded time with diagnostics. This does not identify a historical hang cause. |
| Command framing | PASS; malformed/control/overlong lines are rejected before dispatch, with bounded metadata-only diagnostics. Valid UTF-8 and raw transfer data remain supported. |
| USB downloads | 13 host checks PASS; paced download verifies each chunk and the declared full-file CRC. Timeout recovery is bounded and restricted to read-only USB downloads; a failed download preserves an existing destination. |
| Broader regression run | 98/99 PASS. The old launcher asset-budget assertion fails; see explanation below. |

Recorder tests cover abrupt process death, preserving a torn CSV tail, linked new
segments, durable STOP/no restart, fallback from a torn newest intent bank, both
banks corrupt, dirty STOP retry, card absence/reinsertion, short writes/ENOSPC,
fsync failures, and a sample injected exactly between the first rollover drain
and producer stop. The final drain retains that accepted sample. The small
segment limit deliberately exercises thousands of rollover paths and is not the
production 4 MiB segment size. Host backend stubs do not validate real ESP task
scheduling; task-creation serialization was additionally reviewed in source.

The single broader regression failure uses a 1.4 MB launcher asset-budget limit.
This recovered workspace includes 26 apps and approximately 3.476 MB of launcher
assets. That older assertion does not match the preserved app set; it remains
reported as a failure. It was not weakened, removed or represented as a pass.
The firmware's actual partition-fit check passes independently.

## Recording audit

The user explicitly confirmed BOTH copied recordings are badminton played with
the right hand and the watch on the left wrist. Dominant hand remains unknown.
Original files and their SHA-256 hashes remain unchanged.

| Recording | Duration / samples | Exact C replay estimates |
|---|---|---|
| 0002.csv | 254.040 s / 6,352 | 68.040 s movement, 15 episodes |
| 0005.csv | 267.760 s / 6,695 | 223.760 s movement, 8 episodes |

Both have approximately 25 Hz CSV cadence, finite channels and no engine-sized
sample gaps. Repetition checks concern rounded CSV values, not proven sensor
freshness. Conditional near-current-config sensor-rail observations are documented
in the report; historical settings are unknown, so they do not establish clipping.
The motion parameters were unchanged and were not fitted to these recordings.
Episode counts are not shots/rallies or an accuracy score. Without synchronized
labels these recordings cannot validate sport recognition or wrist detection.

Evidence directories/files:

- `build/sports-reliability/audit-confirmed-badminton-v2/`: report, provenance sidecars, exact replay CSV and labeled SVG/PNG plots.
- `build/sports-reliability/recorder-endurance-final.log`: final recorder endurance/fault results.
- `build/sports-reliability/firmware-device-final-build.log`: final strict firmware build.
- `build/workout-fuzz/20261020T175809109561Z/`: focused randomized public-API evidence.
- `docs/research/SPORTS_EXPANSION_RESEARCH.md`: primary citations, category MET assumptions and measurement/placement limits.

## Verified historical battery evidence

All 253 copied board CSV logs passed their transfer CRCs. The complete event
journal subsequently passed the new paced transfer: 7,834,048 bytes in 496.4 s,
with five retry requests and twelve duplicate frames handled. It contains
122,407 valid records, 122,261 unique IDs, 146 identical replays, no conflicting
IDs, no truncated tail and no record checksum failures. The earlier partial
legacy download is retained separately; it is not used for final duration claims.

`power-analysis-verified/report.json` and its plots show 139.14 covered minutes
in the 19:30–21:54 IST decline: 97.95 minutes SENSOR_IDLE, 41.01 ACTIVE, and
97.76 minutes with a logged 80 MHz ceiling. Idle panel level was at least 128/255
for 90.96 minutes. All 37 valid observations at ≤15% remained ACTIVE or
SENSOR_IDLE. Rail voltage, a requested CPU ceiling and uptime changes do not
measure current, instantaneous CPU frequency, battery capacity or crash causes.

## Device acceptance and limitations

`preflash-manifest.json` records the exact image hashes, source hashes, link-map
features and NVS exclusion. The flashed binary SHA-256 is
`e46d452721fb0e6f16f93e18d1ca6bdda4ac086f36dc1ccdf8a1ff8dd30f8ff0`.
On-device identity matched the ELF hash. Integrated pager navigation,
Solar/Orrery state, Developer Tools/System Trace, the 40-profile catalog
(eight repeated reopenings), Battery (three top-menu openings) and Battery
History (twelve top-menu openings during 100 Hz raw recording) responded.
The initial recording continued with display off and an 80 MHz ceiling.

Serial transport faults affected automation: NUL/substituted or missing
delimiters and a dropped `DONE` were observed. The harness records rejected-line
retries, enables the existing runtime USB reply guard, and resynchronizes before
retrying read-only queries. It does not replay ambiguous mutating commands.
A safe JTAG sample showed the UI/recorder tasks alive; the timeouts alone do not
prove UI deadlock. The first debugger attach reset the target because of its
flash-probe configuration. A later debugger session was followed by a panic
reset; its cause is unproven. These incidents are retained in the evidence and
are not relabeled as clean device passes.

The navigation capture with raw recording and USB command traffic reported
painted presentation intervals p95 59 ms, p99 72 ms and maximum 71.559 ms.
It does not establish the 30 FPS target; this combined workload missed it.
These measurements include automation overhead and are not an isolated
touch-only animation benchmark.

The separate clean recorder run completed 600.06 seconds at a requested 100 Hz
without a debugger or reset, on boot `5ae1d34c`. Production rollover advanced
`0007.csv` to `0008.csv`. Its sampled diagnostic and sports-writer counters
reported zero queue drops or storage errors; no read-only command retry was
needed. Aligning device UTC to the host interval yields 59,993 CSV samples,
99.978 Hz, a 10 ms p99 sample interval and a 142 ms maximum gap at rollover.
This measures delivered timestamps, not sensor accuracy or sample freshness;
the UTC window assumes aligned host/device clocks.

An intentional hardware reset resumed `0008.csv` into `0009.csv` automatically.
STOP verified an inactive checkpoint at sequence 9; a second intentional reset
left it inactive. Diagnostic boot IDs distinguish both USB resets from the
earlier debugger-session panic. These manual resets validate recovery mechanics,
not prevention of panic/watchdog faults.

The new paced copy and offline verifier passed for all four test segments:
99,054 complete rows, no malformed complete rows or incomplete tails, correct
reset/rollover links and matching START/RESUME/STOP journal events. All six
baseline CSV hashes remain unchanged, including both confirmed badminton files.
The copied diagnostic snapshot contains 1,111 unique events, 166 identical
replays removed, and no checksum, truncated-tail or conflicting-ID errors.
Evidence is in `device-clean-recording.json`, `imu-validation.json`,
`recording-timing.json`, `imu-diag.json` and `device-results.json` under
`build/sports-reliability/`.

Allocation and touch-controller faults remain unresolved. The end of the clean
boot reported two allocation failures (latest request 1,536 bytes, capability
mask `0x8`) and a boot-wide internal-heap minimum of 8,820 bytes. There was no
start-of-window allocation snapshot, timestamp or caller trace, so these cannot
be assigned to the clean recording interval or a specific subsystem. The mask
requests 8-bit-capable memory, not specifically internal memory. The same
snapshot reported 17,571 failed touch-I2C transactions out of 22,884 attempts;
touch failures also occurred before recording and after STOP. Last error 259
can represent multiple failed synchronous-transaction paths in this IDF build;
it is not proof of NACK, bus contention, power gating or an electrical cause.
The successful recorder interval does not establish allocation or touch health.

The final saved placement is left wrist / right racket hand, with dominant hand
and all other preference payload fields unchanged. Test recording is stopped.
Labeled wrist/sport trials, physical SD-fault trials and an unplugged battery
duration run remain necessary; synthetic endurance is not device endurance.
