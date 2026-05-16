/*
 * btc-miner-psp — Bitcoin pool miner for the Sony PSP-1000+.
 *
 * Stratum-v1 client + scalar double-SHA-256 nonce sweep, displayed
 * on PSP's 480x272 framebuffer via pspDebugScreen.  Connects to the
 * configured pool over PSP's 802.11b chip after the user picks a
 * saved WLAN profile from XMB's network settings.
 *
 * Hashrate, statistically: ~30-50 kH/s on a 333 MHz MIPS R4000 doing
 * pure software SHA-256d.  Modern Bitcoin difficulty is ~70 T.  That
 * means the time to find ANY hash below the network target is roughly
 *   2^32 / 30000 ≈ 38 hours for the LOWEST possible pool share
 *   difficulty.  Pools that set per-miner difficulty above 1 will
 *   produce shares so slowly that the connection will time out before
 *   we ever submit one.
 *
 * This is academic.  The point is: a 21-year-old, $20-used handheld
 * is doing the exact same network protocol and arithmetic that
 * modern ASICs are doing — just ~10^9 times slower.  The miner runs
 * correctly; it just doesn't earn anything.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspsdk.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputils.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>           /* close() for our socket cleanup on disconnect */

#include "sha256.h"
#include "stratum.h"
#include "target.h"
#include "config.h"

PSP_MODULE_INFO("BTC Miner", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16384);   /* 16 MB — plenty for stratum + miner state */

/* ---- pool config defaults ----
**
** Compiled-in fallbacks. Override at runtime by dropping a file at
** ms0:/PSP/SAVEDATA/btc-miner-psp/params.txt with key=value lines:
**   host=stratum.example.org
**   port=3333
**   user=bc1q...psp
**   pass=x
** Each key is independent — set only what you want to override.
**
** Example pools known to accept any hashrate:
**   stratum.solomining.io:3333     (solo mainnet)
**   solo.ckpool.org:3333           (solo mainnet)
**   stratum.testnet.bitcoin.com... (testnet, lower difficulty)
*/
#define DEFAULT_POOL_HOST    "solo.ckpool.org"
#define DEFAULT_POOL_PORT    3333
#define DEFAULT_POOL_USER    "bc1qexamplebtcaddressgoeshere.psp"
#define DEFAULT_POOL_PASS    "x"
#define DEFAULT_POOL_TLS         0      /* plain TCP by default — most public pools listen on :3333 plaintext */
#define DEFAULT_POOL_TLS_VERIFY  1      /* when TLS is on, verify cert chain + hostname against the embedded Mozilla CA bundle */

