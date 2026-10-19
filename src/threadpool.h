#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* One accepted connection waiting to be served. */
typedef struct {
    int fd;
    struct sockaddr_in addr;        /* client address, for the access log */
    struct timespec accepted_at;    /* CLOCK_MONOTONIC, for latency measurement */
} job_t;

/*
 * Bounded FIFO of jobs: a circular buffer protected by one mutex, with a
 * condition variable that idle workers sleep on.
 */
typedef struct {
    job_t *jobs;
    size_t cap, head, tail, count;
    bool shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
} job_queue_t;

int  queue_init(job_queue_t *q, size_t cap);    /* 0 on success, -1 on failure */
void queue_destroy(job_queue_t *q);

/* Never blocks. Returns false if the queue is full (caller sheds the load). */
bool queue_try_push(job_queue_t *q, job_t job);

/*
 * Blocks until a job is available. Returns false only once shutdown has been
 * requested AND the queue is empty, so already-accepted jobs are still served.
 */
bool queue_pop(job_queue_t *q, job_t *out);

/* Wake every waiting worker and make queue_pop return false once drained. */
void queue_shutdown(job_queue_t *q);

/* Called by a worker for each job. The pool closes job->fd afterwards. */
typedef void (*job_handler_t)(void *ctx, const job_t *job);

typedef struct {
    pthread_t *threads;
    size_t nthreads;
    job_queue_t *queue;
    job_handler_t handler;
    void *ctx;
} thread_pool_t;

/* Start n workers pulling from q. Returns 0, or -1 if no thread could be started. */
int  pool_start(thread_pool_t *pool, size_t n, job_queue_t *q, job_handler_t handler, void *ctx);

/* Wait for every worker to exit (after queue_shutdown) and free the pool. */
void pool_join(thread_pool_t *pool);

#endif
