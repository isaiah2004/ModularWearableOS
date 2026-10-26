/* Immutable canonical grayscale menu art, loaded from a pack file (docs/SD_ASSETS.md).
 * Pixel pointers stay valid for the process lifetime once published; consumers
 * must never rasterize into them or pass them to hal_big_free. desc.offset is
 * the body's offset in the loaded pack's decoded arena. */
#ifndef NOS_NATIVE_MENU_ASSETS_H
#define NOS_NATIVE_MENU_ASSETS_H
#include "sprite.h"
#include <stdint.h>
typedef struct {uint32_t offset,bytes,revision;int16_t w,h,x,y;} native_asset_desc;
typedef struct {
    int state;               /* 1 loaded, 0 not tried, -1 failed */
    const char *source;      /* "sd", "embedded" or "file" */
    const char *error;       /* last failure reason, "" when none */
    int sharp;               /* pack holds level 0 only */
    uint32_t file_bytes,arena_bytes,records,bodies,load_ms;
} native_menu_pack_info;
void native_menu_assets_init(void); /* once, before UI startup: load the pack or leave the dynamic fallback */
int native_menu_assets_reload(void); /* retry after a failure (card inserted later); 1 when loaded */
int native_menu_assets_retry_due(void); /* UI poll, B watch only: 1 when a failed load should be retried now (card mounted, backoff elapsed) */
void native_menu_pack_status(native_menu_pack_info *out);
extern int nos_native_menu_rom_enabled;
void nos_native_menu_rom_set(int enabled);
int native_menu_assets_compatible(void);
const native_asset_desc *native_menu_asset(unsigned family,unsigned entry,unsigned layout,unsigned level);
int native_menu_launcher_matches(unsigned entry,const char *name,int icon);
sprite8 native_menu_sprite(const native_asset_desc *asset);
const uint16_t *native_menu_settings_palette(int red,int level);
const uint16_t *native_menu_launcher_palette(int level,int pressed);
const int16_t *native_menu_launcher_curve(void);
uint32_t native_menu_asset_bytes(void); /* decoded pixel bytes of the loaded pack, 0 when none */
const char *native_menu_source_sha256(void);
#endif
