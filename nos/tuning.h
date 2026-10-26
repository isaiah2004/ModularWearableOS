#ifndef NOS_TUNING_H
#define NOS_TUNING_H
typedef struct { unsigned version; float walk_g, knock_g; int walk_peaks, quiet_ms; } sports_tuning;
void tuning_init(void);
void tuning_get(sports_tuning *out);
unsigned tuning_revision(void); /* active configuration revision within this boot */
int tuning_adjust(int feature,int direction); /* 0 walk g, 1 knock g, 2 peaks, 3 quiet; direction 0 resets */
void tuning_session(int feature,int start,int expected); /* labelled training window */
#endif
