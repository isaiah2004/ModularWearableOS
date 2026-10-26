#ifndef NOS_WEATHER_SERVICE_H
#define NOS_WEATHER_SERVICE_H

/* One INTERNAL8KiB worker.1 means created/already ready;0 means failed or
 * another caller is starting it. The caller owns bounded retry pacing. */
int weather_service_start(void);
/* Cached readiness only; never allocates or accesses NVS/network. */
int weather_service_ready(void);

#endif
