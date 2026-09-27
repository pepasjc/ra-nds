// HTTPS client for RetroAchievements on the DSi; see include/https.h.
#ifdef __NDS__
#include <nds.h>
#include <dswifi9.h>
#else
// Host build (tests/run_host_step2.sh): same code over POSIX sockets
#include <time.h>
#define iprintf printf
#define closesocket close
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
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

// Longest wait for the server between two reads
#define READ_TIMEOUT_MS 15000

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

// Certificates checked: only a full handshake has any (a resumed session was
// verified when it was first made)
static unsigned certs_checked;

static int count_certs(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)ctx, (void)crt, (void)depth, (void)flags;
    certs_checked++;
    return 0;
}

#ifdef __NDS__
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
#else
static struct timespec timer_zero;

void timer_start(void) {
    clock_gettime(CLOCK_MONOTONIC, &timer_zero);
}

unsigned timer_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned)((now.tv_sec - timer_zero.tv_sec) * 1000 +
                      (now.tv_nsec - timer_zero.tv_nsec) / 1000000);
}
#endif

// Most recent failure, for the apps' logs
static char last_error[128];

static void set_error(const char *msg) {
    snprintf(last_error, sizeof(last_error), "%s", msg);
}

static void wait_seconds(int seconds) {
#ifdef __NDS__
    for (int i = 0; i < 60 * seconds; i++) swiWaitForVBlank();
#else
    sleep(seconds);
#endif
}

static void tls_error(const char *what, int err) {
    char msg[100];
    // mbedtls_strerror only knows the modules built in, and the net module
    // isn't: name the socket errors bio_send/bio_recv return
    if (err == MBEDTLS_ERR_NET_CONN_RESET) strcpy(msg, "connection closed");
    else if (err == MBEDTLS_ERR_NET_RECV_FAILED) strcpy(msg, "receive failed");
    else if (err == MBEDTLS_ERR_NET_SEND_FAILED) strcpy(msg, "send failed");
    else if (err == MBEDTLS_ERR_SSL_TIMEOUT) strcpy(msg, "no answer (timeout)");
    else mbedtls_strerror(err, msg, sizeof(msg));
    iprintf("\x1b[31m%s: -0x%04X\n%s\x1b[39m\n", what, -err, msg);
    snprintf(last_error, sizeof(last_error), "%s: -0x%04X %s", what, -err, msg);
}

// Blocking sockets: a negative result is a dropped connection, never "try
// again".  (dswifi's sgIP stack ignores SO_RCVTIMEO: setsockopt is a stub.)
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int n = send(*(int *)ctx, buf, len, 0);
    return n < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int n = recv(*(int *)ctx, buf, len, 0);
    if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : n;
}

// Reads with a deadline: with no socket timeouts, a stalled connection made
// recv() block for good (a full run froze at game 56).  select() for reading
// waits for data, then recv() takes it.
static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout_ms) {
    int sock = *(int *)ctx;
    if (timeout_ms) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int n = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (n == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
        if (n < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return bio_recv(ctx, buf, len);
}

int https_init(const char *agent) {
    static int ready;
    user_agent = agent;
    if (ready) return 0;
    ready = 1;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&ca);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_session_init(&saved_session);

    int r;
    const char *pers = "ra-nds";
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
    mbedtls_ssl_conf_verify(&conf, count_certs, NULL);
    mbedtls_ssl_conf_read_timeout(&conf, READ_TIMEOUT_MS);
    if ((r = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
        tls_error("TLS setup", r);
        return -1;
    }
    return 0;
}

static void (*trace_hook)(const char *step);

void https_set_trace(void (*trace)(const char *step)) {
    trace_hook = trace;
}

#define TRACE(step) do { if (trace_hook) trace_hook(step); } while (0)

// closesocket(), never close(): sgIP numbers sockets from 1 and close() is
// newlib's, so close(1) shut stdout and the console went quiet.
void https_close(void) {
    if (fd >= 0) {
        TRACE("close_notify");
        mbedtls_ssl_close_notify(&ssl);
        TRACE("close socket");
        closesocket(fd);
        TRACE("socket closed");
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
        set_error("DNS failed");
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        iprintf("\x1b[31mNo socket\x1b[39m\n");
        set_error("no socket");
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    addr.sin_addr = *(struct in_addr *)he->h_addr_list[0];
    // Plain blocking connect: sgIP's select() reports a connecting socket
    // writable at once, so a non-blocking connect "succeeds" before the TCP
    // handshake is done and TLS then reads a closed stream.
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        iprintf("\x1b[31mTCP connect failed\x1b[39m\n");
        set_error("TCP connect failed");
        closesocket(fd);
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
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, NULL, bio_recv_timeout);
    // Offer the last session (one host in practice; the server decides)
    if (have_session) mbedtls_ssl_set_session(&ssl, &saved_session);

    certs_checked = 0;
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        tls_error("Handshake", r);
        uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
        if (flags && flags != (uint32_t)-1) {
            char info[256];
            mbedtls_x509_crt_verify_info(info, sizeof(info), " ", flags);
            iprintf("%s", info);
        }
        closesocket(fd);
        fd = -1;
        have_session = 0;
        return -1;
    }
    if (certs_checked) {
        stats.handshakes++;
    } else {
        stats.resumed++;
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
    // Three tries, a second or two apart: right after the WiFi comes up the
    // first connection sometimes closes mid-handshake
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            stats.retries++;
            wait_seconds(attempt);
        }
        int reused = fd >= 0 && strcmp(host, connected_host) == 0;
        if (!reused && https_connect(host)) continue;
        int r = exchange(host, path, post_data, content_type, res);
        if (r == 0) {
            res->ms = timer_ms();
            stats.requests++;
            stats.bytes += res->length;
            return 0;
        }
        https_close();
        // A closed keep-alive connection or a stall (timeout): try again on
        // a new connection.  Safe: RA's read calls are idempotent, and so is
        // an award (a repeat is refused as already unlocked).
        if (!last_error[0]) set_error("no answer from the server");
        iprintf("\x1b[33mRetrying on a new connection\x1b[39m\n");
        free(res->body);
        memset(res, 0, sizeof(*res));
    }
    iprintf("\x1b[31mRequest failed\x1b[39m\n");
    return -1;
}

const char *https_last_error(void) {
    return last_error;
}
