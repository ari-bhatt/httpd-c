#include "server.h"
#include "http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
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

void server_handle_connection(void *ctx, const job_t *job) {
    server_t *srv = ctx;
    int fd = job->fd;
    char raw[MAX_REQ + 1];
    http_request_t req = {0};
    long long bytes = 0;
    int status;

    int rc = read_request(fd, raw);
    if (rc == READ_CLOSED || rc == READ_ERROR) return;   /* nothing to answer */
    if (rc == READ_TIMEOUT) {
        status = 408;
    } else if (rc == READ_TOO_LARGE) {
        status = 431;
    } else if ((status = http_parse(raw, &req)) == 0) {
        status = http_serve_file(fd, srv->cfg.docroot, &req, &bytes);
        if (status == 200) return;                       /* response already sent */
    }
    http_send_error(fd, status, req.head);
}
