#ifndef SERVER_H
#define SERVER_H

#include <limits.h>
#include <stdbool.h>

typedef struct {
    int  port;
    int  threads;               /* worker threads (used once the pool exists) */
    int  queue_cap;             /* job queue capacity (used once the pool exists) */
    bool silent;                /* no access log (for benchmarks) */
    char docroot[PATH_MAX];     /* realpath() of the document root, resolved once */
} server_config_t;

typedef struct {
    server_config_t cfg;
    int listen_fd;
} server_t;

/* Create a TCP socket listening on 0.0.0.0:port. Returns the fd, or -1. */
int server_listen(int port);

/* Read one request from fd and answer it. Does not close fd. */
void server_handle_connection(server_t *srv, int fd);

#endif
