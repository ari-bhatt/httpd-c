#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>

#define MAX_REQ 8192        /* max bytes of request line + headers */

enum read_result { READ_OK, READ_CLOSED, READ_TIMEOUT, READ_TOO_LARGE, READ_ERROR };

typedef struct {
    char method[8];
    char path[1024];        /* URL-decoded, query string removed */
    char version[16];
    bool head;              /* HEAD request: send headers only */
} http_request_t;

/* Read from fd until the blank line ending the headers. buf must hold MAX_REQ + 1 bytes. */
int read_request(int fd, char *buf);

/*
 * Parse the request line in raw. Pure function: no I/O, so it can be unit tested.
 * Returns 0 on success, or the HTTP status code to send back (400, 405, 414, 505).
 */
int http_parse(const char *raw, http_request_t *out);

const char *http_status_text(int status);

/*
 * Send a status line plus the standard headers. extra is NULL or additional header
 * lines, each ending in "\r\n". Returns 0 on success, -1 on a write error.
 */
int http_send_headers(int fd, int status, const char *content_type,
                      long long content_length, const char *extra);

/* Send an error response with a small HTML body. Returns body bytes sent, or -1. */
long long http_send_error(int fd, int status, bool head);

#endif
