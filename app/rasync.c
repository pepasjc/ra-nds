// RA Sync (direct): RetroAchievements for the DSi without the Pi.
//
// nds-bootstrap-ra restarts the console into this app (through Unlaunch, in
// full DSi mode, so WPA2 works) at two points, and the app talks to
// retroachievements.org itself:
//
//  - Prep, before a game with no set yet (sd:/_nds/ra/raprep.nds;
//    nds-bootstrap wrote prep.txt: the ROM, then the loader): hash the ROM,
//    ask RA for its game and write the set to sd:/_nds/ra/sets/<rom>.txt, or
//    <rom>.none when RA has no set for it.  If RA can't be reached,
//    skip_once.txt lets the game start without a set this once.  Then the
//    loader starts again and boots the game.
//
//  - Sync, when a game with a set is quit (sd:/_nds/ra/rasync.nds;
//    nds-bootstrap saved the real quit target in return.txt): move the
//    session's unlocks from ramDump.bin into unlocks.log (as GameSync does)
//    and send the new ones to RA as softcore unlocks.  How far the log has
//    been sent is kept in ra_submitted.txt, apart from GameSync's
//    uploaded.txt.  Without "submit=1" in account.txt it is a dry run.
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

#define PREP_FILE RA_DIR "/prep.txt"
#define SKIP_FILE RA_DIR "/skip_once.txt"
#define AFTER_PREP_FILE RA_DIR "/after_prep.txt"
#define RETURN_FILE RA_DIR "/return.txt"
#define LOG_FILE RA_DIR "/unlocks.log"
#define SUBMITTED_FILE RA_DIR "/ra_submitted.txt"
#define SYNC_LOG RA_DIR "/rasync_log.txt"
#define RAMDUMP "sd:/_nds/nds-bootstrap/ramDump.bin"
#define DEFAULT_RETURN "sd:/_nds/TWiLightMenu/main.srldr"
#define DEFAULT_LOADER "sd:/_nds/nds-bootstrap-nightly.nds"

// Unlock ring in ramDump.bin (nds-bootstrap-ra retail/common/include/ra_engine.h)
#define RA_DUMP_UNLOCK_OFFSET 0x01FE0000
#define RA_UNLOCK_RECORDS 1024
#define RA_UNLOCK_MAGIC 0x31554152  // 'RAU1'

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t achievement_id;
    uint32_t game_id;
    uint32_t points;
    uint32_t frame;
    uint8_t rtc[8];     // year-2000, month, day, weekday, hour, minute, second, 0
    char md5[32];
} RaUnlockRecord;

_Static_assert(sizeof(RaUnlockRecord) == 64, "RaUnlockRecord must be 64 bytes");

static FILE *logf;

// Screen and sd:/_nds/ra/rasync_log.txt
#define SAY(...) do { \
        iprintf(__VA_ARGS__); \
        if (logf) { fprintf(logf, __VA_ARGS__); fflush(logf); } \
    } while (0)

static void pause_frames(int frames) {
    while (frames-- > 0) swiWaitForVBlank();
}

// First line of a small file, without its line end
static int read_line_file(const char *path, char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (!fgets(out, size, f)) out[0] = '\0';
    fclose(f);
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
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

// The hash the current set (its "game" line) or .none file was made for
static int known_hash(const char *set_path, const char *none_path, char out[33]) {
    char line[256];
    out[0] = '\0';
    FILE *f = fopen(set_path, "rb");
    if (f) {
        // "RASET\t1", then "game\t<id>\t<md5>\t<title>"
        if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f) && !strncmp(line, "game\t", 5)) {
            char *md5 = strchr(line + 5, '\t');
            if (md5 && strlen(md5 + 1) >= 32) memcpy(out, md5 + 1, 32);
        }
        fclose(f);
    } else if (read_line_file(none_path, line, sizeof(line)) == 0 && strlen(line) >= 32) {
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

    char set_path[320], none_path[320], id_path[320];
    snprintf(set_path, sizeof(set_path), RA_SETS_DIR "/%s.txt", name);
    snprintf(none_path, sizeof(none_path), RA_SETS_DIR "/%s.none", name);
    snprintf(id_path, sizeof(id_path), RA_SETS_DIR "/%s.id", name);

    // Unless a set (or "none") is there for this ROM, the next start plays
    // without
    int done = 0, fetch = 1;
    char md5[33], known[33];
    // Timed: is hashing cheap enough for nds-bootstrap to do at every start?
    timer_start();
    int hashed = rom[0] && nds_hash_file(rom, md5);
    if (hashed) SAY("Hash %s\n (%u ms)\n\n", md5, timer_ms());
    if (!hashed) {
        SAY("\x1b[31mCan't read the ROM\x1b[39m\n");
        fetch = 0;
    } else if (known_hash(set_path, none_path, known)) {
        // nds-bootstrap sent us because the ROM's .id is missing or differs
        if (strcmp(known, md5) == 0) {
            SAY("ROM checked: achievements\nunchanged\n");
            done = write_rom_id(id_path, md5) == 0;
            fetch = 0;
        } else {
            SAY("\x1b[33mA different ROM:\x1b[39m\n was %s\n now %s\n\n", known, md5);
        }
    }
    if (fetch && !done) {
        remove(set_path);
        remove(none_path);
        remove(id_path);
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
            SAY("\x1b[31mRetroAchievements didn't answer\x1b[39m\n");
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
                    done = ok && rename(tmp, set_path) == 0 && write_rom_id(id_path, md5) == 0;
                }
                if (done) SAY("\x1b[32m%u achievements\x1b[39m (game %lu)\n", count, (unsigned long)game_id);
                else SAY("\x1b[31mCan't write the set\x1b[39m\n");
                free(set);
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
    uint32_t sa = ((const RaUnlockRecord *)a)->seq, sb = ((const RaUnlockRecord *)b)->seq;
    return (sa > sb) - (sa < sb);
}

