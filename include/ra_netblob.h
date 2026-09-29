// ranet.bin: the in-game network stack (ranet/: calico's DSi WiFi driver,
// sgIP, mbedTLS) built as a blob for nds-bootstrap-ra's ARM7 card engine,
// which loads it at RA_NET_BLOB_ADDRESS and drives it from its idle hook.
// It sends unlocks to RetroAchievements over HTTPS by resuming RA Sync's
// TLS session.  Shared with nds-bootstrap-ra (keep both copies the same);
// plain types only.
#ifndef RA_NETBLOB_H
#define RA_NETBLOB_H

#define RA_NET_BLOB_MAGIC   0x544E4152 // 'RANT'
#define RA_NET_BLOB_VERSION 2

// Where the card engine puts it: nds-bootstrap-ra's RA region (0x0CE00000,
// the DSi's extra RAM through the 0x0C000000 mirror) + 512KB.  The blob
// and everything it allocates stay inside RA_NET_BLOB_SIZE.
#define RA_NET_BLOB_ADDRESS 0x0CE80000
#define RA_NET_BLOB_SIZE    0x80000

// Longest user name and token RA hands out, with the terminator
#define RA_NET_USER_MAX  64
#define RA_NET_TOKEN_MAX 64

// What the card engine provides
struct RaNetHost {
	u32 size;                                   // sizeof(struct RaNetHost)
	u32 (*ticks)(void);                         // free-running, 33.5 MHz / 64; may wrap
	u32 (*entropy)(void);                       // anything that varies
	u8 (*i2cRead)(u8 dev, u8 reg);              // DSi I2C (MCU: the WiFi reset line)
	int (*i2cWrite)(u8 dev, u8 reg, u8 data);   // nonzero on success
	int (*nvramRead)(void* dst, u32 addr, u32 len); // firmware flash; nonzero on success
	void (*log)(const char* text, u32 len);     // debug text; may be NULL
};

// Everything a session needs, filled by the loader and card engine
struct RaNetConfig {
	u32 size;                        // sizeof(struct RaNetConfig)
	const void* profile;             // RaNetProfile (ra_netprofile.h)
	const void* tls;                 // RaTlsSession (ra_tlssession.h)
	char user[RA_NET_USER_MAX];
	char token[RA_NET_TOKEN_MAX];
	char md5[33];                    // the game's hash
};

enum RaNetState {
	RA_NET_OFF = 0,
	RA_NET_STARTING,     // WiFi driver coming up
	RA_NET_JOINING,      // access point, WPA
	RA_NET_ONLINE,       // idle, ready
	RA_NET_BUSY,         // sending
	RA_NET_FAILED,       // gave up this session (unlocks stay for RA Sync)
};

// Result of one award
enum RaNetAward {
	RA_AWARD_PENDING = 0,
	RA_AWARD_SENT,       // RA said Success (or "already unlocked")
	RA_AWARD_REFUSED,    // RA answered but refused: RA Sync won't retry either
	RA_AWARD_FAILED,     // no answer: RA Sync will send it
};

// First bytes of ranet.bin.  Every call is from the card engine's ARM7
// context, never from an interrupt.
struct RaNetHeader {
	u32 magic;
	u32 version;
	u32 imageEnd;           // end of what to load (code and data)
	u32 bssEnd;             // RA_NET_BLOB_ADDRESS + everything it uses
	// Clears .bss, keeps host and config (copied); nonzero on success.
	// Nothing touches the WiFi hardware until the first award.
	int (*init)(const struct RaNetHost* host, const struct RaNetConfig* config);
	// Queues an award (starting WiFi if needed); seq is the unlock
	// record's sequence number, for the result.  Nonzero if queued.
	int (*award)(u32 seq, u32 achievementId, int hardcore, u32 secondsSinceUnlock);
	// Runs the stack a little: call often (every frame, idle time)
	void (*poll)(void);
	// Next finished award: nonzero with *seq and *result filled
	int (*result)(u32* seq, u32* result);
	u32 (*state)(void);     // enum RaNetState
	// WiFi off (disassociate, driver down); keep polling until stopped()
	void (*stop)(void);
	int (*stopped)(void);
	// The in-game menu's switch: 0 turns WiFi off and keeps it off (awards
	// are refused); 1 lets it come up again
	void (*enable)(int on);
};

#endif
