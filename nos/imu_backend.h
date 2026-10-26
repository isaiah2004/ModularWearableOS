#ifndef NOS_IMU_BACKEND_H
#define NOS_IMU_BACKEND_H
#include "hal.h"
/* Platform hooks; no UI clock latches or filesystem calls in the sampler. */
uint32_t imu_backend_ms(void);
uint64_t imu_backend_utc_ms(void);
int imu_backend_start(unsigned hz);
void imu_backend_stop(void);
int imu_backend_wake(void);
int imu_worker_pending(void);
void imu_worker_step(void);                 /* sole filesystem owner */
void imu_stream_push(const hal_imu *v, uint32_t now, uint64_t utc);
void imu_stream_missed(unsigned n, int error);
#endif
