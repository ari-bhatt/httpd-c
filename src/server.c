#include "server.h"
#include "http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

int server_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* Lets the server restart immediately instead of waiting out TIME_WAIT. */
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),   /* 0.0.0.0, needed inside Docker */
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

void server_set_timeouts(int fd, int secs) {
    /* Without these, a client that connects and sends nothing would pin a worker
       forever; a few hundred of them (a "Slowloris" attack) would freeze the pool.
       After a receive timeout, recv() fails with EAGAIN, which becomes a 408. */
    struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
        perror("setsockopt(SO_RCVTIMEO)");
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0)
        perror("setsockopt(SO_SNDTIMEO)");
}

/*
 * Copy src into dst for logging, replacing quotes, backslashes and control
 * characters. The path is decoded client input: without this, a request for
 * "/x%0a127.0.0.1 ... 200" could forge a fake line in the log (log injection).
 */
static void sanitize(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c == 0x7f || c == '"' || c == '\\') ? '?' : (char)c;
    }
    dst[i] = '\0';
    if (i == 0 && n > 1) { dst[0] = '-'; dst[1] = '\0'; }
}

/*
 * One line per request, close to the nginx/Apache Common Log Format plus latency:
 *   127.0.0.1 [19/Oct/2026:04:00:00 +0000] "GET /index.html HTTP/1.1" 200 1234 0.412ms
 * Latency is measured from accept(), so it includes time spent waiting in the
 * queue: the latency the client actually experienced.
 */
static void log_access(const job_t *job, const http_request_t *req, int status, long long bytes) {
    char ip[INET_ADDRSTRLEN] = "-";
    inet_ntop(AF_INET, &job->addr.sin_addr, ip, sizeof ip);

    time_t now = time(NULL);
    struct tm tm;
    char when[40];
    gmtime_r(&now, &tm);
    strftime(when, sizeof when, "%d/%b/%Y:%H:%M:%S +0000", &tm);

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = (double)(end.tv_sec - job->accepted_at.tv_sec) * 1e3 +
                (double)(end.tv_nsec - job->accepted_at.tv_nsec) / 1e6;

    char method[sizeof req->method], path[sizeof req->path], version[sizeof req->version];
    sanitize(method,  sizeof method,  req->method);
    sanitize(path,    sizeof path,    req->path);
    sanitize(version, sizeof version, req->version);

    /* Format the whole line first, then write it with ONE call: stdio locks the
       stream per call, so lines from different workers never interleave. */
    char line[1400];
    int n = snprintf(line, sizeof line, "%s [%s] \"%s %s %s\" %d %lld %.3fms\n",
                     ip, when, method, path, version, status, bytes, ms);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) {                     /* truncated: keep the newline */
        n = (int)sizeof line - 1;
        line[n - 1] = '\n';
    }
    fwrite(line, 1, (size_t)n, stdout);
}

void server_handle_connection(void *ctx, const job_t *job) {
    server_t *srv = ctx;
    int fd = job->fd;
    char raw[MAX_REQ + 1];
    http_request_t req = {0};
    long long bytes = 0;
    int status;

    int rc = read_request(fd, raw);
    if (rc == READ_CLOSED || rc == READ_ERROR) return;   /* nothing to answer or log */

    if (rc == READ_TIMEOUT) {
        status = 408;
    } else if (rc == READ_TOO_LARGE) {
        status = 431;
    } else if ((status = http_parse(raw, &req)) == 0) {
        status = http_serve_file(fd, srv->cfg.docroot, &req, &bytes);
    }

    /* Single exit path: every answered request is counted and logged, errors included. */
    if (status == 200) {
        atomic_fetch_add(&srv->stats.served, 1);
    } else {
        bytes = http_send_error(fd, status, req.head);
        if (bytes < 0) bytes = 0;
        atomic_fetch_add(&srv->stats.errors, 1);
    }
    if (!srv->cfg.silent) log_access(job, &req, status, bytes);
}
