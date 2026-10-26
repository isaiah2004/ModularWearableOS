/* orrery_sd.c - the sky pack's big catalogs, <sd>/sky/belt.bin and moons.bin (tools/skypack_c.py), read through
 * the asset worker. UI thread only; nothing here touches FATFS or blocks. One file per job, at most one job
 * (the slot is shared with the card textures). A file is published only after its header, pack date, counts,
 * index and CRC32 agree with the flash tables; any failure frees it and retries later with a growing delay
 * (one ORRERYSD line per failure that is new or wrong content). The 1.43C has no card: it builds with
 * NOS_SKY_EMBEDDED and validates the same two files from the image (CMake embeds assets/sd/sky). The CRC runs
 * in 32 KiB slices so no tick pays for a whole file. */
#include "faces/orrery_data.h"
#include "nos.h"
#include "asset.h"
#include <stdio.h>
#include <string.h>
#ifdef NOS_SKY_EMBEDDED
extern const uint8_t _binary_belt_bin_start[], _binary_belt_bin_end[], _binary_moons_bin_start[], _binary_moons_bin_end[];
#endif

#define HEAD 48u
typedef struct { char magic[4]; uint16_t ver, head; uint32_t count, recsz, bytes, crc; double jd; char built[16]; } sd_head;
_Static_assert(sizeof(sd_head) == HEAD, "sky file header");
_Static_assert(sizeof(od_ast) == 32 && sizeof(od_moon) == 88, "sky file records");
#define IDX (OD_PLANETS * 8u)
#define CRC_SLICE 32768u

enum { K_BELT, K_MOONS };
enum { ST_IDLE, ST_RUN, ST_CRC, ST_READY, ST_FAIL };
typedef struct { void *buf; unsigned size, pos; uint32_t crc, retry, back; uint8_t st, want, ro; } item;   /* ro: buf is image rodata */
static item I[2];
static int s_job = -1;
static unsigned s_tag;

static uint32_t crc_add(uint32_t c, const uint8_t *p, unsigned n)
{
    static const uint32_t t[16] = { 0, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
                                    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c };
    while (n--) { c ^= *p++; c = (c >> 4) ^ t[c & 15]; c = (c >> 4) ^ t[c & 15]; }
    return c;
}
static unsigned want_bytes(int k) { return HEAD + (k == K_BELT ? (unsigned)od_belt_n * 32u : IDX + (unsigned)od_moons_n * 88u); }
static void free_buf(item *it)
{
    if (!it->ro) hal_big_free(it->buf);
    it->buf = NULL;
    it->ro = 0;
}
/* IO trouble (no card, no file, busy worker) retries from 2 s / 3 s up to a minute; a file that read but is wrong
 * (stale pack, bad CRC) every minute, so a card fixed with sdsync is picked up quickly */
static void fail_k(int k, uint32_t now, int content)
{
    item *it = &I[k];
    uint32_t cap = k == K_BELT || content ? 60000 : 30000;
    if (!it->back || content) printf("ORRERYSD ERROR %s: %s\n", k == K_BELT ? "belt" : "moons", content ? "stale, bad or mismatched file" : "no card or file");
    it->st = ST_FAIL;
    it->back = it->back ? (it->back * 2 > cap ? cap : it->back * 2) : k == K_BELT ? 2000 : 3000;
    if (content) it->back = 60000;
    it->retry = now + it->back;
}

/* header, pack date, counts: everything but the CRC and the moon index */
static int head_ok(int k, const uint8_t *b, unsigned size)
{
    sd_head h;
    if (!b || size != want_bytes(k)) return 0;
    memcpy(&h, b, sizeof h);
    return !memcmp(h.magic, k == K_BELT ? "ORBL" : "ORMN", 4) && h.ver == 1 && h.head == HEAD && h.jd == od_pack_jd &&
           h.count == (uint32_t)(k == K_BELT ? od_belt_n : od_moons_n) && h.recsz == (k == K_BELT ? 32u : 88u) &&
           h.bytes == size - HEAD;
}
/* moons.bin: the index must tile the records, in planet order, with the flash counts */
static int moons_ok(const uint8_t *b, unsigned size)
{
    unsigned at = HEAD + IDX;
    for (int p = 0; p < OD_PLANETS; p++) {
        uint32_t off, n;
        memcpy(&off, b + HEAD + p * 8, 4);
        memcpy(&n, b + HEAD + p * 8 + 4, 4);
        if (off != at || n != od_moon_count[p] || at + n * 88u > size) return 0;
        for (uint32_t i = 0; i < n; i++) {
            od_moon m;
            memcpy(&m, b + at + i * 88u, sizeof m);
            if (m.planet != p || !memchr(m.name, 0, sizeof m.name)) return 0;
        }
        at += n * 88u;
    }
    return at == size;
}
static void finish_crc(int k, uint32_t now)
{
    item *it = &I[k];
    sd_head h;
    memcpy(&h, it->buf, sizeof h);
    if (~it->crc == h.crc && (k == K_BELT || moons_ok(it->buf, it->size))) {
        it->st = ST_READY;
        it->back = 0;
        return;
    }
    free_buf(it);
    fail_k(k, now, 1);
}

