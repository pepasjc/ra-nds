// TLS for ranet (tls.c): resumes RA Sync's session over an sgIP connection
#pragma once
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
#include "../../../include/ra_tlssession.h"

#define TLS_WOULD_BLOCK (-1000)



#ifdef RANET_NO_TLS
// Built without mbedTLS (bisecting builds): HTTPS always fails
static inline int tlsOpen(void* rec, const RaTlsSession* saved, const char* host, uint32_t timeout_ms) { return -1; }
static inline int tlsWrite(const void* data, uint32_t len) { return -1; }
static inline int tlsRead(void* buf, uint32_t size) { return -1; }
static inline void tlsClose(void) {}
#else
// Handshake on an established connection, resuming saved; 0 on success
int tlsOpen(void* rec, const RaTlsSession* saved, const char* host, uint32_t timeout_ms);

// Bytes written (0: try again later) or a negative mbedTLS error
int tlsWrite(const void* data, uint32_t len);

// Bytes read, 0 at the end, TLS_WOULD_BLOCK, or a negative mbedTLS error
int tlsRead(void* buf, uint32_t size);

void tlsClose(void);
#endif