/* ---- exit callback boilerplate — required for clean Home-button exit
** back to XMB.  Without this the PSP hangs the kernel thread instead
** of returning, which on real hardware can require a hard reset. */
static int exit_callback(int arg1, int arg2, void *common) {
    (void)arg1; (void)arg2; (void)common;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("ExitCB", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static int setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread,
                                     0x11, 0xFA0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
    return thid;
}

/* ---- PSP networking init.  The order matters: load modules, init
** inet, then apctl, then connect via a saved WLAN profile. */
static int init_network(void) {
    int rc;

    pspDebugScreenPrintf("Loading net modules... ");
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (rc < 0) { pspDebugScreenPrintf("COMMON fail %08X\n", rc); return rc; }
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (rc < 0) { pspDebugScreenPrintf("INET fail %08X\n", rc); return rc; }
    pspDebugScreenPrintf("ok\n");

    pspDebugScreenPrintf("sceNetInit... ");
    rc = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (rc < 0) { pspDebugScreenPrintf("fail %08X\n", rc); return rc; }
    pspDebugScreenPrintf("ok\n");

    pspDebugScreenPrintf("sceNetInetInit... ");
    rc = sceNetInetInit();
    if (rc < 0) { pspDebugScreenPrintf("fail %08X\n", rc); return rc; }
    pspDebugScreenPrintf("ok\n");

    pspDebugScreenPrintf("sceNetApctlInit... ");
    rc = sceNetApctlInit(0x8000, 48);
    if (rc < 0) { pspDebugScreenPrintf("fail %08X\n", rc); return rc; }
    pspDebugScreenPrintf("ok\n");

    pspDebugScreenPrintf("sceNetResolverInit... ");
    rc = sceNetResolverInit();
    if (rc < 0) { pspDebugScreenPrintf("fail %08X\n", rc); return rc; }
    pspDebugScreenPrintf("ok\n");

    return 0;
}

/* Connect via WLAN profile slot 1.  PSP supports up to 10 saved
** profiles in XMB > Settings > Network Settings; profile 1 is the
** default.  The connect call blocks until apctl reports "got IP". */
static int connect_apctl(void) {
    int rc, state;
    pspDebugScreenPrintf("Connecting WLAN profile 1...\n");

    rc = sceNetApctlConnect(1);
    if (rc < 0) { pspDebugScreenPrintf("apctl_connect fail %08X\n", rc); return rc; }

    /* Poll for "got IP".  Real-world this typically completes in 3-8 s. */
    for (int i = 0; i < 60; i++) {  /* up to 30 s @ 500 ms */
        rc = sceNetApctlGetState(&state);
        if (rc < 0) { pspDebugScreenPrintf("get_state fail %08X\n", rc); return rc; }
        if (state == 4) {  /* PSP_NET_APCTL_STATE_GOT_IP */
            pspDebugScreenPrintf("connected.\n");
            return 0;
        }
        sceKernelDelayThread(500 * 1000);
    }
    pspDebugScreenPrintf("timeout waiting for IP\n");
    return -1;
}

/* Resolve a hostname.  PSP's resolver is a thin wrapper around the
** kernel's DNS module; we create + destroy a resolver per lookup
** because the API is bizarrely stateful otherwise. */
static int resolve_host(const char *host, struct in_addr *out) {
    char buf[1024];
    int rid;
    int rc = sceNetResolverCreate(&rid, buf, sizeof(buf));
    if (rc < 0) return rc;
    rc = sceNetResolverStartNtoA(rid, host, out, 2 /* timeout */, 4 /* retry */);
    sceNetResolverDelete(rid);
    return rc;
}

/* ---- Mining loop ------------------------------------------------- */
static volatile int g_should_stop = 0;
static uint64_t g_total_hashes = 0;

/* Get monotonic microseconds.  PSP RTC ticks at 1 MHz. */
static uint64_t now_us(void) {
    u64 tick;
    sceRtcGetCurrentTick(&tick);
    return (uint64_t)tick;
}

/* Mutable mining-loop state. Pulled out so set_difficulty events can
** update the target mid-sweep without restarting the loop. */
static double  g_pool_diff = 1.0;
static uint8_t g_target_be[32];

static void update_target(double diff) {
    g_pool_diff = diff;
    target_from_difficulty(diff, g_target_be);
}

/* Reasons mining_loop returns. */
#define MINING_NEW_JOB     0   /* pool sent a new mining.notify */
#define MINING_DISCONNECT  1   /* socket dropped or hard error  */

/* ---- midstate optimization ----
**
** The 80-byte Bitcoin header double-hashes as:
**   hash2 = SHA256(SHA256(header))
**
** SHA-256 processes blocks of 64 B.  The first 64 B of the header
** are constant for the duration of this job (version + prev_hash +
** merkle_root[0..27]); only bytes 64..79 (last 4 of merkle, ntime,
** nbits, nonce) change per iteration — and within those, only the
** nonce at [76..79] varies per nonce sweep.
**
** So we precompute the SHA-256 state after the first block once
** here, then per nonce:
**   1) memcpy `s1 = midstate`
**   2) patch the nonce into block2[12..15]
**   3) sha256_compress(&s1, block2)          ← block 2 of hash 1
**   4) sha256_state_to_bytes(&s1, finalblk)  ← hash 1 → input of hash 2
**   5) sha256_init(&s2); sha256_compress(&s2, finalblk) ← hash 2
**
** That's 2 compressions per nonce instead of 3.  ~1.5x speedup on
** top of whatever the inline-asm ROTR buys us.
**
** Both the real mining_loop and the offline bench_loop want the same
** initial state, so the prep work lives in a shared helper. */
typedef struct {
    sha256_state_t midstate;   /* SHA-256 state after header[0..63]   */
    uint8_t        block2[64]; /* header[64..79] + SHA-256 padding    */
    uint8_t        finalblk[64];/* hash1[0..31]   + SHA-256 padding   */
} mining_context_t;

static void prepare_mining_context(const uint8_t header[80],
                                   mining_context_t *ctx) {
    unsigned i;

    sha256_init(&ctx->midstate);
    sha256_compress(&ctx->midstate, header);

    /* block2: bytes 64..79 of header, then SHA-256 padding for an
    ** 80-byte message (0x80 marker + zeros + big-endian bit length).
    ** Length = 80 B = 640 bits = 0x0000000000000280. */
    memcpy(ctx->block2, header + 64, 16);
    ctx->block2[16] = 0x80;
    for (i = 17; i < 56; i++) ctx->block2[i] = 0;
    ctx->block2[56] = 0; ctx->block2[57] = 0;
    ctx->block2[58] = 0; ctx->block2[59] = 0;
    ctx->block2[60] = 0; ctx->block2[61] = 0;
    ctx->block2[62] = 0x02; ctx->block2[63] = 0x80;

    /* finalblk: 32 B of hash1 (patched per nonce) + SHA-256 padding
    ** for a 32-byte message.  Length = 32 B = 256 bits = 0x0000000000000100. */
    for (i = 0; i < 32; i++) ctx->finalblk[i] = 0;
    ctx->finalblk[32] = 0x80;
    for (i = 33; i < 56; i++) ctx->finalblk[i] = 0;
    ctx->finalblk[56] = 0; ctx->finalblk[57] = 0;
    ctx->finalblk[58] = 0; ctx->finalblk[59] = 0;
    ctx->finalblk[60] = 0; ctx->finalblk[61] = 0;
    ctx->finalblk[62] = 0x01; ctx->finalblk[63] = 0;
}

/* Hash one nonce against the prepared midstate context.  Writes the
** 32-byte hash2 output for the caller to compare to its target.
**
** Inlined so the per-nonce hot loop body is identical to the v0.6
** code it replaces — no extra call overhead, just a refactor for DRY
** between mining_loop and bench_loop.  ctx->finalblk is mutated as a
** scratch buffer; caller can ignore it after this returns. */
static inline void hash_one_nonce(mining_context_t *ctx, uint32_t nonce,
                                  uint8_t hash2_out[32]) {
    sha256_state_t s1, s2;

    /* Patch nonce into block2 at offset 12 (= header offset 76,
    ** minus the 64-byte first block).  Big-endian on the wire. */
    ctx->block2[12] = (uint8_t)(nonce >> 24);
    ctx->block2[13] = (uint8_t)(nonce >> 16);
    ctx->block2[14] = (uint8_t)(nonce >>  8);
    ctx->block2[15] = (uint8_t)(nonce      );

    /* Hash 1: pick up the midstate, compress the tail block. */
    s1 = ctx->midstate;
    sha256_compress(&s1, ctx->block2);
    sha256_state_to_bytes(&s1, ctx->finalblk);   /* writes finalblk[0..31] */

    /* Hash 2: full SHA-256 of the 32-byte hash1 (one block). */
    sha256_init(&s2);
    sha256_compress(&s2, ctx->finalblk);
    sha256_state_to_bytes(&s2, hash2_out);
}

static int mining_loop(int sock, const stratum_job_t *job) {
    uint8_t          header[80];
    mining_context_t ctx;
    uint8_t          hash2[32];
    uint32_t nonce;
    uint64_t hashes_this_session = 0;
    uint64_t window_start = now_us();

    /* Build initial header from the stratum job.  The miner's nonce
    ** sweep updates header[76..79] every iteration; everything else
    ** is fixed for the duration of this job. */
    stratum_build_header(job, header);
    prepare_mining_context(header, &ctx);

    pspDebugScreenSetXY(0, 8);
    pspDebugScreenPrintf("Mining job %s  pool_diff=%.4g\n",
                         job->job_id, g_pool_diff);
    pspDebugScreenPrintf("ntime=%08lX nbits=%08lX\n",
                         (unsigned long)job->ntime, (unsigned long)job->nbits);

    for (nonce = 0; !g_should_stop; nonce++) {
        hash_one_nonce(&ctx, nonce, hash2);

        hashes_this_session++;
        g_total_hashes++;

        /* Full 256-bit target comparison. Bitcoin convention: hash is
        ** interpreted as little-endian uint256 (byte 31 is MSB), target
        ** as big-endian (byte 0 is MSB). hash_meets_target handles
        ** the byte-swap and the lexicographic compare. */
        if (hash_meets_target(hash2, g_target_be)) {
            /* Found a share — submit. At PSP hashrate this is
            ** astronomically rare even at pool diff = 0.0001, but the
            ** submit path runs regardless. */
            pspDebugScreenPrintf("\n*** SHARE found nonce=%08lX (diff=%.4g) ***\n",
                                 (unsigned long)nonce, g_pool_diff);
            stratum_submit_share(sock, job, nonce);
            /* Keep mining the same job — pool will notify a new one
            ** if our share advanced the chain (it won't). */
        }

        /* Print stats every 16k hashes (≈ once per second at 30 kH/s). */
        if ((nonce & 0x3FFF) == 0) {
            uint64_t now = now_us();
            uint64_t elapsed_us = now - window_start;
            if (elapsed_us > 0) {
                uint64_t rate_h_per_s = (hashes_this_session * 1000000ULL) / elapsed_us;
                pspDebugScreenSetXY(0, 12);
                pspDebugScreenPrintf("Hashrate: %6llu H/s    \n", rate_h_per_s);
                pspDebugScreenPrintf("Total:    %10llu hashes\n", g_total_hashes);
                pspDebugScreenPrintf("Last nce: %08lX         \n",
                                     (unsigned long)nonce);
            }
        }

        /* Check for stratum events every 16k hashes (cheap, non-blocking).
        ** Four cases:
        **   NEW_JOB        — bail; outer loop swaps in the new job and
        **                    re-enters mining_loop.
        **   SET_DIFF       — update the target in place; keep mining.
        **   SET_EXTRANONCE — log; the next NEW_JOB will pick up the
        **                    new extranonce values automatically.
        **   socket dead    — checked via stratum_socket_alive() in the
        **                    same hot path (so a silently-dropped TCP
        **                    connection is caught within seconds). */
        if ((nonce & 0xFFFF) == 0xFFFF) {
            int alive = stratum_socket_alive(sock);
            if (alive <= 0) {
                pspDebugScreenPrintf("\nSocket %s.\n",
                                     alive == 0 ? "closed by peer"
                                                : "error");
                return MINING_DISCONNECT;
            }
            stratum_event_t ev;
            while (stratum_poll_nonblock(sock, &ev) > 0) {
                if (ev.kind == STRATUM_EV_NEW_JOB) {
                    pspDebugScreenPrintf("\nNew job %s, switching...\n",
                                         ev.job.job_id);
                    return MINING_NEW_JOB;
                }
                if (ev.kind == STRATUM_EV_SET_DIFF) {
                    update_target(ev.difficulty);
                    pspDebugScreenSetXY(0, 8);
                    pspDebugScreenPrintf("Mining job %s  pool_diff=%.4g  \n",
                                         job->job_id, g_pool_diff);
                }
                if (ev.kind == STRATUM_EV_SET_EXTRANONCE) {
                    pspDebugScreenPrintf(
                        "\nset_extranonce e1_len=%d e2_size=%d "
                        "(applies on next job)\n",
                        ev.extranonce1_len, ev.extranonce2_size);
                }
                if (ev.kind == STRATUM_EV_NONE) break;
            }
        }
    }
    return MINING_NEW_JOB;   /* g_should_stop path: outer loop exits */
}

/* ---- bench mode (since v0.7) ---------------------------------------
**
** Standalone hashrate benchmark.  No network, no pool, no real header
** — just sweeps nonces against a synthetic 80-byte block (deterministic
** so two runs are comparable) and shows the live H/s on screen.
**
** The point: anyone with the EBOOT can measure their PSP's hashrate
** under different build versions (v0.6 vs v0.7 vs whatever's next)
** without standing up a pool connection.  Enable via `bench=yes` in
** params.txt; the miner skips connect_apctl / init_network entirely.
**
** Implementation note: this calls the exact same hash_one_nonce() that
** mining_loop uses, against a mining_context_t prepared the same way,
** so the measured rate is the rate the real miner gets.  Only
** difference: no socket alive-check, no stratum poll, no share submit
** path — purely the SHA-256d hot loop.  Stats display matches
** mining_loop's so the comparison is one-to-one. */
static void bench_loop(void) {
    /* Synthetic 80-byte header.  Bytes are arbitrary but fixed so the
    ** benchmark is repeatable; the contents don't change the
    ** SHA-256d cycle cost (which is data-independent).  Using a pattern
    ** based on the Bitcoin genesis-block header bytes 0..75, with a
    ** zero nonce that gets overwritten per iteration. */
    static const uint8_t synthetic_header[80] = {
        0x01,0x00,0x00,0x00,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0x3B,0xA3,0xED,0xFD, 0x7A,0x7B,0x12,0xB2, 0x7A,0xC7,0x2C,0x3E,
        0x67,0x76,0x8F,0x61, 0x7F,0xC8,0x1B,0xC3, 0x88,0x8A,0x51,0x32,
        0x3A,0x9F,0xB8,0xAA, 0x4B,0x1E,0x5E,0x4A,
        0x29,0xAB,0x5F,0x49,
        0xFF,0xFF,0x00,0x1D,
        0,0,0,0
    };

    mining_context_t ctx;
    uint8_t          hash2[32];
    uint32_t nonce;
    uint64_t hashes_this_session = 0;
    uint64_t window_start = now_us();
    /* `acc` keeps the hash output observable so the optimizer can't
    ** delete the inner work as dead code.  Updated cheaply, displayed
    ** at the end of each window. */
    volatile uint8_t acc = 0;

    prepare_mining_context(synthetic_header, &ctx);

    pspDebugScreenSetXY(0, 8);
    pspDebugScreenPrintf("BENCH mode  (no network, no pool)\n");
    pspDebugScreenPrintf("synthetic 80-byte header, double-SHA256 sweep\n\n");

    for (nonce = 0; !g_should_stop; nonce++) {
        hash_one_nonce(&ctx, nonce, hash2);
        acc ^= hash2[31];
        hashes_this_session++;
        g_total_hashes++;

        /* Print stats every 16k hashes (≈ once per second at 30 kH/s).
        ** Same cadence as mining_loop so the numbers compare apples-to-
        ** apples. */
        if ((nonce & 0x3FFF) == 0) {
            uint64_t now = now_us();
            uint64_t elapsed_us = now - window_start;
            if (elapsed_us > 0) {
                uint64_t rate_h_per_s = (hashes_this_session * 1000000ULL) / elapsed_us;
                pspDebugScreenSetXY(0, 12);
                pspDebugScreenPrintf("Hashrate: %6llu H/s    \n", rate_h_per_s);
                pspDebugScreenPrintf("Total:    %10llu hashes\n", g_total_hashes);
                pspDebugScreenPrintf("Last nce: %08lX         \n",
                                     (unsigned long)nonce);
                pspDebugScreenPrintf("acc:      %02X (anti-DCE) \n",
                                     (unsigned)acc);
            }
        }
    }
}

/* Build defaults into cfg, then layer in any keys from params.txt. */
static void load_config(miner_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->host, DEFAULT_POOL_HOST, sizeof(cfg->host) - 1);
    cfg->port = DEFAULT_POOL_PORT;
    strncpy(cfg->user, DEFAULT_POOL_USER, sizeof(cfg->user) - 1);
    strncpy(cfg->pass, DEFAULT_POOL_PASS, sizeof(cfg->pass) - 1);
    cfg->use_tls    = DEFAULT_POOL_TLS;
    cfg->tls_verify = DEFAULT_POOL_TLS_VERIFY;

    int rc = config_load(cfg);
    if (rc == 0) {
        pspDebugScreenPrintf("config: no params.txt, using compiled defaults\n");
    } else if (rc < 0) {
        pspDebugScreenPrintf("config: params.txt parse warning (using partial)\n");
    } else {
        pspDebugScreenPrintf("config: loaded from params.txt:%s%s%s%s%s%s%s\n",
            (cfg->loaded_mask & 1)  ? " host"       : "",
            (cfg->loaded_mask & 2)  ? " port"       : "",
            (cfg->loaded_mask & 4)  ? " user"       : "",
            (cfg->loaded_mask & 8)  ? " pass"       : "",
            (cfg->loaded_mask & 16) ? " tls"        : "",
            (cfg->loaded_mask & 32) ? " tls_verify" : "",
            (cfg->loaded_mask & 64) ? " bench"      : "");
    }
    if (cfg->bench) {
        pspDebugScreenPrintf("  ** BENCH MODE ** (network skipped)\n");
    } else {
        pspDebugScreenPrintf("  pool: %s%s:%u user=%s",
                             cfg->use_tls ? "TLS:" : "",
                             cfg->host, (unsigned)cfg->port, cfg->user);
        if (cfg->use_tls) {
            pspDebugScreenPrintf("  verify=%s",
                                 cfg->tls_verify ? "REQUIRED" : "NONE");
        }
        pspDebugScreenPrintf("\n");
    }
    pspDebugScreenPrintf("\n");
}

