// sd:/_nds/ra/tls.bin: the TLS 1.2 session RA Sync / RA Prep / RA Tool last
// had with the RA server, so that nds-bootstrap-ra can resume it while a
// game runs (a resumed handshake needs no public-key math, which the ARM7
// can't do quickly).  Holds the session's master secret: as sensitive as
// the token in account.txt.  Shared with nds-bootstrap-ra; plain types.
#ifndef RA_TLSSESSION_H
#define RA_TLSSESSION_H

#define RA_TLS_SESSION_MAGIC 0x53544152 // 'RATS'
#define RA_TLS_SESSION_VERSION 1
#define RA_TLS_TICKET_MAX 1024

typedef struct RaTlsSession {
    u32 magic;
    u16 version;
    u16 size;               // sizeof(RaTlsSession)
    u32 saved;              // unix time when saved (0: unknown)
    u32 ticket_lifetime;    // seconds, the server's hint
    s32 ciphersuite;        // IANA id
    u8 id_len;
    u8 encrypt_then_mac;
    u8 mfl_code;
    u8 reserved;
    u8 id[32];
    u8 master[48];
    u16 ticket_len;
    u16 reserved2;
    u8 ticket[RA_TLS_TICKET_MAX];
} RaTlsSession;

#endif
