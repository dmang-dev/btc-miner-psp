/*
 * sha256.h — single-shot SHA-256 + low-level compression API for
 * Bitcoin midstate mining.  Pure C with no platform deps; the inner
 * loop uses an Allegrex `rotr` inline-asm specialization when built
 * for MIPS so each of the ~384 rotates per compression collapses from
 * 3 instructions (srl/sll/or) to 1.
 *
 * Ported from hash-bench-3ds / hash-bench-n64.
 */
#ifndef BTCM_SHA256_H
#define BTCM_SHA256_H

#include <stdint.h>
#include <stddef.h>

/* SHA-256 intermediate state (8 × 32-bit chaining variables).  Exposed
** so the miner can compute a per-job midstate once (over the first
** 64 bytes of the Bitcoin block header, which are constant for the
** duration of a job) and then re-use it across the ~4 billion nonces
** in the sweep, saving one full compression per nonce. */
typedef struct {
    uint32_t h[8];
} sha256_state_t;

/* Reset state to the SHA-256 IV (FIPS 180-4 §5.3.3). */
void sha256_init(sha256_state_t *s);

/* Run one 64-byte block through the SHA-256 compression function,
** updating state in place.  Caller is responsible for padding the
** final block per FIPS 180-4 §5.1.1 (see sha256() for an example). */
void sha256_compress(sha256_state_t *s, const uint8_t blk[64]);

/* Serialize the 8 chaining variables to a 32-byte big-endian digest. */
void sha256_state_to_bytes(const sha256_state_t *s, uint8_t out[32]);

/* Convenience one-shot: SHA-256 over `data[0..len)`, write digest to out. */
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Verify implementation against FIPS test vectors + the Bitcoin
** double-SHA256 of the canonical genesis-block header.  Returns 0
** on success, nonzero on any vector mismatch. */
int sha256_selftest(void);

#endif
