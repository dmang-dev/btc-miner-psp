/*
 * sha256.c — FIPS 180-4 SHA-256 with a Bitcoin-midstate-friendly
 * compression API.  The single-block compression function (`sha256_compress`)
 * is now public so the miner can:
 *
 *   1. Compute SHA-256 over the constant first 64 bytes of an 80-byte
 *      Bitcoin block header once per job (the "midstate"), and
 *   2. For each of the 2^32 nonces, run only TWO compressions instead of
 *      three: one for the 16-byte tail+pad (with midstate as the
 *      starting state) and one for the second SHA-256 over the 32-byte
 *      intermediate digest.
 *
 * Plus: the inner round's six rotates per iteration map directly to
 * Allegrex's `rotr` instruction.  psp-gcc 15 does NOT auto-fuse the
 * `(x>>n)|(x<<(32-n))` idiom — verified by disassembling -O2 output and
 * counting `srl`/`sll`/`or` triples vs `rotr` (0 of the latter).  We
 * spell it out in inline asm so each of the ~384 rotates per
 * compression collapses from 3 instructions to 1.
 *
 * Self-test still validates against the FIPS 180-2 §B.1 vector AND the
 * canonical Bitcoin genesis-block header double-hash, so any
 * arithmetic regression from the refactor or the asm trips at boot.
 */
#include "sha256.h"
#include <string.h>

/* Rotate right.  On Allegrex (MIPS-II + ROTR), `rotr rd, rt, sa`
** retires in one cycle.  The C fallback ((x>>n)|(x<<(32-n))) is what
** psp-gcc emits for non-MIPS targets and for host self-test builds. */
#if defined(__mips__) && (defined(__psp__) || defined(_PSP))
static inline uint32_t ror32(uint32_t x, unsigned n) {
    uint32_t r;
    __asm__("rotr %0, %1, %2"
            : "=r"(r)
            : "r"(x), "i"(n));
    return r;
}
#define ROR32(x,n)  ror32((uint32_t)(x), (n))
#else
#define ROR32(x,n)  (((uint32_t)(x) >> (n)) | ((uint32_t)(x) << (32u - (n))))
#endif

#define CH(x,y,z)   (((x) & (y)) ^ ((~(x)) & (z)))
#define MAJ(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)    (ROR32(x,2u)  ^ ROR32(x,13u) ^ ROR32(x,22u))
#define BSIG1(x)    (ROR32(x,6u)  ^ ROR32(x,11u) ^ ROR32(x,25u))
#define SSIG0(x)    (ROR32(x,7u)  ^ ROR32(x,18u) ^ ((x) >> 3u))
#define SSIG1(x)    (ROR32(x,17u) ^ ROR32(x,19u) ^ ((x) >> 10u))

static const uint32_t K2[64] = {
    0x428A2F98UL,0x71374491UL,0xB5C0FBCFUL,0xE9B5DBA5UL,
    0x3956C25BUL,0x59F111F1UL,0x923F82A4UL,0xAB1C5ED5UL,
    0xD807AA98UL,0x12835B01UL,0x243185BEUL,0x550C7DC3UL,
    0x72BE5D74UL,0x80DEB1FEUL,0x9BDC06A7UL,0xC19BF174UL,
    0xE49B69C1UL,0xEFBE4786UL,0x0FC19DC6UL,0x240CA1CCUL,
    0x2DE92C6FUL,0x4A7484AAUL,0x5CB0A9DCUL,0x76F988DAUL,
    0x983E5152UL,0xA831C66DUL,0xB00327C8UL,0xBF597FC7UL,
    0xC6E00BF3UL,0xD5A79147UL,0x06CA6351UL,0x14292967UL,
    0x27B70A85UL,0x2E1B2138UL,0x4D2C6DFCUL,0x53380D13UL,
    0x650A7354UL,0x766A0ABBUL,0x81C2C92EUL,0x92722C85UL,
    0xA2BFE8A1UL,0xA81A664BUL,0xC24B8B70UL,0xC76C51A3UL,
    0xD192E819UL,0xD6990624UL,0xF40E3585UL,0x106AA070UL,
    0x19A4C116UL,0x1E376C08UL,0x2748774CUL,0x34B0BCB5UL,
    0x391C0CB3UL,0x4ED8AA4AUL,0x5B9CCA4FUL,0x682E6FF3UL,
    0x748F82EEUL,0x78A5636FUL,0x84C87814UL,0x8CC70208UL,
    0x90BEFFFAUL,0xA4506CEBUL,0xBEF9A3F7UL,0xC67178F2UL
};

