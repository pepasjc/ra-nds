// RA Direct, step 2: the DSi talks to RetroAchievements with rcheevos' rc_api.
//
// Reads the account from sd:/_nds/ra/account.txt (user=..., token=...: the
// connect token, as the GameSync server keeps it), logs in with the token,
// then for each set already on the card (sd:/_nds/ra/sets/, fetched through
// the Pi) looks the game up by hash and downloads its achievements straight
// from RA.  The set is rendered the way the Pi renders it and written to
// sd:/_nds/ra/direct_test/, never over the live sets, and compared with the
// Pi's copy.  Only read-only API calls: nothing is unlocked or changed.
#include <nds.h>
#include <dswifi9.h>
#include <fat.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "https.h"
#include "raset.h"

#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_consoles.h"

#define RA_DIR "sd:/_nds/ra"
#define SETS_DIR RA_DIR "/sets"
#define TEST_DIR RA_DIR "/direct_test"
#define ACCOUNT_FILE RA_DIR "/account.txt"
#define LOG_FILE TEST_DIR "/log.txt"

#define USER_AGENT "RADirectDS/0.2 (Nintendo DSi) rcheevos/12.5"

static char ra_user[64];
static char ra_token[64];
static FILE *logf;

#define LOG(...) do { if (logf) { fprintf(logf, __VA_ARGS__); fflush(logf); } } while (0)

static void wait_start(void) {
    iprintf("\nPress START to exit\n");
    while (pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START) break;
    }
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(n + 1);
    if (data && fread(data, 1, n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (!data) return NULL;
    data[n] = '\0';
    if (len) *len = n;
    return data;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = '\0';
}

// account.txt: "user=<name>" and "token=<connect token>" lines
static int load_account(void) {
    FILE *f = fopen(ACCOUNT_FILE, "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, "user=", 5)) snprintf(ra_user, sizeof(ra_user), "%s", line + 5);
        else if (!strncmp(line, "token=", 6)) snprintf(ra_token, sizeof(ra_token), "%s", line + 6);
    }
    fclose(f);
    return ra_user[0] && ra_token[0] ? 0 : -1;
}

// Send an rc_api request; fills the server response for rc_api's parsers.
static int api_call(rc_api_request_t *req, https_response *res, rc_api_server_response_t *sr) {
    int r = https_request(req->url, req->post_data, req->content_type, res);
    rc_api_destroy_request(req);
    if (r) return -1;
    sr->body = res->body;
    sr->body_length = res->length;
    sr->http_status_code = res->status;
    return 0;
}

static int login(void) {
    rc_api_login_request_t params = {0};
    params.username = ra_user;
    params.api_token = ra_token;
    rc_api_request_t req;
    if (rc_api_init_login_request(&req, &params) != RC_OK) return -1;
    https_response res;
    rc_api_server_response_t sr;
    if (api_call(&req, &res, &sr)) return -1;

    rc_api_login_response_t login;
    int r = rc_api_process_login_server_response(&login, &sr);
    int ok = r == RC_OK && login.response.succeeded;
    if (ok) {
        iprintf("Logged in as %s (%lu ms)\n", login.display_name ? login.display_name : login.username,
                (unsigned long)res.ms);
        iprintf(" softcore %lu, hardcore %lu pts\n", (unsigned long)login.score_softcore,
                (unsigned long)login.score);
        LOG("login ok: %s softcore=%lu hardcore=%lu %ums\n", login.username,
            (unsigned long)login.score_softcore, (unsigned long)login.score, res.ms);
    } else {
        const char *msg = login.response.error_message ? login.response.error_message : rc_error_str(r);
        iprintf("\x1b[31mLogin failed (HTTP %d):\n %s\x1b[39m\n", res.status, msg);
        LOG("login failed: HTTP %d %s\n", res.status, msg);
    }
    rc_api_destroy_login_response(&login);
    free(res.body);
    return ok ? 0 : -1;
}

static uint32_t resolve_hash(const char *md5, unsigned *ms) {
    rc_api_resolve_hash_request_t params = {0};
    params.game_hash = md5;
    rc_api_request_t req;
    if (rc_api_init_resolve_hash_request(&req, &params) != RC_OK) return 0;
    https_response res;
    rc_api_server_response_t sr;
    if (api_call(&req, &res, &sr)) return 0;
    rc_api_resolve_hash_response_t hash;
    uint32_t id = 0;
    if (rc_api_process_resolve_hash_server_response(&hash, &sr) == RC_OK && hash.response.succeeded)
        id = hash.game_id;
    rc_api_destroy_resolve_hash_response(&hash);
    free(res.body);
    *ms = res.ms;
    return id;
}

typedef struct {
    char name[128];     // set file name
    char md5[33];
    uint32_t game_id;
} set_entry;

// Games from the sets folder, from each file's "game" line
static set_entry *list_sets(int only_test_games, int *count) {
    static const uint32_t test_games[] = { 9878, 5522, 1226 };  // Tetris DS, Castlevania DoS, Ouendan
    DIR *dir = opendir(SETS_DIR);
    if (!dir) return NULL;
    int cap = 64, n = 0;
    set_entry *list = malloc(cap * sizeof(*list));
    struct dirent *de;
    while ((de = readdir(dir))) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcasecmp(de->d_name + len - 4, ".txt") || len >= sizeof(list[0].name)) continue;
        char path[300];
        snprintf(path, sizeof(path), SETS_DIR "/%s", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[256];
        set_entry e = {0};
        if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f) && !strncmp(line, "game\t", 5)) {
            char *id = line + 5, *md5 = strchr(id, '\t');
            if (md5 && strlen(md5 + 1) >= 32) {
                e.game_id = strtoul(id, NULL, 10);
                memcpy(e.md5, md5 + 1, 32);
            }
        }
        fclose(f);
        if (!e.md5[0]) continue;
        if (only_test_games) {
            int wanted = 0;
            for (unsigned i = 0; i < sizeof(test_games) / sizeof(test_games[0]); i++)
                wanted |= e.game_id == test_games[i];
            if (!wanted) continue;
        }
        strcpy(e.name, de->d_name);
        if (n == cap) list = realloc(list, (cap *= 2) * sizeof(*list));
        list[n++] = e;
    }
    closedir(dir);
    *count = n;
    return list;
}

