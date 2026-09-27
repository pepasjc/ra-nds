// RA Direct, proof of concept: HTTPS from a DSi straight to RetroAchievements.
//
// Connects with the console's saved WiFi settings (DSi mode: WPA2 slots 4-6
// work), opens TLS 1.2 to retroachievements.org with mbedTLS, checks the
// certificate chain against the roots in certs.c and the DSi's clock, then
// asks RA for the game id of Tetris DS (USA) by its hash: an unauthenticated
// call that changes nothing.  Prints how long each step took.
#include <nds.h>
#include <dswifi9.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define HOST "retroachievements.org"
#define PATH "/dorequest.php?r=gameid&m=9dbd0337235cd8acf032c0fbfd649d70"
#define USER_AGENT "RADirectDS/0.1 (Nintendo DSi; proof of concept)"

extern const char ra_ca_pem[];
extern const size_t ra_ca_pem_len;

// Timers 0+1 cascaded at the bus clock (33.513982 MHz): elapsed time
static void timer_start(void) {
    TIMER_CR(0) = 0;
    TIMER_CR(1) = 0;
    TIMER_DATA(0) = 0;
    TIMER_DATA(1) = 0;
    TIMER_CR(1) = TIMER_ENABLE | TIMER_CASCADE;
    TIMER_CR(0) = TIMER_ENABLE | TIMER_DIV_1;
}

static u32 timer_ms(void) {
    u32 ticks = (u32)TIMER_DATA(0) | ((u32)TIMER_DATA(1) << 16);
    return (u32)((u64)ticks * 1000 / 33513982);
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int n = send(*(int *)ctx, buf, len, 0);
    return n < 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int n = recv(*(int *)ctx, buf, len, 0);
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return n < 0 ? MBEDTLS_ERR_SSL_WANT_READ : n;
}

static void fail(const char *what, int err) {
    char msg[100];
    mbedtls_strerror(err, msg, sizeof(msg));
    iprintf("\x1b[31m%s: -0x%04X\n%s\x1b[39m\n", what, -err, msg);
}

static void wait_start(void) {
    iprintf("\nPress START to exit\n");
    while (pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START) break;
    }
}

int main(void) {
    consoleDemoInit();
    iprintf("RA Direct: HTTPS test\n");
    iprintf("%s mode\n\n", isDSiMode() ? "DSi" : "DS");

    iprintf("WiFi (saved connections)...\n");
    timer_start();
    if (!Wifi_InitDefault(WFC_CONNECT)) {
        iprintf("\x1b[31mNo WiFi connection\x1b[39m\n");
        wait_start();
        return 0;
    }
    iprintf(" up in %lu ms\n", (unsigned long)timer_ms());

    timer_start();
    struct hostent *he = gethostbyname(HOST);
    if (!he) {
        iprintf("\x1b[31mDNS failed for %s\x1b[39m\n", HOST);
        wait_start();
        return 0;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    addr.sin_addr = *(struct in_addr *)he->h_addr_list[0];
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        iprintf("\x1b[31mTCP connect failed\x1b[39m\n");
        wait_start();
        return 0;
    }
    iprintf("DNS + TCP: %lu ms\n", (unsigned long)timer_ms());

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&ca);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);

    int r;
    const char *pers = "ra-direct";
    if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char *)pers, strlen(pers))) != 0) {
        fail("RNG seed", r);
        goto done;
    }
    if ((r = mbedtls_x509_crt_parse(&ca, (const unsigned char *)ra_ca_pem, ra_ca_pem_len)) != 0) {
        fail("CA parse", r);
        goto done;
    }
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if ((r = mbedtls_ssl_setup(&ssl, &conf)) != 0 || (r = mbedtls_ssl_set_hostname(&ssl, HOST)) != 0) {
        fail("TLS setup", r);
        goto done;
    }
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);

    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    iprintf("Clock: %04d-%02d-%02d %02d:%02d\n", tm->tm_year + 1900, tm->tm_mon + 1,
            tm->tm_mday, tm->tm_hour, tm->tm_min);

    iprintf("TLS handshake...\n");
    timer_start();
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fail("Handshake", r);
            u32 flags = mbedtls_ssl_get_verify_result(&ssl);
            if (flags) {
                char info[256];
                mbedtls_x509_crt_verify_info(info, sizeof(info), " ", flags);
                iprintf("%s", info);
            }
            goto done;
        }
    }
    iprintf(" done in %lu ms\n", (unsigned long)timer_ms());
    iprintf(" %s\n", mbedtls_ssl_get_ciphersuite(&ssl));
    iprintf(" certificate verified\n");

    char req[256];
    int len = snprintf(req, sizeof(req),
                       "GET " PATH " HTTP/1.1\r\nHost: " HOST "\r\nUser-Agent: " USER_AGENT
                       "\r\nConnection: close\r\n\r\n");
    timer_start();
    for (int sent = 0; sent < len;) {
        r = mbedtls_ssl_write(&ssl, (const unsigned char *)req + sent, len - sent);
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (r < 0) {
            fail("Write", r);
            goto done;
        }
        sent += r;
    }
    static char resp[4096];
    int got = 0;
    while (got < (int)sizeof(resp) - 1) {
        r = mbedtls_ssl_read(&ssl, (unsigned char *)resp + got, sizeof(resp) - 1 - got);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) break;
        got += r;
    }
    resp[got] = '\0';
    iprintf("Request: %lu ms, %d bytes\n\n", (unsigned long)timer_ms(), got);

    char *eol = strstr(resp, "\r\n");
    if (eol) *eol = '\0';
    iprintf("%s\n", resp);
    char *body = eol ? strstr(eol + 2, "\r\n\r\n") : NULL;
    if (body) iprintf("%.200s\n", body + 4);

    mbedtls_ssl_close_notify(&ssl);

done:
    close(fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&ca);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    wait_start();
    return 0;
}