// The last session's unlocks, from the ring in ramDump.bin into unlocks.log
// (as GameSync's rasync and nds-bootstrap-ra's flushUnlocks do).  The ring
// is cleared once they are in the log.
static int move_ring(void) {
    FILE *dump = fopen(RAMDUMP, "r+b");
    if (!dump) return 0;
    const size_t ring_size = RA_UNLOCK_RECORDS * sizeof(RaUnlockRecord);
    RaUnlockRecord *records = malloc(ring_size);
    size_t got = 0;
    if (records && fseek(dump, RA_DUMP_UNLOCK_OFFSET, SEEK_SET) == 0)
        got = fread(records, sizeof(RaUnlockRecord), RA_UNLOCK_RECORDS, dump);
    int valid = 0;
    for (size_t i = 0; i < got; i++)
        if (records[i].magic == RA_UNLOCK_MAGIC) records[valid++] = records[i];
    if (valid > 0) {
        qsort(records, valid, sizeof(RaUnlockRecord), record_seq_compare);
        FILE *log = fopen(LOG_FILE, "ab");
        if (log) {
            for (int i = 0; i < valid; i++) {
                const RaUnlockRecord *r = &records[i];
                // achievement, game, md5, points, local time, frame
                fprintf(log, "%lu\t%lu\t%.32s\t%lu\t20%02u-%02u-%02u %02u:%02u:%02u\t%lu\n",
                        (unsigned long)r->achievement_id, (unsigned long)r->game_id, r->md5,
                        (unsigned long)r->points, r->rtc[0], r->rtc[1], r->rtc[2], r->rtc[4], r->rtc[5],
                        r->rtc[6], (unsigned long)r->frame);
            }
            fclose(log);
            memset(records, 0, ring_size);
            fseek(dump, RA_DUMP_UNLOCK_OFFSET, SEEK_SET);
            fwrite(records, 1, ring_size, dump);
        } else {
            valid = 0;  // the ring stays for next time
        }
    }
    free(records);
    fclose(dump);
    return valid;
}

// ra_submitted.txt: "<byte offset>\n<first line of unlocks.log>\n"; the
// first line notices a replaced log (then it starts over: RA answers
// "already has" to repeats)
static long load_offset(const char *first_line, long log_size) {
    FILE *f = fopen(SUBMITTED_FILE, "r");
    if (!f) return 0;
    char line[160], first[160] = "";
    long offset = 0;
    if (fgets(line, sizeof(line), f)) offset = strtol(line, NULL, 10);
    if (fgets(first, sizeof(first), f)) first[strcspn(first, "\r\n")] = '\0';
    fclose(f);
    return (offset < 0 || offset > log_size || strcmp(first, first_line) != 0) ? 0 : offset;
}

static void save_offset(long offset, const char *first_line) {
    FILE *f = fopen(SUBMITTED_FILE, "w");
    if (!f) return;
    fprintf(f, "%ld\n%s\n", offset, first_line);
    fclose(f);
}

static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

typedef struct {
    uint32_t id;
    char md5[33];
    uint32_t ago;       // seconds since the unlock, by the DS clock
} unlock_line;

