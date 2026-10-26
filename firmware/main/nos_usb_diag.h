#ifndef NOS_USB_DIAG_H
#define NOS_USB_DIAG_H
#include <stdint.h>
/* Counters describe observed calls, not physical delivery or optical presentation.
 * Enable once after init; no reset API. uint32 counters wrap modulo 2^32. */
typedef struct {
    uint32_t connected_checks, connected_false;
    uint32_t guard_enabled, connected_overrides;
    uint32_t zero_calls, zero_fail, zero_us, zero_max_us;
    uint32_t wait_calls, wait_fail, wait_us, wait_max_us;
    uint32_t usb_lines, queue_accepted, queue_fail, dispatch;
    uint32_t reply_calls, reply_incomplete, reply_last_requested, reply_last_accepted;
} nos_usb_diag_stats;
void nos_usb_diag_start(void);
/* UI caller brackets exactly one dequeued USB command, including early returns. */
void nos_usb_diag_reply_begin(void);
void nos_usb_diag_reply_end(void);
void nos_usb_diag_snapshot(nos_usb_diag_stats *out);
void nos_usb_diag_usb_line(void);
void nos_usb_diag_queue_failed(void);
void nos_usb_diag_queue_accepted(void);
void nos_usb_diag_dispatched(void);
/* Recognizes usb_diag/usb_guard including malformed arguments, sends one checked direct
 * driver frame and consumes the command; never emits generic DONE or retries. */
int nos_usb_diag_command(const char *command);
#endif
