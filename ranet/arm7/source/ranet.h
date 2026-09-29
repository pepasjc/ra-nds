// In-game network stack for RetroAchievements (ARM7, no OS): calico's DSi
// WiFi driver on cooperative threads, sgIP for TCP/IP, plain HTTP to the
// RA server.  Needs RA Sync's network profile (ra_netprofile.h) and the
// console's WiFi settings (NVRAM) for the key.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
#include "../../../include/ra_netprofile.h"

#define RANET_HOST "retroachievements.org"
#define RANET_USER_AGENT "RA-NDS/0.1 (Nintendo DSi) rcheevos/12.5"

typedef enum RanetState {
	RanetState_Off = 0,
	RanetState_Starting,      // WiFi driver coming up
	RanetState_Associating,   // joining the access point (and WPA)
	RanetState_Online,        // IP up, ready for requests
	RanetState_Busy,          // a request is running
	RanetState_Failed,
} RanetState;

// Host: reads the console's NVRAM (SPI flash)
bool ranetHostNvramRead(void* dst, u32 addr, u32 len);

// Starts bringing the network up in the background; arena: memory for
// packets and TCP (RANET_ARENA_SIZE bytes, word aligned)
#define RANET_ARENA_SIZE (256 * 1024)
bool ranetStart(const RaNetProfile* profile, void* arena);

// Call often (every frame at least): runs the stack for a while
void ranetPoll(void);

RanetState ranetGetState(void);

// Disconnects and shuts the WiFi driver down (keep calling ranetPoll()
// until ranetStopped()): a later WiFi program, or the next start, finds
// the chip as it expects
void ranetStop(void);
bool ranetStopped(void);

// Debug: logs thread states and WiFi registers
void ranetDebugDump(void);

// Requests go over HTTPS, resuming this session (RA Sync's tls.bin; must
// stay valid while in use); NULL or invalid: plain HTTP
#include "../../../include/ra_tlssession.h"
void ranetSetTlsSession(const RaTlsSession* session);

// One HTTP GET/POST to the RA server, in the background.  path: e.g.
// "/dorequest.php?r=...", body NULL for GET.  The reply (status line,
// headers and body, cut at reply_size - 1) lands in reply, 0-terminated;
// ranetRequestDone() then turns true.
bool ranetRequest(const char* path, const char* body, char* reply, u32 reply_size);
bool ranetRequestDone(int* http_status);
