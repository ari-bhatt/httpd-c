#include "http.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int read_request(int fd, char *buf) {
    /* TCP is a byte stream: one recv() may return only part of the request. */
    size_t used = 0;
    while (used < MAX_REQ) {
        ssize_t n = recv(fd, buf + used, MAX_REQ - used, 0);
        if (n == 0) return READ_CLOSED;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return READ_TIMEOUT;
            return READ_ERROR;
        }
        used += (size_t)n;
        buf[used] = '\0';
        /* memmem rather than strstr, so a stray NUL byte can't hide the terminator. */
        if (memmem(buf, used, "\r\n\r\n", 4) != NULL) return READ_OK;
    }
    return READ_TOO_LARGE;
}

int http_parse(const char *raw, http_request_t *out) {
    memset(out, 0, sizeof *out);

    const char *eol = strstr(raw, "\r\n");
    if (eol == NULL || eol == raw) return 400;            /* empty or no line ending */

    char line[MAX_REQ + 1];
    size_t len = (size_t)(eol - raw);
    if (len > MAX_REQ) return 400;
    memcpy(line, raw, len);
    line[len] = '\0';

    /* strtok_r, not strtok: strtok keeps hidden global state and isn't thread-safe. */
    char *save = NULL;
    char *method  = strtok_r(line, " ", &save);
    char *target  = strtok_r(NULL, " ", &save);
    char *version = strtok_r(NULL, " ", &save);
    if (method == NULL || target == NULL || version == NULL) return 400;
    if (strtok_r(NULL, " ", &save) != NULL) return 400;   /* extra tokens */

    /* Keep copies (possibly truncated) so error responses can still be logged. */
    snprintf(out->method,  sizeof out->method,  "%s", method);
    snprintf(out->version, sizeof out->version, "%s", version);

    if (strncmp(version, "HTTP/", 5) != 0) return 400;
    if (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0) return 505;

    if (strcmp(method, "GET") == 0) {
        out->head = false;
    } else if (strcmp(method, "HEAD") == 0) {
        out->head = true;
    } else {
        return 405;
    }

    char *query = strpbrk(target, "?#");                  /* drop query string / fragment */
    if (query != NULL) *query = '\0';

    if (target[0] != '/') return 400;
    if (strlen(target) >= sizeof out->path) return 414;

    int rc = url_decode(target, out->path, sizeof out->path);
    if (rc == -1) return 400;
    if (rc == -2) return 414;
    return 0;
}

const char *http_status_text(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int http_send_headers(int fd, int status, const char *content_type,
                      long long content_length, const char *extra) {
    char date[64];
    format_http_date(date, sizeof date);

    char hdr[1024];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Server: httpd-c/1.0\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     status, http_status_text(status), date, content_type,
                     content_length, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof hdr) return -1;
    return write_all(fd, hdr, (size_t)n) < 0 ? -1 : 0;
}

long long http_send_error(int fd, int status, bool head) {
    char body[256];
    int len = snprintf(body, sizeof body,
                       "<!DOCTYPE html><html><head><title>%d %s</title></head>"
                       "<body><h1>%d %s</h1></body></html>\n",
                       status, http_status_text(status), status, http_status_text(status));

    const char *extra = NULL;
    if (status == 405) extra = "Allow: GET, HEAD\r\n";
    if (status == 503) extra = "Retry-After: 1\r\n";

    if (http_send_headers(fd, status, "text/html; charset=utf-8", len, extra) < 0) return -1;
    if (head) return 0;                                    /* HEAD: headers only */
    return write_all(fd, body, (size_t)len) < 0 ? -1 : len;
}
