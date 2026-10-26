#ifndef NOS_RUNTIME_ADMISSION_H
#define NOS_RUNTIME_ADMISSION_H
/* UI requests must stop creating new file/sensor owners while the host is
 * quiescing or an update owns flash. Final shutdown also rechecks real owners. */
#ifdef ESP_PLATFORM
int ota_busy(void);
int nos_host_shutdown_pending(void);
static inline int nos_runtime_recording_allowed(void)
{ return !ota_busy() && !nos_host_shutdown_pending(); }
#else
static inline int nos_runtime_recording_allowed(void) { return 1; }
#endif
#endif
