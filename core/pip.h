#ifndef WATCH_PIP_H
#define WATCH_PIP_H
#include <stdint.h>
#include "hal.h"

typedef enum {
    PIP_IDLE, PIP_CURIOUS, PIP_HAPPY, PIP_PETTED, PIP_BOOP,
    PIP_STARTLED, PIP_DIZZY, PIP_ROCKED, PIP_SLEEPY, PIP_ASLEEP,
    PIP_WAKE, PIP_PLAY, PIP_BORED, PIP_STRETCH, PIP_WINK, PIP_PEEK,
    PIP_GRUMPY, PIP_LONELY, PIP_EXCITED, PIP_RELIEVED, PIP_NOTIFY,
    PIP_ACTION_COUNT
} pip_action;
typedef struct {
    uint32_t magic, version, familiarity, visits, seed;
} pip_memory;
typedef struct {
    float x, y, width, height, upper, lower, wink, lean, glow;
    float joy, surprise, dizzy, sleep, ball_x, ball_y, ball_visible;
    float blink, asymmetry, anger;
    int wink_side;
} pip_pose;
typedef struct {
    uint32_t now;
    int hour, battery, imu_valid;
    hal_imu imu;
} pip_input;
typedef struct {
    pip_memory memory;
    pip_pose pose;
    pip_action action, previous;
    uint32_t last_ms, action_ms, duration_ms, last_attention, next_idle_ms;
    uint32_t blink_ms, blink_wait, gaze_ms, gaze_wait, motion_ms, reward_ms;
    uint32_t touch_ms, tap_ms, stable_ms, face_down_ms, last_motion_ms;
    uint32_t wake_ms, last_stroke_ms;
    uint32_t rock_ms;
    uint32_t startle_ms;
    float comfort, energy, curiosity, arousal;
    /* Slow care context is supplied separately from momentary touch reactions.
     * social/rest: 1 = satisfied/rested, valence: -1..1, others: 0..1. */
    float context_energy, context_valence, context_social, context_rest, context_grumpy;
    float valence, social, rest, grumpy;
    float gravity_x, gravity_y, gravity_z, neutral_x, neutral_y;
    float bias_x, bias_y, bias_z, motion, shaking, rocking, spin;
    float gaze_x, gaze_y, touch_x, touch_y, path, still_time;
    float ball_x, ball_y, ball_vx, ball_vy;
    float breath_phase;
    int initialized, imu_ready, imu_valid, down, stroke, touch_zone;
    int had_tap, woke, face_down, dirty;
    int rock_sign, rock_reversals;
    int context_active, gesture_side;
    int quiet_override, routine_wake;
    pip_action last_idle_action;
} pip_model;

int pip_memory_valid(const pip_memory *memory);
void pip_init(pip_model *model, const pip_memory *memory, uint32_t now);
void pip_step(pip_model *model, const pip_input *input);
/* Call after pip_init. Finite targets are clamped; nonfinite values retain the
 * previous target. Does not reset the current action, timing, or relationship. */
void pip_set_context(pip_model *model, float energy, float valence,
                     float social, float rest, float grumpy);
/* Care routine: 1 = quiet/rest window, 0 = awake window, -1 = legacy 22..7.
 * A transition out of quiet wakes a sleeping pet on its next step. */
void pip_set_quiet(pip_model *model, int quiet);
/* Pointer coordinates use a 240x240 design grid. phase: 1 down, 2 move, 0 up. */
void pip_pointer(pip_model *model, int phase, float x, float y, uint32_t now);
void pip_tap(pip_model *model, uint32_t now);
void pip_hold(pip_model *model, uint32_t now);
void pip_center(pip_model *model);
int pip_force(pip_model *model, const char *name, uint32_t now);
const char *pip_action_name(pip_action action);
#endif