void sha256_init(sha256_state_t *s) {
    s->h[0] = 0x6A09E667UL; s->h[1] = 0xBB67AE85UL;
    s->h[2] = 0x3C6EF372UL; s->h[3] = 0xA54FF53AUL;
    s->h[4] = 0x510E527FUL; s->h[5] = 0x9B05688CUL;
    s->h[6] = 0x1F83D9ABUL; s->h[7] = 0x5BE0CD19UL;
}

void sha256_compress(sha256_state_t *s, const uint8_t blk[64]) {
    uint32_t W[16];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    unsigned i;

    for (i = 0; i < 16u; i++) {
        W[i] = ((uint32_t)blk[i*4u]      << 24u) |
               ((uint32_t)blk[i*4u + 1u] << 16u) |
               ((uint32_t)blk[i*4u + 2u] <<  8u) |
                (uint32_t)blk[i*4u + 3u];
    }

    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];

    for (i = 0; i < 64u; i++) {
        uint32_t w;
        if (i < 16u) {
            w = W[i];
        } else {
            w = SSIG1(W[(i - 2u)  & 0xFu]) + W[(i - 7u)  & 0xFu]
              + SSIG0(W[(i - 15u) & 0xFu]) + W[(i - 16u) & 0xFu];
            W[i & 0xFu] = w;
        }
        t1 = h + BSIG1(e) + CH(e, f, g) + K2[i] + w;
        t2 = BSIG0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_state_to_bytes(const sha256_state_t *s, uint8_t out[32]) {
    unsigned i;
    for (i = 0; i < 8u; i++) {
        out[i*4u]      = (uint8_t)(s->h[i] >> 24);
        out[i*4u + 1u] = (uint8_t)(s->h[i] >> 16);
        out[i*4u + 2u] = (uint8_t)(s->h[i] >>  8);
        out[i*4u + 3u] = (uint8_t)(s->h[i]);
    }
}

void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    sha256_state_t s;
    uint8_t  block[64];
    size_t   off;
    uint32_t bit_count_lo, bit_count_hi;
    unsigned rem, i;

    sha256_init(&s);

    for (off = 0; off + 64u <= len; off += 64u) {
        sha256_compress(&s, data + off);
    }

    rem = (unsigned)(len - off);
    for (i = 0; i < rem; i++) block[i] = data[off + i];
    block[rem++] = 0x80u;
    if (rem > 56u) {
        while (rem < 64u) block[rem++] = 0;
        sha256_compress(&s, block);
        rem = 0;
    }
    while (rem < 56u) block[rem++] = 0;

    /* 64-bit big-endian length in BITS. */
    bit_count_hi = (uint32_t)((uint64_t)len >> 29);
    bit_count_lo = (uint32_t)((uint64_t)len <<  3);
    block[56] = (uint8_t)(bit_count_hi >> 24);
    block[57] = (uint8_t)(bit_count_hi >> 16);
    block[58] = (uint8_t)(bit_count_hi >>  8);
    block[59] = (uint8_t)(bit_count_hi);
    block[60] = (uint8_t)(bit_count_lo >> 24);
    block[61] = (uint8_t)(bit_count_lo >> 16);
    block[62] = (uint8_t)(bit_count_lo >>  8);
    block[63] = (uint8_t)(bit_count_lo);
    sha256_compress(&s, block);

    sha256_state_to_bytes(&s, out);
}

/* ---- self-test --------------------------------------------------- */

/* FIPS 180-2 §B.1 test vector. */
static const uint8_t TV1_MSG[]    = { 'a','b','c' };
static const uint8_t TV1_DIGEST[] = {
    0xBA,0x78,0x16,0xBF, 0x8F,0x01,0xCF,0xEA, 0x41,0x41,0x40,0xDE,
    0x5D,0xAE,0x22,0x23, 0xB0,0x03,0x61,0xA3, 0x96,0x17,0x7A,0x9C,
    0xB4,0x10,0xFF,0x61, 0xF2,0x00,0x15,0xAD
};

