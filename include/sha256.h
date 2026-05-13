/*
 * sha256.h — single-shot SHA-256 + self-test, ported from
 * hash-bench-3ds / hash-bench-n64.  Pure C with no platform deps;
 * MIPS R4000 compiles it cleanly under psp-gcc.
 */
#ifndef BTCM_SHA256_H
#define BTCM_SHA256_H

#include <stdint.h>
#include <stddef.h>

/* Compute SHA-256 of `data[0..len)`, write 32-byte digest to out. */
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Verify implementation against FIPS test vectors + the Bitcoin
** double-SHA256 of the canonical genesis-block header.  Returns 0
** on success, nonzero on any vector mismatch. */
int sha256_selftest(void);

#endif
