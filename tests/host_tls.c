// Host check of the DSi TLS setup: same mbedTLS config, roots and calls as
// source/main.c, with POSIX sockets and a host entropy source.
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

extern const char ra_ca_pem[];
extern const size_t ra_ca_pem_len;

int mbedtls_hardware_poll(void *d, unsigned char *o, size_t l, size_t *ol) {
    FILE *f = fopen("/dev/urandom", "rb"); fread(o, 1, l, f); fclose(f); *ol = l; (void)d; return 0;
}
static int s_send(void *c, const unsigned char *b, size_t l) { return send(*(int *)c, b, l, 0); }
static int s_recv(void *c, unsigned char *b, size_t l) { int n = recv(*(int *)c, b, l, 0); return n == 0 ? MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : n; }

int main(void) {
    struct addrinfo *ai; getaddrinfo("retroachievements.org", "443", NULL, &ai);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    for (; ai; ai = ai->ai_next) if (ai->ai_family == AF_INET && connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    mbedtls_entropy_context e; mbedtls_ctr_drbg_context d; mbedtls_x509_crt ca; mbedtls_ssl_config conf; mbedtls_ssl_context ssl;
    mbedtls_entropy_init(&e); mbedtls_ctr_drbg_init(&d); mbedtls_x509_crt_init(&ca); mbedtls_ssl_config_init(&conf); mbedtls_ssl_init(&ssl);
    int r = mbedtls_ctr_drbg_seed(&d, mbedtls_entropy_func, &e, NULL, 0); if (r) { printf("seed %d\n", r); return 1; }
    r = mbedtls_x509_crt_parse(&ca, (const unsigned char *)ra_ca_pem, ra_ca_pem_len); if (r) { printf("ca %d\n", r); return 1; }
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &d);
    mbedtls_ssl_setup(&ssl, &conf); mbedtls_ssl_set_hostname(&ssl, "retroachievements.org");
    mbedtls_ssl_set_bio(&ssl, &fd, s_send, s_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        char m[128]; mbedtls_strerror(r, m, sizeof m); printf("handshake -0x%x %s\n", -r, m);
        char info[256]; mbedtls_x509_crt_verify_info(info, sizeof info, " ", mbedtls_ssl_get_verify_result(&ssl)); printf("%s\n", info); return 1;
    }
    printf("handshake ok: %s, verify=%u\n", mbedtls_ssl_get_ciphersuite(&ssl), mbedtls_ssl_get_verify_result(&ssl));
    const char *req = "GET /dorequest.php?r=gameid&m=9dbd0337235cd8acf032c0fbfd649d70 HTTP/1.1\r\nHost: retroachievements.org\r\nUser-Agent: RADirectDS/0.1 (Nintendo DSi; proof of concept)\r\nConnection: close\r\n\r\n";
    mbedtls_ssl_write(&ssl, (const unsigned char *)req, strlen(req));
    static char buf[8192]; int got = 0;
    while ((r = mbedtls_ssl_read(&ssl, (unsigned char *)buf + got, sizeof buf - 1 - got)) > 0) got += r;
    buf[got] = 0; char *eol = strstr(buf, "\r\n"); if (eol) *eol = 0;
    printf("%s\n%s\n", buf, eol ? strstr(eol + 2, "\r\n\r\n") + 4 : "");
    return 0;
}
