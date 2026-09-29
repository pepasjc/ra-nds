// Shared by RA Prep, RA Sync and RA Tool; see include/ra_sync.h.
//
// Signatures are HMAC-SHA256 under the console key (ra_key.c), the same key
// nds-bootstrap-ra signs with; it is derived in RAM and never stored.
#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "https.h"
#include "nds_hash.h"
#include "ra_key.h"
#include "ra_sync.h"

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

FILE *ra_logf;
static unsigned char console_key[32];
static int have_key;

void ra_say(const char *format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    iprintf("%s", text);
    ra_note("%s", text);
}

// Log only.  Synced to the card each time: a run that hangs otherwise
// leaves an empty log (the file size is only written on close).
void ra_note(const char *format, ...) {
    if (!ra_logf) return;
    va_list args;
    va_start(args, format);
    vfprintf(ra_logf, format, args);
    va_end(args);
    fflush(ra_logf);
    fsync(fileno(ra_logf));
}

static void note_network_step(const char *step) {
    ra_note("%ld net: %s\n", (long)time(NULL), step);
}

int ra_sync_init(void) {
    https_set_trace(note_network_step);  // where a hang happened, in the log
    mkdir(RA_DIR, 0777);  // a fresh card has neither
    mkdir(RA_SETS_DIR, 0777);
    have_key = ra_key_derive(console_key) == 0;
    return have_key;
}

int ra_have_key(void) {
    return have_key;
}

void ra_key_id(char out[9]) {
    static const char label[] = "RA-NDS key id";
    char hex[65];
    if (!have_key) {
        strcpy(out, "none");
        return;
    }
    ra_hmac_hex(console_key, label, sizeof(label) - 1, hex);
    memcpy(out, hex, 8);
    out[8] = '\0';
}

int ra_read_line(const char *path, int n, char *out, size_t size) {
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    // Skip whole lines whatever out's size (fgets into a small buffer would
    // count a long line as several: a 3DS start once lost its "3ds" mark)
    int c = 0;
    for (int i = 0; i < n && c != EOF; i++) {
        while ((c = fgetc(f)) != EOF && c != '\n') {}
    }
    int ok = c != EOF && fgets(out, size, f) != NULL;
    fclose(f);
    if (!ok) out[0] = '\0';
    out[strcspn(out, "\r\n")] = '\0';
    return ok ? 0 : -1;
}

void ra_write_text(const char *path, const char *text) {
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
    if (!have_key || ra_read_line(path, 0, stored, sizeof(stored))) return 0;
    char *set = read_all(set_path, &length);
    if (!set) return 0;
    ra_hmac_hex(console_key, set, length, hex);
    free(set);
    return strncmp(stored, hex, 64) == 0;
}

