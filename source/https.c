// HTTPS client for RetroAchievements on the DSi; see include/https.h.
#include <nds.h>
#include <dswifi9.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "https.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define CONNECT_TIMEOUT_SECONDS 10
#define IO_TIMEOUT_SECONDS 20

extern const char ra_ca_pem[];
extern const size_t ra_ca_pem_len;

static const char *user_agent;

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt ca;
static mbedtls_ssl_config conf;
static mbedtls_ssl_context ssl;
static mbedtls_ssl_session saved_session;
static int have_session;

static int fd = -1;
static char connected_host[64];

// Buffered reads from the TLS stream
static unsigned char rbuf[4096];
static int rpos, rlen;

static https_stats stats;

// Timers 0+1 cascaded at the bus clock (33.513982 MHz): elapsed time
void timer_start(void) {
    TIMER_CR(0) = 0;
    TIMER_CR(1) = 0;
    TIMER_DATA(0) = 0;
    TIMER_DATA(1) = 0;
    TIMER_CR(1) = TIMER_ENABLE | TIMER_CASCADE;
    TIMER_CR(0) = TIMER_ENABLE | TIMER_DIV_1;
}

unsigned timer_ms(void) {
    u32 ticks = (u32)TIMER_DATA(0) | ((u32)TIMER_DATA(1) << 16);
    return (unsigned)((u64)ticks * 1000 / 33513982);
}

static void tls_error(const char *what, int err) {
    char msg[100];
    mbedtls_strerror(err, msg, sizeof(msg));
    iprintf("\x1b[31m%s: -0x%04X\n%s\x1b[39m\n", what, -err, msg);
}

// Blocking sockets with SO_RCVTIMEO/SO_SNDTIMEO: a negative result is a
// timeout or a dropped connection, never "try again".
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int n = send(*(int *)ctx, buf, len, 0);
    return n < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int n = recv(*(int *)ctx, buf, len, 0);
    if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
}

// connect() with a timeout: a dead host would otherwise block for minutes.
// Success is judged by select() + SO_ERROR rather than errno.
static int connect_with_timeout(int sock, const struct sockaddr *addr, socklen_t len) {
    int on = 1;
    ioctl(sock, FIONBIO, &on);
    int r = connect(sock, addr, len);
    if (r < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { CONNECT_TIMEOUT_SECONDS, 0 };
        r = -1;
        if (select(sock + 1, NULL, &wfds, NULL, &tv) > 0 && FD_ISSET(sock, &wfds)) {
            int err = 0;
            socklen_t err_len = sizeof(err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0) r = 0;
        }
    }
    int off = 0;
    ioctl(sock, FIONBIO, &off);
    return r;
}

int https_init(const char *agent) {
    user_agent = agent;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&ca);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_session_init(&saved_session);

    int r;
    const char *pers = "ra-direct";
    if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char *)pers, strlen(pers))) != 0) {
        tls_error("RNG seed", r);
        return -1;
    }
    if ((r = mbedtls_x509_crt_parse(&ca, (const unsigned char *)ra_ca_pem, ra_ca_pem_len)) != 0) {
        tls_error("CA parse", r);
        return -1;
    }
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    if ((r = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
        tls_error("TLS setup", r);
        return -1;
    }
    return 0;
}

void https_close(void) {
    if (fd >= 0) {
        mbedtls_ssl_close_notify(&ssl);
        close(fd);
        fd = -1;
    }
    connected_host[0] = '\0';
    rpos = rlen = 0;
}

const https_stats *https_get_stats(void) {
    return &stats;
}

static int https_connect(const char *host) {
    https_close();
    struct hostent *he = gethostbyname(host);
    if (!he) {
        iprintf("\x1b[31mDNS failed for %s\x1b[39m\n", host);
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        iprintf("\x1b[31mNo socket\x1b[39m\n");
        return -1;
    }
    struct timeval tv = { IO_TIMEOUT_SECONDS, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    addr.sin_addr = *(struct in_addr *)he->h_addr_list[0];
    if (connect_with_timeout(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        iprintf("\x1b[31mTCP connect failed\x1b[39m\n");
        close(fd);
        fd = -1;
        return -1;
    }
    stats.connects++;

    int r;
    mbedtls_ssl_session_reset(&ssl);
    if ((r = mbedtls_ssl_set_hostname(&ssl, host)) != 0) {
        tls_error("TLS hostname", r);
        https_close();
        return -1;
    }
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
    // Offer the last session (one host in practice; the server decides)
    if (have_session) mbedtls_ssl_set_session(&ssl, &saved_session);

    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        tls_error("Handshake", r);
        u32 flags = mbedtls_ssl_get_verify_result(&ssl);
        if (flags && flags != (u32)-1) {
            char info[256];
            mbedtls_x509_crt_verify_info(info, sizeof(info), " ", flags);
            iprintf("%s", info);
        }
        close(fd);
        fd = -1;
        have_session = 0;
        return -1;
    }
    // A server that resumes echoes the session id we offered; the
    // certificate was verified when that session was first made.
    const mbedtls_ssl_session *now = ssl.session;
    if (have_session && now && now->id_len && now->id_len == saved_session.id_len &&
        memcmp(now->id, saved_session.id, now->id_len) == 0) {
        stats.resumed++;
    } else {
        stats.handshakes++;
    }
    mbedtls_ssl_session_free(&saved_session);
    mbedtls_ssl_session_init(&saved_session);
    have_session = mbedtls_ssl_get_session(&ssl, &saved_session) == 0;

    snprintf(connected_host, sizeof(connected_host), "%s", host);
    rpos = rlen = 0;
    return 0;
}

static int write_all(const char *data, size_t len) {
    for (size_t sent = 0; sent < len;) {
        int r = mbedtls_ssl_write(&ssl, (const unsigned char *)data + sent, len - sent);
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (r < 0) return r;
        sent += r;
    }
    return 0;
}

// Refill rbuf: bytes read, 0 at the end of the stream, <0 on error
static int fill(void) {
    int r;
    do {
        r = mbedtls_ssl_read(&ssl, rbuf, sizeof(rbuf));
    } while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) r = 0;
    rpos = 0;
    rlen = r > 0 ? r : 0;
    return r;
}

