#ifndef NOS_COMMAND_INPUT_H
#define NOS_COMMAND_INPUT_H
#include <stdint.h>
#define COMMAND_INPUT_CAP 192u
enum { COMMAND_INPUT_NONE, COMMAND_INPUT_READY, COMMAND_INPUT_REJECTED, COMMAND_INPUT_EMPTY };
enum { COMMAND_INPUT_BAD_CONTROL=1, COMMAND_INPUT_TOO_LONG=2 };
typedef struct {
    char text[COMMAND_INPUT_CAP];
    uint32_t bytes;
    unsigned used;
    uint8_t reason, first_bad, pending_cr, nonspace;
    uint32_t first_bad_offset;
    uint32_t nul_count, first_nul_offset, last_nul_offset;
    uint32_t pending_cr_offset;
} command_input;
/* Byte framing only; UTF-8 bytes, spaces and tabs remain unchanged. Caller
 * resets after every non-NONE result. Raw transfer payload/ACK readers bypass
 * this parser. bytes excludes LF, includes optional trailing CR. Overflow has
 * first_bad=0, so no ordinary character/credential byte is logged. Offsets are
 * one-based among bytes before LF; zero means absent. Counts and offsets
 * saturate at UINT32_MAX. NUL observations include bytes after a prior error;
 * an overflow's first_bad=0 does not itself count as a NUL. pending_cr_offset
 * retains the original CR position until its following byte resolves it. */
void command_input_reset(command_input *input);
int command_input_push(command_input *input,uint8_t byte);
#endif
