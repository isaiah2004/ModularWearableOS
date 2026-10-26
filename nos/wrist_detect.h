#ifndef NOS_WRIST_DETECT_H
#define NOS_WRIST_DETECT_H
#include "hal.h"
/* Experimental guided template matching, never an anatomical wrist classifier.
 * Screen up, 12 o'clock toward elbow; hold still, then rotate palm up once.
 * A 180-degree strap reversal around gravity is not observable from these
 * compact templates: a VALID result is a candidate requiring user confirmation.
 * refs = left signature, left arc, right signature, right arc.
 * Signature: +X=1,-X=2,+Y=3,-Y=4,+Z=5,-Z=6; bit7 = negative start gravity Z.
 * Arc is rounded integrated degrees / 2. Zero means uncalibrated. */
enum { WRIST_DETECT=0, WRIST_LEARN_LEFT=1, WRIST_LEARN_RIGHT=2 };
enum { WRIST_IDLE, WRIST_CAPTURING, WRIST_VALID, WRIST_UNKNOWN };
enum { WRIST_REASON_NONE, WRIST_REASON_MISSING_REFS, WRIST_REASON_AMBIGUOUS_REFS,
       WRIST_REASON_GAP, WRIST_REASON_SENSOR, WRIST_REASON_POSE,
       WRIST_REASON_NOT_QUIET, WRIST_REASON_ARC, WRIST_REASON_MIXED_AXIS,
       WRIST_REASON_NO_MATCH, WRIST_REASON_CANCELLED };
enum { WRIST_CANDIDATE_UNKNOWN, WRIST_CANDIDATE_LEFT, WRIST_CANDIDATE_RIGHT };
typedef struct {
    int state, reason, candidate;
    uint32_t remaining_ms, received, coverage_ms;
    uint8_t signature, arc;
} wrist_detect_status;
/* UI calls begin/get/cancel. Fresh producer alone calls feed. No allocation/I/O.
 * begin returns 0 for unusable detection references, leaving UNKNOWN + reason. */
int wrist_detect_begin(int mode, const uint8_t refs[4]);
void wrist_detect_feed(const hal_imu *v,uint32_t sample_ms);
void wrist_detect_get(wrist_detect_status *out);
void wrist_detect_cancel(void);
#endif
