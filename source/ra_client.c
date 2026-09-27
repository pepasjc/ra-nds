// RetroAchievements calls and DSi plumbing shared by the apps; see
// include/ra_client.h.
#include <nds.h>
#include <dswifi9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "https.h"
#include "ra_client.h"
#include "raset.h"

#include "rc_api_runtime.h"
#include "rc_api_user.h"

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = '\0';
}

int ra_account_load(ra_account *account) {
    memset(account, 0, sizeof(*account));
    FILE *f = fopen(RA_ACCOUNT_FILE, "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!strncmp(line, "user=", 5)) snprintf(account->user, sizeof(account->user), "%s", line + 5);
        else if (!strncmp(line, "token=", 6)) snprintf(account->token, sizeof(account->token), "%s", line + 6);
        else if (!strncmp(line, "password=", 9)) snprintf(account->password, sizeof(account->password), "%s", line + 9);
        else if (!strncmp(line, "submit=", 7)) account->submit = atoi(line + 7) != 0;
    }
    fclose(f);
    return account->user[0] && (account->token[0] || account->password[0]) ? 0 : -1;
}

int ra_account_save(const ra_account *account) {
    FILE *f = fopen(RA_ACCOUNT_FILE, "w");
    if (!f) return -1;
    fprintf(f, "user=%s\ntoken=%s\nsubmit=%d\n", account->user, account->token, account->submit);
    return fclose(f) == 0 ? 0 : -1;
}