void od_sd_poll(void)
{
    if (s_job < 0) return;
    hal_asset_poll();
    asset_request rq;
    asset_result rs;
    if (!hal_asset_take(&rq, &rs)) return;
    int k = s_job;
    item *it = &I[k];
    uint32_t now = hal_millis();
    s_job = -1;
    if (it->st != ST_RUN || rq.tag != s_tag || rs.code != ASSET_OK || !head_ok(k, rq.pages[0], rq.bytes)) {
        hal_big_free(rq.pages[0]);
        if (it->st != ST_RUN) return;           /* released while loading */
        if (rs.code == ASSET_CANCELLED) { it->st = ST_FAIL; it->retry = now + 1000; }
        else fail_k(k, now, rs.code == ASSET_OK);
        return;
    }
    it->buf = rq.pages[0];
    it->size = rq.bytes;
    it->pos = HEAD;
    it->crc = ~0u;
    it->st = ST_CRC;
}
static void submit(int k, uint32_t now)
{
    item *it = &I[k];
#ifdef NOS_SKY_EMBEDDED
    const uint8_t *e = k == K_BELT ? _binary_belt_bin_start : _binary_moons_bin_start;
    unsigned en = (unsigned)((k == K_BELT ? _binary_belt_bin_end : _binary_moons_bin_end) - e);
    if (!head_ok(k, e, en)) { fail_k(k, now, 1); return; }
    it->buf = (void *)e;
    it->ro = 1;
    it->size = en;
    it->pos = HEAD;
    it->crc = ~0u;
    it->st = ST_CRC;
    return;
#else
    hal_storage sd;
    hal_storage_get(&sd);
    if (!sd.mounted) { fail_k(k, now, 0); return; }
    unsigned size = want_bytes(k);
    void *buf = hal_big_alloc(size);
    if (!buf) { it->st = ST_FAIL; it->retry = now + 1000; return; }
    asset_request rq = {0};
    snprintf(rq.path, sizeof rq.path, "%s/sky/%s", hal_sd_root(), k == K_BELT ? "belt.bin" : "moons.bin");
    rq.pages[0] = buf;
    rq.page_bytes = rq.bytes = size;
    rq.page_count = 1;
    rq.tag = ++s_tag ? s_tag : ++s_tag;
    int a = hal_asset_submit(&rq);
    if (a == 1) { it->st = ST_RUN; s_job = k; return; }
    hal_big_free(buf);
    if (a < 0 || !hal_asset_busy()) fail_k(k, now, 0);   /* 0 while busy: the slot is taken, ask again next pump */
#endif
}
void od_sd_pump(void)
{
    od_sd_poll();
    uint32_t now = hal_millis();
    for (int k = 0; k < 2; k++) {
        item *it = &I[k];
        if (it->st != ST_CRC) continue;
        unsigned n = it->size - it->pos < CRC_SLICE ? it->size - it->pos : CRC_SLICE;
        it->crc = crc_add(it->crc, (const uint8_t *)it->buf + it->pos, n);
        it->pos += n;
        if (it->pos == it->size) finish_crc(k, now);
        return;                                 /* one slice per pump */
    }
    if (s_job >= 0 || hal_asset_busy()) return;
    for (int k = K_MOONS; k >= K_BELT; k--) {  /* the card is on screen: moons first */
        item *it = &I[k];
        if (!it->want || (it->st != ST_IDLE && !(it->st == ST_FAIL && (int32_t)(now - it->retry) >= 0))) continue;
        submit(k, now);
        return;
    }
}

void od_belt_want(void) { I[K_BELT].want = 1; }
const od_ast *od_belt_rows(void) { return I[K_BELT].st == ST_READY ? (const od_ast *)((const uint8_t *)I[K_BELT].buf + HEAD) : NULL; }
void od_belt_drop(void)
{
    item *it = &I[K_BELT];
    if (it->st != ST_READY) return;
    free_buf(it);
    memset(it, 0, sizeof *it);
    it->want = 0;
}

int od_moons_known(int planet) { return planet >= 0 && planet < OD_PLANETS ? od_moon_count[planet] : 0; }
int od_moons_use(int planet)
{
    item *it = &I[K_MOONS];
    if (planet < 0 || planet >= OD_PLANETS || !od_moon_count[planet]) return 1;
    it->want = 1;
    return it->st == ST_READY ? 1 : it->back ? -1 : 0;   /* a retry in flight still reads as missing: no flicker */
}
int od_moons_ready(void) { return I[K_MOONS].st == ST_READY; }
int od_moons_failed(void) { return I[K_MOONS].st != ST_READY && I[K_MOONS].back; }
const od_moon *od_moons_of(int planet, int *n)
{
    item *it = &I[K_MOONS];
    uint32_t off = 0;
    *n = 0;
    if (it->st != ST_READY || planet < 0 || planet >= OD_PLANETS || !od_moon_count[planet]) return NULL;
    memcpy(&off, (const uint8_t *)it->buf + HEAD + planet * 8, 4);
    *n = od_moon_count[planet];
    return (const od_moon *)((const uint8_t *)it->buf + off);
}
void od_moons_release(void)
{
    item *it = &I[K_MOONS];
    it->want = 0;
    if (it->st == ST_RUN) { hal_asset_cancel(); it->st = ST_IDLE; return; }   /* the job's pages are freed when it is taken */
    if (it->st == ST_READY || it->st == ST_CRC) free_buf(it);
    memset(it, 0, sizeof *it);
}
