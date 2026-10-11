/* png_write.h — dependency-free PNG writer for host-side preview tools.
 *
 * Header-only (all functions static). Writes 8-bit RGB PNGs using stored
 * (uncompressed) deflate blocks, so there is no zlib dependency; files are
 * ~3 bytes/px, which is fine for specimen sheets.
 *
 *   png_write_rgb  (path, w, h, rgb)        rgb: w*h*3 bytes, row-major
 *   png_write_565  (path, w, h, px, stride) px:  RGB565, `stride` pixels per row
 *
 * Both return 0 on success, -1 on I/O failure. */
#ifndef TOOLS_PNG_WRITE_H
#define TOOLS_PNG_WRITE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t png__crc_table[256];
static int png__crc_ready;

static uint32_t png__crc(uint32_t crc, const uint8_t *p, size_t n)
{
    if (!png__crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            png__crc_table[i] = c;
        }
        png__crc_ready = 1;
    }
    crc ^= 0xFFFFFFFFu;
    while (n--) crc = png__crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void png__be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int png__chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t hdr[8];
    png__be32(hdr, n);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = png__crc(0, hdr + 4, 4);
    crc = png__crc(crc, data, n);
    uint8_t tail[4];
    png__be32(tail, crc);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;
    return fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

static int png_write_rgb(const char *path, int w, int h, const uint8_t *rgb)
{
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;                                  /* filter: none */
        memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    /* zlib stream: header, stored blocks of <= 65535 bytes, adler32 */
    size_t nblk = raw_n / 65535 + 1;
    size_t z_n = 2 + nblk * 5 + raw_n + 4;
    uint8_t *z = (uint8_t *)malloc(z_n);
    if (!z) { free(raw); return -1; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    size_t left = raw_n, src = 0;
    do {
        uint32_t n = left > 65535 ? 65535 : (uint32_t)left;
        z[o++] = left <= 65535 ? 1 : 0;                    /* BFINAL, BTYPE=00 */
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + src, n);
        o += n; src += n; left -= n;
    } while (left);
    png__be32(z + o, (b << 16) | a); o += 4;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ihdr[13];
    png__be32(ihdr, (uint32_t)w); png__be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* 8-bit RGB */
    int err = fwrite(sig, 1, 8, f) != 8;
    err |= png__chunk(f, "IHDR", ihdr, 13) != 0;
    err |= png__chunk(f, "IDAT", z, (uint32_t)o) != 0;
    err |= png__chunk(f, "IEND", NULL, 0) != 0;
    err |= fclose(f) != 0;
    free(z);
    return err ? -1 : 0;
}

static int png_write_565(const char *path, int w, int h, const uint16_t *px, int stride)
{
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) return -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t c = px[(size_t)y * stride + x];
            uint8_t *d = rgb + ((size_t)y * w + x) * 3;
            d[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            d[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            d[2] = (uint8_t)((c & 31) * 255 / 31);
        }
    int r = png_write_rgb(path, w, h, rgb);
    free(rgb);
    return r;
}
#endif
