// ranet.bin's entry points (include/ra_netblob.h): nds-bootstrap-ra's card
// engine loads the blob at RA_NET_BLOB_ADDRESS and calls these.  Awards go
// out one at a time over HTTPS, resuming RA Sync's TLS session, as the POST
// rcheevos' rc_api_init_award_achievement_request() builds.
#include <calico/types.h>
#include <calico/system/dietprint.h>
#include <string.h>
#include "ranet.h"
#include "ranet_host.h"
#include "md5.h"

#include "../../include/ra_netblob.h"

extern u8 __bss_start[], __bss_end[], __image_end[];

static struct RaNetHost s_host;
static struct RaNetConfig s_config;
static RaNetProfile s_profile;
static RaTlsSession s_tls;

// 32-byte aligned arena for the stack's threads, packets and heap
static u8 s_arena[RANET_ARENA_SIZE] __attribute__((aligned(32)));

// ranet_host.h, served by the card engine
uint32_t ranetHostTicks(void) { return s_host.ticks(); }
uint32_t ranetHostEntropy(void) { return s_host.entropy(); }
uint8_t ranetHostI2cRead(uint8_t dev, uint8_t reg) { return s_host.i2cRead(dev, reg); }
bool ranetHostI2cWrite(uint8_t dev, uint8_t reg, uint8_t data) { return s_host.i2cWrite(dev, reg, data) != 0; }
bool ranetHostNvramRead(void* dst, u32 addr, u32 len) { return s_host.nvramRead(dst, addr, len) != 0; }
void ranetHostLog(const char* buf, unsigned size)
{
	if (!s_host.log) return;
	if (buf) {
		s_host.log(buf, size);
	} else {
		while (size --) s_host.log(" ", 1);
	}
}

// newlib's errno (sgIP sets it) without its thread support
struct _reent;
extern struct _reent* _impure_ptr;
struct _reent* __getreent(void)
{
	return _impure_ptr;
}

static void logPrint(const char* buf, size_t size)
{
	ranetHostLog(buf, size);
}

// ---------------------------------------------------------------------------
// Awards: a small queue, one request at a time
// ---------------------------------------------------------------------------

#define QUEUE_MAX 16

typedef struct Award {
	u32 seq;
	u32 id;
	u32 hardcore;
	u32 seconds;
	u32 result; // enum RaNetAward
} Award;

static Award s_queue[QUEUE_MAX];
static u32 s_queued, s_sending, s_reported; // counters into the ring
enum { SESSION_OFF, SESSION_ON, SESSION_STOPPING };
static int s_session;
static bool s_busy;
static u16 s_gpioWl;
static u32 s_idleSince;
static u32 s_stoppedAt;     // ticks when the last session ended
static bool s_everStopped;
static bool s_disabled;     // the in-game menu turned it off (enable())
static char s_body[512];
static char s_reply[4096];

// The account's unlocks for the game (accountUnlocks()), fetched once per
// init when connected and nothing is waiting to be sent
#define UNLOCKS_MAX 512
static u32 s_unlockIds[UNLOCKS_MAX];
static u32 s_unlockCount;
static enum { FETCH_TODO, FETCH_BUSY, FETCH_READY, FETCH_TAKEN, FETCH_FAILED } s_fetch;

static u32 append(char* buf, u32 at, u32 size, const char* text)
{
	while (*text && at + 1 < size) buf[at ++] = *text ++;
	buf[at] = '\0';
	return at;
}

static const char* utoa10(u32 value, char out[12])
{
	char* p = out + 11;
	*p = '\0';
	do {
		*--p = '0' + value % 10;
		value /= 10;
	} while (value);
	return p;
}

// URL-encodes text (user names are [A-Za-z0-9_] in practice; tokens are
// alphanumeric)
static u32 appendEncoded(char* buf, u32 at, u32 size, const char* text)
{
	static const char hex[] = "0123456789ABCDEF";
	for (; *text && at + 4 < size; text ++) {
		unsigned char c = (unsigned char)*text;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			buf[at ++] = c;
		} else {
			buf[at ++] = '%';
			buf[at ++] = hex[c >> 4];
			buf[at ++] = hex[c & 15];
		}
	}
	buf[at] = '\0';
	return at;
}

