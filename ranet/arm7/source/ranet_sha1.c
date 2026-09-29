// SHA-1 (FIPS 180-4); see sha1.h
#include <string.h>
#include "sha1.h"

static uint32_t rol(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

static void sha1Block(Sha1Ctx* ctx, const uint8_t* p)
{
	uint32_t w[16];
	for (int i = 0; i < 16; i ++) {
		w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
	}
	uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
	for (int i = 0; i < 80; i ++) {
		uint32_t wi;
		if (i < 16) {
			wi = w[i];
		} else {
			wi = rol(w[(i+13)&15] ^ w[(i+8)&15] ^ w[(i+2)&15] ^ w[i&15], 1);
			w[i&15] = wi;
		}
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		uint32_t t = rol(a, 5) + f + e + k + wi;
		e = d;
		d = c;
		c = rol(b, 30);
		b = a;
		a = t;
	}
	ctx->h[0] += a;
	ctx->h[1] += b;
	ctx->h[2] += c;
	ctx->h[3] += d;
	ctx->h[4] += e;
}

void sha1Init(Sha1Ctx* ctx)
{
	ctx->h[0] = 0x67452301;
	ctx->h[1] = 0xEFCDAB89;
	ctx->h[2] = 0x98BADCFE;
	ctx->h[3] = 0x10325476;
	ctx->h[4] = 0xC3D2E1F0;
	ctx->length = 0;
	ctx->used = 0;
}

void sha1Update(Sha1Ctx* ctx, const void* data, size_t len)
{
	const uint8_t* p = (const uint8_t*)data;
	ctx->length += len;
	while (len) {
		unsigned n = 64 - ctx->used;
		if (n > len) n = len;
		memcpy(ctx->block + ctx->used, p, n);
		ctx->used += n;
		p += n;
		len -= n;
		if (ctx->used == 64) {
			sha1Block(ctx, ctx->block);
			ctx->used = 0;
		}
	}
}

void sha1Final(Sha1Ctx* ctx, void* digest)
{
	uint64_t bits = ctx->length * 8;
	uint8_t pad = 0x80;
	sha1Update(ctx, &pad, 1);
	pad = 0;
	while (ctx->used != 56) sha1Update(ctx, &pad, 1);
	uint8_t len[8];
	for (int i = 0; i < 8; i ++) len[i] = (uint8_t)(bits >> (56 - 8*i));
	sha1Update(ctx, len, 8);
	uint8_t* out = (uint8_t*)digest;
	for (int i = 0; i < 5; i ++) {
		out[4*i] = ctx->h[i] >> 24;
		out[4*i+1] = ctx->h[i] >> 16;
		out[4*i+2] = ctx->h[i] >> 8;
		out[4*i+3] = ctx->h[i];
	}
}

void sha1(void* digest, const void* data, size_t len)
{
	Sha1Ctx ctx;
	sha1Init(&ctx);
	sha1Update(&ctx, data, len);
	sha1Final(&ctx, digest);
}