void ra_write_account_unlocks(const ra_account *account, const char *name, uint32_t game_id) {
    char path[320];
    snprintf(path, sizeof(path), RA_SETS_DIR "/%s.unl", name);
    FILE *f = NULL;
    for (int hardcore = 0; hardcore <= 1; hardcore++) {
        uint32_t *ids = NULL, count = 0;
        if (ra_fetch_user_unlocks(account, game_id, hardcore, &ids, &count)) {
            ra_say("(account unlocks not updated: %.60s)\n", https_last_error());
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
        if (hardcore == 0) ra_say("Account: %lu unlocked here before\n", (unsigned long)count);
        free(ids);
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Sets
// ---------------------------------------------------------------------------

int ra_known_hash(const char *set_path, const char *none_path, char out[33], uint32_t *game_id) {
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
    } else if (none_path && ra_read_line(none_path, 0, line, sizeof(line)) == 0 && strlen(line) >= 32) {
        memcpy(out, line, 32);
    }
    out[out[0] ? 32 : 0] = '\0';
    return out[0] != '\0';
}

// <rom>.id: nds-bootstrap-ra's fingerprint of the ROM (checked at every
// start, ra_boot.cpp romMatchesId), then its hash.  No fingerprint (RA
// Tool): no .id, so the loader checks the ROM once through RA Prep.
static int write_rom_id(const char *id_path, const char *fingerprint, const char *md5) {
    if (!fingerprint || !fingerprint[0]) return 0;
    char text[128];
    snprintf(text, sizeof(text), "%s\n%s\n", fingerprint, md5);
    FILE *out = fopen(id_path, "wb");
    if (!out) return -1;
    int ok = fputs(text, out) >= 0;
    return fclose(out) == 0 && ok ? 0 : -1;
}

int ra_prepare_rom(const ra_account *account, int have_account, const char *rom, const char *fingerprint,
                   ra_online_fn online, int *went_online) {
    *went_online = 0;
    const char *name = strrchr(rom, '/');
    name = name ? name + 1 : rom;

    char set_path[320], none_path[320], id_path[320], sig_path[320], unl_path[320];
    snprintf(set_path, sizeof(set_path), RA_SETS_DIR "/%s.txt", name);
    snprintf(none_path, sizeof(none_path), RA_SETS_DIR "/%s.none", name);
    snprintf(id_path, sizeof(id_path), RA_SETS_DIR "/%s.id", name);
    snprintf(sig_path, sizeof(sig_path), RA_SETS_DIR "/%s.sig", name);
    snprintf(unl_path, sizeof(unl_path), RA_SETS_DIR "/%s.unl", name);

    char md5[33], known[33];
    uint32_t known_game = 0;
    ra_note("%ld hashing %s\n", (long)time(NULL), rom);
    if (!rom[0] || !nds_hash_file(rom, md5)) {
        ra_say("\x1b[31mCan't read the ROM\x1b[39m\n");
        return 0;
    }
    if (!have_key) {
        ra_say("\x1b[31mNo console key (eMMC CID)\x1b[39m\n");
        return 0;
    }
    if (ra_known_hash(set_path, none_path, known, &known_game)) {
        if (strcmp(known, md5) != 0) {
            ra_say("\x1b[33mA different ROM:\x1b[39m\n was %s\n now %s\n\n", known, md5);
        } else if (known_game && !set_sig_ok(name, set_path)) {
            ra_say("Set not signed by this console:\nfetching it again\n");
        } else {
            ra_say("ROM checked: achievements\nunchanged\n");
            return write_rom_id(id_path, fingerprint, md5) == 0;
        }
    }
    remove(set_path);
    remove(none_path);
    remove(id_path);
    remove(sig_path);
    remove(unl_path);

    if (!have_account) {
        ra_say("No account in\n " RA_ACCOUNT_FILE "\n");
        return 0;
    }
    ra_note("%ld hash %s, going online\n", (long)time(NULL), md5);
    if (!online()) return 0;
    *went_online = 1;

    int done = 0;
    uint32_t game_id = 0;
    ra_note("%ld looking up the hash\n", (long)time(NULL));
    if (ra_resolve_hash(md5, &game_id)) {
        ra_say("\x1b[31mRetroAchievements didn't answer\x1b[39m\n %.80s\n", https_last_error());
    } else if (game_id == 0) {
        ra_write_text(none_path, md5);
        ra_say("No achievements for this ROM\n(hash %s)\n", md5);
        done = write_rom_id(id_path, fingerprint, md5) == 0;
    } else {
        char *set = NULL;
        size_t length = 0;
        unsigned count = 0;
        ra_note("%ld fetching the set of game %lu\n", (long)time(NULL), (unsigned long)game_id);
        if (ra_fetch_set(account, game_id, md5, &set, &length, &count) == 0) {
            char tmp[330];
            snprintf(tmp, sizeof(tmp), "%s.tmp", set_path);
            FILE *out = fopen(tmp, "wb");
            if (out) {
                int ok = fwrite(set, 1, length, out) == length;
                ok = fclose(out) == 0 && ok;
                remove(set_path);
                done = ok && rename(tmp, set_path) == 0 && write_set_sig(name, set, length) == 0
                       && write_rom_id(id_path, fingerprint, md5) == 0;
            }
            if (done) {
                ra_say("\x1b[32m%u achievements\x1b[39m (game %lu)\n", count, (unsigned long)game_id);
                ra_write_account_unlocks(account, name, game_id);
            } else {
                ra_say("\x1b[31mCan't write the set\x1b[39m\n");
            }
            free(set);
        } else {
            ra_say("\x1b[31mNo set from RetroAchievements\x1b[39m\n %.80s\n", https_last_error());
        }
    }
    return done;
}

// ---------------------------------------------------------------------------
// Unlocks
// ---------------------------------------------------------------------------

static int record_seq_compare(const void *a, const void *b) {
    uint32_t sa = ((const RaSignedUnlock *)a)->record.seq, sb = ((const RaSignedUnlock *)b)->record.seq;
    return (sa > sb) - (sa < sb);
}

// As nds-bootstrap-ra's flushUnlocks does at the next start: only records
// that verify, with a line each in unlocks_history.txt; the ring is cleared
// once they are in
int ra_move_ring(void) {
    if (!have_key) return 0;  // can't check them: the ring stays
    FILE *dump = fopen(RA_RAMDUMP, "r+b");
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
        FILE *bin = fopen(RA_UNLOCKS_FILE, "ab");
        if (bin && fwrite(ring, sizeof(RaSignedUnlock), valid, bin) == (size_t)valid && fclose(bin) == 0) {
            FILE *history = fopen(RA_HISTORY_FILE, "ab");
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
    if (rejected) ra_say("\x1b[33m%d unlock%s failed the signature check\x1b[39m\n", rejected, rejected == 1 ? "" : "s");
    return valid;
}

// ra_submitted.txt: "<records sent>\n<MAC of the first record, hex>\n"; a
// different first record means a new unlocks.bin (then it starts over: RA
// answers "already has" to repeats)
static long load_sent(const char *first_mac, long records) {
    char line[160], mac[80] = "";
    if (ra_read_line(RA_SUBMITTED_FILE, 0, line, sizeof(line))) return 0;
    long sent = strtol(line, NULL, 10);
    ra_read_line(RA_SUBMITTED_FILE, 1, mac, sizeof(mac));
    return (sent < 0 || sent > records || strcmp(mac, first_mac) != 0) ? 0 : sent;
}

static void save_sent(long sent, const char *first_mac) {
    char text[128];
    snprintf(text, sizeof(text), "%ld\n%s\n", sent, first_mac);
    ra_write_text(RA_SUBMITTED_FILE, text);
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

// Records in unlocks.bin and the first one's MAC (hex), for ra_submitted.txt
static long unlock_records(char first_mac[65]) {
    first_mac[0] = '\0';
    FILE *bin = fopen(RA_UNLOCKS_FILE, "rb");
    if (!bin) return 0;
    fseek(bin, 0, SEEK_END);
    long records = ftell(bin) / (long)sizeof(RaSignedUnlock);
    RaSignedUnlock first;
    fseek(bin, 0, SEEK_SET);
    if (records && fread(&first, sizeof(first), 1, bin) == 1) {
        for (int i = 0; i < 32; i++) snprintf(first_mac + i * 2, 3, "%02x", first.mac[i]);
    }
    fclose(bin);
    return records;
}

long ra_pending_unlocks(void) {
    char first_mac[65];
    long records = unlock_records(first_mac);
    return records - load_sent(first_mac, records);
}

void ra_send_unlocks(const ra_account *account) {
    char first_mac[65];
    long records = unlock_records(first_mac);
    long done = load_sent(first_mac, records);
    if (done >= records) return;
    FILE *bin = fopen(RA_UNLOCKS_FILE, "rb");
    if (!bin) return;
    time_t now = time(NULL);
    int sent = 0, already = 0, refused = 0, forged = 0, stopped = 0;
    RaSignedUnlock u;
    fseek(bin, done * (long)sizeof(RaSignedUnlock), SEEK_SET);
    while (fread(&u, sizeof(u), 1, bin) == 1) {
        const RaUnlockRecord *r = &u.record;
        if (!unlock_ok(&u)) {
            forged++;
            ra_say("\x1b[31m%lu: bad signature, not sent\x1b[39m\n", (unsigned long)r->achievement_id);
        } else {
            char md5[33], error[96];
            memcpy(md5, r->md5, 32);
            md5[32] = '\0';
            const int hardcore = RA_SUBMIT_HARDCORE && (r->rtc[7] & 1);
            const int result = ra_award(account, r->achievement_id, md5, hardcore, seconds_ago(r, now), error, sizeof(error));
            if (result == RA_AWARD_NETWORK) {
                ra_say("\x1b[31m%lu: no answer\x1b[39m\n %.80s\n", (unsigned long)r->achievement_id, https_last_error());
                stopped = 1;
                break;
            }
            if (result == RA_AWARD_OK) {
                sent++;
                ra_say("\x1b[32m%lu unlocked\x1b[39m\n", (unsigned long)r->achievement_id);
            } else if (result == RA_AWARD_ALREADY) {
                already++;
                ra_say("%lu already unlocked\n", (unsigned long)r->achievement_id);
            } else {
                refused++;
                ra_say("\x1b[33m%lu refused:\x1b[39m %.60s\n", (unsigned long)r->achievement_id, error);
            }
        }
        done++;
        save_sent(done, first_mac);
    }
    fclose(bin);
    ra_say("\nSent %d, already %d, refused %d\n", sent, already, refused);
    if (forged) ra_say("%d with a bad signature skipped\n", forged);
    if (stopped) ra_say("The rest goes next time\n");
}