int ra_wifi_connect(int attempts) {
    static int connected;
    if (connected) return 1;
    for (int attempt = 1; attempt <= attempts && !connected; attempt++) {
        iprintf("WiFi %d/%d...\n", attempt, attempts);
        timer_start();
        if (Wifi_InitDefault(WFC_CONNECT)) {
            iprintf(" up in %u ms\n", timer_ms());
            connected = 1;
            return 1;
        }
        for (int i = 0; attempt < attempts && i < 60 * attempt; i++) swiWaitForVBlank();
    }
    return 0;
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

int ra_login(const ra_account *account) {
    rc_api_login_request_t params = {0};
    params.username = account->user;
    params.api_token = account->token;
    rc_api_request_t req;
    if (rc_api_init_login_request(&req, &params) != RC_OK) return -1;
    https_response res;
    rc_api_server_response_t sr;
    if (api_call(&req, &res, &sr)) return -1;

    rc_api_login_response_t login;
    int r = rc_api_process_login_server_response(&login, &sr);
    int ok = r == RC_OK && login.response.succeeded;
    if (ok) {
        iprintf("%s: %lu pts (softcore)\n", login.display_name ? login.display_name : login.username,
                (unsigned long)login.score_softcore);
    } else {
        iprintf("\x1b[31mLogin failed: %s\x1b[39m\n",
                login.response.error_message ? login.response.error_message : rc_error_str(r));
    }
    rc_api_destroy_login_response(&login);
    free(res.body);
    return ok ? 0 : -1;
}

int ra_login_password(ra_account *account) {
    rc_api_login_request_t params = {0};
    params.username = account->user;
    params.password = account->password;
    rc_api_request_t req;
    int ok = 0;
    if (rc_api_init_login_request(&req, &params) == RC_OK) {
        https_response res;
        rc_api_server_response_t sr;
        if (api_call(&req, &res, &sr) == 0) {
            rc_api_login_response_t login;
            int r = rc_api_process_login_server_response(&login, &sr);
            ok = r == RC_OK && login.response.succeeded && login.api_token && login.api_token[0];
            if (ok) {
                snprintf(account->token, sizeof(account->token), "%s", login.api_token);
                if (login.username) snprintf(account->user, sizeof(account->user), "%s", login.username);
            } else {
                iprintf("\x1b[31mLogin failed: %s\x1b[39m\n",
                        login.response.error_message ? login.response.error_message : rc_error_str(r));
            }
            rc_api_destroy_login_response(&login);
            free(res.body);
        }
    }
    memset(account->password, 0, sizeof(account->password));
    return ok ? 0 : -1;
}

int ra_resolve_hash(const char *md5, uint32_t *game_id) {
    *game_id = 0;
    rc_api_resolve_hash_request_t params = {0};
    params.game_hash = md5;
    rc_api_request_t req;
    if (rc_api_init_resolve_hash_request(&req, &params) != RC_OK) return -1;
    https_response res;
    rc_api_server_response_t sr;
    if (api_call(&req, &res, &sr)) return -1;
    rc_api_resolve_hash_response_t hash;
    int r = rc_api_process_resolve_hash_server_response(&hash, &sr);
    int ok = r == RC_OK && hash.response.succeeded;
    if (ok) *game_id = hash.game_id;
    rc_api_destroy_resolve_hash_response(&hash);
    free(res.body);
    return ok ? 0 : -1;
}

int ra_fetch_set(const ra_account *account, uint32_t game_id, const char *md5, char **set,
                 size_t *length, unsigned *achievements) {
    *set = NULL;
    rc_api_fetch_game_data_request_t params = {0};
    params.username = account->user;
    params.api_token = account->token;
    params.game_id = game_id;
    rc_api_request_t req;
    if (rc_api_init_fetch_game_data_request(&req, &params) != RC_OK) return -1;
    https_response res;
    rc_api_server_response_t sr;
    if (api_call(&req, &res, &sr)) return -1;
    rc_api_fetch_game_data_response_t game;
    int r = rc_api_process_fetch_game_data_server_response(&game, &sr);
    int ok = r == RC_OK && game.response.succeeded;
    if (ok) *set = raset_render(&game, md5, achievements, length);
    else iprintf("\x1b[31mSet: %s\x1b[39m\n",
                 game.response.error_message ? game.response.error_message : rc_error_str(r));
    rc_api_destroy_fetch_game_data_response(&game);
    free(res.body);
    return ok && *set ? 0 : -1;
}

int ra_fetch_user_unlocks(const ra_account *account, uint32_t game_id, int hardcore, uint32_t **ids,
                          uint32_t *count) {
    *ids = NULL;
    *count = 0;
    rc_api_fetch_user_unlocks_request_t params = {0};
    params.username = account->user;
    params.api_token = account->token;
    params.game_id = game_id;
    params.hardcore = hardcore ? 1 : 0;
    rc_api_request_t req;
    if (rc_api_init_fetch_user_unlocks_request(&req, &params) != RC_OK) return -1;
    https_response res;
    rc_api_server_response_t sr;
    if (api_call(&req, &res, &sr)) return -1;
    rc_api_fetch_user_unlocks_response_t unlocks;
    int r = rc_api_process_fetch_user_unlocks_server_response(&unlocks, &sr);
    int ok = r == RC_OK && unlocks.response.succeeded;
    if (ok && unlocks.num_achievement_ids) {
        *ids = malloc(unlocks.num_achievement_ids * sizeof(uint32_t));
        if (*ids) {
            memcpy(*ids, unlocks.achievement_ids, unlocks.num_achievement_ids * sizeof(uint32_t));
            *count = unlocks.num_achievement_ids;
        } else {
            ok = 0;
        }
    }
    rc_api_destroy_fetch_user_unlocks_response(&unlocks);
    free(res.body);
    return ok ? 0 : -1;
}

int ra_award(const ra_account *account, uint32_t achievement_id, const char *md5, int hardcore,
             uint32_t seconds_since_unlock, char *error, size_t error_size) {
    error[0] = '\0';
    rc_api_award_achievement_request_t params = {0};
    params.username = account->user;
    params.api_token = account->token;
    params.achievement_id = achievement_id;
    params.hardcore = hardcore ? 1 : 0;
    params.game_hash = md5;
    params.seconds_since_unlock = seconds_since_unlock;
    rc_api_request_t req;
    if (rc_api_init_award_achievement_request(&req, &params) != RC_OK) return RA_AWARD_REFUSED;
    https_response res;
    rc_api_server_response_t sr;
    if (api_call(&req, &res, &sr)) return RA_AWARD_NETWORK;
    rc_api_award_achievement_response_t award;
    int r = rc_api_process_award_achievement_server_response(&award, &sr);
    int result;
    if (r == RC_OK && award.response.succeeded) {
        // rcheevos counts "User already has ..." as success; tell them apart
        const char *msg = award.response.error_message;
        result = msg && !strncmp(msg, "User already has", 16) ? RA_AWARD_ALREADY : RA_AWARD_OK;
    } else {
        // A 5xx or an unreadable answer is worth another go later
        result = (res.status >= 500 || r != RC_OK) && !award.response.error_message ? RA_AWARD_NETWORK
                                                                                     : RA_AWARD_REFUSED;
        snprintf(error, error_size, "%s",
                 award.response.error_message ? award.response.error_message : rc_error_str(r));
    }
    rc_api_destroy_award_achievement_response(&award);
    free(res.body);
    return result;
}

void ra_unlaunch_autoload(const char *path) {
    u8 *info = (u8 *)0x02000800;
    memcpy(info, "AutoLoadInfo", 12);
    *(u16 *)(info + 0x0C) = 0x3F0;           // length covered by the CRC
    *(u16 *)(info + 0x0E) = 0;               // CRC, below
    *(u32 *)(info + 0x10) = BIT(0) | BIT(1); // load the path; use the colours
    *(u16 *)(info + 0x14) = 0x7FFF;          // top screen colour
    *(u16 *)(info + 0x16) = 0x7FFF;          // bottom screen colour
    memset(info + 0x18, 0, 0x20 + 0x208 + 0x1C0);
    u16 *name = (u16 *)(info + 0x38);        // UTF-16, 0-terminated
    for (int i = 0; i < 255 && path[i]; i++) name[i] = (u8)path[i];
    *(u16 *)(info + 0x0E) = swiCRC16(0xFFFF, info + 0x10, 0x3F0);
    DC_FlushAll();
}
