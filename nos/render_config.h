#ifndef NOS_RENDER_CONFIG_H
#define NOS_RENDER_CONFIG_H
/* Nominal animation cadence, device presentation target and acceptance floor.
 * Keep authored durations unchanged when experimenting with presentation rate.
 * CPU budget is a validation constraint;
 * ESP32-S3 only supports 80/160/240 MHz, not a 200 MHz clock setting. */
#ifndef NOS_FPS
#define NOS_FPS 40
#endif
/* Default presentation target; Developer Tools can cap the live watch and
 * simulator separately from the authored clock. Script replay uses NOS_FPS. */
#ifndef NOS_PRESENT_FPS
#define NOS_PRESENT_FPS 60
#endif
#define NOS_MIN_FPS 30
#define NOS_PERIOD_MS (1000.0f / NOS_FPS)
#define NOS_CPU_BUDGET_MHZ 200
#define NOS_CPU_REFERENCE_MHZ 240
/* Sharp experiment is selected by the firmware build, independently of the
 * simulator/reference renderer. It removes temporal storage and sprite filters. */
#ifndef NOS_RENDER_SHARP
#define NOS_RENDER_SHARP 0
#endif
/* Keep canonical 16-row rendering for streaming blur and retained layers. Split
 * each raw stripe into smaller rotated DMA transfers: two 16-row raw buffers
 * plus two 8-row DMA buffers use 44,736 bytes instead of 59,648. */
#ifndef NOS_STRIPE_ROWS
#define NOS_STRIPE_ROWS 16
#endif
#ifndef NOS_DMA_ROWS
#define NOS_DMA_ROWS 8
#endif
#endif
