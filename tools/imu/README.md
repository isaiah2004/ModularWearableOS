# Offline recording audit

`audit.py` reads copied watch CSV files without changing them and compiles
`motion_replay.c` with the production `nos/workout_motion.c`. It requires GCC and
Python with matplotlib. It never accesses serial ports, devices or SD settings.

```powershell
python tools/imu/audit.py --out build/sports-reliability/my-audit `
  build/sports-reliability/watch-copy/imu/0002.csv
```

The output directory must be new. The CSV header supplies explicit units:
`ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps`; other columns are retained in the
immutable original but not used by the movement engine. No resampling, filtering,
threshold fitting or extrapolation occurs. Malformed rows are listed and skipped;
their timestamp gaps remain. Nonfinite sensor values reach the engine, which
invalidates their continuity. Engine clock arithmetic is uint32; the audit also
reports raw timestamp reversals rather than silently calling them rollovers.

Outputs include `report.json`, source/binary SHA-256 hashes, compiler invocation
and version, per-sample cumulative engine outputs, and SVG/PNG scientific plots.
Green shading shows episode state including release hysteresis. Its visual width
is not the cumulative movement time, which counts qualifying movement and confirmed
onset only. Gaps exceeding 250 ms appear red. Missing/empty files are not fabricated
into sessions. Exact repeated rounded values do not prove stale sensor reads.

Current source config says +/-8g and +/-512 deg/s. Near-rail counts are per axis,
conditional on this config, and are not proof of historical clipping: recording
firmware/settings/filtering provenance is unknown. Vector magnitudes may exceed
an individual axis limit without violating that limit.

Use `--context "user-confirmed ..."` only when the recording identity and placement
are actually confirmed. Otherwise the default explicitly leaves them unknown.
These files have no synchronized event labels and cannot establish shot/rally
counts, sporting accuracy, left/right wrist, or playing-hand classification.

The confirmed badminton evidence is in
`build/sports-reliability/audit-confirmed-badminton-v2/report.json`, with per-recording
`*-context.json` sidecars and labeled SVG/PNG plots. The user explicitly confirmed
in this conversation that BOTH 0002 and 0005 are badminton played with the RIGHT
hand while the watch was on the LEFT wrist. Dominant hand was not stated and
remains unknown. `build/sports-reliability/badminton-context.json` records this
provenance. Pass `--metadata PATH.json` to attach the same structured context to
all inputs in one run; select only files to which that context applies.

0002 has 68.040 seconds of estimated movement and 15 episodes in 254.040 seconds;
0005 has 223.760 seconds and 8 episodes in 267.760 seconds. Both have approximately
25 Hz CSV cadence and complete adjacent-sample coverage under this engine.
Sustained movement can form one long episode, so these are not shot counts.
Original SHA-256 hashes and engine outputs match the initial unconfirmed audit
in `build/sports-reliability/audit-v1/`; that historical report remains untouched.
No movement parameters were changed or fitted using these recordings.

Run the input-integrity/gap fixture check with:

```powershell
python -m unittest discover -s tests -p test_imu_audit.py -v
```
