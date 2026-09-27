// Host check of step 2: the DSi's https.c, rcheevos rc_api and raset.c
// against the real RetroAchievements server, compared with set files the
// Pi rendered.  RA_USER / RA_TOKEN come from the environment; arguments are
// set files (their "game" line gives the hash).  Read-only API calls.
//
// Exercises keep-alive (several requests per connection) and, by closing
// the connection before every other game, TLS session resumption.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "https.h"
#include "raset.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"

int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen) {
    FILE *f = fopen("/dev/urandom", "rb");
    size_t n = fread(out, 1, len, f);
    fclose(f);
    *olen = n;
    (void)data;
    return 0;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(n + 1);
    if (fread(data, 1, n, f) != (size_t)n) n = 0;
    data[n] = '\0';
    fclose(f);
    return data;
}

static int call(rc_api_request_t *req, https_response *res, rc_api_server_response_t *sr) {
    int r = https_request(req->url, req->post_data, req->content_type, res);
    rc_api_destroy_request(req);
    if (r) return -1;
    sr->body = res->body;
    sr->body_length = res->length;
    sr->http_status_code = res->status;
    return 0;
}

int main(int argc, char **argv) {
    const char *user = getenv("RA_USER"), *token = getenv("RA_TOKEN");
    if (!user || !token) {
        printf("RA_USER and RA_TOKEN must be set\n");
        return 2;
    }
    if (https_init("RADirectDS/0.2 (host test) rcheevos/12.5")) return 1;

    rc_api_request_t req;
    https_response res;
    rc_api_server_response_t sr;

    rc_api_login_request_t lp = {0};
    lp.username = user;
    lp.api_token = token;
    rc_api_init_login_request(&req, &lp);
    if (call(&req, &res, &sr)) return 1;
    rc_api_login_response_t login;
    int r = rc_api_process_login_server_response(&login, &sr);
    printf("login: rc=%d ok=%d user=%s softcore=%lu (%u ms, HTTP %d)\n", r, login.response.succeeded,
           login.username ? login.username : "-", (unsigned long)login.score_softcore, res.ms, res.status);
    int failures = !(r == RC_OK && login.response.succeeded);
    rc_api_destroy_login_response(&login);
    free(res.body);

    for (int i = 1; i < argc; i++) {
        if (i % 2 == 0) https_close();  // next request reconnects: resumption
        char *pi = read_file(argv[i]);
        char *md5 = pi ? strstr(pi, "\ngame\t") : NULL;
        if (md5) md5 = strchr(md5 + 6, '\t');
        if (!md5) {
            printf("%s: no game line\n", argv[i]);
            failures++;
            continue;
        }
        char hash[33] = {0};
        memcpy(hash, md5 + 1, 32);

        rc_api_resolve_hash_request_t hp = {0};
        hp.game_hash = hash;
        rc_api_init_resolve_hash_request(&req, &hp);
        if (call(&req, &res, &sr)) return 1;
        rc_api_resolve_hash_response_t hr;
        rc_api_process_resolve_hash_server_response(&hr, &sr);
        uint32_t id = hr.game_id;
        printf("%s\n  gameid %lu (%u ms)\n", argv[i], (unsigned long)id, res.ms);
        rc_api_destroy_resolve_hash_response(&hr);
        free(res.body);

        rc_api_fetch_game_data_request_t gp = {0};
        gp.username = user;
        gp.api_token = token;
        gp.game_id = id;
        rc_api_init_fetch_game_data_request(&req, &gp);
        if (call(&req, &res, &sr)) return 1;
        rc_api_fetch_game_data_response_t game;
        r = rc_api_process_fetch_game_data_server_response(&game, &sr);
        unsigned count = 0;
        char *set = raset_render(&game, hash, &count, NULL);
        int same = !strcmp(set, pi);
        printf("  patch rc=%d HTTP %d %lu bytes %u ms: %u achievements, %s\n", r, res.status,
               (unsigned long)res.length, res.ms, count, same ? "SAME as Pi" : "DIFFERENT");
        if (!same) {
            FILE *f = fopen("/tmp/direct_set.txt", "wb");
            fputs(set, f);
            fclose(f);
            printf("  (written to /tmp/direct_set.txt)\n");
            failures++;
        }
        rc_api_destroy_fetch_game_data_response(&game);
        free(set);
        free(res.body);
        free(pi);
    }
    https_close();
    const https_stats *st = https_get_stats();
    printf("%u requests, %lu bytes, %u connections, %u full handshakes, %u resumed\n", st->requests,
           st->bytes, st->connects, st->handshakes, st->resumed);
    return failures != 0;
}
