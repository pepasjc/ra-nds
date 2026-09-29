// ranet test app, ARM9: loads RA Sync's network profile, starts the ARM7
// network stack and shows its log (also sd:/ranet_log.txt).
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
#include "../../../include/ra_netprofile.h"
#include "../../common.h"

#define RANET_ARENA_SIZE (160 * 1024)
#define REPLY_SIZE 2048

static const char* stateName(int s)
{
	static const char* names[] = { "off", "starting WiFi", "joining AP", "online", "request", "FAILED" };
	return s >= 0 && s < 6 ? names[s] : "?";
}

int main(void)
{
	consoleDemoInit();
	iprintf("ranet: RA over WiFi from ARM7\n\n");
	if (!fatInitDefault()) {
		iprintf("SD card not readable\n");
		while (1) swiWaitForVBlank();
	}
	FILE* logf = fopen("sd:/ranet_log.txt", "a");

	static RaNetProfile profile;
	FILE* f = fopen("sd:/_nds/ra/net.bin", "rb");
	int ok = f && fread(&profile, 1, sizeof(profile), f) == sizeof(profile);
	if (f) fclose(f);
	if (!ok || profile.magic != RA_NET_PROFILE_MAGIC) {
		iprintf("No sd:/_nds/ra/net.bin:\nrun RA Tool (A) once first\n");
		while (1) swiWaitForVBlank();
	}
	u8* ip = (u8*)&profile.ip;
	u8* srv = (u8*)&profile.ra_server;
	iprintf("AP %.*s ch %d\nIP %d.%d.%d.%d\nRA %d.%d.%d.%d\n\n", profile.ssid_len, profile.ssid, profile.channel,
		ip[0], ip[1], ip[2], ip[3], srv[0], srv[1], srv[2], srv[3]);

	// Memory the ARM7 uses, uncached through the 0x0C mirror isn't needed:
	// the ARM9 only reads the log and reply after invalidating
	void* arena = memalign(32, RANET_ARENA_SIZE);
	static RanetLog log ALIGN(32);
	static char reply[REPLY_SIZE] ALIGN(32);
	memset(&log, 0, sizeof(log));
	memset(reply, 0, sizeof(reply));
	DC_FlushAll();

	static RanetStartMsg msg;
	msg.profile = (u32)&profile;
	msg.arena = (u32)arena;
	msg.log = (u32)&log;
	msg.reply = (u32)reply;
	msg.replySize = REPLY_SIZE;
	snprintf(msg.path, sizeof(msg.path), "/dorequest.php?r=gameid&m=00000000000000000000000000000000");
	DC_FlushAll();
	u32 t0 = 0;
	fifoSendAddress(FIFO_USER_01, &msg); // too big for a FIFO data message

	// The log is kept here and written at the end: SD access on the ARM9
	// goes through the ARM7, which may be the one that's stuck
	static char saved[64 * 1024];
	u32 savedLen = 0;
#define SAVE(...) do { if (savedLen < sizeof(saved) - 256) savedLen += snprintf(saved + savedLen, sizeof(saved) - savedLen, __VA_ARGS__); } while (0)

	u32 shown = 0, frames = 0, lastBeat = 0, stuckFrames = 0;
	while (1) {
		swiWaitForVBlank();
		frames ++;
		DC_InvalidateRange(&log, sizeof(log));
		while (shown < log.head) {
			char c = log.text[shown % RANET_LOG_SIZE];
			iprintf("%c", c);
			if (savedLen < sizeof(saved) - 1) saved[savedLen ++] = c;
			shown ++;
		}
		while (fifoCheckValue32(FIFO_USER_02)) {
			u32 v = fifoGetValue32(FIFO_USER_02);
			int state = v & 0xFF;
			if (v & 0x80000000) {
				int http = (v >> 8) & 0xFFFF;
				DC_InvalidateRange(reply, sizeof(reply));
				const char* body = strstr(reply, "\r\n\r\n");
				iprintf("\n\x1b[32mHTTP %d\x1b[39m after %lu s\n%.200s\n", http, (frames - t0) / 60, body ? body + 4 : reply);
				SAVE("HTTP %d\n%s\n", http, reply);
			} else {
				iprintf("\x1b[33m[%lus] %s\x1b[39m\n", frames / 60, stateName(state));
				SAVE("[%lus] %s\n", frames / 60, stateName(state));
			}
		}
		// ARM7 heartbeat, top right
		if (log.beat != lastBeat) {
			lastBeat = log.beat;
			stuckFrames = 0;
		} else if (++stuckFrames == 120) {
			iprintf("\x1b[31m[%lus] ARM7 not responding\x1b[39m\n", frames / 60);
			SAVE("[%lus] ARM7 not responding (beat %lu)\n", frames / 60, lastBeat);
		}
		iprintf("\x1b[s\x1b[0;22H%08lx\x1b[u", lastBeat);
		scanKeys();
		if (keysDown() & KEY_START) break;
	}
	iprintf("\nSaving the log...\n");
	if (logf) {
		fprintf(logf, "--- run\n");
		fwrite(saved, 1, savedLen, logf);
		fclose(logf);
	}
	return 0;
}
