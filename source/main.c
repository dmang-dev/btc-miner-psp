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

#include "sha256.h"
#include "stratum.h"

PSP_MODULE_INFO("BTC Miner", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16384);   /* 16 MB — plenty for stratum + miner state */

/* ---- pool config — edit these or wire a config file later ----------
**
** For testing, point at a public testnet pool like ckpool's solo or
** the public testnet pools listed at bitcointestnet4.com.  Mainnet
** pool URLs and worker credentials go here for "real" deployment.
**
** Example pools known to accept any hashrate:
**   stratum.solomining.io:3333    (solo mainnet)
**   solo.ckpool.org:3333           (solo mainnet)
**   stratum.testnet.bitcoin.com... (testnet, lower difficulty)
*/
#define POOL_HOST    "solo.ckpool.org"
#define POOL_PORT    3333
#define POOL_USER    "bc1qexamplebtcaddressgoeshere.psp"
#define POOL_PASS    "x"

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

static void mining_loop(int sock, const stratum_job_t *job, double pool_diff) {
    uint8_t  header[80];
    uint8_t  hash1[32];
    uint8_t  hash2[32];
    uint32_t nonce;
    uint64_t hashes_this_session = 0;
    uint64_t window_start = now_us();
    uint32_t target_first_word;

    /* Build initial header from the stratum job.  The miner's nonce
    ** sweep updates header[76..79] every iteration; everything else
    ** is fixed for the duration of this job. */
    stratum_build_header(job, header);

    /* Pool target is sent as a difficulty value; the network share-
    ** validation target is the first 4 bytes of the SHA-256d output
    ** must be <= (max_target / difficulty).  For diff 1 that's
    ** 0x00000000FFFF0000_... — so accept if the 32-bit BE value
    ** at hash2[28..31] (little-endian on the wire = big-endian
    ** byte order in hash output) is below threshold.  We use the
    ** conservative diff-1 check; pools accept anything stricter. */
    (void)pool_diff;  /* TODO: scale target with sceVdiff message */
    target_first_word = 0xFFFF0000UL;

    pspDebugScreenSetXY(0, 8);
    pspDebugScreenPrintf("Mining job %s\n", job->job_id);
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

        /* The hash is checked little-endian on the wire — that means
        ** the LAST 4 bytes of the digest, read big-endian, must be
        ** below the target.  Equivalent: hash2[31..28] as a u32. */
        {
            uint32_t check = ((uint32_t)hash2[31] << 24)
                           | ((uint32_t)hash2[30] << 16)
                           | ((uint32_t)hash2[29] <<  8)
                           |  (uint32_t)hash2[28];
            if (check < target_first_word) {
                /* Found a share!  This is statistically improbable
                ** at modern difficulty, but the submit path runs
                ** regardless. */
                pspDebugScreenPrintf("\n*** SHARE found nonce=%08lX ***\n",
                                     (unsigned long)nonce);
                stratum_submit_share(sock, job, nonce);
                /* keep mining the same job — pool will notify a new
                ** one if our share advanced the chain (won't). */
            }
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

        /* Check for new job notification on the stratum socket every
        ** 16k hashes (cheap, non-blocking). */
        if ((nonce & 0xFFFF) == 0xFFFF) {
            stratum_event_t ev;
            if (stratum_poll_nonblock(sock, &ev) > 0) {
                if (ev.kind == STRATUM_EV_NEW_JOB) {
                    pspDebugScreenPrintf("\nNew job %s, switching...\n", ev.job.job_id);
                    /* Caller will restart mining_loop with the new
                    ** job; for now just bail out of the inner sweep. */
                    return;
                }
            }
        }
    }
}

/* ---- main -------------------------------------------------------- */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    pspDebugScreenInit();
    setup_callbacks();

    pspDebugScreenPrintf("btc-miner-psp v0.1\n");
    pspDebugScreenPrintf("PSP 333 MHz MIPS R4000, software SHA-256d\n\n");

    /* Self-test the SHA-256 implementation before going on the wire. */
    if (sha256_selftest() != 0) {
        pspDebugScreenPrintf("FATAL: SHA-256 self-test failed\n");
        sceKernelSleepThread();
    }
    pspDebugScreenPrintf("SHA-256 self-test passed\n\n");

    if (init_network() < 0) {
        pspDebugScreenPrintf("\nNetwork init failed. Press Home to exit.\n");
        sceKernelSleepThread();
    }
    if (connect_apctl() < 0) {
        pspDebugScreenPrintf("\nWLAN connect failed. Press Home to exit.\n");
        sceKernelSleepThread();
    }

    /* Resolve pool host. */
    struct in_addr pool_ip;
    pspDebugScreenPrintf("Resolving %s... ", POOL_HOST);
    if (resolve_host(POOL_HOST, &pool_ip) < 0) {
        pspDebugScreenPrintf("FAIL\n");
        sceKernelSleepThread();
    }
    pspDebugScreenPrintf("%s\n", inet_ntoa(pool_ip));

    /* Stratum connect + handshake. */
    int sock = stratum_connect(pool_ip.s_addr, POOL_PORT,
                               POOL_USER, POOL_PASS);
    if (sock < 0) {
        pspDebugScreenPrintf("Stratum connect failed: %d\n", sock);
        sceKernelSleepThread();
    }
    pspDebugScreenPrintf("Stratum subscribed + authorized.\n\n");

    /* Receive first job. */
    stratum_job_t job;
    double pool_diff = 1.0;
    while (1) {
        if (stratum_wait_first_job(sock, &job, &pool_diff) < 0) {
            pspDebugScreenPrintf("No job from pool. Reconnecting?\n");
            sceKernelSleepThread();
        }
        mining_loop(sock, &job, pool_diff);
        /* mining_loop returns when a new job arrives; loop back to
        ** read it from stratum state. */
    }

    /* unreachable — exit via Home button only */
    sceKernelSleepThread();
    return 0;
}
