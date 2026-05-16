#include "target.h"

#include <string.h>

void target_from_difficulty(double difficulty, uint8_t target_be[32]) {
    memset(target_be, 0, 32);

    /* Treat <=0 and NaN as diff=1 for safety. The check `difficulty > 0`
     * is false for both negatives and NaN (NaN comparisons return false),
     * so the fallback covers both. */
    if (!(difficulty > 0.0)) difficulty = 1.0;

    /* bdiff-1 top 64 bits: bytes 0..7 of the target (big-endian) =
     * 0x00 0x00 0x00 0x00 0xFF 0xFF 0x00 0x00 = 0x00000000FFFF0000. */
    const double bdiff_1_top64 = 281470681743360.0;   /* 0xFFFF0000 << 16 */

    /* For diff >= 1 the result is smaller than bdiff-1; we fit
     * comfortably in uint64. For diff < 1 we may overflow uint64
     * (e.g. diff=2^-32 scales the top by 2^32, blowing the 48-bit
     * range of the bdiff-1 top word) — clamp to UINT64_MAX in that
     * case, which corresponds to a (still very large) target. */
    double r = bdiff_1_top64 / difficulty;
    uint64_t top;
    if (r >= 1.8446744073709552e19) {       /* >= 2^64 */
        top = 0xFFFFFFFFFFFFFFFFULL;
    } else {
        top = (uint64_t)r;
    }

    target_be[0] = (uint8_t)((top >> 56) & 0xFF);
    target_be[1] = (uint8_t)((top >> 48) & 0xFF);
    target_be[2] = (uint8_t)((top >> 40) & 0xFF);
    target_be[3] = (uint8_t)((top >> 32) & 0xFF);
    target_be[4] = (uint8_t)((top >> 24) & 0xFF);
    target_be[5] = (uint8_t)((top >> 16) & 0xFF);
    target_be[6] = (uint8_t)((top >>  8) & 0xFF);
    target_be[7] = (uint8_t)( top        & 0xFF);
    /* Lower 192 bits stay zero. See header for why this is safe. */
}

int hash_meets_target(const uint8_t hash_le[32], const uint8_t target_be[32]) {
    /* Bitcoin LE convention: hash_le[31] is the most significant byte
     * of the uint256 hash. target_be[0] is the MSB. Compare from
     * MSB downward. */
    for (int i = 0; i < 32; i++) {
        uint8_t h = hash_le[31 - i];
        uint8_t t = target_be[i];
        if (h < t) return 1;
        if (h > t) return 0;
    }
    return 1;  /* exactly equal — counts as valid */
}
