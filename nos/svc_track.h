/* svc_track.h — GPS track recorder service (outdoor team).
 *
 * Recording lives here, not in the Navigate app, so a track keeps going while the
 * user is on the face or in another app. While a track is active the service keeps
 * the GPS enabled and reads it every frame (so the module's idle auto-stop never
 * fires), adds a point every 5 m, and on stop writes a GPX file to the SD card
 * (device: /sdcard/watch/tracks/NNNN.gpx, sim: build/nos_out/tracks/NNNN.gpx).
 *
 * svc_track_tick() must run every frame (svc_tick calls it). It is safe to call more
 * than once per frame: only the first call in a frame does work. */
#ifndef NOS_SVC_TRACK_H
#define NOS_SVC_TRACK_H
#include <stdint.h>

typedef struct { double lat, lon; float alt; uint32_t epoch; } svc_track_pt;
#define SVC_TRACK_MAXPTS 4096           /* points kept (then every other is dropped) */

void svc_track_tick(float dt);

void svc_track_start(void);          /* begins a new track (discards the previous one) */
/* Ends the track and writes the GPX. Returns the file number (1..9999), 0 if it
 * could not be written (no card), -1 if there was nothing to save. The points stay
 * readable until the next start. */
int  svc_track_stop(void);
int  svc_track_active(void);

int  svc_track_points(const svc_track_pt **out);   /* count; *out = oldest first */
double   svc_track_distance(void);   /* metres along the track */
uint32_t svc_track_elapsed_ms(void); /* since start (frozen at stop) */
unsigned svc_track_version(void);    /* changes whenever a point is added or a track starts */

/* System REC chip (face status slot, launcher tile, control centre): show it while
 * svc_track_active() is true, as a red dot + svc_track_label() ("REC 12:34", or
 * "REC 1:02:03" past an hour), the same words as Navigate's header. */
void svc_track_label(char *out, int n);
#endif
