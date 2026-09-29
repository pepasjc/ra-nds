// What the calico WiFi driver expects from the rest of calico, served by
// the host (ranet_host.h) instead: time, I2C, the DSi's extra interrupt
// controller (polled), packet buffers, logging, and the WPA helpers that
// calico runs on the DSi BIOS (here in software: a game may have the DS
// BIOS mapped).
#include <calico/types.h>
#include <calico/system/tick.h>
#include <calico/system/mutex.h>
#include <calico/system/irq.h>
#include <calico/system/dietprint.h>
#include <calico/dev/netbuf.h>
#include <calico/dev/wpa.h>
#include <calico/nds/env.h>
#include <calico/nds/arm7/i2c.h>
#include <calico/nds/arm7/gpio.h>
#include <calico/nds/lcd.h>
#include <string.h>
#include "ranet_host.h"
#include "sha1.h"

EnvExtraInfo g_ranetEnv;
Mutex g_i2cMutex;

// Time: the host's 32-bit tick counter (TICK_FREQ), widened
u64 tickGetCount(void)
{
	static u32 last, high;
	u32 now = ranetHostTicks();
	if (now < last) high ++;
	last = now;
	return ((u64)high << 32) | now;
}

// DSi-only interrupts: never taken (the host's interrupt handling doesn't
// know them).  ranetPollIrq2() keeps them masked and calls each handler
// every time; the TMIO handler checks the controller's own status bits, so
// a call with nothing pending does nothing.
static IrqHandler s_irq2[32];
static u32 s_irq2Mask;

void irqSet2(IrqMask mask, IrqHandler handler)
{
	for (int i = 0; i < 32; i ++) {
		if (mask & (1U << i)) s_irq2[i] = handler;
	}
	if (handler) s_irq2Mask |= mask; else s_irq2Mask &= ~mask;
}

void ranetPollIrq2(void)
{
	if (!s_irq2Mask) return;
	IrqState st = irqLock();
	REG_IE2 &= ~s_irq2Mask; // irqEnable2() sets them
	REG_IF2 = s_irq2Mask;
	for (int i = 0; i < 32; i ++) {
		if ((s_irq2Mask & (1U << i)) && s_irq2[i]) s_irq2[i]();
	}
	irqUnlock(st);
}

u8 i2cReadRegister8(I2cDevice dev, u8 reg)
{
	return ranetHostI2cRead(dev, reg);
}

bool i2cWriteRegister8(I2cDevice dev, u8 reg, u8 data)
{
	return ranetHostI2cWrite(dev, reg, data);
}

void gpioSetWlModule(GpioWlModule module)
{
	if (module == GpioWlModule_Mitsumi) {
		REG_GPIO_WL |= 1U << 8;
	} else {
		REG_GPIO_WL &= ~(1U << 8);
	}
}

// Packet buffers: one pool of full-size buffers from the host's arena
#define NETBUF_CAPACITY 1664
#define NETBUF_COUNT 16
static NetBuf* s_free;

void ranetNetbufInit(void* arena)
{
	u8* p = (u8*)arena;
	s_free = NULL;
	for (int i = 0; i < NETBUF_COUNT; i ++) {
		NetBuf* nb = (NetBuf*)p;
		memset(nb, 0, sizeof(*nb));
		nb->capacity = NETBUF_CAPACITY;
		nb->link.next = s_free;
		s_free = nb;
		p += sizeof(NetBuf) + NETBUF_CAPACITY;
	}
}

unsigned ranetNetbufArenaSize(void)
{
	return NETBUF_COUNT * (sizeof(NetBuf) + NETBUF_CAPACITY);
}

NetBuf* netbufAlloc(unsigned hdr_headroom_sz, unsigned data_sz, NetBufPool pool)
{
	(void)pool;
	if (hdr_headroom_sz + data_sz > NETBUF_CAPACITY) return NULL;
	IrqState st = irqLock();
	NetBuf* nb = s_free;
	if (nb) s_free = nb->link.next;
	irqUnlock(st);
	if (nb) {
		nb->link.next = nb->link.prev = NULL;
		nb->pos = hdr_headroom_sz;
		nb->len = data_sz;
	}
	return nb;
}

void netbufFree(NetBuf* nb)
{
	if (!nb) return;
	IrqState st = irqLock();
	nb->link.next = s_free;
	s_free = nb;
	irqUnlock(st);
}

void netbufFlush(NetBuf* nb)
{
	(void)nb; // ARM7: no cache
}

// WPA helpers (calico: wpa_hmac_sha1.twl.32.c, on the DSi BIOS)
void wpaHmacSha1(void* out, const void* key, size_t key_len, const void* data, size_t data_len)
{
	u8 keyblock[64];
	const u8* keyp = (const u8*)key;
	if (key_len > sizeof(keyblock)) {
		sha1(keyblock, key, key_len);
		keyp = keyblock;
		key_len = SHA1_DIGEST_SZ;
	}
	size_t i;
	for (i = 0; i < key_len; i ++) keyblock[i] = keyp[i] ^ 0x36;
	for (; i < sizeof(keyblock); i ++) keyblock[i] = 0x36;

	Sha1Ctx ctx;
	sha1Init(&ctx);
	sha1Update(&ctx, keyblock, sizeof(keyblock));
	sha1Update(&ctx, data, data_len);
	sha1Final(&ctx, out);

	for (i = 0; i < sizeof(keyblock); i ++) keyblock[i] ^= 0x36 ^ 0x5c;
	sha1Init(&ctx);
	sha1Update(&ctx, keyblock, sizeof(keyblock));
	sha1Update(&ctx, out, SHA1_DIGEST_SZ);
	sha1Final(&ctx, out);
}

void wpaGenerateEapolNonce(void* out)
{
	u32 data[8];
	memcpy(data, g_ranetEnv.wlmgr_macaddr, 6);
	data[1] = (data[1] & 0xFFFF) | ((u32)lcdGetVCount() << 16);
	data[2] = ranetHostTicks();
	data[3] = ranetHostEntropy();
	data[4] = (u32)tickGetCount();
	data[5] = ranetHostEntropy();
	data[6] = lcdGetVCount();
	data[7] = ranetHostTicks();

	u8 inner[SHA1_DIGEST_SZ];
	sha1(inner, data, sizeof(data));
	for (unsigned i = 0; i < 8; i ++) data[i] ^= ((u32*)inner)[i % 5];
	// WPA_EAPOL_NONCE_LEN (32) from two digests
	u8 a[SHA1_DIGEST_SZ], b[SHA1_DIGEST_SZ];
	sha1(a, data, sizeof(data));
	data[0] ^= 0x5A5A5A5A;
	sha1(b, data, sizeof(data));
	memcpy(out, a, 16);
	memcpy((u8*)out + 16, b, 16);
}
