// ranet test app: ARM9 -> ARM7 start message (FIFO_USER_01)
#pragma once

#define RANET_LOG_SIZE 8192

typedef struct {
	volatile u32 beat;      // ARM7 main loop passes (alive?)
	volatile u32 head;      // bytes written so far (ring position = head % size)
	char text[RANET_LOG_SIZE];
} RanetLog;

typedef struct {
	u32 profile;   // RaNetProfile*
	u32 arena;     // RANET_ARENA_SIZE bytes
	u32 log;       // RanetLog*
	u32 reply;     // reply buffer
	u32 replySize;
	u32 tls;       // RaTlsSession* or 0: plain HTTP
	char path[160];
} RanetStartMsg;

// ARM7 -> ARM9 status word (FIFO_USER_02): state | http status << 8
