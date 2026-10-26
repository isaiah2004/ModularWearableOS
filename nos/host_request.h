#ifndef NOS_HOST_REQUEST_H
#define NOS_HOST_REQUEST_H
/* Parse only the bounded textual envelope; transfer bodies/ACKs bypass it.
 * Returns 1 envelope, 0 legacy, -1 malformed. No source bytes are changed. */
int host_request_parse(const char *text, char id[9], const char **command);
int host_request_transfer(const char *command);
#endif
