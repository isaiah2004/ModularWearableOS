/* Bounded development diagnostics. Producers never perform filesystem I/O.
 * Event arguments are integers with a documented schema, never credentials. */
#ifndef NOS_DIAG_LOG_H
#define NOS_DIAG_LOG_H
#include <stdint.h>
enum {
    DIAG_BOOT=1, DIAG_RESET_CONTEXT, DIAG_STORAGE_ERROR, DIAG_QUEUE_DROPS, DIAG_FAULT_SUMMARY,
    DIAG_POWER_SAMPLE=0x100, DIAG_POWER_STATE, DIAG_POWER_POLICY,
    DIAG_WAKE, DIAG_SLEEP_BLOCK, DIAG_HEAP, DIAG_FRAME_PACE, DIAG_ACTION=0x200,
    DIAG_NAVIGATION, DIAG_COMMAND, DIAG_COMMAND_INPUT, DIAG_COMMAND_INPUT_DETAIL,
    DIAG_ACTION_REQUEST=0x210, DIAG_ACTION_RESULT, DIAG_ACTION_CONTEXT,
    DIAG_DMA_HEAP=0x107, DIAG_ALLOC_FAILURE, DIAG_ALLOC_CONTEXT, DIAG_ALLOC_HEAP,
    DIAG_CRASH_ARTIFACT, DIAG_CRASH_PC,
    DIAG_DISPLAY_FAULT=0x10e,
    DIAG_WORKOUT=0x300,
    DIAG_IMU_START=0x310, DIAG_IMU_RESUME, DIAG_IMU_STOP, DIAG_IMU_ERROR,
    DIAG_WORKOUT_RECOVERY=0x320, DIAG_SENSOR_QUALITY
};
typedef struct {
    uint32_t boot, reset_reason, queued, written, dropped, errors;
    uint32_t previous_events, crash_boots;
    int available, ready;
    uint32_t routine_dropped, priority_dropped, emergency_dropped, coalesced;
    uint32_t faults, evidence_lost;
    uint32_t last_request, last_target, last_result_request;
    int32_t last_result, last_result_detail;
    uint32_t crash_state, crash_pc, crash_build;
} diag_log_status;
typedef struct {
    uint32_t boot, last_boot, code, first_sequence, last_sequence;
    uint32_t first_ms, last_ms, count;
    int32_t first_a, first_b, first_c, last_a, last_b, last_c;
} diag_fault_summary;
enum { DIAG_DOMAIN_NAVIGATION=1, DIAG_DOMAIN_CONTROL, DIAG_DOMAIN_NOTIFICATION,
       DIAG_DOMAIN_FACE, DIAG_DOMAIN_ALARM, DIAG_DOMAIN_TIMER, DIAG_DOMAIN_PHONE,
       DIAG_DOMAIN_SMS, DIAG_DOMAIN_MEMO, DIAG_DOMAIN_GNSS, DIAG_DOMAIN_WEATHER,
       DIAG_DOMAIN_SENSOR, DIAG_DOMAIN_MUSIC, DIAG_DOMAIN_FIND, DIAG_DOMAIN_DEVELOPER };
enum { DIAG_ACT_OPEN=1, DIAG_ACT_DISMISS, DIAG_ACT_SET, DIAG_ACT_START, DIAG_ACT_STOP,
       DIAG_ACT_FIRE, DIAG_ACT_TIMEOUT, DIAG_ACT_PAUSE, DIAG_ACT_RESUME, DIAG_ACT_SAVE,
       DIAG_ACT_SEND, DIAG_ACT_RECEIVE, DIAG_ACT_CANCEL, DIAG_ACT_TRANSITION,
       DIAG_ACT_REJECT, DIAG_ACT_MARK };
enum { DIAG_RESULT_PENDING=0, DIAG_RESULT_SUCCESS=1, DIAG_RESULT_CANCELLED=2,
       DIAG_RESULT_FAILED=-1, DIAG_RESULT_REJECTED=-2, DIAG_RESULT_STALE=-3 };
/* v1 disk format: 32-byte DLGF file header and 44-byte DLG1 records,
 * little endian with IEEE CRC32. See tools/diag_export.py for full schema.
 * RTC breadcrumbs survive supported warm resets, never loss of power. */
void diag_log_init(void);
void diag_log_tick(void);
void diag_log_event(uint16_t code, int32_t a, int32_t b, int32_t c); /* task context, no I/O */
void diag_log_get(diag_log_status *out);
void diag_log_report(void);
/* Sparse committed actions only. A request is scoped by (boot,request), carries
 * a numeric target; context provides source/old-generation, result records actual
 * completion. Never pass credentials, message/caller/location content or hashes. */
uint32_t diag_action_request(uint16_t domain,uint16_t action,int32_t target);
void diag_action_context(uint32_t request,int32_t source,int32_t value);
void diag_action_result(uint32_t request,int32_t result,int32_t detail);
/* Independent checked RTC first/last/count slots survive supported warm resets.
 * Reads never touch SD. A full emergency ledger is sticky and never evicts the
 * first failure just to retain routine events. Cold power loss clears RTC. */
int diag_log_fault_get(unsigned index,diag_fault_summary *out);
void diag_log_crash_state(uint32_t state,uint32_t pc,uint32_t build);
/* Sole filesystem consumer. Target worker owns this; hosts call between frames. */
void diag_log_service(void);
int diag_log_set_directory(const char *directory); /* hosts, before init */
uint32_t diag_log_name_id(const char *name); /* stable FNV-1a screen/action ID */
#endif