// First line that differs, for the log
static void log_first_difference(const char *a, const char *b) {
    int line = 1;
    while (*a && *a == *b) {
        if (*a == '\n') line++;
        a++;
        b++;
    }
    while (line > 1 && a[-1] != '\n') a--, b--;
    LOG("  first difference on line %d\n  pi:     %.160s\n  direct: %.160s\n", line, a, b);
}

enum { RESULT_SAME, RESULT_DIFF, RESULT_NOHASH, RESULT_FAIL };

static int check_game(const set_entry *e, unsigned *achievements) {
    unsigned hash_ms = 0;
    uint32_t id = resolve_hash(e->md5, &hash_ms);
    LOG("%s\n  md5 %s: set game %lu, RA gameid %lu (%ums)\n", e->name, e->md5,
        (unsigned long)e->game_id, (unsigned long)id, hash_ms);
    if (!id) return RESULT_NOHASH;

    rc_api_fetch_game_data_request_t params = {0};
    params.username = ra_user;
    params.api_token = ra_token;
    params.game_id = id;
    rc_api_request_t req;
    if (rc_api_init_fetch_game_data_request(&req, &params) != RC_OK) return RESULT_FAIL;
    https_response res;
    rc_api_server_response_t sr;
    if (api_call(&req, &res, &sr)) return RESULT_FAIL;

    rc_api_fetch_game_data_response_t game;
    int r = rc_api_process_fetch_game_data_server_response(&game, &sr);
    int result = RESULT_FAIL;
    if (r == RC_OK && game.response.succeeded) {
        size_t set_len = 0;
        char *set = raset_render(&game, e->md5, achievements, &set_len);
        LOG("  patch: HTTP %d, %u bytes, %ums, %u of %lu achievements\n", res.status,
            (unsigned)res.length, res.ms, *achievements, (unsigned long)game.num_achievements);

        char path[300];
        snprintf(path, sizeof(path), TEST_DIR "/%s", e->name);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(set, 1, set_len, f);
            fclose(f);
        }
        snprintf(path, sizeof(path), SETS_DIR "/%s", e->name);
        char *pi = read_file(path, NULL);
        if (pi && !strcmp(pi, set)) {
            result = RESULT_SAME;
            LOG("  same as the Pi's set\n");
        } else {
            result = RESULT_DIFF;
            if (pi) log_first_difference(pi, set);
        }
        free(pi);
        free(set);
    } else {
        const char *msg = game.response.error_message ? game.response.error_message : rc_error_str(r);
        LOG("  patch failed: HTTP %d %s\n", res.status, msg);
    }
    rc_api_destroy_fetch_game_data_response(&game);
    free(res.body);
    return result;
}

