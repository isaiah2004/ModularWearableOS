/* orrery_data.h - the sky pack: small tables in flash (generated nos/faces/orrery_data.c), the asteroid belt and
 * every moon on the SD card (sky/belt.bin, sky/moons.bin; nos/faces/orrery_sd.c). Both come from tools/skypack_c.py.
 * Rebuild: python tools/skypack.py (fetch from NASA/JPL) then python tools/skypack_c.py, push build/sd with sdsync. */
#ifndef ORRERY_DATA_H
#define ORRERY_DATA_H
#include <stdint.h>
#include "ephem.h"

/* one asteroid: a (au), e, i, node, w (deg), M at epoch (deg), epoch - 2451545 (days), H x 10 */
typedef struct { float a, e, i, node, w, ma, ep; int8_t h10; } od_ast;
/* named small bodies; kind 0 asteroid, 1 dwarf planet, 2 Kuiper-belt object, 3 comet, 4 interstellar */
enum { OD_ASTEROID, OD_DWARF, OD_KBO, OD_COMET, OD_INTERSTELLAR };
typedef struct { const char *name, *short_name; uint8_t kind; ep_conic el; float diam_km; } od_small;
typedef struct { const char *name; ep_conic el; float m1; } od_comet;                 /* m1 99 = unknown */
/* spacecraft: planet (EP_*, 10 = Pluto, -1 in cruise), status as reported when the pack was built,
 * heliocentric xyz (au) at od_pack_jd, and planet-centred elements for orbiters */
typedef struct { const char *name; int8_t planet; const char *status; double xyz[3]; uint8_t has_orbit; ep_sat orbit; } od_craft;
/* moons (a record of sky/moons.bin, 88 B): planet (EP_*, 10 = Pluto), fresh = elements re-seeded from Horizons at
 * pack time (ecliptic frame); otherwise JPL mean elements (Laplace plane), display grade */
typedef struct { char name[16]; int8_t planet; uint8_t fresh; uint16_t pad; uint32_t code; ep_sat el; } od_moon;
typedef struct { const char *des; double jd; float au, kms, h; } od_flyby;

extern const double od_pack_jd;
extern const char od_built[];
extern const int od_belt_n;               /* rows expected in sky/belt.bin */
extern const od_small od_small_bodies[];  extern const int od_small_n;
extern const od_comet od_comets[];        extern const int od_comets_n;
extern const od_craft od_crafts[];        extern const int od_crafts_n;
extern const int od_moons_n;              /* records expected in sky/moons.bin */
#define OD_PLANETS 11                     /* moons.bin is indexed by planet: EP_* 0..7, 10 = Pluto */
extern const uint16_t od_moon_count[OD_PLANETS];   /* known moons per planet (flash: needs no SD) */
extern const od_flyby od_flybys[];        extern const int od_flybys_n;

/* The big catalogs, UI thread only. Loads run on the asset worker (hal_asset_*, one job slot shared with the
 * card textures), never block, and publish only after magic, version, pack date, counts and CRC32 check out.
 * A missing card or file just leaves the data absent; failed loads retry with a growing delay. */
void od_sd_poll(void);                    /* adopt a finished job; also from the card's asset poll and the shutdown drain */
void od_sd_pump(void);                    /* poll + one CRC slice + start a due load; cheap when idle */
void od_belt_want(void);                  /* the belt is wanted: loads from the next pump */
const od_ast *od_belt_rows(void);         /* od_belt_n rows once verified, else NULL */
void od_belt_drop(void);                  /* the caller derived what it needs: free the rows */
/* moons.bin is read whole (about 40 KB: the asset API has no seek) and kept until od_moons_release(); the
 * open planet's block is found through its index. */
int od_moons_known(int planet);           /* how many (flash: exact even when the file is not there) */
int od_moons_use(int planet);             /* 1 data ready, 0 loading, -1 missing (no card / file / bad CRC); asks for it */
int od_moons_ready(void);
int od_moons_failed(void);
const od_moon *od_moons_of(int planet, int *n);   /* that planet's moons (NULL, 0 until ready) */
void od_moons_release(void);              /* the card closed: free */
#endif
