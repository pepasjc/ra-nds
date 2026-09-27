// RA Sync (direct): RetroAchievements for the DSi without the Pi.
//
// nds-bootstrap-ra restarts the console into this app (through Unlaunch, in
// full DSi mode, so WPA2 works) at two points, and the app talks to
// retroachievements.org itself:
//
//  - Prep, before a game whose set is missing, unsigned or for another ROM
//    (sd:/_nds/ra/raprep.nds; nds-bootstrap wrote prep.txt: the ROM, the
//    loader, the ROM's fingerprint): hash the ROM, ask RA for its game,
//    write the set to sd:/_nds/ra/sets/<rom>.txt with its signature
//    (<rom>.sig) and the account's unlocks (<rom>.unl), or <rom>.none when
//    RA has no set for it.  If RA can't be reached, skip_once.txt lets the
//    game start without a set this once.  Then the loader starts again.
//
//  - Sync, when a game with a set is quit (sd:/_nds/ra/rasync.nds;
//    nds-bootstrap saved the real quit target and the game in return.txt):
//    move the session's signed unlocks from ramDump.bin into unlocks.bin and
//    send the new ones to RA, softcore only for now.  Records whose signature
//    doesn't verify are never sent.  How far unlocks.bin has been sent is in
//    ra_submitted.txt.  Without "submit=1" in account.txt it is a dry run.
//
// Signatures are HMAC-SHA256 under the console key (ra_key.c), the same key
// nds-bootstrap-ra signs with; it is derived in RAM and never stored.
//
// Both end by asking Unlaunch for the next program and restarting.
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "https.h"
#include "nds_hash.h"
#include "ra_client.h"
#include "ra_key.h"

#define PREP_FILE RA_DIR "/prep.txt"
#define SKIP_FILE RA_DIR "/skip_once.txt"
#define AFTER_PREP_FILE RA_DIR "/after_prep.txt"
#define RETURN_FILE RA_DIR "/return.txt"
#define UNLOCKS_FILE RA_DIR "/unlocks.bin"
#define HISTORY_FILE RA_DIR "/unlocks_history.txt"
#define SUBMITTED_FILE RA_DIR "/ra_submitted.txt"
#define SYNC_LOG RA_DIR "/rasync_log.txt"
#define RAMDUMP "sd:/_nds/nds-bootstrap/ramDump.bin"
#define DEFAULT_RETURN "sd:/_nds/TWiLightMenu/main.srldr"
#define DEFAULT_LOADER "sd:/_nds/nds-bootstrap-nightly.nds"

// Hardcore unlocks are recorded but not sent as hardcore until
// RetroAchievements accepts this client (nds-bootstrap-ra's
// RA_HARDCORE_AVAILABLE); until then everything goes as softcore.
#define SUBMIT_HARDCORE 0

// Unlock ring in ramDump.bin (nds-bootstrap-ra retail/common/include/ra_engine.h)
#define RA_DUMP_UNLOCK_OFFSET 0x01FE0000
#define RA_UNLOCK_RECORDS 512
#define RA_UNLOCK_MAGIC 0x32554152  // 'RAU2'

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t achievement_id;
    uint32_t game_id;
    uint32_t points;
    uint32_t frame;
    uint8_t rtc[8];     // year-2000, month, day, weekday, hour, minute, second, flags (bit 0: hardcore)
    char md5[32];
} RaUnlockRecord;

typedef struct {
    RaUnlockRecord record;
    uint8_t mac[32];    // HMAC-SHA256 of record under the console key
} RaSignedUnlock;

_Static_assert(sizeof(RaUnlockRecord) == 64, "RaUnlockRecord must be 64 bytes");
_Static_assert(sizeof(RaSignedUnlock) == 96, "RaSignedUnlock must be 96 bytes");

static FILE *logf;
static unsigned char console_key[32];
static int have_key;

// Screen and sd:/_nds/ra/rasync_log.txt
#define SAY(...) do { \
        iprintf(__VA_ARGS__); \
        if (logf) { fprintf(logf, __VA_ARGS__); fflush(logf); } \
    } while (0)