static void buildAward(const Award* a)
{
	char n[12];
	u32 at = 0;
	at = append(s_body, at, sizeof(s_body), "r=awardachievement&u=");
	at = appendEncoded(s_body, at, sizeof(s_body), s_config.user);
	at = append(s_body, at, sizeof(s_body), "&t=");
	at = appendEncoded(s_body, at, sizeof(s_body), s_config.token);
	at = append(s_body, at, sizeof(s_body), "&a=");
	at = append(s_body, at, sizeof(s_body), utoa10(a->id, n));
	at = append(s_body, at, sizeof(s_body), a->hardcore ? "&h=1" : "&h=0");
	if (s_config.md5[0]) {
		at = append(s_body, at, sizeof(s_body), "&m=");
		at = append(s_body, at, sizeof(s_body), s_config.md5);
	}
	if (a->seconds) {
		at = append(s_body, at, sizeof(s_body), "&o=");
		at = append(s_body, at, sizeof(s_body), utoa10(a->seconds, n));
	}

	// Signature: md5(id, user, hardcore[, id, seconds]) in hex
	md5_state_t md5;
	md5_byte_t digest[16];
	md5_init(&md5);
	const char* s = utoa10(a->id, n);
	md5_append(&md5, (const md5_byte_t*)s, strlen(s));
	md5_append(&md5, (const md5_byte_t*)s_config.user, strlen(s_config.user));
	md5_append(&md5, (const md5_byte_t*)(a->hardcore ? "1" : "0"), 1);
	if (a->seconds) {
		s = utoa10(a->id, n);
		md5_append(&md5, (const md5_byte_t*)s, strlen(s));
		s = utoa10(a->seconds, n);
		md5_append(&md5, (const md5_byte_t*)s, strlen(s));
	}
	md5_finish(&md5, digest);
	char sig[33];
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 16; i ++) {
		sig[2*i] = hex[digest[i] >> 4];
		sig[2*i+1] = hex[digest[i] & 15];
	}
	sig[32] = '\0';
	at = append(s_body, at, sizeof(s_body), "&v=");
	append(s_body, at, sizeof(s_body), sig);
}

// rcheevos' rc_api_init_fetch_user_unlocks_request(): softcore unlocks
static void buildUnlocks(void)
{
	char n[12];
	u32 at = 0;
	at = append(s_body, at, sizeof(s_body), "r=unlocks&u=");
	at = appendEncoded(s_body, at, sizeof(s_body), s_config.user);
	at = append(s_body, at, sizeof(s_body), "&t=");
	at = appendEncoded(s_body, at, sizeof(s_body), s_config.token);
	at = append(s_body, at, sizeof(s_body), "&g=");
	at = append(s_body, at, sizeof(s_body), utoa10(s_config.gameId, n));
	append(s_body, at, sizeof(s_body), "&h=0");
}

// {"Success":true,...,"UserUnlocks":[1,2,3]}
static bool parseUnlocks(int status)
{
	const char* body = status == 200 ? strstr(s_reply, "\r\n\r\n") : NULL;
	const char* list = body ? strstr(body, "\"UserUnlocks\":[") : NULL;
	if (!list || !strstr(body, "\"Success\":true")) return false;
	s_unlockCount = 0;
	for (const char* p = list + 15; *p && *p != ']' && s_unlockCount < UNLOCKS_MAX; ) {
		if (*p >= '0' && *p <= '9') {
			u32 v = 0;
			while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
			s_unlockIds[s_unlockCount++] = v;
		} else {
			p++;
		}
	}
	return true;
}

