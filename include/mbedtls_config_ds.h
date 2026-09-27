// mbedTLS 2.28 configuration for a Nintendo DSi TLS 1.2 client
// (ARM946E-S, calico/libnds 2.x, BSD sockets from dswifi v2).
//
// Only what an HTTPS client to retroachievements.org (Cloudflare: ECDHE-ECDSA
// P-256, AES-GCM / ChaCha20-Poly1305, certificate chain up to GTS Root R4 or
// GlobalSign Root CA) and similar servers needs.
#ifndef MBEDTLS_CONFIG_DS_H
#define MBEDTLS_CONFIG_DS_H

// System
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE          // certificate validity from the DSi RTC
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_NO_PLATFORM_ENTROPY     // no /dev/urandom
#define MBEDTLS_ENTROPY_HARDWARE_ALT    // mbedtls_hardware_poll() in source/entropy.c
#define MBEDTLS_DEPRECATED_REMOVED

// TLS client
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION   // Cloudflare needs SNI
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_SESSION_TICKETS          // cheap reconnects (resumption)

// Key exchange
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED     // GTS Root R4 is P-384
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_RSA_C                        // GlobalSign Root CA, RSA servers
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

// Ciphers and hashes
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C                       // older certificates
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C                     // SHA-384
#define MBEDTLS_AES_ROM_TABLES               // tables in .rodata, not built at run time

// Randomness
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// Certificates
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

// Debug output (the POC prints handshake problems)
#define MBEDTLS_ERROR_C

// Memory: the DSi has 16MB; keep records within a sane size anyway
#define MBEDTLS_SSL_MAX_CONTENT_LEN 16384

#include "mbedtls/check_config.h"

#endif
