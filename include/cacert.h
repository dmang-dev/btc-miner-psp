/* GENERATED header for source/cacert_data.c. */
#ifndef BTCM_CACERT_H
#define BTCM_CACERT_H

#include <stddef.h>

/* Mozilla CA bundle as a single PEM blob, NUL-terminated.
 * Pass directly to mbedtls_x509_crt_parse(). */
extern const unsigned char btcm_ca_bundle_pem[];
extern const size_t        btcm_ca_bundle_pem_len;

#endif
