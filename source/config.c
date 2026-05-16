#include "config.h"

#include <pspiofilemgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Per-binary savedata path.  Override at compile time with e.g.
**   -DCONFIG_PATH='"ms0:/PSP/SAVEDATA/btc-miner-psp-nopragma/params.txt"'
** so multiple parallel installs (for A/B benchmark comparisons) can
** each keep their own params.txt under the matching SAVEDATA dir. */
#ifndef CONFIG_PATH
#define CONFIG_PATH "ms0:/PSP/SAVEDATA/btc-miner-psp/params.txt"
#endif
#define CONFIG_MAX_BYTES 4096

/* Copy `n` chars from `src` into `dst[max]`, NUL-terminating. */
static void copy_field(char *dst, int max, const char *src, int n) {
    if (n >= max) n = max - 1;
    memcpy(dst, src, (size_t)n);
    dst[n] = 0;
}

/* Trim trailing CR/LF/space in-place; return new length. */
static int rtrim(char *s, int n) {
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n'
                  || s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = 0;
    }
    return n;
}

/* Apply one parsed key=value pair to cfg. */
static void apply_kv(miner_config_t *cfg, const char *key, int klen,
                                          const char *val, int vlen) {
    if (klen == 4 && memcmp(key, "host", 4) == 0) {
        copy_field(cfg->host, CONFIG_HOST_MAX, val, vlen);
        cfg->loaded_mask |= 1u << 0;
    } else if (klen == 4 && memcmp(key, "port", 4) == 0) {
        char buf[16];
        copy_field(buf, sizeof(buf), val, vlen);
        long p = strtol(buf, NULL, 10);
        if (p > 0 && p <= 65535) {
            cfg->port = (uint16_t)p;
            cfg->loaded_mask |= 1u << 1;
        }
    } else if (klen == 4 && memcmp(key, "user", 4) == 0) {
        copy_field(cfg->user, CONFIG_USER_MAX, val, vlen);
        cfg->loaded_mask |= 1u << 2;
    } else if (klen == 4 && memcmp(key, "pass", 4) == 0) {
        copy_field(cfg->pass, CONFIG_PASS_MAX, val, vlen);
        cfg->loaded_mask |= 1u << 3;
    } else if (klen == 3 && memcmp(key, "tls", 3) == 0) {
        char buf[8];
        copy_field(buf, sizeof(buf), val, vlen);
        int on = (buf[0] == 'y' || buf[0] == 'Y' ||
                  buf[0] == 't' || buf[0] == 'T' ||
                  buf[0] == '1');
        cfg->use_tls = on ? 1 : 0;
        cfg->loaded_mask |= 1u << 4;
    } else if (klen == 10 && memcmp(key, "tls_verify", 10) == 0) {
        char buf[8];
        copy_field(buf, sizeof(buf), val, vlen);
        int on = (buf[0] == 'y' || buf[0] == 'Y' ||
                  buf[0] == 't' || buf[0] == 'T' ||
                  buf[0] == '1');
        cfg->tls_verify = on ? 1 : 0;
        cfg->loaded_mask |= 1u << 5;
    } else if (klen == 5 && memcmp(key, "bench", 5) == 0) {
        char buf[8];
        copy_field(buf, sizeof(buf), val, vlen);
        int on = (buf[0] == 'y' || buf[0] == 'Y' ||
                  buf[0] == 't' || buf[0] == 'T' ||
                  buf[0] == '1');
        cfg->bench = on ? 1 : 0;
        cfg->loaded_mask |= 1u << 6;
    } else if (klen == 11 && memcmp(key, "bench_naive", 11) == 0) {
        char buf[8];
        copy_field(buf, sizeof(buf), val, vlen);
        int on = (buf[0] == 'y' || buf[0] == 'Y' ||
                  buf[0] == 't' || buf[0] == 'T' ||
                  buf[0] == '1');
        cfg->bench_naive = on ? 1 : 0;
        cfg->loaded_mask |= 1u << 7;
    }
    /* Unknown keys silently ignored — future-proof. */
}

int config_load(miner_config_t *cfg) {
    int fd = sceIoOpen(CONFIG_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;   /* missing is not an error */

    char buf[CONFIG_MAX_BYTES + 1];
    int  n = sceIoRead(fd, buf, CONFIG_MAX_BYTES);
    sceIoClose(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    /* Parse line-by-line. We rewrite buf in place but never beyond `n`. */
    int   start_loaded_mask = cfg->loaded_mask;
    char *p   = buf;
    char *end = buf + n;
    while (p < end) {
        /* Find end of line (or buffer). */
        char *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;
        int line_len = (int)(line_end - p);

        /* Trim CR/space at end. */
        int trimmed = rtrim(p, line_len);

        /* Skip leading whitespace and comments. */
        char *lp = p;
        char *le = p + trimmed;
        while (lp < le && (*lp == ' ' || *lp == '\t')) lp++;

        if (lp < le && *lp != '#' && *lp != 0) {
            /* Find '='. */
            char *eq = lp;
            while (eq < le && *eq != '=') eq++;
            if (eq < le && eq != lp) {
                /* Trim spaces inside key/value. */
                char *kend = eq;
                while (kend > lp && (*(kend - 1) == ' ' || *(kend - 1) == '\t')) kend--;
                char *vstart = eq + 1;
                while (vstart < le && (*vstart == ' ' || *vstart == '\t')) vstart++;
                int klen = (int)(kend - lp);
                int vlen = (int)(le - vstart);
                if (klen > 0) apply_kv(cfg, lp, klen, vstart, vlen);
            }
        }

        if (line_end >= end) break;
        p = line_end + 1;
    }

    return (cfg->loaded_mask != start_loaded_mask) ? 1 : -1;
}