// RA's answer: {"Success":true,...}, or false with "User already has ..."
// when it was sent before (as rc_api treats it)
static u32 judge(int status)
{
	if (status != 200) return RA_AWARD_FAILED;
	const char* body = strstr(s_reply, "\r\n\r\n");
	if (!body) return RA_AWARD_FAILED;
	if (strstr(body, "\"Success\":true")) return RA_AWARD_SENT;
	if (strstr(body, "User already has")) return RA_AWARD_SENT;
	return RA_AWARD_FAILED; // RA Sync tries again and reports why
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

static int blobInit(const struct RaNetHost* host, const struct RaNetConfig* config)
{
	struct RaNetHost h;
	struct RaNetConfig c;
	if (!host || host->size != sizeof(h) || !config || config->size != sizeof(c)) return 0;
	memcpy(&h, host, sizeof(h));
	memcpy(&c, config, sizeof(c));
	memset(__bss_start, 0, __bss_end - __bss_start);
	s_host = h;
	s_config = c;
	s_config.user[RA_NET_USER_MAX - 1] = '\0';
	s_config.token[RA_NET_TOKEN_MAX - 1] = '\0';
	s_config.md5[32] = '\0';
	memcpy(&s_profile, c.profile, sizeof(s_profile));
	memcpy(&s_tls, c.tls, sizeof(s_tls));
	dietPrintSetFunc(logPrint);
	return s_profile.magic == RA_NET_PROFILE_MAGIC && s_tls.magic == RA_TLS_SESSION_MAGIC
		&& s_config.user[0] && s_config.token[0];
}

static int blobAward(u32 seq, u32 achievementId, int hardcore, u32 secondsSinceUnlock)
{
	if (s_disabled || s_queued - s_reported >= QUEUE_MAX) return 0;
	if (ranetGetState() == RanetState_Failed) return 0;
	Award* a = &s_queue[s_queued % QUEUE_MAX];
	a->seq = seq;
	a->id = achievementId;
	a->hardcore = hardcore != 0;
	a->seconds = secondsSinceUnlock;
	a->result = RA_AWARD_PENDING;
	s_queued ++;
	return 1;
}

extern u32 coopCpsr(void);

// The WiFi session: joined 10 s into the game and kept, so that an unlock
// only costs its HTTPS request.  Not at the unlock itself: the popup stops
// the game (and the card engine's interrupts) for a while, and bringing the
// driver up right after it hung the ARM7.  A session that fails is stopped
// (the game gets its DS WiFi module back) and tried again a minute later.
#define TICKS_PER_SECOND 523656u // 33.5 MHz / 64
#define START_DELAY (10u * TICKS_PER_SECOND)
#define RETRY_DELAY (60u * TICKS_PER_SECOND)

static void sessionStart(void)
{
	// nds-bootstrap selects the DS WiFi module for DS games; ours is the
	// DSi's, put back when the session ends
	s_gpioWl = *(vu16*)0x04004C04;
	ranetSetTlsSession(&s_tls);
	s_session = ranetStart(&s_profile, s_arena) ? SESSION_ON : SESSION_OFF;
	s_idleSince = ranetHostTicks();
	dietPrint(s_session ? "[blob] WiFi on\n" : "[blob] can't start\n");
}

// The WiFi LED (MCU register 0x30, bit 0) on while connected, as the DSi
// shows WiFi in use (nds-bootstrap turns it off for DS games)
static bool s_ledOn;
static void wifiLed(bool on)
{
	if (s_ledOn == on) return;
	s_ledOn = on;
	u8 reg = ranetHostI2cRead(0x4A, 0x30);
	ranetHostI2cWrite(0x4A, 0x30, on ? (reg | 1) : (reg & ~1));
}

static void sessionPoll(void)
{
	if (s_session == SESSION_STOPPING) {
		ranetPoll();
		if (ranetStopped()) {
			wifiLed(false);
			*(vu16*)0x04004C04 = s_gpioWl;
			s_session = SESSION_OFF;
			s_stoppedAt = ranetHostTicks();
			s_everStopped = true;
			dietPrint("[blob] WiFi off\n");
		}
		return;
	}
	if (s_session == SESSION_OFF) {
		const u32 now = ranetHostTicks();
		if (!s_disabled && now > START_DELAY && (!s_everStopped || now - s_stoppedAt > RETRY_DELAY)) sessionStart();
		return;
	}
	ranetPoll();
	RanetState st = ranetGetState();
	if (st == RanetState_Online || st == RanetState_Busy) wifiLed(true);
	if (s_busy) {
		int status = 0;
		if (ranetRequestDone(&status)) {
			if (s_fetch == FETCH_BUSY) {
				const bool ok = parseUnlocks(status);
				s_fetch = ok ? FETCH_READY : FETCH_FAILED;
				dietPrint("[blob] account unlocks: %s (%lu)\n", ok ? "ok" : "failed", (unsigned long)s_unlockCount);
			} else {
				Award* a = &s_queue[s_sending % QUEUE_MAX];
				a->result = judge(status);
				dietPrint("[blob] award %lu: %s\n", (unsigned long)a->id, a->result == RA_AWARD_SENT ? "sent" : "failed");
				s_sending ++;
			}
			s_busy = false;
		}
	} else if (s_fetch == FETCH_TODO && s_sending == s_queued && st == RanetState_Online && s_config.gameId) {
		buildUnlocks();
		s_busy = ranetRequest("/dorequest.php", s_body, s_reply, sizeof(s_reply));
		if (s_busy) s_fetch = FETCH_BUSY;
	} else if (s_sending < s_queued) {
		if (st == RanetState_Failed) {
			// Everything waiting goes to RA Sync
			s_queue[s_sending % QUEUE_MAX].result = RA_AWARD_FAILED;
			s_sending ++;
		} else if (st == RanetState_Online) {
			buildAward(&s_queue[s_sending % QUEUE_MAX]);
			s_busy = ranetRequest("/dorequest.php", s_body, s_reply, sizeof(s_reply));
		}
		s_idleSince = ranetHostTicks();
	} else if (st == RanetState_Failed) {
		ranetStop();
		s_session = SESSION_STOPPING;
	}
}

static void blobPoll(void)
{
	// Only from system mode (the game's code, where the card engine's halt
	// hook runs): interrupts arriving while a thread runs then use its big
	// stack the way they would the game's.  And never re-entered (the
	// game's ARM7 threads may switch in the middle).
	static bool inPoll;
	if (inPoll || (coopCpsr() & 0x1F) != 0x1F) return;
	inPoll = true;
	sessionPoll();
	// Debug: a sign of life every 2 s while WiFi is on
	static u32 lastBeat;
	if (s_session != SESSION_OFF && ranetHostTicks() - lastBeat > 2u * 523656u) {
		lastBeat = ranetHostTicks();
		dietPrint("[blob] alive, state %d, %lu/%lu sent\n", (int)ranetGetState(),
			(unsigned long)s_sending, (unsigned long)s_queued);
	}
	inPoll = false;
}

static int blobResult(u32* seq, u32* result)
{
	if (s_reported >= s_sending) return 0;
	const Award* a = &s_queue[s_reported % QUEUE_MAX];
	*seq = a->seq;
	*result = a->result;
	s_reported ++;
	return 1;
}

static u32 blobState(void)
{
	if (s_session != SESSION_ON) return RA_NET_OFF;
	switch (ranetGetState()) {
		case RanetState_Off: return RA_NET_OFF;
		case RanetState_Starting: return RA_NET_STARTING;
		case RanetState_Associating: return RA_NET_JOINING;
		case RanetState_Online: return s_busy ? RA_NET_BUSY : RA_NET_ONLINE;
		case RanetState_Busy: return RA_NET_BUSY;
		default: return RA_NET_FAILED;
	}
}

static void blobStop(void)
{
	if (s_session == SESSION_ON) {
		ranetStop();
		s_session = SESSION_STOPPING;
	}
}

static int blobStopped(void)
{
	return s_session == SESSION_OFF;
}

static int blobAccountUnlocks(const u32** ids, u32* count)
{
	if (s_fetch != FETCH_READY) return 0;
	s_fetch = FETCH_TAKEN;
	*ids = s_unlockIds;
	*count = s_unlockCount;
	return 1;
}

static void blobEnable(int on)
{
	if (on) {
		// Up again at the next poll, not after the retry delay
		s_disabled = false;
		s_everStopped = false;
		dietPrint("[blob] turned on\n");
	} else {
		s_disabled = true;
		blobStop();
		// Queued awards stay for RA Sync
		while (s_sending < s_queued) {
			s_queue[s_sending % QUEUE_MAX].result = RA_AWARD_FAILED;
			s_sending ++;
		}
		s_busy = false;
		dietPrint("[blob] turned off\n");
	}
}

__attribute__((section(".header"), used))
const struct RaNetHeader ranetHeader = {
	.magic = RA_NET_BLOB_MAGIC,
	.version = RA_NET_BLOB_VERSION,
	.imageEnd = (u32)__image_end,
	.bssEnd = (u32)__bss_end,
	.init = blobInit,
	.award = blobAward,
	.poll = blobPoll,
	.result = blobResult,
	.state = blobState,
	.stop = blobStop,
	.stopped = blobStopped,
	.enable = blobEnable,
	.accountUnlocks = blobAccountUnlocks,
};
