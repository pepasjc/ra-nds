// RetroAchievements calls and DSi plumbing shared by the apps; see
// include/ra_client.h.
#include <nds.h>
#include <dswifi9.h>
#include <netdb.h>
#include <sys/socket.h>
#include <wfc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "https.h"
#include "ra_client.h"
#include "ra_netprofile.h"
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

// The WiFi bring-up runs on its own thread so that it can't take the app
// down with it: after nds-bootstrap's restart the DSi's WiFi sometimes
// hangs in Wifi_InitDefault() for good (a game with DS wireless may leave
// the chip in its DS mode; a power cycle cures it).  The main thread gives
// up after a deadline instead.
static volatile int wifi_state;        // 0 trying, 1 up, -1 failed
static volatile int wifi_attempt;
static int wifi_attempts;
static Thread wifi_thread;
static u8 wifi_stack[16 * 1024] __attribute__((aligned(8)));

static int wifi_worker(void *arg) {
    (void)arg;
    for (int attempt = 1; attempt <= wifi_attempts; attempt++) {
        wifi_attempt = attempt;
        if (Wifi_InitDefault(WFC_CONNECT)) {
            wifi_state = 1;
            return 0;
        }
        for (int i = 0; attempt < wifi_attempts && i < 60 * attempt; i++) threadWaitForVBlank();
    }
    wifi_state = -1;
    return 0;
}

int ra_wifi_connect(int attempts) {
    static int started;
    if (started) return wifi_state == 1;  // one try per run: a hung driver stays hung
    started = 1;
    wifi_attempts = attempts;
    wifi_state = 0;
    threadPrepare(&wifi_thread, wifi_worker, NULL, &wifi_stack[sizeof(wifi_stack)], MAIN_THREAD_PRIO + 1);
    threadStart(&wifi_thread);

    // Deadline: three tries take ~15 s when the network is just missing
    const int frames = 60 * RA_WIFI_TIMEOUT_SECONDS;
    int shown = -1, last_attempt = 0;
    for (int frame = 0; frame < frames && wifi_state == 0; frame++) {
        swiWaitForVBlank();
        if (wifi_attempt != last_attempt) {
            last_attempt = wifi_attempt;
            iprintf("WiFi %d/%d...\n", last_attempt, attempts);
        }
        int left = (frames - frame) / 60;
        if (left != shown && left % 5 == 0) {
            shown = left;
            iprintf(" (%ds, B: skip)\n", left);
        }
        scanKeys();
        if (keysDown() & KEY_B) break;
    }
    if (wifi_state == 1) {
        iprintf(" WiFi up\n");
        ra_net_profile_save();
        return 1;
    }
    if (wifi_state == 0) iprintf("\x1b[33mWiFi didn't come up\x1b[39m\n");
    return 0;
}

// sd:/_nds/ra/net.bin for nds-bootstrap-ra's in-game sending (see
// include/ra_netprofile.h): the access point in use, the DHCP lease and
// the server's address.  Nothing secret: the key stays in NVRAM.
void ra_net_profile_save(void) {
    WfcConnSlot *slot = wfcGetActiveSlot();
    if (!slot) return;
    RaNetProfile p;
    memset(&p, 0, sizeof(p));
    p.magic = RA_NET_PROFILE_MAGIC;
    p.version = RA_NET_PROFILE_VERSION;
    p.size = sizeof(p);
    p.conn_type = slot->conn_type;

    // The strongest scanned access point with the slot's SSID
    unsigned count = 0;
    WlanBssDesc *list = wfcGetScanBssList(&count);
    const WlanBssDesc *bss = NULL;
    for (unsigned i = 0; list && i < count; i++) {
        if (list[i].ssid_len == slot->ssid_len && !memcmp(list[i].ssid, slot->ssid, slot->ssid_len)
         && (!bss || list[i].rssi > bss->rssi)) bss = &list[i];
    }
    if (!bss) return;
    memcpy(p.bssid, bss->bssid, sizeof(p.bssid));
    p.ssid_len = bss->ssid_len;
    memcpy(p.ssid, bss->ssid, sizeof(p.ssid));
    p.ieee_caps = bss->ieee_caps;
    p.ieee_basic_rates = bss->ieee_basic_rates;
    p.ieee_all_rates = bss->ieee_all_rates;
    p.auth_type = bss->auth_type;
    p.channel = bss->channel;
    p.rssi = bss->rssi;

    struct in_addr gateway, netmask, dns1, dns2;
    struct in_addr ip = wfcGetIPConfig(&gateway, &netmask, &dns1, &dns2);
    p.ip = ip.s_addr;
    p.netmask = netmask.s_addr;
    p.gateway = gateway.s_addr;
    p.dns[0] = dns1.s_addr;
    p.dns[1] = dns2.s_addr;
    struct hostent *he = gethostbyname(RA_HOST);
    if (he && he->h_addrtype == AF_INET && he->h_addr_list[0]) memcpy(&p.ra_server, he->h_addr_list[0], 4);

    FILE *f = fopen(RA_NET_PROFILE_FILE, "wb");
    if (!f) return;
    fwrite(&p, 1, sizeof(p), f);
    fclose(f);
}

int ra_wifi_hung(void) {
    return wifi_state == 0 && threadIsValid(&wifi_thread) && !threadIsFinished(&wifi_thread);
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
