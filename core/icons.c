#include "icons.h"
#include "font.h"
#include <math.h>

#define PI 3.14159265f

/* end point of a spoke from (cx,cy): angle 0 = up, clockwise */
static void ray(gfx_fb *fb, float cx, float cy, float a,
                float r0, float r1, float w, uint16_t col)
{
    /* fast_sin/cos: this fires for every spoke of every launcher icon glyph,
     * on the overlay's full-redraw path — soft sinf/cosf here is pure overhead. */
    float s = gfx_fast_sin(a), c = -gfx_fast_cos(a);
    gfx_line(fb, cx + s * r0, cy + c * r0, cx + s * r1, cy + c * r1, w, col);
}

void icon_glyph(gfx_fb *fb, int id, float cx, float cy, float r, uint16_t col)
{
    float w = r * 0.16f;
    if (w < GFX_S(2)) w = GFX_S(2);

    switch (id) {
    case ICON_CLOCK:    /* Lucide `clock`: ring + two hands to 12 and 3 */
        gfx_ring(fb, cx, cy, r * 0.9f, w, col);
        ray(fb, cx, cy, 0,          0, r * 0.54f, w, col);   /* minute (up) */
        ray(fb, cx, cy, PI * 0.5f,  0, r * 0.42f, w, col);   /* hour (right) */
        break;

    case ICON_STOPWATCH: {  /* Lucide `timer`: case + top button + hand */
        float scy = cy + r * 0.1f;
        gfx_ring(fb, cx, scy, r * 0.8f, w, col);
        gfx_line(fb, cx - r * 0.26f, cy - r * 0.82f, cx + r * 0.26f, cy - r * 0.82f, w, col); /* top bar */
        gfx_line(fb, cx, cy - r * 0.82f, cx, cy - r * 0.62f, w, col);                         /* stem */
        ray(fb, cx, scy, 0.7f, 0, r * 0.5f, w, col);                                          /* hand */
        break;
    }

    case ICON_TIMER: {  /* Lucide `hourglass`: two caps + crossed body */
        float hx = r * 0.52f, hy = r * 0.74f;
        gfx_line(fb, cx - hx, cy - hy, cx + hx, cy - hy, w, col);
        gfx_line(fb, cx - hx, cy + hy, cx + hx, cy + hy, w, col);
        gfx_line(fb, cx - hx * 0.9f, cy - hy, cx + hx * 0.9f, cy + hy, w, col);
        gfx_line(fb, cx + hx * 0.9f, cy - hy, cx - hx * 0.9f, cy + hy, w, col);
        break;
    }

    case ICON_WEATHER:  /* Lucide `sun`: ring core + 8 rays */
        gfx_ring(fb, cx, cy, r * 0.38f, w, col);
        for (int i = 0; i < 8; i++)
            ray(fb, cx, cy, i * (PI / 4), r * 0.6f, r * 0.92f, w, col);
        break;

    case ICON_ACTIVITY: {   /* Lucide `activity`: heartbeat pulse line */
        float y0v = cy;
        gfx_line(fb, cx - r * 0.9f,  y0v,          cx - r * 0.34f, y0v,          w, col);
        gfx_line(fb, cx - r * 0.34f, y0v,          cx - r * 0.1f,  cy - r * 0.6f, w, col);
        gfx_line(fb, cx - r * 0.1f,  cy - r * 0.6f, cx + r * 0.14f, cy + r * 0.6f, w, col);
        gfx_line(fb, cx + r * 0.14f, cy + r * 0.6f, cx + r * 0.36f, y0v,          w, col);
        gfx_line(fb, cx + r * 0.36f, y0v,          cx + r * 0.9f,  y0v,          w, col);
        break;
    }

    case ICON_SETTINGS: /* Lucide `settings`: toothed gear + hub */
        for (int i = 0; i < 8; i++)
            ray(fb, cx, cy, i * (PI / 4), r * 0.55f, r * 0.92f, w * 1.25f, col);
        gfx_ring(fb, cx, cy, r * 0.5f, w, col);
        gfx_fill_circle(fb, cx, cy, r * 0.18f, col);
        break;

    case ICON_MUSIC: {  /* Lucide `music`: two beamed notes */
        float lx = cx - r * 0.34f, rx = cx + r * 0.3f;
        gfx_fill_circle(fb, lx, cy + r * 0.44f, r * 0.2f, col);
        gfx_fill_circle(fb, rx, cy + r * 0.24f, r * 0.2f, col);
        gfx_line(fb, lx + r * 0.19f, cy + r * 0.44f, lx + r * 0.19f, cy - r * 0.5f, w, col);
        gfx_line(fb, rx + r * 0.19f, cy + r * 0.24f, rx + r * 0.19f, cy - r * 0.7f, w, col);
        gfx_line(fb, lx + r * 0.19f, cy - r * 0.5f, rx + r * 0.19f, cy - r * 0.7f, w * 1.4f, col);
        break;
    }

    case ICON_WIFI: {   /* Lucide `wifi`: fan of arcs above a dot */
        float by = cy + r * 0.6f;
        gfx_fill_circle(fb, cx, by, w * 0.8f, col);
        gfx_arc(fb, cx, by, r * 0.55f, w, -0.7f, 0.7f, col);
        gfx_arc(fb, cx, by, r * 0.95f, w, -0.7f, 0.7f, col);
        gfx_arc(fb, cx, by, r * 1.35f, w, -0.7f, 0.7f, col);
        break;
    }

    case ICON_FLASH:    /* Lucide `zap`: lightning bolt */
        gfx_line(fb, cx + r * 0.18f, cy - r * 0.85f, cx - r * 0.3f, cy + r * 0.05f, w, col);
        gfx_line(fb, cx - r * 0.3f,  cy + r * 0.05f, cx + r * 0.1f, cy + r * 0.05f, w, col);
        gfx_line(fb, cx + r * 0.1f,  cy + r * 0.05f, cx - r * 0.15f, cy + r * 0.85f, w, col);
        break;

    case ICON_BELL:     /* Lucide `bell`: dome + flared skirt + clapper */
        gfx_fill_circle(fb, cx, cy - r * 0.68f, w * 0.7f, col);          /* top nub */
        gfx_arc(fb, cx, cy - r * 0.12f, r * 0.5f, w, -1.6f, 1.6f, col);  /* dome */
        gfx_line(fb, cx - r * 0.5f, cy - r * 0.12f, cx - r * 0.62f, cy + r * 0.45f, w, col);
        gfx_line(fb, cx + r * 0.5f, cy - r * 0.12f, cx + r * 0.62f, cy + r * 0.45f, w, col);
        gfx_line(fb, cx - r * 0.62f, cy + r * 0.45f, cx + r * 0.62f, cy + r * 0.45f, w, col);
        gfx_fill_circle(fb, cx, cy + r * 0.62f, w * 0.9f, col);          /* clapper */
        break;

    case ICON_PLUS:
        gfx_line(fb, cx - r * 0.6f, cy, cx + r * 0.6f, cy, w * 1.3f, col);
        gfx_line(fb, cx, cy - r * 0.6f, cx, cy + r * 0.6f, w * 1.3f, col);
        break;

    case ICON_MINUS:
        gfx_line(fb, cx - r * 0.6f, cy, cx + r * 0.6f, cy, w * 1.3f, col);
        break;

    case ICON_ALARM: {  /* Lucide `alarm-clock`: clock + two bell ears */
        float acy = cy + r * 0.06f;
        gfx_ring(fb, cx, acy, r * 0.62f, w, col);
        ray(fb, cx, acy, 0,         0, r * 0.34f, w, col);   /* minute */
        ray(fb, cx, acy, PI * 0.5f, 0, r * 0.26f, w, col);   /* hour */
        gfx_line(fb, cx - r * 0.72f, cy - r * 0.5f, cx - r * 0.42f, cy - r * 0.74f, w, col);
        gfx_line(fb, cx + r * 0.72f, cy - r * 0.5f, cx + r * 0.42f, cy - r * 0.74f, w, col);
        break;
    }

    case ICON_GLOBE:    /* Lucide `globe`: ring + meridian + latitude lines */
        gfx_ring(fb, cx, cy, r * 0.85f, w, col);
        gfx_line(fb, cx, cy - r * 0.85f, cx, cy + r * 0.85f, w, col);   /* meridian */
        gfx_line(fb, cx - r * 0.85f, cy, cx + r * 0.85f, cy, w, col);   /* equator */
        gfx_line(fb, cx - r * 0.6f, cy - r * 0.55f, cx + r * 0.6f, cy - r * 0.55f, w, col);
        gfx_line(fb, cx - r * 0.6f, cy + r * 0.55f, cx + r * 0.6f, cy + r * 0.55f, w, col);
        break;

    case ICON_CALENDAR: {   /* Lucide `calendar`: body + header + two hangers */
        float bw = r * 0.72f, bt = cy - r * 0.58f, bb = cy + r * 0.74f;
        gfx_line(fb, cx - bw, bt, cx + bw, bt, w, col);
        gfx_line(fb, cx - bw, bb, cx + bw, bb, w, col);
        gfx_line(fb, cx - bw, bt, cx - bw, bb, w, col);
        gfx_line(fb, cx + bw, bt, cx + bw, bb, w, col);
        gfx_line(fb, cx - bw, bt + r * 0.32f, cx + bw, bt + r * 0.32f, w, col);   /* header */
        gfx_line(fb, cx - r * 0.4f, bt - r * 0.2f, cx - r * 0.4f, bt + r * 0.06f, w, col);
        gfx_line(fb, cx + r * 0.4f, bt - r * 0.2f, cx + r * 0.4f, bt + r * 0.06f, w, col);
        break;
    }

    case ICON_MIC: {    /* Lucide `mic`: capsule + cradle + stand */
        float mw = r * 0.28f, mtop = cy - r * 0.72f, mbot = cy + r * 0.02f;
        gfx_arc(fb, cx, mtop + mw, mw, w, -PI * 0.5f, PI * 0.5f, col);   /* capsule top */
        gfx_line(fb, cx - mw, mtop + mw, cx - mw, mbot, w, col);
        gfx_line(fb, cx + mw, mtop + mw, cx + mw, mbot, w, col);
        gfx_arc(fb, cx, cy - r * 0.12f, r * 0.5f, w, PI * 0.5f, PI * 1.5f, col);  /* cradle */
        gfx_line(fb, cx, cy + r * 0.38f, cx, cy + r * 0.72f, w, col);             /* stand */
        gfx_line(fb, cx - r * 0.32f, cy + r * 0.72f, cx + r * 0.32f, cy + r * 0.72f, w, col);
        break;
    }

    case ICON_WAVEFORM: {   /* Lucide `audio-lines`: 5 vertical bars */
        static const float hx[5] = { -0.6f, -0.3f, 0.0f, 0.3f, 0.6f };
        static const float hh[5] = { 0.32f, 0.62f, 0.46f, 0.8f, 0.4f };
        for (int i = 0; i < 5; i++)
            gfx_line(fb, cx + hx[i] * r, cy - hh[i] * r, cx + hx[i] * r, cy + hh[i] * r, w, col);
        break;
    }

    case ICON_PHONE: {  /* Lucide `smartphone`: body + earpiece + home dot */
        float pw = r * 0.42f, pt = cy - r * 0.82f, pb = cy + r * 0.82f;
        gfx_line(fb, cx - pw, pt, cx + pw, pt, w, col);
        gfx_line(fb, cx - pw, pb, cx + pw, pb, w, col);
        gfx_line(fb, cx - pw, pt, cx - pw, pb, w, col);
        gfx_line(fb, cx + pw, pt, cx + pw, pb, w, col);
        gfx_line(fb, cx - r * 0.14f, pt + r * 0.16f, cx + r * 0.14f, pt + r * 0.16f, w, col);
        gfx_fill_circle(fb, cx, pb - r * 0.16f, w * 0.7f, col);
        break;
    }

    case ICON_BLUETOOTH: {  /* Lucide `bluetooth`: the rune (single stroke) */
        float d5 = r * 0.42f, d10 = r * 0.84f;
        gfx_line(fb, cx - d5, cy - d5, cx + d5, cy + d5, w, col);
        gfx_line(fb, cx + d5, cy + d5, cx,      cy + d10, w, col);
        gfx_line(fb, cx,      cy + d10, cx,     cy - d10, w, col);
        gfx_line(fb, cx,      cy - d10, cx + d5, cy - d5, w, col);
        gfx_line(fb, cx + d5, cy - d5, cx - d5, cy + d5, w, col);
        break;
    }

    case ICON_SIGNAL: { /* Lucide `signal`: ascending bars */
        float base = cy + r * 0.68f;
        static const float sh[4] = { 0.32f, 0.56f, 0.82f, 1.08f };
        for (int i = 0; i < 4; i++) {
            float bx = cx + (i - 1.5f) * r * 0.42f;
            gfx_line(fb, bx, base, bx, base - sh[i] * r, w * 1.1f, col);
        }
        break;
    }

    case ICON_GAUGE:    /* Lucide `gauge`: dial arc + needle + hub */
        gfx_arc(fb, cx, cy + r * 0.25f, r * 0.8f, w, -PI * 0.72f, PI * 0.72f, col);
        ray(fb, cx, cy + r * 0.25f, 0.9f, 0, r * 0.62f, w, col);
        gfx_fill_circle(fb, cx, cy + r * 0.25f, w * 1.1f, col);
        break;

    case ICON_LEVEL: {  /* spirit level: vial + centered bubble + guide marks */
        float lw = r * 0.9f, lh = r * 0.3f;
        gfx_line(fb, cx - lw, cy - lh, cx + lw, cy - lh, w, col);
        gfx_line(fb, cx - lw, cy + lh, cx + lw, cy + lh, w, col);
        gfx_line(fb, cx - lw, cy - lh, cx - lw, cy + lh, w, col);
        gfx_line(fb, cx + lw, cy - lh, cx + lw, cy + lh, w, col);
        gfx_ring(fb, cx, cy, lh * 0.62f, w, col);
        gfx_line(fb, cx - lh * 0.95f, cy - lh, cx - lh * 0.95f, cy + lh, w, col);
        gfx_line(fb, cx + lh * 0.95f, cy - lh, cx + lh * 0.95f, cy + lh, w, col);
        break;
    }

    case ICON_CALC: {   /* Lucide `calculator`: body + display + button grid */
        float cw = r * 0.6f, ct = cy - r * 0.82f, cb = cy + r * 0.82f;
        gfx_line(fb, cx - cw, ct, cx + cw, ct, w, col);
        gfx_line(fb, cx - cw, cb, cx + cw, cb, w, col);
        gfx_line(fb, cx - cw, ct, cx - cw, cb, w, col);
        gfx_line(fb, cx + cw, ct, cx + cw, cb, w, col);
        gfx_line(fb, cx - cw, ct + r * 0.42f, cx + cw, ct + r * 0.42f, w, col);   /* display */
        for (int i = 0; i < 4; i++)
            gfx_fill_circle(fb, cx + ((i & 1) ? r * 0.28f : -r * 0.28f),
                            cy + ((i >> 1) ? r * 0.56f : r * 0.16f), w * 0.7f, col);
        break;
    }

    case ICON_GRID: {   /* Lucide `layout-grid`: framed 2x2 grid */
        float g = r * 0.72f;
        gfx_line(fb, cx - g, cy - g, cx + g, cy - g, w, col);
        gfx_line(fb, cx - g, cy + g, cx + g, cy + g, w, col);
        gfx_line(fb, cx - g, cy - g, cx - g, cy + g, w, col);
        gfx_line(fb, cx + g, cy - g, cx + g, cy + g, w, col);
        gfx_line(fb, cx, cy - g, cx, cy + g, w, col);
        gfx_line(fb, cx - g, cy, cx + g, cy, w, col);
        break;
    }

    case ICON_BATTERY: {    /* Lucide `battery`: body + terminal nub */
        float bw = r * 0.78f, bh = r * 0.42f;
        gfx_line(fb, cx - bw, cy - bh, cx + bw, cy - bh, w, col);
        gfx_line(fb, cx - bw, cy + bh, cx + bw, cy + bh, w, col);
        gfx_line(fb, cx - bw, cy - bh, cx - bw, cy + bh, w, col);
        gfx_line(fb, cx + bw, cy - bh, cx + bw, cy + bh, w, col);
        gfx_line(fb, cx + bw + w, cy - bh * 0.45f, cx + bw + w, cy + bh * 0.45f, w * 1.4f, col);
        break;
    }

    case ICON_FLASHLIGHT: { /* Lucide `flashlight`: torch head + tapering body */
        float hw = r * 0.34f, htop = cy - r * 0.82f, ntop = cy - r * 0.5f;
        float bw = r * 0.24f, bbot = cy + r * 0.62f;
        gfx_line(fb, cx - hw, htop, cx + hw, htop, w, col);   /* head top */
        gfx_line(fb, cx - hw, htop, cx - hw, ntop, w, col);   /* head left */
        gfx_line(fb, cx + hw, htop, cx + hw, ntop, w, col);   /* head right */
        gfx_line(fb, cx - hw, ntop, cx + hw, ntop, w, col);   /* neck seam */
        gfx_line(fb, cx - hw, ntop, cx - bw, bbot, w, col);   /* body left */
        gfx_line(fb, cx + hw, ntop, cx + bw, bbot, w, col);   /* body right */
        gfx_line(fb, cx - bw, bbot, cx + bw, bbot, w, col);   /* body bottom */
        break;
    }

    case ICON_BREATHE: {    /* four-petal bloom (breathe) */
        float pr = r * 0.42f, off = r * 0.42f;
        gfx_ring(fb, cx - off, cy - off, pr, w, col);
        gfx_ring(fb, cx + off, cy - off, pr, w, col);
        gfx_ring(fb, cx - off, cy + off, pr, w, col);
        gfx_ring(fb, cx + off, cy + off, pr, w, col);
        break;
    }

    case ICON_SYNC: {   /* clock ringed by a circular retry arrow (re-sync time) */
        gfx_ring(fb, cx, cy, r * 0.46f, w, col);              /* clock face */
        ray(fb, cx, cy, 0.0f,      0, r * 0.30f, w, col);    /* minute -> 12 */
        ray(fb, cx, cy, PI * 0.5f, 0, r * 0.22f, w, col);    /* hour -> 3 */
        float R = r * 0.9f, ae = 5.75f;                      /* outer retry arc (~298 deg) */
        gfx_arc(fb, cx, cy, R, w, 0.55f, ae, col);
        float px = cx + gfx_fast_sin(ae) * R, py = cy - gfx_fast_cos(ae) * R;   /* arrowhead at the end */
        float tx = gfx_fast_cos(ae), ty = gfx_fast_sin(ae), bl = r * 0.32f;
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            float ang = (float)sgn * 2.4f, ca = gfx_fast_cos(ang), sa = gfx_fast_sin(ang);
            gfx_line(fb, px, py, px + (tx * ca - ty * sa) * bl, py + (tx * sa + ty * ca) * bl, w, col);
        }
        break;
    }

    case ICON_TRASH: {  /* Lucide `trash-2`: lid + handle + can body + 2 slits */
        float hw = r * 0.5f, lid = cy - r * 0.5f;
        gfx_line(fb, cx - hw - r * 0.14f, lid, cx + hw + r * 0.14f, lid, w, col);   /* lid bar */
        gfx_line(fb, cx - r * 0.2f, lid, cx - r * 0.14f, cy - r * 0.72f, w, col);   /* handle */
        gfx_line(fb, cx + r * 0.2f, lid, cx + r * 0.14f, cy - r * 0.72f, w, col);
        gfx_line(fb, cx - r * 0.2f, cy - r * 0.72f, cx + r * 0.2f, cy - r * 0.72f, w, col);
        gfx_line(fb, cx - hw * 0.86f, lid, cx - hw * 0.66f, cy + r * 0.72f, w, col); /* body sides */
        gfx_line(fb, cx + hw * 0.86f, lid, cx + hw * 0.66f, cy + r * 0.72f, w, col);
        gfx_line(fb, cx - hw * 0.66f, cy + r * 0.72f, cx + hw * 0.66f, cy + r * 0.72f, w, col);
        gfx_line(fb, cx - r * 0.18f, cy - r * 0.28f, cx - r * 0.18f, cy + r * 0.5f, w, col); /* slits */
        gfx_line(fb, cx + r * 0.18f, cy - r * 0.28f, cx + r * 0.18f, cy + r * 0.5f, w, col);
        break;
    }

    default:
        break;
    }
}

void icon_badge(gfx_fb *fb, float cx, float cy, float r,
                uint16_t accent, int icon_id, const char *label)
{
    gfx_fill_circle(fb, cx, cy, r, accent);
    if (icon_id != ICON_NONE) {
        icon_glyph(fb, icon_id, cx, cy, r * 0.6f, GFX_WHITE);
        return;
    }
    char init[2] = { label && label[0] ? label[0] : '?', 0 };
    if (init[0] >= 'a' && init[0] <= 'z') init[0] -= 32;
    const font_t *f = r > GFX_S(24) ? &font_medium : &font_small;
    font_draw_centered(fb, f, (int)cx, (int)(cy + f->ascent * 0.34f), init, GFX_WHITE);
}
