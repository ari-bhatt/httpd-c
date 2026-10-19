#ifndef SERVER_H
#define SERVER_H

#include "threadpool.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>

/* Seconds a client may stay idle while sending its request or reading our reply. */
#define CLIENT_TIMEOUT_SECS 5

typedef struct {
    int  port;
    int  threads;               /* worker threads */
    int  queue_cap;             /* job queue capacity; beyond this, clients get 503 */
    bool silent;                /* no access log (for benchmarks) */
    char docroot[PATH_MAX];     /* realpath() of the document root, resolved once */
} server_config_t;

/* Updated by many threads at once, so they are C11 atomics, not plain counters. */
typedef struct {
    atomic_ulong served;        /* 200 responses */
    atomic_ulong rejected;      /* 503s from a full queue (load shedding) */
    atomic_ulong errors;        /* other error responses (4xx/5xx) */
} server_stats_t;

typedef struct {
    server_config_t cfg;
    int listen_fd;
    job_queue_t queue;
    thread_pool_t pool;
    server_stats_t stats;
} server_t;

/* Create a TCP socket listening on 0.0.0.0:port. Returns the fd, or -1. */
int server_listen(int port);

/* Apply receive and send timeouts to a client socket (Slowloris protection). */
void server_set_timeouts(int fd, int secs);

/* Worker entry point: read one request from job->fd and answer it.
   ctx is the server_t. Does not close the fd (the pool does). */
void server_handle_connection(void *ctx, const job_t *job);

#endif
