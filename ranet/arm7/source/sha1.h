// SHA-1 (FIPS 180-4), small and table-free, for WPA's HMAC-SHA1
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_SZ 20

typedef struct Sha1Ctx {
	uint32_t h[5];
	uint64_t length;    // bytes so far
	uint8_t block[64];
	unsigned used;      // bytes in block
} Sha1Ctx;

void sha1Init(Sha1Ctx* ctx);
void sha1Update(Sha1Ctx* ctx, const void* data, size_t len);
void sha1Final(Sha1Ctx* ctx, void* digest);
void sha1(void* digest, const void* data, size_t len);