// One line without its CRLF.  0 on success.
static int read_line(char *out, size_t max) {
    size_t n = 0;
    for (;;) {
        if (rpos >= rlen && fill() <= 0) return -1;
        char c = rbuf[rpos++];
        if (c == '\n') break;
        if (c != '\r' && n + 1 < max) out[n++] = c;
    }
    out[n] = '\0';
    return 0;
}

// Case-insensitive substring test (newlib has no strcasestr)
static int contains_nocase(const char *s, const char *word) {
    size_t n = strlen(word);
    for (; *s; s++)
        if (!strncasecmp(s, word, n)) return 1;
    return 0;
}

// Growable body buffer
typedef struct {
    char *data;
    size_t len, cap;
} buf_t;

static int buf_reserve(buf_t *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 16384;
    while (cap < b->len + extra + 1) cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

// Append exactly n bytes (or up to the end of the stream when n is -1)
static int read_body(buf_t *b, long n) {
    while (n != 0) {
        if (rpos >= rlen) {
            int r = fill();
            if (r < 0) return -1;
            if (r == 0) return n < 0 ? 0 : -1;
        }
        size_t take = rlen - rpos;
        if (n >= 0 && (long)take > n) take = n;
        if (buf_reserve(b, take)) return -1;
        memcpy(b->data + b->len, rbuf + rpos, take);
        b->len += take;
        rpos += take;
        if (n > 0) n -= take;
    }
    return 0;
}

// Send the request and read the response on the open connection.
// Returns 0, -1 on a failure before any response byte (the caller may retry
// on a fresh connection), or -2 on a later failure.
static int exchange(const char *host, const char *path, const char *post_data,
                    const char *content_type, https_response *res) {
    size_t post_len = post_data ? strlen(post_data) : 0;
    char head[512];
    int head_len;
    if (post_data) {
        head_len = snprintf(head, sizeof(head),
                            "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
                            "Content-Type: %s\r\nContent-Length: %u\r\n\r\n",
                            path, host, user_agent,
                            content_type ? content_type : "application/x-www-form-urlencoded",
                            (unsigned)post_len);
    } else {
        head_len = snprintf(head, sizeof(head),
                            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n\r\n",
                            path, host, user_agent);
    }
    if (head_len >= (int)sizeof(head)) return -2;
    if (write_all(head, head_len) || (post_len && write_all(post_data, post_len))) return -1;

    char line[512];
    if (read_line(line, sizeof(line))) return -1;
    if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12) return -2;
    res->status = atoi(line + 9);

    long content_length = -1;
    int chunked = 0, closing = 0;
    for (;;) {
        if (read_line(line, sizeof(line))) return -2;
        if (!line[0]) break;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ') value++;
        if (!strcasecmp(line, "Content-Length")) content_length = atol(value);
        else if (!strcasecmp(line, "Transfer-Encoding") && contains_nocase(value, "chunked")) chunked = 1;
        else if (!strcasecmp(line, "Connection") && contains_nocase(value, "close")) closing = 1;
    }

    buf_t body = {0};
    int err = 0;
    if (chunked) {
        for (;;) {
            if (read_line(line, sizeof(line))) { err = 1; break; }
            long size = strtol(line, NULL, 16);
            if (size <= 0) {
                // trailers up to the blank line
                while (!(err = read_line(line, sizeof(line)) != 0) && line[0]) {}
                break;
            }
            if (read_body(&body, size) || read_line(line, sizeof(line))) { err = 1; break; }
        }
    } else if (content_length >= 0) {
        err = read_body(&body, content_length) != 0;
    } else {
        err = read_body(&body, -1) != 0;
        closing = 1;
    }
    if (err || buf_reserve(&body, 0)) {
        free(body.data);
        return -2;
    }
    body.data[body.len] = '\0';
    res->body = body.data;
    res->length = body.len;
    if (closing) https_close();
    return 0;
}

int https_request(const char *url, const char *post_data, const char *content_type,
                  https_response *res) {
    memset(res, 0, sizeof(*res));
    if (strncmp(url, "https://", 8) != 0) {
        iprintf("\x1b[31mNot https: %s\x1b[39m\n", url);
        return -1;
    }
    char host[64];
    const char *path = strchr(url + 8, '/');
    size_t host_len = path ? (size_t)(path - (url + 8)) : strlen(url + 8);
    if (host_len >= sizeof(host)) return -1;
    memcpy(host, url + 8, host_len);
    host[host_len] = '\0';
    if (!path) path = "/";

    timer_start();
    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = fd >= 0 && strcmp(host, connected_host) == 0;
        if (!reused && https_connect(host)) return -1;
        int r = exchange(host, path, post_data, content_type, res);
        if (r == 0) {
            res->ms = timer_ms();
            stats.requests++;
            stats.bytes += res->length;
            return 0;
        }
        https_close();
        // The server may have closed an idle keep-alive connection: retry
        // once on a new one.  Anything else is a real failure.
        if (!(r == -1 && reused)) break;
    }
    iprintf("\x1b[31mRequest failed\x1b[39m\n");
    return -1;
}
