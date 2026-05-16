/*
 * target.h — Bitcoin pool-difficulty target computation + share check.
 *
 * Stratum sends share difficulty as a floating-point value. The
 * underlying check is: SHA-256d(header) <= target, where
 *
 *     target = bdiff_1_target / difficulty
 *     bdiff_1_target = 0x00000000FFFF0000_<224 zero bits>
 *
 * Bitcoin's convention treats the SHA-256d output as a little-endian
 * uint256 when comparing to the target (which is conventionally
 * written big-endian). So byte 31 of the hash is its most-significant
 * byte for comparison purposes.
 *
 * The miner's previous v0.1 share check was effectively "first 4 bytes
 * < 0xFFFF0000" (a much weaker check than diff-1 because it ignored
 * the lower 224 bits). v0.2 does the full 256-bit comparison and
 * scales with pool difficulty.
 */
#ifndef BTCM_TARGET_H
#define BTCM_TARGET_H

#include <stdint.h>

/* Compute the 256-bit pool target for a given difficulty.
 *
 * difficulty: pool's diff value (typical: 1, 8, 1024, ... but also
 *             non-integer like 0.001 for very slow miners).
 * target_be:  output, 32 bytes, big-endian (target_be[0] is MSB).
 *
 * For difficulty >= 1, computes 0x00000000FFFF0000... / difficulty with
 * uint64 precision in the top 64 bits and zero below. For difficulty
 * < 1, scales up so the resulting target is bigger than bdiff-1.
 * Lower bits beyond the top 64 are zero — fine because the PSP will
 * never compute a hash whose lower 192 bits matter for the comparison
 * (probabilistically that requires far more than the project's run
 * time times PSP hashrate).
 */
void target_from_difficulty(double difficulty, uint8_t target_be[32]);

/* Compare a SHA-256d hash output (Bitcoin LE convention) against a
 * target (BE).  Returns 1 if hash <= target, else 0. */
int hash_meets_target(const uint8_t hash_le[32], const uint8_t target_be[32]);

#endif