// "<id>\t<game>\t<md5>\t<points>\tYYYY-MM-DD HH:MM:SS\t<frame>"
static int parse_unlock(char *line, time_t now, unlock_line *out) {
    char *fields[6];
    int n = 0;
    for (char *p = line; n < 6;) {
        fields[n++] = p;
        p = strchr(p, '\t');
        if (!p) break;
        *p++ = '\0';
    }
    if (n < 5 || strlen(fields[2]) != 32) return 0;
    out->id = strtoul(fields[0], NULL, 10);
    if (!out->id) return 0;
    for (int i = 0; i < 32; i++) {
        char c = fields[2][i];
        out->md5[i] = (c >= 'A' && c <= 'F') ? c + 32 : c;
    }
    out->md5[32] = '\0';
    out->ago = 0;
    int y, mo, d, h, mi, s;
    if (sscanf(fields[4], "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 && mo >= 1 && mo <= 12) {
        long long logged = (long long)days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
        long long ago = (long long)now - logged;
        out->ago = ago < 0 ? 0 : ago > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)ago;
    }
    return 1;
}

static void sync(const ra_account *account, int have_account) {
    int moved = move_ring();
    if (moved) SAY("%d new unlock%s from the game\n", moved, moved == 1 ? "" : "s");

    FILE *log = fopen(LOG_FILE, "rb");
    if (!log) {
        SAY("No unlocks yet\n");
        return;
    }
    fseek(log, 0, SEEK_END);
    long size = ftell(log);
    char first[160] = "";
    fseek(log, 0, SEEK_SET);
    if (fgets(first, sizeof(first), log)) first[strcspn(first, "\r\n")] = '\0';
    long offset = load_offset(first, size);

    // Pending lines
    int pending = 0;
    fseek(log, offset, SEEK_SET);
    for (int c; (c = fgetc(log)) != EOF;)
        if (c == '\n') pending++;
    if (!pending) {
        fclose(log);
        SAY("Nothing new to send\n");
        return;
    }
    SAY("%d unlock%s to send\n", pending, pending == 1 ? "" : "s");
    if (!have_account) {
        fclose(log);
        SAY("No account in\n " RA_ACCOUNT_FILE "\n");
        return;
    }
    if (!account->submit) {
        fclose(log);
        SAY("Dry run (submit=1 in account.txt\nsends them): kept for later\n");
        return;
    }
    if (!ra_wifi_connect(3) || https_init(RA_USER_AGENT)) {
        fclose(log);
        SAY("\x1b[31mNo connection:\x1b[39m kept for next time\n");
        return;
    }

    time_t now = time(NULL);
    int sent = 0, already = 0, refused = 0, stopped = 0;
    char line[256];
    fseek(log, offset, SEEK_SET);
    while (fgets(line, sizeof(line), log)) {
        long next = ftell(log);
        if (!strchr(line, '\n')) break;  // a line still being written
        line[strcspn(line, "\r\n")] = '\0';
        unlock_line u;
        if (line[0] && parse_unlock(line, now, &u)) {
            char error[96];
            int r = ra_award(account, u.id, u.md5, u.ago, error, sizeof(error));
            if (r == RA_AWARD_NETWORK) {
                SAY("\x1b[31m%lu: no answer\x1b[39m\n", (unsigned long)u.id);
                stopped = 1;
                break;
            }
            if (r == RA_AWARD_OK) {
                sent++;
                SAY("\x1b[32m%lu unlocked\x1b[39m\n", (unsigned long)u.id);
            } else if (r == RA_AWARD_ALREADY) {
                already++;
                SAY("%lu already unlocked\n", (unsigned long)u.id);
            } else {
                refused++;
                SAY("\x1b[33m%lu refused:\x1b[39m %.60s\n", (unsigned long)u.id, error);
            }
        }
        offset = next;
        save_offset(offset, first);
    }
    fclose(log);
    https_close();
    SAY("\nSent %d, already %d, refused %d\n", sent, already, refused);
    if (stopped) SAY("The rest goes next time\n");
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
        ra_account account;
        int have_account = ra_account_load(&account) == 0;

        FILE *f = fopen(PREP_FILE, "rb");
        if (f) {
            fclose(f);
            prep(&account, have_account);  // leaves the loader in Unlaunch's auto-load
            if (logf) fclose(logf);
            return 0;
        }
        if (read_line_file(RETURN_FILE, next, sizeof(next)) || !next[0])
            snprintf(next, sizeof(next), "%s", DEFAULT_RETURN);
        sync(&account, have_account);
        pause_frames(90);
        if (logf) fclose(logf);
    }
    iprintf("\nBack to %s\n", next);
    ra_unlaunch_autoload(next);
    // No jump target: in DSi mode calico restarts the console and Unlaunch
    // boots the path above
    return 0;
}
