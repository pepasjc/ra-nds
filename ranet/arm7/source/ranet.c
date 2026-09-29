// In-game network stack; see ranet.h.  Everything runs on cooperative
// threads (coop.c) driven by ranetPoll(): the WiFi driver's own threads,
// "net" (bring-up, then requests) and "tick" (sgIP's timers).
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/system/dietprint.h>
#include <calico/dev/netbuf.h>
#include <calico/dev/wlan.h>
#include <calico/nds/env.h>
#include <calico/nds/arm7/twlwifi.h>
#include <string.h>
#include "sgIP.h"
#include "coop.h"
#include "ranet.h"
#include "ranet_host.h"
#include "tls.h"

extern EnvExtraInfo g_ranetEnv; // ranet_platform.c

// ---------------------------------------------------------------------------
// Memory: a first-fit heap in the host's arena, for sgIP
// ---------------------------------------------------------------------------

typedef struct Block {
	u32 size;          // bytes after this header
	struct Block* next; // free list only
} Block;

static Block* s_heap;

static void heapInit(void* mem, u32 size)
{
	s_heap = (Block*)mem;
	s_heap->size = size - sizeof(Block);
	s_heap->next = NULL;
}

void* sgIP_malloc(int size)
{
	u32 need = ((u32)size + 7) & ~7;
	Block** link = &s_heap;
	for (Block* b = s_heap; b; link = &b->next, b = b->next) {
		if (b->size < need) continue;
		if (b->size >= need + sizeof(Block) + 16) {
			Block* rest = (Block*)((u8*)(b + 1) + need);
			rest->size = b->size - need - sizeof(Block);
			rest->next = b->next;
			b->size = need;
			*link = rest;
		} else {
			*link = b->next;
		}
		return b + 1;
	}
	dietPrint("[net] out of memory (%d)\n", size);
	return NULL;
}

void sgIP_free(void* ptr)
{
	if (!ptr) return;
	Block* b = (Block*)ptr - 1;
	// Keep the free list in address order and merge neighbours
	Block** link = &s_heap;
	while (*link && *link < b) link = &(*link)->next;
	b->next = *link;
	*link = b;
	if (b->next && (u8*)(b + 1) + b->size == (u8*)b->next) {
		b->size += sizeof(Block) + b->next->size;
		b->next = b->next->next;
	}
	if (link != &s_heap) {
		Block* prev = (Block*)((u8*)link - offsetof(Block, next));
		if ((u8*)(prev + 1) + prev->size == (u8*)b) {
			prev->size += sizeof(Block) + b->size;
			prev->next = b->next;
		}
	}
}

void sgIP_IntrWaitEvent(void)
{
	threadYield();
}

// ---------------------------------------------------------------------------
// The WiFi key: the console's connection setting with the profile's SSID
// (as dswifi's wfc.c reads them)
// ---------------------------------------------------------------------------

typedef struct WfcConnSlot {
	char proxy_user[32];
	char proxy_password[32];
	char ssid[32];
	char aoss_ssid[32];
	u8   wep_keys[4][16];
	u32  ipv4_addr;
	u32  ipv4_gateway;
	u32  ipv4_dns[2];
	u8   ipv4_subnet;
	u8   wep_keys_aoss[4][5];
	u8   _pad_0xe5;
	u8   wep_mode     : 2;
	u8   wep_is_ascii : 1;
	u8   _pad_0xe6    : 5;
	u8   conn_type;
	u16  ssid_len;
	u16  mtu;
	u8   _pad_0xec[3];
	u8   config;
	u8   wfc_user_id[14];
	u16  crc16;
} WfcConnSlot;

typedef struct WfcConnSlotEx {
	WfcConnSlot base;
	u8   wpa_pmk[32];
	char wpa_psk[64];
	u8   _pad_0x160[33];
	u8   wpa_mode;
	u8   proxy_enable;
	u8   proxy_has_auth;
	char proxy_name[48];
	u8   _pad_0x1b4[52];
	u16  proxy_port;
	u8   _pad_0x1ea[20];
	u16  crc16;
} WfcConnSlotEx;

_Static_assert(sizeof(WfcConnSlot) == 0x100, "WfcConnSlot");
_Static_assert(sizeof(WfcConnSlotEx) == 0x200, "WfcConnSlotEx");

static u16 crc16(const void* data, u32 len)
{
	const u8* p = (const u8*)data;
	u16 crc = 0; // svcGetCRC16(0, ...)
	for (u32 i = 0; i < len; i ++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b ++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
	}
	return crc;
}

