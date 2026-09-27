// RetroAchievements calls the DSi apps share, on top of https.c and
// rcheevos' rc_api, plus the DSi plumbing around them (account file, WiFi,
// Unlaunch).
#ifndef RA_CLIENT_H
#define RA_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define RA_DIR "sd:/_nds/ra"
#define RA_SETS_DIR RA_DIR "/sets"
#define RA_ACCOUNT_FILE RA_DIR "/account.txt"

#define RA_USER_AGENT "RADirectDS/0.3 (Nintendo DSi) rcheevos/12.5"

typedef struct {
    char user[64];
    char token[64];
    char password[128]; // only until the first login, then replaced by the token
    int submit;      // "submit=1": send unlocks to RA; otherwise a dry run
} ra_account;

// account.txt: "user=", "token=" (the connect token) and "submit=" lines,
// or "password=" instead of the token for the first run.  0 when user and
// token or password are there.
int ra_account_load(ra_account *account);

// Rewrites account.txt with user, token and submit (no password)
int ra_account_save(const ra_account *account);

// Logs in with the password and keeps RA's connect token in account; the
// password is wiped from memory either way.  0 on success.
int ra_login_password(ra_account *account);

// Saved WiFi connections, a few tries a second or two apart (the first try
// on a DSi sometimes fails).  1 when connected.
int ra_wifi_connect(int attempts);

// Logs in with the token: checks it and prints the user's score.  0 on success.
int ra_login(const ra_account *account);

// Game id for a ROM hash (0 when RA doesn't know the hash).  0 on success,
// -1 when RA couldn't be asked.
int ra_resolve_hash(const char *md5, uint32_t *game_id);

// The game's set, rendered as the engine reads it (raset.h), malloc'd.
// 0 on success.
int ra_fetch_set(const ra_account *account, uint32_t game_id, const char *md5, char **set,
                 size_t *length, unsigned *achievements);

// The account's unlocks for a game in one mode: a malloc'd id array.
// 0 on success.
int ra_fetch_user_unlocks(const ra_account *account, uint32_t game_id, int hardcore, uint32_t **ids,
                          uint32_t *count);

enum { RA_AWARD_OK, RA_AWARD_ALREADY, RA_AWARD_REFUSED, RA_AWARD_NETWORK };

// An unlock (hardcore 0: softcore).  RA_AWARD_REFUSED puts RA's message in
// error.
int ra_award(const ra_account *account, uint32_t achievement_id, const char *md5, int hardcore,
             uint32_t seconds_since_unlock, char *error, size_t error_size);

// Ask Unlaunch to boot `path` after the next restart; returning from main()
// in DSi mode then restarts the console.
void ra_unlaunch_autoload(const char *path);

#endif