/* Resolve + stratum-connect + wait-first-job. Returns the socket fd
** ready for mining, or negative on failure. Side effect: fills `job`
** and updates the global target via `update_target`. */
static int connect_and_subscribe(const miner_config_t *cfg, stratum_job_t *job) {
    struct in_addr pool_ip;
    pspDebugScreenPrintf("  resolving %s... ", cfg->host);
    if (resolve_host(cfg->host, &pool_ip) < 0) {
        pspDebugScreenPrintf("FAIL\n");
        return -1;
    }
    pspDebugScreenPrintf("%s\n", inet_ntoa(pool_ip));

    int sock = stratum_connect(pool_ip.s_addr, cfg->port,
                               cfg->user, cfg->pass,
                               cfg->use_tls, cfg->tls_verify,
                               cfg->host);
    if (sock < 0) {
        pspDebugScreenPrintf("  stratum_connect failed: %d\n", sock);
        return -2;
    }
    pspDebugScreenPrintf("  subscribed + authorized.\n");

    double diff = 1.0;
    if (stratum_wait_first_job(sock, job, &diff) < 0) {
        pspDebugScreenPrintf("  wait_first_job failed.\n");
        stratum_disconnect(sock);
        return -3;
    }
    update_target(diff);
    return sock;
}

/* ---- main -------------------------------------------------------- */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    pspDebugScreenInit();
    setup_callbacks();

    pspDebugScreenPrintf("btc-miner-psp v0.7\n");
    pspDebugScreenPrintf("PSP 333 MHz MIPS R4000, software SHA-256d (midstate + ROTR + unroll)\n\n");

    if (sha256_selftest() != 0) {
        pspDebugScreenPrintf("FATAL: SHA-256 self-test failed\n");
        sceKernelSleepThread();
    }
    pspDebugScreenPrintf("SHA-256 self-test passed\n\n");

    miner_config_t cfg;
    load_config(&cfg);

    /* Bench mode: skip every byte of network/stratum work and just
    ** loop on the SHA-256d hot path against a synthetic header so the
    ** user can read the live H/s display.  Useful for measuring build
    ** version deltas without a pool connection.  Set `bench=yes` in
    ** params.txt to enable. */
    if (cfg.bench) {
        bench_loop();
        sceKernelSleepThread();   /* g_should_stop path: Home to exit */
    }

    if (init_network() < 0) {
        pspDebugScreenPrintf("\nNetwork init failed. Press Home to exit.\n");
        sceKernelSleepThread();
    }
    if (connect_apctl() < 0) {
        pspDebugScreenPrintf("\nWLAN connect failed. Press Home to exit.\n");
        sceKernelSleepThread();
    }

    /* ---- reconnect loop -------------------------------------------
    ** Exponential backoff on every failure (resolve / connect / job
    ** wait / mid-mining disconnect). Cap at 60 s between attempts;
    ** retry forever — the user exits via Home if they want out.
    ** Backoff resets to 1 s after any session that produced at least
    ** one job (i.e. we successfully started mining). */
    uint32_t backoff_ms = 1000;
    uint32_t reconnect_count = 0;
    for (;;) {
        if (reconnect_count > 0) {
            pspDebugScreenPrintf("\n--- reconnect #%u (backoff %u ms) ---\n",
                                 (unsigned)reconnect_count,
                                 (unsigned)backoff_ms);
            sceKernelDelayThread(backoff_ms * 1000);
            backoff_ms = backoff_ms * 2;
            if (backoff_ms > 60000) backoff_ms = 60000;
        }
        reconnect_count++;

        stratum_job_t job;
        int sock = connect_and_subscribe(&cfg, &job);
        if (sock < 0) continue;          /* transient — backoff + retry */

        /* Session started successfully — reset backoff for the next
        ** disconnect. */
        backoff_ms = 1000;

        /* Inner job loop: mine current job, then wait for the next.
        ** Either step can return "disconnected", which breaks back to
        ** the outer reconnect loop. */
        int reason;
        do {
            reason = mining_loop(sock, &job);
            if (reason == MINING_DISCONNECT) break;
            /* mining_loop says NEW_JOB; wait_first_job picks it up
            ** (skipping any set_difficulty/set_extranonce in between). */
            double diff = g_pool_diff;
            if (stratum_wait_first_job(sock, &job, &diff) < 0) {
                pspDebugScreenPrintf(
                    "\nLost connection while waiting for next job.\n");
                reason = MINING_DISCONNECT;
                break;
            }
            update_target(diff);
        } while (reason == MINING_NEW_JOB);

        stratum_disconnect(sock);
        /* outer loop continues */
    }

    sceKernelSleepThread();
    return 0;
}