/* Bitcoin genesis-block header (80 bytes), known to double-hash to
** the genesis-block hash (which displayed in reversed byte order is
** the famous 0000...19d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f).
**
** Header layout: ver(4) prevhash(32) merkle(32) ntime(4) nbits(4) nonce(4)
** All little-endian on the wire. */
static const uint8_t GENESIS_HEADER[80] = {
    /* version = 1 */
    0x01, 0x00, 0x00, 0x00,
    /* prev_block_hash = 0 */
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    /* merkle_root = 4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b
    ** stored little-endian on the wire (byte-reversed from how it's displayed): */
    0x3B,0xA3,0xED,0xFD, 0x7A,0x7B,0x12,0xB2, 0x7A,0xC7,0x2C,0x3E,
    0x67,0x76,0x8F,0x61, 0x7F,0xC8,0x1B,0xC3, 0x88,0x8A,0x51,0x32,
    0x3A,0x9F,0xB8,0xAA, 0x4B,0x1E,0x5E,0x4A,
    /* ntime = 0x495FAB29 = 1231006505 (Jan 3 2009 18:15:05 UTC) */
    0x29, 0xAB, 0x5F, 0x49,
    /* nbits = 0x1D00FFFF */
    0xFF, 0xFF, 0x00, 0x1D,
    /* nonce = 0x7C2BAC1D = 2083236893 */
    0x1D, 0xAC, 0x2B, 0x7C
};

/* Expected double-SHA-256 of the genesis header, big-endian byte order
** (i.e., as bytes; the hash *displayed* as a Bitcoin block ID is this
** with the byte order reversed): */
static const uint8_t GENESIS_DHASH[32] = {
    0x6F,0xE2,0x8C,0x0A, 0xB6,0xF1,0xB3,0x72, 0xC1,0xA6,0xA2,0x46,
    0xAE,0x63,0xF7,0x4F, 0x93,0x1E,0x83,0x65, 0xE1,0x5A,0x08,0x9C,
    0x68,0xD6,0x19,0x00, 0x00,0x00,0x00,0x00
};

int sha256_selftest(void) {
    uint8_t digest[32];
    uint8_t inter[32];

    /* Single-hash test vector. */
    sha256(TV1_MSG, sizeof(TV1_MSG), digest);
    if (memcmp(digest, TV1_DIGEST, 32) != 0) return -1;

    /* Bitcoin genesis-block double-hash via the one-shot API
    ** (validates sha256_init + sha256_compress + sha256_state_to_bytes
    ** all wired up correctly). */
    sha256(GENESIS_HEADER, 80, inter);
    sha256(inter, 32, digest);
    if (memcmp(digest, GENESIS_DHASH, 32) != 0) return -2;

    /* And again via the midstate API the miner actually uses, so any
    ** mistake in the per-job/per-nonce split also trips here.  Split
    ** the 80-byte header into a 64-byte midstate block plus a 16-byte
    ** tail that gets padded to a second block.  The second SHA-256 of
    ** the 32-byte intermediate digest is one full block (32B + 1 padding
    ** byte + zero pad + 8-byte length = 64 B). */
    {
        sha256_state_t mid, tail;
        uint8_t block2[64];
        uint8_t finalblk[64];
        uint8_t inter_mid[32];
        unsigned i;

        sha256_init(&mid);
        sha256_compress(&mid, GENESIS_HEADER);          /* bytes 0..63 */
        memcpy(block2, GENESIS_HEADER + 64, 16);        /* bytes 64..79 */
        block2[16] = 0x80;
        for (i = 17; i < 56; i++) block2[i] = 0;
        /* length = 80 bytes = 640 bits = 0x00000280 */
        block2[56] = 0; block2[57] = 0; block2[58] = 0; block2[59] = 0;
        block2[60] = 0; block2[61] = 0; block2[62] = 0x02; block2[63] = 0x80;
        sha256_compress(&mid, block2);
        sha256_state_to_bytes(&mid, inter_mid);
        if (memcmp(inter_mid, inter, 32) != 0) return -3;

        sha256_init(&tail);
        memcpy(finalblk, inter_mid, 32);
        finalblk[32] = 0x80;
        for (i = 33; i < 56; i++) finalblk[i] = 0;
        /* length = 32 bytes = 256 bits = 0x00000100 */
        finalblk[56] = 0; finalblk[57] = 0; finalblk[58] = 0; finalblk[59] = 0;
        finalblk[60] = 0; finalblk[61] = 0; finalblk[62] = 0x01; finalblk[63] = 0;
        sha256_compress(&tail, finalblk);
        sha256_state_to_bytes(&tail, digest);
        if (memcmp(digest, GENESIS_DHASH, 32) != 0) return -4;
    }

    return 0;
}
