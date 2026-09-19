#ifndef WATCH_PIP_CARE_H
#define WATCH_PIP_CARE_H

#include <stdint.h>

/* Initial care tuning; these are game parameters, not health targets. */
#define PIP_CARE_MAX 1000u
#define PIP_CARE_SOCIAL_PER_HOUR 35u
#define PIP_CARE_STIMULATION_PER_HOUR 28u
#define PIP_CARE_REST_PER_HOUR 38u
#define PIP_CARE_REST_RECOVERY_PER_HOUR 90u
#define PIP_CARE_LOW_PERCENT 20
#define PIP_CARE_RECOVERED_PERCENT 30
#define PIP_CARE_LOW_DWELL_MS 60000u
#define PIP_CARE_CHARGE_GRACE_MS 1800000u
#define PIP_CARE_OFFLINE_CAP_S 28800u
#define PIP_CARE_REWARD_GAP_MS 300000u
#define PIP_CARE_WALK_WINDOW_MS 180000u
#define PIP_CARE_WALK_REQUIRED_S 120u
#define PIP_CARE_WALK_COOLDOWN_MS 1800000u
#define PIP_CARE_HISTORY_COUNT 4

typedef enum {
    PIP_CARE_CONTENT, PIP_CARE_GRUMPY, PIP_CARE_LONELY, PIP_CARE_SLEEPY,
    PIP_CARE_HAPPY, PIP_CARE_EXCITED, PIP_CARE_RELIEVED,
    PIP_CARE_MOOD_COUNT
} pip_care_mood;
typedef enum {
    PIP_CARE_REASON_NONE, PIP_CARE_REASON_SOCIAL, PIP_CARE_REASON_POWER,
    PIP_CARE_REASON_REST, PIP_CARE_REASON_STIMULATION,
    PIP_CARE_REASON_TOUCH, PIP_CARE_REASON_WALK,
    PIP_CARE_REASON_NOTIFICATION, PIP_CARE_REASON_ALARM,
    PIP_CARE_REASON_REUNION, PIP_CARE_REASON_COUNT
} pip_care_reason;
typedef enum {
    PIP_CARE_EVENT_NONE, PIP_CARE_EVENT_GREETING, PIP_CARE_EVENT_PETTED,
    PIP_CARE_EVENT_PLAY, PIP_CARE_EVENT_LOW_POWER, PIP_CARE_EVENT_OVERDUE,
    PIP_CARE_EVENT_POWER_RELIEF, PIP_CARE_EVENT_WALK,
    PIP_CARE_EVENT_NOTIFICATION, PIP_CARE_EVENT_ALARM,
    PIP_CARE_EVENT_COUNT
} pip_care_event_type;
typedef struct {
    uint64_t epoch_s;
    uint32_t reason, event;
} pip_care_history;
/* Explicit bounded fields; checksum covers field values, not struct padding. */
typedef struct {
    uint64_t last_epoch_s, last_walk_epoch_s, last_care_epoch_s, last_greet_epoch_s;
    uint32_t magic, version, generation, size;
    uint16_t social, stimulation, rest, comfort;
    uint32_t request_state, request_observed_ms;
    uint32_t last_request_day, last_walk_day, walk_rewards, care_episodes;
    uint32_t last_greet_day, history_head, history_count, clock_valid;
    uint32_t awake_start_hour, awake_end_hour, checksum, reserved;
    pip_care_history history[PIP_CARE_HISTORY_COUNT];
} pip_care_memory;
typedef struct {
    uint32_t now_ms;
    uint64_t epoch_s;
    int clock_valid, local_hour, local_minute;
    uint32_t local_day; /* stable caller date key, e.g. YYYYMMDD; zero if unknown */
    int battery_percent, battery_valid;
    int external_power; /* -1 unknown; 0 credible battery operation; 1 external power */
    uint32_t active_seconds, steps; /* cumulative counters; reset/wrap guarded */
    int activity_valid, walking, visible;
} pip_care_input;
typedef struct {
    pip_care_mood mood;
    pip_care_reason reason;
    uint16_t social, stimulation, rest, comfort;
    uint16_t energy; /* rest with trustworthy battery limitation; 0..1000 */
    uint16_t intensity; /* strength of current mood; 0..1000 */
    int quiet, low_power, charge_requested, charge_overdue;
} pip_care_output;
typedef struct {
    pip_care_event_type type;
    pip_care_reason reason;
    uint32_t count; /* coalesced arrivals for notifications */
} pip_care_event;
typedef struct {
    pip_care_memory memory;
    pip_care_output output;
    pip_care_input input;
    uint32_t last_ms, low_ms, recovered_ms, last_reward_ms, last_walk_ms;
    uint32_t last_greeting_ms, last_greeting_reward_ms, positive_ms, positive_duration_ms;
    uint32_t last_notify_ms, notification_sequence, alarm_sequence;
    uint32_t active_baseline, steps_baseline, walk_slot, activity_budget_ms;
    uint32_t social_fraction, stimulation_fraction, rest_fraction, comfort_fraction;
    uint16_t walk_buckets[6];
    pip_care_event pending[4];
    uint32_t pending_ms[4];
    pip_care_mood positive_mood;
    pip_care_reason positive_reason;
    int initialized, dirty, have_activity, have_reward, have_walk, have_greeting;
    int have_notification, have_alarm, last_visible, have_greeting_reward;
} pip_care_model;

int pip_care_memory_valid(const pip_care_memory *memory);
void pip_care_init(pip_care_model *model, const pip_care_memory *memory,
                   const pip_care_input *input);
void pip_care_step(pip_care_model *model, const pip_care_input *input);
/* Snapshot proposes the next generation without committing it. Failed writes
 * retry the same generation/slot. No state changes may occur between taking a
 * snapshot and acknowledging its successful write with pip_care_saved(). */
void pip_care_snapshot(pip_care_model *model, pip_care_memory *out);
void pip_care_saved(pip_care_model *model);
void pip_care_greet(pip_care_model *model, uint32_t now_ms);
void pip_care_pet(pip_care_model *model, uint32_t now_ms);
void pip_care_play(pip_care_model *model, uint32_t now_ms);
/* Arrival sequence is supplied by the producer, not notification count. */
void pip_care_notify(pip_care_model *model, uint32_t sequence, uint32_t now_ms);
void pip_care_alarm(pip_care_model *model, uint32_t sequence, uint32_t now_ms);
/* Old cosmetic events expire; alarm and power events receive higher priority. */
int pip_care_take_event(pip_care_model *model, uint32_t now_ms, pip_care_event *out);
const char *pip_care_mood_name(pip_care_mood mood);
const char *pip_care_reason_name(pip_care_reason reason);

#endif
