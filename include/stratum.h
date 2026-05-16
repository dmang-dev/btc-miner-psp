/*
 * stratum.h — minimal Stratum-v1 mining client for PSP.
 *
 * Speaks the JSON-RPC line protocol over plain TCP (no TLS).  Supports
 * just enough of the protocol to subscribe, authorize, receive the
 * first mining.notify, and submit shares.  No variable-difficulty
 * support yet (mining.set_difficulty silently uses pool's value as
 * informational only — share validation always uses diff-1 target,
 * which is conservative).
 */
#ifndef BTCM_STRATUM_H
#define BTCM_STRATUM_H

#include <stdint.h>

/* Maximum length of a single stratum job's variable fields.  These
** are bigger than they need to be — Bitcoin coinbase is typically
** 100-200 bytes, prev_hash is always 64 hex chars, merkle branch is
** rarely >10 levels deep. */
#define STRATUM_MAX_COINB1    256
#define STRATUM_MAX_COINB2    256
#define STRATUM_MAX_MERKLE_BRANCH  16
#define STRATUM_MAX_JOB_ID     32
#define STRATUM_MAX_EXTRANONCE 16

typedef struct {
    char     job_id[STRATUM_MAX_JOB_ID];
    uint8_t  prev_hash[32];           /* big-endian on wire, swapped to
                                       ** display order in build_header */
    uint8_t  coinb1[STRATUM_MAX_COINB1];
    int      coinb1_len;
    uint8_t  coinb2[STRATUM_MAX_COINB2];
    int      coinb2_len;
    uint8_t  merkle_branch[STRATUM_MAX_MERKLE_BRANCH][32];
    int      merkle_branch_count;
    uint32_t version;                  /* block version */
    uint32_t nbits;                    /* difficulty target compact form */
    uint32_t ntime;                    /* block timestamp */
    int      clean_jobs;
    /* Subscription state — copied into each job for header building. */
    uint8_t  extranonce1[STRATUM_MAX_EXTRANONCE];
    int      extranonce1_len;
    int      extranonce2_size;
    uint32_t extranonce2;              /* incremented per share */
} stratum_job_t;

typedef enum {
    STRATUM_EV_NEW_JOB,
    STRATUM_EV_SET_DIFF,
    STRATUM_EV_SET_EXTRANONCE,
    STRATUM_EV_NONE
} stratum_event_kind_t;

typedef struct {
    stratum_event_kind_t kind;
    stratum_job_t        job;          /* valid when kind == NEW_JOB         */
    double               difficulty;   /* valid when kind == SET_DIFF        */
    /* Updated subscription state — valid when kind == SET_EXTRANONCE. The
    ** next mining.notify will produce a job pre-populated with these
    ** values via the parse_notify path, so callers don't need to do
    ** anything with the event beyond logging it. */
    uint8_t  extranonce1[STRATUM_MAX_EXTRANONCE];
    int      extranonce1_len;
    int      extranonce2_size;
} stratum_event_t;

/* Open socket to pool, optionally wrap in TLS, send mining.subscribe
** + mining.authorize, parse subscription response into a stratum_state
** singleton. Returns the socket fd on success, negative on failure.
**
** `use_tls` non-zero turns on mbedtls TLS 1.2 handshake before any
** stratum bytes flow. `hostname` is sent as SNI and used for
** certificate-CN matching (currently informational — verification is
** soft per the stratum_set_tls_verify control; see README).
**
** Pass `hostname = NULL` for plain TCP. */
int stratum_connect(uint32_t pool_ip_be, uint16_t pool_port,
                    const char *user, const char *pass,
                    int use_tls, const char *hostname);

/* Cleanup TLS state (if active) and close the underlying socket.
** Caller's previous habit of bare close(sock) on the fd still works
** for plain TCP but skips the mbedtls teardown for TLS sessions. */
void stratum_disconnect(int sock);

/* Block until pool sends the first mining.notify, fill `job` with it.
** `*pool_diff` is updated when set_difficulty arrives (default 1.0).
** Returns 0 on success, negative on socket error / parse error. */
int stratum_wait_first_job(int sock, stratum_job_t *job, double *pool_diff);

/* Non-blocking poll: read at most one stratum line, parse it, fill
** `ev`.  Returns 0 if no data ready, >0 if event filled, <0 on error. */
int stratum_poll_nonblock(int sock, stratum_event_t *ev);

/* Non-blocking liveness check: returns 1 if the socket is still up
** (whether or not data is ready), 0 if the peer closed it cleanly
** (EOF), -1 on hard socket error. mining_loop calls this every ~16k
** iterations so a silently-dropped TCP connection during the inner
** sweep is noticed within seconds rather than waiting forever for the
** next mining.notify that will never come. */
int stratum_socket_alive(int sock);

/* Build the 80-byte block header from the current job + the next
** extranonce2 value.  Increments job->extranonce2 internally so a
** subsequent call mines the next nonce-space slice (~4 billion
** hashes per slice). */
void stratum_build_header(const stratum_job_t *job, uint8_t header_out[80]);

/* Submit a found share back to the pool. */
int stratum_submit_share(int sock, const stratum_job_t *job, uint32_t nonce);

#endif
