// What RA Prep, RA Sync (app/rasync.c) and RA Tool (app/ratool.c) share:
// the SD card files, their signatures, preparing a ROM's set, and sending
// the unlocks nds-bootstrap-ra recorded.
#ifndef RA_SYNC_H
#define RA_SYNC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ra_client.h"

#define RA_UNLOCKS_FILE RA_DIR "/unlocks.bin"
#define RA_HISTORY_FILE RA_DIR "/unlocks_history.txt"
#define RA_SUBMITTED_FILE RA_DIR "/ra_submitted.txt"
#define RA_RAMDUMP "sd:/_nds/nds-bootstrap/ramDump.bin"

// Hardcore unlocks are recorded but not sent as hardcore until
// RetroAchievements accepts this client (nds-bootstrap-ra's
// RA_HARDCORE_AVAILABLE); until then everything goes as softcore.
#define RA_SUBMIT_HARDCORE 0

// Log file ra_say() copies the screen output to (NULL: screen only)
extern FILE *ra_logf;
void ra_say(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Derives the console key.  1 when there is one (an eMMC CID).
int ra_sync_init(void);
int ra_have_key(void);

// A short, harmless id of the console key (the first 4 bytes of its HMAC of
// "RA-NDS key id", hex): nds-bootstrap-ra writes the same to
// loader_status.txt, to check both derive the same key
void ra_key_id(char out[9]);

// Line n (0-based) of a small file, without its line end.  0 on success.
int ra_read_line(const char *path, int n, char *out, size_t size);
void ra_write_text(const char *path, const char *text);

// The hash a set (its "game" line) or .none file was made for, and the
// set's game id.  1 when there is one.
int ra_known_hash(const char *set_path, const char *none_path, char out[33], uint32_t *game_id);

// Returns 1 once RetroAchievements can be reached (connecting if needed)
typedef int (*ra_online_fn)(void);

// Makes sure sets/<rom file name>.* are right for this ROM: hashes it; if
// the set (or .none) matches and is signed, only writes <rom>.id (when a
// fingerprint from nds-bootstrap-ra is given); otherwise fetches the set
// from RA, signs it and saves the account's unlocks, or writes .none.  1 on
// success (set or .none in place), 0 otherwise.  *went_online: RA was used.
int ra_prepare_rom(const ra_account *account, int have_account, const char *rom, const char *fingerprint,
                   ra_online_fn online, int *went_online);

// The last session's unlocks from the ring in ramDump.bin into unlocks.bin
// (verified ones only).  Returns how many moved.
int ra_move_ring(void);

// Unlocks in unlocks.bin not sent yet
long ra_pending_unlocks(void);

// Sends the pending unlocks; RetroAchievements must be reachable
void ra_send_unlocks(const ra_account *account);

// sets/<name>.unl: the account's unlocks for the game, as RA lists them
void ra_write_account_unlocks(const ra_account *account, const char *name, uint32_t game_id);

#endif
