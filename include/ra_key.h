// The console key nds-bootstrap-ra signs unlock records and sets with:
// SHA-256 of the build secret (ra_secret.h, not in git; the same file as in
// nds-bootstrap-ra), the eMMC CID the DSi keeps at 0x02FFD7BC, and a label.
#ifndef RA_KEY_H
#define RA_KEY_H

#include <stddef.h>

// 0 with the key in key[32]; -1 without an eMMC CID
int ra_key_derive(unsigned char key[32]);

void ra_hmac(const unsigned char key[32], const void *data, size_t size, unsigned char out[32]);

// HMAC as 64 lowercase hex digits and a NUL
void ra_hmac_hex(const unsigned char key[32], const void *data, size_t size, char out[65]);

#endif
