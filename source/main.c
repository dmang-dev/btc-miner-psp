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

static int mining_loop(int sock, const stratum_job_t *job) {
    uint8_t  header[80];
    uint8_t  hash1[32];
    uint8_t  hash2[32];
    uint32_t nonce;
    uint64_t hashes_this_session = 0;
    uint64_t window_start = now_us();

    /* Build initial header from the stratum job.  The miner's nonce
    ** sweep updates header[76..79] every iteration; everything else
    ** is fixed for the duration of this job. */
    stratum_build_header(job, header);

    pspDebugScreenSetXY(0, 8);
    pspDebugScreenPrintf("Mining job %s  pool_diff=%.4g\n",
                         job->job_id, g_pool_diff);
    pspDebugScreenPrintf("ntime=%08lX nbits=%08lX\n",
                         (unsigned long)job->ntime, (unsigned long)job->nbits);

    for (nonce = 0; !g_should_stop; nonce++) {
        /* Patch nonce (last 4 bytes, big-endian). */
        header[76] = (uint8_t)(nonce >> 24);
        header[77] = (uint8_t)(nonce >> 16);
        header[78] = (uint8_t)(nonce >>  8);
        header[79] = (uint8_t)(nonce      );

        /* Bitcoin double-SHA256: SHA256(SHA256(header)). */
        sha256(header, 80, hash1);
        sha256(hash1, 32, hash2);

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
        pspDebugScreenPrintf("config: loaded from params.txt:%s%s%s%s%s%s\n",
            (cfg->loaded_mask & 1)  ? " host"       : "",
            (cfg->loaded_mask & 2)  ? " port"       : "",
            (cfg->loaded_mask & 4)  ? " user"       : "",
            (cfg->loaded_mask & 8)  ? " pass"       : "",
            (cfg->loaded_mask & 16) ? " tls"        : "",
            (cfg->loaded_mask & 32) ? " tls_verify" : "");
    }
    pspDebugScreenPrintf("  pool: %s%s:%u user=%s",
                         cfg->use_tls ? "TLS:" : "",
                         cfg->host, (unsigned)cfg->port, cfg->user);
    if (cfg->use_tls) {
        pspDebugScreenPrintf("  verify=%s",
                             cfg->tls_verify ? "REQUIRED" : "NONE");
    }
    pspDebugScreenPrintf("\n\n");
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

    pspDebugScreenPrintf("btc-miner-psp v0.5\n");
    pspDebugScreenPrintf("PSP 333 MHz MIPS R4000, software SHA-256d\n\n");

    if (sha256_selftest() != 0) {
        pspDebugScreenPrintf("FATAL: SHA-256 self-test failed\n");
        sceKernelSleepThread();
    }
    pspDebugScreenPrintf("SHA-256 self-test passed\n\n");

    miner_config_t cfg;
    load_config(&cfg);

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
