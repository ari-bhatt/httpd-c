#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <sys/types.h>

/* Send all len bytes, looping over partial writes. Returns len, or -1 on error. */
ssize_t write_all(int fd, const void *buf, size_t len);

/*
 * Decode %XX escapes from src into dst (dstsz bytes, always NUL-terminated on success).
 * Returns 0 on success, -1 on a malformed escape or a decoded NUL byte,
 * -2 if the decoded result does not fit in dst.
 */
int url_decode(const char *src, char *dst, size_t dstsz);

/* Current time as an HTTP date, e.g. "Mon, 19 Oct 2026 04:00:00 GMT". */
void format_http_date(char *buf, size_t n);

#endif
