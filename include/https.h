// HTTPS client for RetroAchievements on the DSi: mbedTLS over dswifi sockets.
//
// One connection is kept open between requests (HTTP/1.1 keep-alive), so a
// run of API calls pays for one TLS handshake; a dropped connection is
// reopened with an abbreviated handshake from the saved session.
#ifndef HTTPS_H
#define HTTPS_H

#include <stddef.h>

typedef struct {
    int status;        // HTTP status code
    char *body;        // malloc'd, NUL-terminated; free() it
    size_t length;     // body bytes (excluding the NUL)
    unsigned ms;       // time for the whole request
} https_response;

typedef struct {
    unsigned connects;     // TCP connections opened
    unsigned handshakes;   // full TLS handshakes
    unsigned resumed;      // abbreviated (resumed session) handshakes
    unsigned requests;
    unsigned long bytes;   // response bodies
} https_stats;

// Seed the RNG and load the trust anchors.  0 on success.
int https_init(const char *user_agent);

// POST post_data (or GET when post_data is NULL) to an https:// URL.
// 0 on success (any HTTP status); otherwise prints why and returns -1.
int https_request(const char *url, const char *post_data, const char *content_type,
                  https_response *res);

void https_close(void);
const https_stats *https_get_stats(void);

// Elapsed milliseconds from timers 0+1 (wraps after about two minutes)
void timer_start(void);
unsigned timer_ms(void);

#endif
