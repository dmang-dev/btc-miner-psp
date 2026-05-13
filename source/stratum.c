/*
 * stratum.c — minimal Stratum-v1 client.  Speaks JSON-RPC line protocol
 * over plain TCP with a tiny ad-hoc parser (no cJSON dependency to
 * keep psp-gcc link clean for v0.1).
 *
 * Protocol reference: https://en.bitcoin.it/wiki/Stratum_mining_protocol
 *
 * Implementation notes:
 *  - One line per RPC message, '\n' terminated.
 *  - We only parse the response fields we actually need.  The mini
 *    JSON parser handles strings, ints, and nested arrays at the top
 *    levels needed for mining.notify / mining.subscribe; it does
 *    NOT handle escape sequences, Unicode, or fractional doubles.
 *    Pools don't put those in mining messages anyway.
 *  - Buffered receive — we hold a per-connection rx buffer and pull
 *    one line at a time from it.  A single recv() can deliver
 *    multiple RPC frames or split one frame across calls.
 */
#include "stratum.h"
#include "sha256.h"

#include <pspdebug.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define RX_BUF_SIZE 8192

/* Single-connection state — we mine to one pool at a time. */
static struct {
    int      authorized;
    int      msg_id;
    char     rx_buf[RX_BUF_SIZE];
    int      rx_len;
    uint8_t  extranonce1[STRATUM_MAX_EXTRANONCE];
    int      extranonce1_len;
    int      extranonce2_size;
    stratum_job_t pending_job;
    int      pending_job_valid;
    double   current_diff;
} S = { 0, 0, "", 0, {0}, 0, 0, {{0}}, 0, 1.0 };

