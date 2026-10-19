#include "util.h"

#include <errno.h>
#include <sys/socket.h>
#include <time.h>

ssize_t write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        /* MSG_NOSIGNAL: a peer that already closed gives EPIPE instead of SIGPIPE. */
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p    += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode(const char *src, char *dst, size_t dstsz) {
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            /* Short-circuit keeps us from reading past the terminator on "%" or "%a". */
            int hi = hex_value(src[i + 1]);
            int lo = (hi < 0) ? -1 : hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)(hi * 16 + lo);
            if (c == '\0') return -1;          /* %00 could truncate paths later */
            i += 2;
        }
        if (out + 1 >= dstsz) return -2;
        dst[out++] = (char)c;
    }
    dst[out] = '\0';
    return 0;
}

void format_http_date(char *buf, size_t n) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);                       /* gmtime() is not thread-safe */
    strftime(buf, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}