int main(void) {
    consoleDemoInit();
    iprintf("RA Direct: step 2 (rc_api)\n\n");
    if (!fatInitDefault()) {
        iprintf("\x1b[31mNo SD card\x1b[39m\n");
        wait_start();
        return 0;
    }
    if (load_account()) {
        iprintf("\x1b[31mNo account in\n " ACCOUNT_FILE "\x1b[39m\n");
        iprintf("\nuser=<RA user name>\ntoken=<connect token>\n");
        wait_start();
        return 0;
    }
    mkdir(TEST_DIR, 0777);
    logf = fopen(LOG_FILE, "w");

    iprintf("A: Tetris, Castlevania, Ouendan\nX: every set on the card\nSTART: exit\n\n");
    int all = -1;
    while (pmMainLoop() && all < 0) {
        swiWaitForVBlank();
        scanKeys();
        u32 k = keysDown();
        if (k & KEY_A) all = 0;
        if (k & KEY_X) all = 1;
        if (k & KEY_START) break;
    }
    if (all < 0) {
        if (logf) fclose(logf);
        return 0;
    }

    int count = 0;
    set_entry *sets = list_sets(!all, &count);
    iprintf("%d sets to check\n", count);
    LOG("%d sets to check\n", count);

    iprintf("WiFi (saved connections)...\n");
    timer_start();
    if (!Wifi_InitDefault(WFC_CONNECT)) {
        iprintf("\x1b[31mNo WiFi connection\x1b[39m\n");
        wait_start();
        return 0;
    }
    iprintf(" up in %u ms\n", timer_ms());
    if (https_init(USER_AGENT) || login()) {
        https_close();
        if (logf) fclose(logf);
        wait_start();
        return 0;
    }

    time_t started = time(NULL);
    int totals[4] = {0};
    static const char *const labels[] = { "same", "\x1b[33mdiff\x1b[39m", "\x1b[33mno hash\x1b[39m",
                                          "\x1b[31mfail\x1b[39m" };
    for (int i = 0; i < count; i++) {
        unsigned achievements = 0;
        int result = check_game(&sets[i], &achievements);
        totals[result]++;
        iprintf("%3d/%d %5lu %3ua %s\n", i + 1, count, (unsigned long)sets[i].game_id, achievements,
                labels[result]);
        scanKeys();
        if (keysHeld() & KEY_B) {
            iprintf("Stopped (B)\n");
            break;
        }
    }
    https_close();

    const https_stats *st = https_get_stats();
    long seconds = (long)(time(NULL) - started);
    iprintf("\nsame %d, diff %d, no hash %d,\nfailed %d in %lds\n", totals[0], totals[1], totals[2],
            totals[3], seconds);
    iprintf("%u requests, %lu KB\n", st->requests, st->bytes / 1024);
    iprintf("%u TCP, %u full + %u resumed TLS\n", st->connects, st->handshakes, st->resumed);
    LOG("\nsame %d, diff %d, no hash %d, failed %d in %lds\n%u requests, %lu bytes, %u connections, "
        "%u full handshakes, %u resumed\n",
        totals[0], totals[1], totals[2], totals[3], seconds, st->requests, st->bytes, st->connects,
        st->handshakes, st->resumed);
    if (logf) fclose(logf);
    free(sets);
    wait_start();
    return 0;
}
