// Console key; see include/ra_key.h.  Must match nds-bootstrap-ra's
// ra_boot.cpp deriveKey() byte for byte.
#include <stdio.h>
#include <string.h>

#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "ra_key.h"
#include "ra_secret.h"

#define EMMC_CID ((const volatile unsigned char *)0x02FFD7BC)

static const char key_label[] = "RetroAchievements DSi console key 1";

int ra_key_derive(unsigned char key[32]) {
    unsigned char cid[16];
    int present = 0;
    for (int i = 0; i < 16; i++) {
        cid[i] = EMMC_CID[i];
        present |= cid[i] != 0;
    }
    if (!present) return -1;
    mbedtls_sha256_context s;
    mbedtls_sha256_init(&s);
    mbedtls_sha256_starts_ret(&s, 0);
    mbedtls_sha256_update_ret(&s, raBuildSecret, sizeof(raBuildSecret));
    mbedtls_sha256_update_ret(&s, cid, sizeof(cid));
    mbedtls_sha256_update_ret(&s, (const unsigned char *)key_label, sizeof(key_label) - 1);
    mbedtls_sha256_finish_ret(&s, key);
    mbedtls_sha256_free(&s);
    return 0;
}

void ra_hmac(const unsigned char key[32], const void *data, size_t size, unsigned char out[32]) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, data, size, out);
}

void ra_hmac_hex(const unsigned char key[32], const void *data, size_t size, char out[65]) {
    unsigned char mac[32];
    ra_hmac(key, data, size, mac);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", mac[i]);
}
