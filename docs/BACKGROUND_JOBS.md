Background Jobs is labelled Jobs in the app launcher, beside Energy. The first page shows
motion acquisition, step tracking, the activity calorie estimate, and battery
sampling. The arrow buttons reveal SD logging, system tracing, recordings,
workouts, and optional sensors.

Tap a job for its current value, update count, sample age and a two-minute graph.
Tap the value to change the plotted metric. Tap the graph to inspect a historical
point; tap again to return live. Readings shows timestamped one-second summaries;
Events shows the latest 16 status changes or error/drop counter changes. Those
views freeze their snapshot while browsing. Resume live returns to the graph.

Open records leads to the existing Activity, Energy, Battery history, IMU,
Sports, Sensor logs, or System trace application. These retain their existing
storage and recording policies. The monitor's short history and event list are
RAM only and reset on restart; the SD journals remain the durable evidence.

Motion and optional-sensor updates come from acquisition-side sports events,
including when the disk queue is full. The monitor never enables a sensor,
renews a streaming lease, performs sensor bus I/O, or writes its own files.
Steps and activity energy are one-second service checks, labelled CHECKS rather
than sensor samples. Activity energy is the existing step-based estimate
(0.04 kcal per step). It is not a separate calorimeter or resting-energy model.
Battery percentage stays unavailable while the hardware reports only supply
voltage, including on USB power.

The collector runs through the services hook with any app visible and during
the host's one-second display-off/AOD service ticks. Deep sleep and power-off
stop sampling. Flat values still count as observations; missing samples are
gaps. Stale acquisition cannot be refreshed by opening the app. Optional
modules report NO MODULE when absent and IDLE between on-demand streams.
WAITING can mean a stream is acquiring a pulse or waiting for a GPS fix.
SD writer health uses its actual drain heartbeat and committed-write counters;
a stuck writer eventually becomes STALE even if a previous write succeeded.

For a passive USB snapshot, run `python tools/watchctl.py --port COM3 jobs`.
This command does not wake the display. It reports status, update time/count,
validity bits, three values, and error/drop totals per job. Field names and units
are defined in `nos/background.c`. Sensor history is at most one latest sample
per second, not a raw high-rate waveform; use the IMU recorder for raw captures.

Validated and installed on COM3 on 2026-10-22. The build identity verifies
Background Jobs, Solar/Orrery, Developer Tools, the integrated home pager and
the sports apps. The 7,795,216-byte application fits its existing partition with
69,104 bytes remaining. The checked flash excluded NVS, and the SD preference
hash remained `4e7f2e46` before and after installation.

49 focused host tests passed. Simulator checks covered navigation, record links,
steady redraw limits, absent-sensor idle rendering, and 707 frames that matched
forced full repaints. The 857-frame steady run had no estimated late frames
at the simulator's 180 MHz cost setting; this is a model, not an optical FPS test.
On the watch, five seconds with the display off produced 757 motion updates,
five activity checks and 656 committed journal records. Navigation through the
graph, readings, events and IMU link was checked on the device. No allocation
failures were reported after the app was opened.

Three older harness checks also exposed existing assumptions: watchctl forbids
the already-installed `board` command, boot resources searches for the removed
`const int64_t period` declaration, and the launcher extraction fixture omits
the cost-profile header now required by `ui_text`. Their triggering source is
already present in the archived touch-update source, before this feature. The
harnesses were left intact; these are not represented as passing checks.
