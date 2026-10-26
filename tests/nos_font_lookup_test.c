/* Exact pointer selection, including duplicate codes with differing metrics. */
#include "font.h"
#include <assert.h>
#include <stdio.h>
const font_glyph *test_core_lookup(const font_t *, unsigned);
const font_glyph *test_ui_lookup(const font_t *, unsigned);
extern const font_t *test_fonts[];
extern unsigned test_font_count;
static const font_glyph *reference(const font_t *f, unsigned code)
{
    int lo = 0, hi = f->nglyphs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].code == code) return &f->glyphs[mid];
        if (f->glyphs[mid].code < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}
static unsigned rng = 7477;
static unsigned random_word(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void check(const font_t *f, unsigned code)
{
    const font_glyph *expected = reference(f, code);
    assert(test_core_lookup(f, code) == expected);
    assert(test_ui_lookup(f, code) == expected);
}
int main(void)
{
    unsigned long cases = 0;
    for (unsigned k = 0; k < test_font_count; k++) for (unsigned code = 0; code <= 65535; code++) {
        check(test_fonts[k], code); cases++;
    }
    font_t empty = {0};
    check(&empty, 0); check(&empty, ~0u);
    empty.nglyphs = -1; check(&empty, 0);
    font_glyph glyphs[128] = {0};
    font_t font = {0}; font.glyphs = glyphs;
    for (int t = 0; t < 5000; t++) {
        font.nglyphs = (int)(random_word() % 129);
        unsigned code = random_word() % 64;
        for (int i = 0; i < font.nglyphs; i++) {
            code += random_word() % 3;
            glyphs[i].code = (uint16_t)code;
            glyphs[i].xadv = (uint8_t)(i + 1); /* duplicates have distinct metrics */
        }
        for (code = 0; code < 384; code++) { check(&font, code); cases++; }
        check(&font, ~0u); check(&font, 0x10000u);
    }
    printf("PASS %lu font lookup cases: actual tables, empty/sparse/duplicate fonts, unsigned bounds\n", cases);
    return 0;
}
