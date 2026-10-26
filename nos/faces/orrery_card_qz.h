#ifndef ORRERY_CARD_QZ_H
#define ORRERY_CARD_QZ_H
#include <stdint.h>
/* Exact IEEE32 division by32767 for the published LUT domain0..32767.
 * n*2^(23-k) is an exact integer significand A. Divide32768/32767
 * as1+1/32767, then round positive rational A/32767 to nearest integer.
 * Odd denominator means no half-way tie. Only n32767 reaches mantissa2^24.
 */
static inline __attribute__((always_inline)) float card_qz_exact(uint16_t n)
{
    if(!n)return 0.0f;
    if(n>32767)return n/32767.0f;
    unsigned k=31u-(unsigned)__builtin_clz((unsigned)n);
    uint32_t a=(uint32_t)n<<(23u-k);
    uint32_t mant=a+(a+16383u)/32767u;
    union {uint32_t u;float f;} out={((k+112u)<<23)+(mant-0x800000u)};
    return out.f;
}
static inline float card_qz_legacy(uint16_t n) { return n / 32767.0f; }
/* Debug APIs: exhaustive check is quiet, allocation-free and does not select a mode. */
extern int orrery_card_qz_exact;
unsigned orrery_card_qz_check(void);
#endif
