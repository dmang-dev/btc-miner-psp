/*
 * config.h — miner configuration loaded from memstick at boot.
 *
 * Loads `ms0:/PSP/SAVEDATA/btc-miner-psp/params.txt` if present and
 * overrides the compiled-in defaults with whatever keys it finds.
 * Missing file or missing keys fall back to defaults (passed in by
 * the caller). The file format is one `key=value` per line, '#' for
 * comments. Trailing whitespace is trimmed; quotes are NOT special.
 *
 * Recognized keys:
 *   host = stratum.host.example
 *   port = 3333
 *   user = bc1qexampleaddress.psp
 *   pass = x
 *
 * Anything else is silently ignored (so a future v0.4 can add keys
 * without breaking old configs).
 */
#ifndef BTCM_CONFIG_H
#define BTCM_CONFIG_H

#include <stdint.h>

#define CONFIG_HOST_MAX 128
#define CONFIG_USER_MAX 128
#define CONFIG_PASS_MAX  64

typedef struct {
    char     host[CONFIG_HOST_MAX];
    uint16_t port;
    char     user[CONFIG_USER_MAX];
    char     pass[CONFIG_PASS_MAX];
    uint8_t  use_tls;     /* 0 = plain TCP, 1 = TLS via mbedtls         */
    /* Source per field — for the boot-time "loaded from params.txt"
     * report. Bits: 0=host, 1=port, 2=user, 3=pass, 4=tls. Set means
     * "came from file"; clear means "compiled default". */
    uint8_t  loaded_mask;
} miner_config_t;

/* Caller pre-fills `cfg` with compiled defaults; this populates any
 * keys present in params.txt over them and sets `loaded_mask` bits
 * for the keys that came from the file. Returns:
 *   1  — file existed and at least one key was loaded
 *   0  — file missing (cfg unchanged)
 *  -1  — file present but parse failed (cfg may be partially mutated) */
int config_load(miner_config_t *cfg);

#endif