static bool findKey(const RaNetProfile* p, WlanBssDesc* bss, WlanAuthData* auth)
{
	u16 off_div8 = 0;
	if (!ranetHostNvramRead(&off_div8, 0x20, sizeof(off_div8))) return false;
	u32 off_slots = off_div8 * 8 - 4 * sizeof(WfcConnSlot);
	u32 off_slots_ex = off_slots - 3 * sizeof(WfcConnSlotEx);

	static WfcConnSlotEx slot;
	for (int i = 0; i < 6; i ++) {
		bool ex = i < 3;
		memset(&slot, 0, sizeof(slot));
		u32 addr = ex ? off_slots_ex + i * sizeof(WfcConnSlotEx) : off_slots + (i - 3) * sizeof(WfcConnSlot);
		if (!ranetHostNvramRead(&slot, addr, ex ? sizeof(WfcConnSlotEx) : sizeof(WfcConnSlot))) continue;
		if (slot.base.conn_type == 0xFF) continue;
		if (crc16(&slot.base, offsetof(WfcConnSlot, crc16)) != slot.base.crc16) continue;
		if (ex && crc16(&slot.base + 1, offsetof(WfcConnSlotEx, crc16) - sizeof(WfcConnSlot)) != slot.crc16) continue;
		if (slot.base.ssid_len != p->ssid_len || memcmp(slot.base.ssid, p->ssid, p->ssid_len)) continue;

		memset(auth, 0, sizeof(*auth));
		switch (slot.base.conn_type) {
			case 0x10: // WPA
			case 0x13: // WPA (WPS)
				if (!ex) continue;
				bss->auth_type = (WlanBssAuthType)slot.wpa_mode;
				memcpy(auth->wpa_psk, slot.wpa_pmk, WLAN_WPA_PSK_LEN);
				break;
			default: // WEP or open
				switch (slot.base.wep_mode) {
					default: bss->auth_type = WlanBssAuthType_Open; break;
					case 1: bss->auth_type = WlanBssAuthType_WEP_40; memcpy(auth->wep_key, slot.base.wep_keys[0], WLAN_WEP_40_LEN); break;
					case 2: bss->auth_type = WlanBssAuthType_WEP_104; memcpy(auth->wep_key, slot.base.wep_keys[0], WLAN_WEP_104_LEN); break;
					case 3: bss->auth_type = WlanBssAuthType_WEP_128; memcpy(auth->wep_key, slot.base.wep_keys[0], WLAN_WEP_128_LEN); break;
				}
				break;
		}
		dietPrint("[net] key: setting %d\n", i < 3 ? i + 4 : i - 2);
		memset(&slot, 0, sizeof(slot));
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Link: packets between the driver and sgIP (as dswifi's wfc.c)
// ---------------------------------------------------------------------------

static sgIP_Hub_HWInterface* s_iface;

static int ifaceSend(sgIP_Hub_HWInterface* hw, sgIP_memblock* mb)
{
	(void)hw;
	if (mb->next || mb->thislength != mb->totallength) {
		sgIP_memblock_free(mb);
		return 1;
	}
	NetBuf* pkt = netbufAlloc(g_ranetEnv.wlmgr_hdr_headroom_sz, mb->thislength, NetBufPool_Tx);
	if (!pkt) {
		sgIP_memblock_free(mb);
		return 1;
	}
	memcpy(netbufGet(pkt), mb->datastart, mb->thislength);
	sgIP_memblock_free(mb);
	twlwifiTx(pkt);
	return 0;
}

// From the driver (weak in wifi.twl.32.c): a received Ethernet frame
void _netbufRx(NetBuf* pkt)
{
	sgIP_memblock* mb = s_iface ? sgIP_memblock_alloc(2 + pkt->len) : NULL;
	if (mb) {
		sgIP_memblock_exposeheader(mb, -2);
		memcpy(mb->datastart, netbufGet(pkt), pkt->len);
	}
	netbufFree(pkt);
	if (mb) {
		SGIP_INTR_PROTECT();
		sgIP_Hub_ReceiveHardwarePacket(s_iface, mb);
		SGIP_INTR_UNPROTECT();
	}
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

static volatile RanetState s_state;
static RaNetProfile s_profile;
static WlanBssDesc s_bss;
static WlanAuthData s_auth;
static volatile int s_assoc; // 0 waiting, 1 joined, -1 refused

static Thread s_netThread, s_tickThread;
// Stacks come from the arena (coopInit); these only satisfy threadPrepare()
#define RANET_STACKS_SIZE (6 * 6144)
static u8 s_netStack[8], s_tickStack[8];

// The request handed to the net thread
static const RaTlsSession* s_tls; // HTTPS when set (ranetSetTlsSession)
static const char* s_reqPath;
static const char* s_reqBody;
static char* s_reply;
static u32 s_replySize;
static volatile bool s_reqPending, s_reqDone;
static volatile bool s_stop, s_stopped; // ranetStop()
static int s_httpStatus;

static void onAssoc(void* user, bool success, unsigned reason)
{
	(void)user;
	dietPrint("[net] assoc %s (%u)\n", success ? "ok" : "failed", reason);
	s_assoc = success ? 1 : -1;
}

static int tickMain(void* arg)
{
	(void)arg;
	while (!s_stop) {
		SGIP_INTR_PROTECT();
		sgIP_Timer(50);
		SGIP_INTR_UNPROTECT();
		threadSleep(50000);
	}
	return 0;
}

static bool waitFor(volatile int* flag, int ms)
{
	for (int t = 0; t < ms && *flag == 0; t += 10) threadSleep(10000);
	return *flag > 0;
}

static u32 ms(void)
{
	return (u32)(tickGetCount() / (TICK_FREQ / 1000));
}

// Text helpers (no stdio here)
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

// One HTTP exchange over a fresh connection ("Connection: close")
static int httpExchange(void)
{
	s_reply[0] = '\0';
	const bool secure = s_tls != NULL;
	SGIP_INTR_PROTECT();
	sgIP_Record_TCP* rec = sgIP_TCP_AllocRecord();
	int ok = rec // port 443 or 80, network order
		&& sgIP_TCP_Connect(rec, s_profile.ra_server, secure ? 0xBB01 : 0x5000) == 0;
	SGIP_INTR_UNPROTECT();
	if (!ok) {
		dietPrint("[net] tcp: can't start\n");
		if (rec) sgIP_TCP_FreeRecord(rec);
		return -1;
	}

	u32 start = ms();
	while (rec->tcpstate == SGIP_TCP_STATE_SYN_SENT && ms() - start < 10000) threadSleep(10000);
	if (rec->tcpstate != SGIP_TCP_STATE_ESTABLISHED) {
		dietPrint("[net] tcp: no connection (%d)\n", rec->tcpstate);
		SGIP_INTR_PROTECT();
		sgIP_TCP_Close(rec);
		SGIP_INTR_UNPROTECT();
		return -1;
	}
	dietPrint("[net] tcp: connected in %lu ms\n", ms() - start);
	if (secure) {
		u32 t = ms();
		if (tlsOpen(rec, s_tls, RANET_HOST, 15000) != 0) {
			SGIP_INTR_PROTECT();
			sgIP_TCP_Close(rec);
			SGIP_INTR_UNPROTECT();
			return -2;
		}
		dietPrint("[net] tls: resumed in %lu ms\n", ms() - t);
	}

	// Request
	static char head[512];
	u32 body_len = s_reqBody ? strlen(s_reqBody) : 0;
	u32 n = 0;
	n = append(head, n, sizeof(head), s_reqBody ? "POST " : "GET ");
	n = append(head, n, sizeof(head), s_reqPath);
	n = append(head, n, sizeof(head), " HTTP/1.1\r\nHost: " RANET_HOST "\r\nUser-Agent: " RANET_USER_AGENT "\r\n");
	if (s_reqBody) {
		char len[12];
		n = append(head, n, sizeof(head), "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ");
		n = append(head, n, sizeof(head), utoa10(body_len, len));
		n = append(head, n, sizeof(head), "\r\n");
	}
	n = append(head, n, sizeof(head), "Connection: close\r\n\r\n");
	const char* parts[2] = { head, s_reqBody };
	u32 lens[2] = { (u32)n, body_len };
	for (int i = 0; i < 2; i ++) {
		u32 sent = 0;
		while (sent < lens[i] && ms() - start < 20000) {
			int r;
			if (secure) {
				r = tlsWrite(parts[i] + sent, lens[i] - sent);
			} else {
				SGIP_INTR_PROTECT();
				r = sgIP_TCP_Send(rec, parts[i] + sent, lens[i] - sent, 0);
				SGIP_INTR_UNPROTECT();
			}
			if (r > 0) sent += r;
			else if (secure && r < 0) break;
			else threadSleep(10000);
		}
	}

	// Reply, until the server closes
	u32 got = 0;
	start = ms();
	while (ms() - start < 20000 && got < s_replySize - 1) {
		int r;
		bool open;
		if (secure) {
			r = tlsRead(s_reply + got, s_replySize - 1 - got);
			open = r == TLS_WOULD_BLOCK;
		} else {
			SGIP_INTR_PROTECT();
			r = sgIP_TCP_Recv(rec, s_reply + got, s_replySize - 1 - got, 0);
			open = rec->tcpstate == SGIP_TCP_STATE_ESTABLISHED;
			SGIP_INTR_UNPROTECT();
		}
		if (r > 0) {
			got += r;
			continue;
		}
		if (!open) break;
		threadSleep(10000);
	}
	s_reply[got] = '\0';
	if (secure) tlsClose();
	SGIP_INTR_PROTECT();
	sgIP_TCP_Close(rec);
	SGIP_INTR_UNPROTECT();

	int status = -1;
	if (!strncmp(s_reply, "HTTP/1.", 7) && s_reply[8] == ' ') {
		status = (s_reply[9] - '0') * 100 + (s_reply[10] - '0') * 10 + (s_reply[11] - '0');
	}
	dietPrint("[net] http %d, %lu bytes\n", status, (unsigned long)got);
	return status;
}

// Joins, then serves requests until ranetStop(); false if it couldn't join
static bool netSession(void);

static int netMain(void* arg)
{
	(void)arg;
	s_state = RanetState_Starting;
	// Debug: the hardware state the driver starts from (in-game vs menu)
	dietPrint("[net] SCFG_EXT %08lx CLK %04x WL %04x GPIO_WL %04x POWCNT %04x\n",
		(unsigned long)*(vu32*)0x04004008, *(vu16*)0x04004004, *(vu16*)0x04004020,
		*(vu16*)0x04004C04, *(vu16*)0x04000304);
	dietPrint("[net] MCU30 %02x MCU31 %02x TMIO1 portsel %04x clk %04x opt %04x\n",
		ranetHostI2cRead(0x4A, 0x30), ranetHostI2cRead(0x4A, 0x31),
		*(vu16*)0x04004A02, *(vu16*)0x04004A24, *(vu16*)0x04004A28);
	u32 t0 = ms();
	if (!twlwifiInit()) {
		dietPrint("[net] WiFi driver failed\n");
		s_state = RanetState_Failed;
		return -1;
	}
	dietPrint("[net] driver up in %lu ms\n", ms() - t0);
	bool ok = netSession();

	// Leave the chip as the next WiFi program expects it: not associated,
	// no keys installed, driver threads gone (else it won't come up again
	// until the console restarts)
	while (!s_stop) threadSleep(20000);
	s_iface = NULL;
	twlwifiExit();
	dietPrint("[net] WiFi off\n");
	s_state = ok ? RanetState_Off : RanetState_Failed;
	s_stopped = true;
	return 0;
}

static bool netSession(void)
{
	u32 t0;

	// The access point RA Sync last used, with the key from NVRAM
	memset(&s_bss, 0, sizeof(s_bss));
	memcpy(s_bss.bssid, s_profile.bssid, 6);
	s_bss.ssid_len = s_profile.ssid_len;
	memcpy(s_bss.ssid, s_profile.ssid, sizeof(s_bss.ssid));
	s_bss.ieee_caps = s_profile.ieee_caps;
	s_bss.ieee_basic_rates = s_profile.ieee_basic_rates;
	s_bss.ieee_all_rates = s_profile.ieee_all_rates;
	s_bss.rssi = s_profile.rssi;
	s_bss.channel = s_profile.channel;
	if (!findKey(&s_profile, &s_bss, &s_auth)) {
		dietPrint("[net] no WiFi setting for %.*s\n", s_profile.ssid_len, s_profile.ssid);
		s_state = RanetState_Failed;
		return false;
	}

	// A few tries: the chip may still be joined from the program before
	// (the first try then fails at once with a disassociation)
	s_state = RanetState_Associating;
	t0 = ms();
	bool joined = false;
	for (int attempt = 1; attempt <= 3 && !joined && !s_stop; attempt ++) {
		s_assoc = 0;
		joined = twlwifiAssociate(&s_bss, &s_auth, onAssoc, NULL) && waitFor(&s_assoc, 15000);
		if (!joined) {
			dietPrint("[net] join attempt %d failed\n", attempt);
			twlwifiDisassociate();
			threadSleep(500000);
		}
	}
	if (!joined) {
		dietPrint("[net] couldn't join the access point\n");
		memset(&s_auth, 0, sizeof(s_auth));
		s_state = RanetState_Failed;
		return false;
	}
	memset(&s_auth, 0, sizeof(s_auth));
	dietPrint("[net] joined in %lu ms\n", ms() - t0);

	// IP: RA Sync's DHCP lease
	SGIP_INTR_PROTECT();
	sgIP_Init();
	s_iface = sgIP_Hub_AddHardwareInterface(ifaceSend, NULL);
	s_iface->hwaddrlen = 6;
	memcpy(s_iface->hwaddr, g_ranetEnv.wlmgr_macaddr, 6);
	s_iface->ipaddr = s_profile.ip;
	s_iface->gateway = s_profile.gateway;
	s_iface->snmask = s_profile.netmask;
	s_iface->dns[0] = s_profile.dns[0];
	s_iface->dns[1] = s_profile.dns[1];
	s_iface->flags |= SGIP_FLAG_HWINTERFACE_ENABLED;
	sgIP_ARP_SendGratARP(s_iface);
	SGIP_INTR_UNPROTECT();
	threadPrepare(&s_tickThread, tickMain, NULL, &s_tickStack[sizeof(s_tickStack)], 0x20);
	threadStart(&s_tickThread);
	s_state = RanetState_Online;

	for (;;) {
		while (!s_reqPending && !s_stop) threadSleep(20000);
		if (s_stop) break;
		s_state = RanetState_Busy;
		s_httpStatus = httpExchange();
		s_reqPending = false;
		s_reqDone = true;
		s_state = RanetState_Online;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------

bool ranetStart(const RaNetProfile* profile, void* arena)
{
	// Off, or a session that has been stopped (then it starts over)
	if (s_state != RanetState_Off && !s_stopped) return false;
	if (profile->magic != RA_NET_PROFILE_MAGIC || profile->size != sizeof(RaNetProfile) || !profile->ra_server) return false;
	memcpy(&s_profile, profile, sizeof(s_profile));
	s_stop = s_stopped = false;
	s_reqPending = s_reqDone = false;
	s_iface = NULL;
	// Arena: thread stacks, packet buffers, then sgIP's heap
	u8* at = (u8*)arena;
	coopInit(at, RANET_STACKS_SIZE);
	at += RANET_STACKS_SIZE;
	u32 netbufs = (ranetNetbufArenaSize() + 7) & ~7;
	ranetNetbufInit(at);
	at += netbufs;
	heapInit(at, RANET_ARENA_SIZE - (at - (u8*)arena));
	threadPrepare(&s_netThread, netMain, NULL, &s_netStack[sizeof(s_netStack)], 0x20);
	threadStart(&s_netThread);
	s_state = RanetState_Starting;
	return true;
}

void ranetStop(void)
{
	s_stop = true;
}

bool ranetStopped(void)
{
	return s_stopped || s_state == RanetState_Off
		|| (s_state == RanetState_Failed && coopAllDone());
}

void ranetPoll(void)
{
	ranetPollIrq2();
	coopRun();
}

// Debug: thread states and the DSi interrupt/WiFi registers
void ranetDebugDump(void)
{
	dietPrint("[dbg] IE2 %08lx IF2 %08lx GPIO_WL %04x SCFG_EXT %08lx\n",
		(unsigned long)REG_IE2, (unsigned long)REG_IF2, *(vu16*)0x04004C04, (unsigned long)*(vu32*)0x04004008);
	coopDump();
}

RanetState ranetGetState(void)
{
	return s_state;
}

void ranetSetTlsSession(const RaTlsSession* session)
{
	s_tls = (session && session->magic == RA_TLS_SESSION_MAGIC && session->size == sizeof(RaTlsSession)
		&& session->ticket_len && session->ticket_len <= RA_TLS_TICKET_MAX) ? session : NULL;
}

bool ranetRequest(const char* path, const char* body, char* reply, u32 reply_size)
{
	if (s_state != RanetState_Online || s_reqPending || reply_size < 2) return false;
	s_reqPath = path;
	s_reqBody = body;
	s_reply = reply;
	s_replySize = reply_size;
	s_reqDone = false;
	s_reqPending = true;
	return true;
}

bool ranetRequestDone(int* http_status)
{
	if (!s_reqDone) return false;
	if (http_status) *http_status = s_httpStatus;
	return true;
}

// sgIP's sockets layer isn't built (the requests use its TCP calls directly)
void sgIP_sockets_Init(void) {}
void sgIP_sockets_Timer1000ms(void) {}

// Byte order (declared by the legacy sys/socket.h; libdswifi7 has no code)
unsigned short htons(unsigned short num) { return __builtin_bswap16(num); }
unsigned long htonl(unsigned long num) { return __builtin_bswap32(num); }