/* ---- hex utilities ---------------------------------------------- */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hexdecode(const char *s, int s_len, uint8_t *out, int out_max) {
    if (s_len & 1) return -1;
    int n = s_len / 2;
    if (n > out_max) return -1;
    for (int i = 0; i < n; i++) {
        int hi = hexval(s[i*2]);
        int lo = hexval(s[i*2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static void hexencode(const uint8_t *in, int n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i*2]     = H[(in[i] >> 4) & 0xF];
        out[i*2 + 1] = H[in[i] & 0xF];
    }
    out[n*2] = 0;
}

/* ---- mini JSON parsing — find a field by key, return ptr to value
** (just past the colon, skipping whitespace).  Doesn't handle
** quoted-key escapes; doesn't care because pool message keys are
** all plain ASCII. */
static const char* json_find_key(const char *json, const char *key) {
    char needle[64];
    int klen = (int)strlen(key);
    if (klen + 3 >= (int)sizeof(needle)) return NULL;
    needle[0] = '"';
    memcpy(needle + 1, key, klen);
    needle[klen + 1] = '"';
    needle[klen + 2] = 0;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += klen + 2;
    while (*p == ' ' || *p == ':') p++;
    return p;
}

/* Note: this v0.1 file uses an inline string-parsing pattern in
** parse_notify rather than json_get_* helpers; the helpers are
** kept (commented out) as a guide if a future version moves to a
** more general parser.
**
** static int json_get_string(...)  -- by-key string extractor
** static int json_get_int   (...)  -- by-key int extractor
*/

/* ---- TCP line I/O ------------------------------------------------ */

static int send_line(int sock, const char *buf) {
    int len = (int)strlen(buf);
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Read until '\n', store line in `out` (null-terminated, '\n' stripped).
** Returns line length on success, 0 on EOF, -1 on error.  Blocking. */
static int recv_line(int sock, char *out, int out_max) {
    while (1) {
        /* Look for '\n' in the buffer. */
        for (int i = 0; i < S.rx_len; i++) {
            if (S.rx_buf[i] == '\n') {
                int line_len = i;
                if (line_len > out_max - 1) line_len = out_max - 1;
                memcpy(out, S.rx_buf, line_len);
                out[line_len] = 0;
                /* Shift remaining bytes. */
                int rest = S.rx_len - (i + 1);
                memmove(S.rx_buf, S.rx_buf + i + 1, rest);
                S.rx_len = rest;
                return line_len;
            }
        }
        /* No newline yet — read more. */
        if (S.rx_len >= RX_BUF_SIZE - 1) return -1;  /* line too long */
        int n = recv(sock, S.rx_buf + S.rx_len, RX_BUF_SIZE - S.rx_len, 0);
        if (n <= 0) return n;
        S.rx_len += n;
    }
}

/* Non-blocking peek: return 1 if a complete line is buffered, 0 if not.
** We can't truly cheap-poll without making the socket non-blocking,
** which would complicate other code paths.  For v0.1 the polling
** path just checks the buffer; on next blocking recv it'll catch up. */
static int line_available(void) {
    for (int i = 0; i < S.rx_len; i++) {
        if (S.rx_buf[i] == '\n') return 1;
    }
    return 0;
}

/* ---- subscribe / authorize -------------------------------------- */

int stratum_connect(uint32_t pool_ip_be, uint16_t pool_port,
                    const char *user, const char *pass) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = pool_ip_be;
    addr.sin_port = htons(pool_port);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -2;
    }

    /* mining.subscribe */
    char msg[256];
    snprintf(msg, sizeof(msg),
        "{\"id\":%d,\"method\":\"mining.subscribe\",\"params\":[\"btc-miner-psp/0.1\"]}\n",
        ++S.msg_id);
    if (send_line(sock, msg) < 0) { close(sock); return -3; }

    /* Read the subscribe response.  Format:
    ** {"id":1,"result":[[["mining.set_difficulty","..."],
    **                    ["mining.notify","..."]],
    **                   "<extranonce1_hex>","<extranonce2_size_int>"],
    **  "error":null} */
    char line[2048];
    int n = recv_line(sock, line, sizeof(line));
    if (n <= 0) { close(sock); return -4; }
    pspDebugScreenPrintf("  subscribe rsp: %.60s%s\n", line, n > 60 ? "..." : "");

    /* Find extranonce1 — it's the second-to-last array element in
    ** "result".  Cheap hack: grep for the last quoted hex string
    ** before the closing ]. */
    const char *result_p = strstr(line, "\"result\":[");
    if (!result_p) { close(sock); return -5; }
    /* Walk to the inner ']' that closes the subscription list, then
    ** the next two elements are extranonce1 (string) and
    ** extranonce2_size (int). */
    const char *p = result_p;
    int depth = 0;
    int found_inner_close = 0;
    while (*p) {
        if (*p == '[') depth++;
        else if (*p == ']') {
            depth--;
            if (depth == 1) { found_inner_close = 1; p++; break; }
        }
        p++;
    }
    if (!found_inner_close) { close(sock); return -6; }
    /* skip comma + whitespace */
    while (*p == ',' || *p == ' ') p++;
    /* now p points to "extranonce1" (a quoted hex string) */
    if (*p != '"') { close(sock); return -7; }
    p++;
    const char *e1_start = p;
    while (*p && *p != '"') p++;
    int e1_hex_len = (int)(p - e1_start);
    S.extranonce1_len = hexdecode(e1_start, e1_hex_len,
                                  S.extranonce1, STRATUM_MAX_EXTRANONCE);
    if (S.extranonce1_len < 0) { close(sock); return -8; }
    p++;  /* skip closing " */
    while (*p == ',' || *p == ' ') p++;
    /* now extranonce2_size as int */
    S.extranonce2_size = (int)strtol(p, NULL, 10);
    pspDebugScreenPrintf("  e1_len=%d e2_size=%d\n",
                         S.extranonce1_len, S.extranonce2_size);

    /* mining.authorize */
    snprintf(msg, sizeof(msg),
        "{\"id\":%d,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"%s\"]}\n",
        ++S.msg_id, user, pass);
    if (send_line(sock, msg) < 0) { close(sock); return -9; }

    n = recv_line(sock, line, sizeof(line));
    if (n <= 0) { close(sock); return -10; }
    pspDebugScreenPrintf("  authorize rsp: %.60s\n", line);
    if (!strstr(line, "\"result\":true")) {
        pspDebugScreenPrintf("  authorize REJECTED\n");
        close(sock);
        return -11;
    }
    S.authorized = 1;
    return sock;
}

/* ---- job handling ----------------------------------------------- */

/* Parse a mining.notify line into `job`.  Format:
** {"id":null,"method":"mining.notify","params":[
**   "<job_id>","<prevhash>","<coinb1>","<coinb2>",
**   ["<merkle_branch_1>","<merkle_branch_2>",...],
**   "<version>","<nbits>","<ntime>",<clean_jobs>]}
*/
static int parse_notify(const char *line, stratum_job_t *job) {
    const char *p = strstr(line, "\"params\":[");
    if (!p) return -1;
    p += strlen("\"params\":[");

    /* helper: read next quoted string into buf, advance p past it */
    #define READ_QSTR(BUF, BUFLEN) do { \
        while (*p == ' ' || *p == ',') p++; \
        if (*p != '"') return -1; \
        p++; \
        int _n = 0; \
        while (*p && *p != '"' && _n < (BUFLEN) - 1) { (BUF)[_n++] = *p++; } \
        if (*p != '"') return -1; \
        (BUF)[_n] = 0; p++; \
    } while (0)

    char job_id_buf[STRATUM_MAX_JOB_ID];
    char prevhash_hex[65];
    char coinb1_hex[STRATUM_MAX_COINB1 * 2 + 1];
    char coinb2_hex[STRATUM_MAX_COINB2 * 2 + 1];

    READ_QSTR(job_id_buf, sizeof(job_id_buf));
    READ_QSTR(prevhash_hex, sizeof(prevhash_hex));
    READ_QSTR(coinb1_hex, sizeof(coinb1_hex));
    READ_QSTR(coinb2_hex, sizeof(coinb2_hex));

    strncpy(job->job_id, job_id_buf, STRATUM_MAX_JOB_ID - 1);
    job->job_id[STRATUM_MAX_JOB_ID - 1] = 0;
    if (hexdecode(prevhash_hex, (int)strlen(prevhash_hex),
                  job->prev_hash, 32) != 32) return -2;
    job->coinb1_len = hexdecode(coinb1_hex, (int)strlen(coinb1_hex),
                                job->coinb1, STRATUM_MAX_COINB1);
    job->coinb2_len = hexdecode(coinb2_hex, (int)strlen(coinb2_hex),
                                job->coinb2, STRATUM_MAX_COINB2);
    if (job->coinb1_len < 0 || job->coinb2_len < 0) return -3;

    /* Merkle branch array. */
    while (*p == ' ' || *p == ',') p++;
    if (*p != '[') return -4;
    p++;
    job->merkle_branch_count = 0;
    while (*p && *p != ']') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') return -5;
        p++;
        char hex64[65];
        int n = 0;
        while (*p && *p != '"' && n < 64) { hex64[n++] = *p++; }
        hex64[n] = 0;
        if (*p != '"') return -6;
        p++;
        if (job->merkle_branch_count < STRATUM_MAX_MERKLE_BRANCH) {
            if (hexdecode(hex64, n,
                          job->merkle_branch[job->merkle_branch_count],
                          32) != 32) return -7;
            job->merkle_branch_count++;
        }
    }
    if (*p != ']') return -8;
    p++;

    /* version, nbits, ntime as quoted hex strings. */
    char ver_hex[16], nbits_hex[16], ntime_hex[16];
    READ_QSTR(ver_hex, sizeof(ver_hex));
    READ_QSTR(nbits_hex, sizeof(nbits_hex));
    READ_QSTR(ntime_hex, sizeof(ntime_hex));

    job->version = (uint32_t)strtoul(ver_hex,   NULL, 16);
    job->nbits   = (uint32_t)strtoul(nbits_hex, NULL, 16);
    job->ntime   = (uint32_t)strtoul(ntime_hex, NULL, 16);

    /* clean_jobs bool — true/false. */
    while (*p == ' ' || *p == ',') p++;
    job->clean_jobs = (*p == 't');

    /* Carry subscription state into the job for build_header. */
    memcpy(job->extranonce1, S.extranonce1, S.extranonce1_len);
    job->extranonce1_len  = S.extranonce1_len;
    job->extranonce2_size = S.extranonce2_size;
    job->extranonce2 = 0;

    #undef READ_QSTR
    return 0;
}