static void pause_frames(int frames) {
    while (frames-- > 0) swiWaitForVBlank();
}

// Line n (0-based) of a small file, without its line end
static int read_line_n(const char *path, int n, char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int ok = 0;
    for (int i = 0; i <= n; i++) ok = fgets(out, size, f) != NULL;
    fclose(f);
    if (!ok) out[0] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    return ok ? 0 : -1;
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static char *read_all(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = n > 0 ? malloc(n + 1) : NULL;
    if (data && fread(data, 1, n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) {
        data[n] = '\0';
        *length = n;
    }
    return data;
}

// ---------------------------------------------------------------------------
// Signatures
// ---------------------------------------------------------------------------

static int unlock_ok(const RaSignedUnlock *u) {
    if (!have_key || u->record.magic != RA_UNLOCK_MAGIC) return 0;
    unsigned char mac[32];
    ra_hmac(console_key, &u->record, sizeof(u->record), mac);
    return memcmp(mac, u->mac, 32) == 0;
}

// <rom>.sig: the set's HMAC in hex; nds-bootstrap-ra loads only sets that match
static int write_set_sig(const char *name, const char *set, size_t length) {
    char path[320], hex[65];
    snprintf(path, sizeof(path), RA_SETS_DIR "/%s.sig", name);
    ra_hmac_hex(console_key, set, length, hex);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fputs(hex, f) >= 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

static int set_sig_ok(const char *name, const char *set_path) {
    char path[320], stored[80], hex[65];
    size_t length = 0;
    snprintf(path, sizeof(path), RA_SETS_DIR "/%s.sig", name);
    if (!have_key || read_line_n(path, 0, stored, sizeof(stored))) return 0;
    char *set = read_all(set_path, &length);
    if (!set) return 0;
    ra_hmac_hex(console_key, set, length, hex);
    free(set);
    return strncmp(stored, hex, 64) == 0;
}

// <rom>.unl: the account's unlocks for the game as RA lists them,
// "S <ids>" and "H <ids>"; nds-bootstrap-ra doesn't pop those up again
static void write_account_unlocks(const ra_account *account, const char *name, uint32_t game_id) {
    char path[320];
    snprintf(path, sizeof(path), RA_SETS_DIR "/%s.unl", name);
    FILE *f = NULL;
    for (int hardcore = 0; hardcore <= 1; hardcore++) {
        uint32_t *ids = NULL, count = 0;
        if (ra_fetch_user_unlocks(account, game_id, hardcore, &ids, &count)) {
            SAY("(account unlocks not updated: %.60s)\n", https_last_error());
            if (f) fclose(f);
            return;
        }
        if (!f && !(f = fopen(path, "wb"))) {
            free(ids);
            return;
        }
        fputc(hardcore ? 'H' : 'S', f);
        for (uint32_t i = 0; i < count; i++) fprintf(f, " %lu", (unsigned long)ids[i]);
        fputc('\n', f);
        if (hardcore == 0) SAY("Account: %lu unlocked here before\n", (unsigned long)count);
        free(ids);
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Prep: fetch the set of the game about to start
// ---------------------------------------------------------------------------

// The ROM's fingerprint (size, header CRC, file time) as nds-bootstrap-ra
// computed it and passed it in prep.txt.  Kept verbatim: the loader's FAT
// library might render the file time differently from this one.
static char fingerprint[64];

// <rom>.id: the fingerprint, which the loader checks at every start
// (ra_boot.cpp romMatchesId), then the ROM's hash.  0 on success.
static int write_rom_id(const char *id_path, const char *md5) {
    if (!fingerprint[0]) return -1;
    char text[128];
    snprintf(text, sizeof(text), "%s\n%s\n", fingerprint, md5);
    FILE *out = fopen(id_path, "wb");
    if (!out) return -1;
    int ok = fputs(text, out) >= 0;
    return fclose(out) == 0 && ok ? 0 : -1;
}

// The hash the current set (its "game" line) or .none file was made for,
// and the set's game id
static int known_hash(const char *set_path, const char *none_path, char out[33], uint32_t *game_id) {
    char line[256];
    out[0] = '\0';
    *game_id = 0;
    FILE *f = fopen(set_path, "rb");
    if (f) {
        // "RASET\t1", then "game\t<id>\t<md5>\t<title>"
        if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f) && !strncmp(line, "game\t", 5)) {
            *game_id = strtoul(line + 5, NULL, 10);
            char *md5 = strchr(line + 5, '\t');
            if (md5 && strlen(md5 + 1) >= 32) memcpy(out, md5 + 1, 32);
        }
        fclose(f);
    } else if (read_line_n(none_path, 0, line, sizeof(line)) == 0 && strlen(line) >= 32) {
        memcpy(out, line, 32);
    }
    out[out[0] ? 32 : 0] = '\0';
    return out[0] != '\0';
}

static void prep(const ra_account *account, int have_account) {
    char rom[256] = "", loader[256] = "";
    FILE *f = fopen(PREP_FILE, "rb");
    if (f) {
        if (fgets(rom, sizeof(rom), f)) rom[strcspn(rom, "\r\n")] = '\0';
        if (fgets(loader, sizeof(loader), f)) loader[strcspn(loader, "\r\n")] = '\0';
        if (fgets(fingerprint, sizeof(fingerprint), f)) fingerprint[strcspn(fingerprint, "\r\n")] = '\0';
        fclose(f);
    }
    remove(PREP_FILE);
    if (!loader[0]) snprintf(loader, sizeof(loader), "%s", DEFAULT_LOADER);

    const char *name = strrchr(rom, '/');
    name = name ? name + 1 : rom;
    SAY("Achievements for\n %.60s\n\n", name);

    char set_path[320], none_path[320], id_path[320], sig_path[320], unl_path[320];
    snprintf(set_path, sizeof(set_path), RA_SETS_DIR "/%s.txt", name);
    snprintf(none_path, sizeof(none_path), RA_SETS_DIR "/%s.none", name);
    snprintf(id_path, sizeof(id_path), RA_SETS_DIR "/%s.id", name);
    snprintf(sig_path, sizeof(sig_path), RA_SETS_DIR "/%s.sig", name);
    snprintf(unl_path, sizeof(unl_path), RA_SETS_DIR "/%s.unl", name);

    // Unless a set (or "none") is there for this ROM, the next start plays
    // without
    int done = 0, fetch = 1;
    char md5[33], known[33];
    uint32_t known_game = 0;
    int hashed = rom[0] && nds_hash_file(rom, md5);
    if (!hashed) {
        SAY("\x1b[31mCan't read the ROM\x1b[39m\n");
        fetch = 0;
    } else if (!have_key) {
        SAY("\x1b[31mNo console key (eMMC CID)\x1b[39m\n");
        fetch = 0;
    } else if (known_hash(set_path, none_path, known, &known_game)) {
        // nds-bootstrap sent us because the ROM's .id or the set's signature
        // is missing or wrong
        int is_set = known_game != 0;
        if (strcmp(known, md5) != 0) {
            SAY("\x1b[33mA different ROM:\x1b[39m\n was %s\n now %s\n\n", known, md5);
        } else if (is_set && !set_sig_ok(name, set_path)) {
            SAY("Set not signed by this console:\nfetching it again\n");
        } else {
            SAY("ROM checked: achievements\nunchanged\n");
            done = write_rom_id(id_path, md5) == 0;
            fetch = 0;
        }
    }
    if (fetch && !done) {
        remove(set_path);
        remove(none_path);
        remove(id_path);
        remove(sig_path);
        remove(unl_path);
    }

    if (!fetch || done) {
        // nothing to fetch
    } else if (!have_account) {
        SAY("No account in\n " RA_ACCOUNT_FILE "\n");
    } else if (!ra_wifi_connect(3) || https_init(RA_USER_AGENT)) {
        SAY("\x1b[31mNo connection\x1b[39m\n");
    } else {
        uint32_t game_id = 0;
        if (ra_resolve_hash(md5, &game_id)) {
            SAY("\x1b[31mRetroAchievements didn't answer\x1b[39m\n %.80s\n", https_last_error());
        } else if (game_id == 0) {
            write_text(none_path, md5);
            SAY("No achievements for this ROM\n(hash %s)\n", md5);
            done = write_rom_id(id_path, md5) == 0;
        } else {
            char *set = NULL;
            size_t length = 0;
            unsigned count = 0;
            if (ra_fetch_set(account, game_id, md5, &set, &length, &count) == 0) {
                char tmp[330];
                snprintf(tmp, sizeof(tmp), "%s.tmp", set_path);
                FILE *out = fopen(tmp, "wb");
                if (out) {
                    int ok = fwrite(set, 1, length, out) == length;
                    ok = fclose(out) == 0 && ok;
                    remove(set_path);
                    done = ok && rename(tmp, set_path) == 0 && write_set_sig(name, set, length) == 0
                           && write_rom_id(id_path, md5) == 0;
                }
                if (done) {
                    SAY("\x1b[32m%u achievements\x1b[39m (game %lu)\n", count, (unsigned long)game_id);
                    write_account_unlocks(account, name, game_id);
                } else {
                    SAY("\x1b[31mCan't write the set\x1b[39m\n");
                }
                free(set);
            } else {
                SAY("\x1b[31mNo set from RetroAchievements\x1b[39m\n %.80s\n", https_last_error());
            }
        }
        https_close();
    }
    if (!done && rom[0]) {
        write_text(SKIP_FILE, rom);
        SAY("Starting without achievements;\nnext start tries again\n");
    }
    pause_frames(90);
    // The loader started from here can't quit straight to TWiLight Menu++
    // (the game hangs on its last frame); this tells it to quit through RA
    // Sync, which returns through Unlaunch
    if (rom[0]) write_text(AFTER_PREP_FILE, rom);
    SAY("\nStarting the game...\n");
    ra_unlaunch_autoload(loader);
}

// ---------------------------------------------------------------------------
// Sync: send the new unlocks
// ---------------------------------------------------------------------------

static int record_seq_compare(const void *a, const void *b) {
    uint32_t sa = ((const RaSignedUnlock *)a)->record.seq, sb = ((const RaSignedUnlock *)b)->record.seq;
    return (sa > sb) - (sa < sb);
}

// The last session's unlocks, from the ring in ramDump.bin into unlocks.bin
// (as nds-bootstrap-ra's flushUnlocks does at the next start): only records
// that verify, with a line each in unlocks_history.txt.  The ring is cleared
// once they are in.
static int move_ring(void) {
    if (!have_key) return 0;  // can't check them: the ring stays
    FILE *dump = fopen(RAMDUMP, "r+b");
    if (!dump) return 0;
    const size_t ring_size = RA_UNLOCK_RECORDS * sizeof(RaSignedUnlock);
    RaSignedUnlock *ring = malloc(ring_size);
    size_t got = 0;
    if (ring && fseek(dump, RA_DUMP_UNLOCK_OFFSET, SEEK_SET) == 0)
        got = fread(ring, sizeof(RaSignedUnlock), RA_UNLOCK_RECORDS, dump);
    int valid = 0, rejected = 0;
    for (size_t i = 0; i < got; i++) {
        if (ring[i].record.magic != RA_UNLOCK_MAGIC) continue;
        if (unlock_ok(&ring[i])) ring[valid++] = ring[i];
        else rejected++;
    }
    if (valid || rejected) {
        qsort(ring, valid, sizeof(RaSignedUnlock), record_seq_compare);
        FILE *bin = fopen(UNLOCKS_FILE, "ab");
        if (bin && fwrite(ring, sizeof(RaSignedUnlock), valid, bin) == (size_t)valid && fclose(bin) == 0) {
            FILE *history = fopen(HISTORY_FILE, "ab");
            for (int i = 0; history && i < valid; i++) {
                const RaUnlockRecord *r = &ring[i].record;
                fprintf(history, "20%02u-%02u-%02u %02u:%02u:%02u\t%lu\t%lu\t%.32s\t%lu\t%s\n", r->rtc[0],
                        r->rtc[1], r->rtc[2], r->rtc[4], r->rtc[5], r->rtc[6], (unsigned long)r->achievement_id,
                        (unsigned long)r->game_id, r->md5, (unsigned long)r->points,
                        (r->rtc[7] & 1) ? "hardcore" : "softcore");
            }
            if (history && rejected) fprintf(history, "(%d records failed their signature check and were dropped)\n", rejected);
            if (history) fclose(history);
            memset(ring, 0, ring_size);
            fseek(dump, RA_DUMP_UNLOCK_OFFSET, SEEK_SET);
            fwrite(ring, 1, ring_size, dump);
        } else {
            if (bin) fclose(bin);
            valid = 0;  // the ring stays for next time
        }
    }
    free(ring);
    fclose(dump);
    if (rejected) SAY("\x1b[33m%d unlock%s failed the signature check\x1b[39m\n", rejected, rejected == 1 ? "" : "s");
    return valid;
}

// ra_submitted.txt: "<records sent>\n<MAC of the first record, hex>\n"; a
// different first record means a new unlocks.bin (then it starts over: RA
// answers "already has" to repeats)
static long load_sent(const char *first_mac, long records) {
    char line[160], mac[80] = "";
    if (read_line_n(SUBMITTED_FILE, 0, line, sizeof(line))) return 0;
    long sent = strtol(line, NULL, 10);
    read_line_n(SUBMITTED_FILE, 1, mac, sizeof(mac));
    return (sent < 0 || sent > records || strcmp(mac, first_mac) != 0) ? 0 : sent;
}

static void save_sent(long sent, const char *first_mac) {
    char text[128];
    snprintf(text, sizeof(text), "%ld\n%s\n", sent, first_mac);
    write_text(SUBMITTED_FILE, text);
}

static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// Seconds since the unlock, both times by the DS clock
static uint32_t seconds_ago(const RaUnlockRecord *r, time_t now) {
    const uint8_t *t = r->rtc;
    if (t[1] < 1 || t[1] > 12) return 0;
    long long logged = (long long)days_from_civil(2000 + t[0], t[1], t[2]) * 86400 + t[4] * 3600 + t[5] * 60 + t[6];
    long long ago = (long long)now - logged;
    return ago < 0 ? 0 : ago > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)ago;
}

static void sync(const ra_account *account, int have_account, const char *played_rom) {
    int moved = move_ring();
    if (moved) SAY("%d new unlock%s from the game\n", moved, moved == 1 ? "" : "s");

    FILE *bin = fopen(UNLOCKS_FILE, "rb");
    if (!bin) {
        SAY("No unlocks yet\n");
        return;
    }
    fseek(bin, 0, SEEK_END);
    long records = ftell(bin) / (long)sizeof(RaSignedUnlock);
    RaSignedUnlock first;
    char first_mac[65] = "";
    fseek(bin, 0, SEEK_SET);
    if (records && fread(&first, sizeof(first), 1, bin) == 1) {
        for (int i = 0; i < 32; i++) snprintf(first_mac + i * 2, 3, "%02x", first.mac[i]);
    }
    long sent_before = load_sent(first_mac, records);
    long pending = records - sent_before;
    if (pending <= 0) {
        fclose(bin);
        SAY("Nothing new to send\n");
        return;
    }
    SAY("%ld unlock%s to send\n", pending, pending == 1 ? "" : "s");
    if (!have_account) {
        fclose(bin);
        SAY("No account in\n " RA_ACCOUNT_FILE "\n");
        return;
    }
    if (!account->submit) {
        fclose(bin);
        SAY("Dry run (submit=1 in account.txt\nsends them): kept for later\n");
        return;
    }
    if (!ra_wifi_connect(3) || https_init(RA_USER_AGENT)) {
        fclose(bin);
        SAY("\x1b[31mNo connection:\x1b[39m kept for next time\n");
        return;
    }

    time_t now = time(NULL);
    int sent = 0, already = 0, refused = 0, forged = 0, stopped = 0;
    long done = sent_before;
    RaSignedUnlock u;
    fseek(bin, done * (long)sizeof(RaSignedUnlock), SEEK_SET);
    while (fread(&u, sizeof(u), 1, bin) == 1) {
        const RaUnlockRecord *r = &u.record;
        if (!unlock_ok(&u)) {
            forged++;
            SAY("\x1b[31m%lu: bad signature, not sent\x1b[39m\n", (unsigned long)r->achievement_id);
        } else {
            char md5[33], error[96];
            memcpy(md5, r->md5, 32);
            md5[32] = '\0';
            int hardcore = SUBMIT_HARDCORE && (r->rtc[7] & 1);
            int result = ra_award(account, r->achievement_id, md5, hardcore, seconds_ago(r, now), error, sizeof(error));
            if (result == RA_AWARD_NETWORK) {
                SAY("\x1b[31m%lu: no answer\x1b[39m\n %.80s\n", (unsigned long)r->achievement_id, https_last_error());
                stopped = 1;
                break;
            }
            if (result == RA_AWARD_OK) {
                sent++;
                SAY("\x1b[32m%lu unlocked\x1b[39m\n", (unsigned long)r->achievement_id);
            } else if (result == RA_AWARD_ALREADY) {
                already++;
                SAY("%lu already unlocked\n", (unsigned long)r->achievement_id);
            } else {
                refused++;
                SAY("\x1b[33m%lu refused:\x1b[39m %.60s\n", (unsigned long)r->achievement_id, error);
            }
        }
        done++;
        save_sent(done, first_mac);
    }
    fclose(bin);
    SAY("\nSent %d, already %d, refused %d\n", sent, already, refused);
    if (forged) SAY("%d with a bad signature skipped\n", forged);
    if (stopped) SAY("The rest goes next time\n");

    // The account's unlocks for the game just played, for the next start
    if (played_rom[0]) {
        const char *name = strrchr(played_rom, '/');
        name = name ? name + 1 : played_rom;
        char set_path[320], known[33];
        uint32_t game_id = 0;
        snprintf(set_path, sizeof(set_path), RA_SETS_DIR "/%s.txt", name);
        if (known_hash(set_path, "", known, &game_id) && game_id) write_account_unlocks(account, name, game_id);
    }
    https_close();
}

int main(void) {
    consoleDemoInit();
    iprintf("RA Sync (direct)\n\n");

    char next[256] = DEFAULT_RETURN;
    if (!fatInitDefault()) {
        iprintf("SD card not readable\n");
    } else {
        logf = fopen(SYNC_LOG, "a");
        time_t now = time(NULL);
        if (logf) fprintf(logf, "\n--- %s", ctime(&now));
        have_key = ra_key_derive(console_key) == 0;
        ra_account account;
        int have_account = ra_account_load(&account) == 0;

        FILE *f = fopen(PREP_FILE, "rb");
        if (f) {
            fclose(f);
            prep(&account, have_account);  // leaves the loader in Unlaunch's auto-load
            if (logf) fclose(logf);
            return 0;
        }
        // return.txt: the quit target, then the game that was played
        char played[256] = "";
        if (read_line_n(RETURN_FILE, 0, next, sizeof(next)) || !next[0])
            snprintf(next, sizeof(next), "%s", DEFAULT_RETURN);
        read_line_n(RETURN_FILE, 1, played, sizeof(played));
        if (!have_key) SAY("\x1b[31mNo console key (eMMC CID):\x1b[39m\nunlocks can't be checked\n");
        sync(&account, have_account, played);
        pause_frames(90);
        if (logf) fclose(logf);
    }
    iprintf("\nBack to %s\n", next);
    ra_unlaunch_autoload(next);
    // No jump target: in DSi mode calico restarts the console and Unlaunch
    // boots the path above
    return 0;
}
