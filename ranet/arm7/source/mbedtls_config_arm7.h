// mbedTLS for ranet on the ARM7: a TLS 1.2 client that resumes the session
// RA Sync saved (include/ra_tlssession.h).  The key exchange code is only
// there because mbedTLS needs one configured: a full handshake would stop
// at the certificate (no trust anchors here), so nothing is ever sent over
// an unauthenticated connection.
#ifndef MBEDTLS_CONFIG_ARM7_H
#define MBEDTLS_CONFIG_ARM7_H

#include <stddef.h>

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY          // calloc/free from ranet's heap
// ...from the start, so that newlib's malloc isn't linked (the blob has none)
void* ranet_calloc(size_t n, size_t size);
void ranet_free(void* ptr);
#define MBEDTLS_PLATFORM_STD_CALLOC ranet_calloc
#define MBEDTLS_PLATFORM_STD_FREE ranet_free
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT     // mbedtls_hardware_poll() in tls.c
#define MBEDTLS_DEPRECATED_REMOVED

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_SESSION_TICKETS

#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C                 // SHA-384 suites

#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// Records from the server can be full size; ours are small
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#include "mbedtls/check_config.h"
#endif
