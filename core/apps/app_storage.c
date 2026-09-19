#include "apps.h"
#include "../font.h"
#include "../hal.h"
#include "../icons.h"
#include <stdio.h>

static void draw(gfx_fb *fb)
{
    hal_storage sd; hal_storage_get(&sd);
    gfx_clear(fb, GFX_BLACK);
    font_draw_centered(fb, &font_small, GFX_CX, GFX_S(48), "STORAGE", GFX_ACCENT);
    font_draw_centered(fb, &font_medium, GFX_CX, GFX_S(90),
        sd.mounted ? "SD card ready" : "No SD card", sd.mounted ? GFX_GREEN : GFX_GREY);
    char line[48];
    if (sd.mounted) {
        snprintf(line, sizeof line, "%.1f / %.1f GB free",
            sd.free_bytes / 1000000000.0, sd.total_bytes / 1000000000.0);
        font_draw_centered(fb, &font_small, GFX_CX, GFX_S(122), line, GFX_WHITE);
        font_draw_centered(fb, &font_small, GFX_CX, GFX_S(146), "Logs: /watch/logs", GFX_GREY);
        font_draw_centered(fb, &font_small, GFX_CX, GFX_S(170), "Eject over USB before removal", GFX_GREY);
    } else {
        font_draw_centered(fb, &font_small, GFX_CX, GFX_S(125), "Use a FAT32 microSD card", GFX_WHITE);
        font_draw_centered(fb, &font_small, GFX_CX, GFX_S(150), "Insert with power off", GFX_GREY);
    }
}
const wos_app app_storage = { "Storage", NULL, NULL, draw, GFX_ACCENT, ICON_SETTINGS };