int stratum_wait_first_job(int sock, stratum_job_t *job, double *pool_diff) {
    char line[3072];
    while (1) {
        int n = recv_line(sock, line, sizeof(line));
        if (n <= 0) return -1;
        if (strstr(line, "\"mining.set_difficulty\"")) {
            /* Parse difficulty from params:[N]. */
            const char *p = strstr(line, "\"params\":[");
            if (p) *pool_diff = strtod(p + strlen("\"params\":["), NULL);
            continue;
        }
        if (strstr(line, "\"mining.notify\"")) {
            return parse_notify(line, job);
        }
        /* ignore other messages (ping, etc.) */
    }
}

int stratum_poll_nonblock(int sock, stratum_event_t *ev) {
    (void)sock;
    /* Cheap poll: only consume if a complete line is already in buf
    ** from a prior recv.  Doesn't initiate a new recv() so this is
    ** never blocking.  A fuller implementation would make the socket
    ** non-blocking and read until EAGAIN. */
    if (!line_available()) return 0;
    char line[3072];
    int n = recv_line(sock, line, sizeof(line));
    if (n <= 0) return -1;
    if (strstr(line, "\"mining.notify\"")) {
        ev->kind = STRATUM_EV_NEW_JOB;
        return parse_notify(line, &ev->job) == 0 ? 1 : -1;
    }
    if (strstr(line, "\"mining.set_difficulty\"")) {
        const char *p = strstr(line, "\"params\":[");
        if (p) ev->difficulty = strtod(p + strlen("\"params\":["), NULL);
        ev->kind = STRATUM_EV_SET_DIFF;
        return 1;
    }
    ev->kind = STRATUM_EV_NONE;
    return 1;
}

/* ---- header build ----------------------------------------------- */

void stratum_build_header(const stratum_job_t *job, uint8_t header_out[80]) {
    /* The Bitcoin block header is 80 bytes, little-endian on the wire:
    **   version (4) | prev_hash (32) | merkle_root (32) | ntime (4)
    **                                                   | nbits (4) | nonce (4)
    **
    ** merkle_root is computed from the coinbase + merkle_branch.
    ** Coinbase = coinb1 || extranonce1 || extranonce2 || coinb2
    ** merkle_root = repeated double-SHA-256 with each branch sibling. */

    /* Build coinbase. */
    uint8_t coinbase[1024];
    int     cb_len = 0;
    memcpy(coinbase + cb_len, job->coinb1, job->coinb1_len);
    cb_len += job->coinb1_len;
    memcpy(coinbase + cb_len, job->extranonce1, job->extranonce1_len);
    cb_len += job->extranonce1_len;
    /* extranonce2: write LSB-first, fill to extranonce2_size bytes. */
    for (int i = 0; i < job->extranonce2_size; i++) {
        coinbase[cb_len + i] = (uint8_t)(job->extranonce2 >> (8 * i));
    }
    cb_len += job->extranonce2_size;
    memcpy(coinbase + cb_len, job->coinb2, job->coinb2_len);
    cb_len += job->coinb2_len;

    /* coinbase hash = SHA256d(coinbase). */
    uint8_t hash[32], tmp[32];
    sha256(coinbase, cb_len, tmp);
    sha256(tmp, 32, hash);

    /* Fold merkle branches in. */
    for (int i = 0; i < job->merkle_branch_count; i++) {
        uint8_t pair[64];
        memcpy(pair,      hash,                   32);
        memcpy(pair + 32, job->merkle_branch[i], 32);
        sha256(pair, 64, tmp);
        sha256(tmp, 32, hash);
    }
    /* `hash` is now the merkle root in internal byte order. */

    /* Now build the header. */
    /* version (4) little-endian */
    header_out[0] = (uint8_t)(job->version);
    header_out[1] = (uint8_t)(job->version >>  8);
    header_out[2] = (uint8_t)(job->version >> 16);
    header_out[3] = (uint8_t)(job->version >> 24);

    /* prev_hash (32 bytes).  The stratum field is sent as "natural"
    ** display order but we need it byte-reversed in 4-byte chunks
    ** to put it in protocol order. */
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            header_out[4 + i*4 + j] = job->prev_hash[i*4 + (3 - j)];
        }
    }

    /* merkle_root (32 bytes) — already in internal byte order. */
    memcpy(header_out + 36, hash, 32);

    /* ntime (4 bytes) — big-endian on wire? actually little-endian like
    ** every other 32-bit field in the header. */
    header_out[68] = (uint8_t)(job->ntime);
    header_out[69] = (uint8_t)(job->ntime >>  8);
    header_out[70] = (uint8_t)(job->ntime >> 16);
    header_out[71] = (uint8_t)(job->ntime >> 24);

    /* nbits (4 bytes) little-endian. */
    header_out[72] = (uint8_t)(job->nbits);
    header_out[73] = (uint8_t)(job->nbits >>  8);
    header_out[74] = (uint8_t)(job->nbits >> 16);
    header_out[75] = (uint8_t)(job->nbits >> 24);

    /* nonce is the LAST 4 bytes; the mining loop writes those each
    ** iteration.  Initialize to zero here. */
    header_out[76] = 0; header_out[77] = 0; header_out[78] = 0; header_out[79] = 0;
}

int stratum_submit_share(int sock, const stratum_job_t *job, uint32_t nonce) {
    /* Submit format:
    ** {"id":N,"method":"mining.submit",
    **  "params":["<user>","<job_id>","<extranonce2_hex>",
    **            "<ntime_hex>","<nonce_hex>"]} */
    char e2_hex[33], ntime_hex[9], nonce_hex[9];
    /* extranonce2 as hex, little-endian byte order, extranonce2_size bytes. */
    uint8_t e2_bytes[16];
    for (int i = 0; i < job->extranonce2_size; i++) {
        e2_bytes[i] = (uint8_t)(job->extranonce2 >> (8 * i));
    }
    hexencode(e2_bytes, job->extranonce2_size, e2_hex);
    snprintf(ntime_hex, sizeof(ntime_hex), "%08x", (unsigned)job->ntime);
    snprintf(nonce_hex, sizeof(nonce_hex), "%08x", (unsigned)nonce);

    char msg[512];
    /* Note: pool user is hardcoded in main.c's POOL_USER; for v0.1
    ** we use the same global, but a cleaner API would carry it in
    ** stratum_state.  Encode the call directly with a fixed worker
    ** name passed in via the job.  Cheap shortcut: leave as "x". */
    snprintf(msg, sizeof(msg),
        "{\"id\":%d,\"method\":\"mining.submit\","
        "\"params\":[\"x\",\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
        ++S.msg_id, job->job_id, e2_hex, ntime_hex, nonce_hex);
    return send_line(sock, msg);
}
