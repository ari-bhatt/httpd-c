#include "threadpool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int queue_init(job_queue_t *q, size_t cap) {
    memset(q, 0, sizeof *q);
    q->jobs = calloc(cap, sizeof *q->jobs);
    if (q->jobs == NULL) return -1;
    q->cap = cap;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q->jobs);
        return -1;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        free(q->jobs);
        return -1;
    }
    return 0;
}

void queue_destroy(job_queue_t *q) {
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
    free(q->jobs);
    q->jobs = NULL;
}

bool queue_try_push(job_queue_t *q, job_t job) {
    pthread_mutex_lock(&q->lock);
    if (q->count == q->cap || q->shutdown) {        /* full: caller sends 503 */
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    q->jobs[q->tail] = job;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    pthread_cond_signal(&q->not_empty);             /* wake exactly one idle worker */
    pthread_mutex_unlock(&q->lock);
    return true;
}

bool queue_pop(job_queue_t *q, job_t *out) {
    pthread_mutex_lock(&q->lock);
    /* WHILE, not IF: wakeups can be spurious, and another worker may take the
       job first. Always re-check the condition after waking. */
    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->lock);
    if (q->count == 0) {                            /* shutdown and fully drained */
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    *out = q->jobs[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return true;
}

void queue_shutdown(job_queue_t *q) {
    pthread_mutex_lock(&q->lock);
    q->shutdown = true;
    pthread_cond_broadcast(&q->not_empty);          /* every sleeping worker re-checks */
    pthread_mutex_unlock(&q->lock);
}

static void *worker_main(void *arg) {
    thread_pool_t *pool = arg;
    job_t job;
    while (queue_pop(pool->queue, &job)) {
        pool->handler(pool->ctx, &job);
        close(job.fd);
    }
    return NULL;
}

int pool_start(thread_pool_t *pool, size_t n, job_queue_t *q, job_handler_t handler, void *ctx) {
    pool->threads = calloc(n, sizeof *pool->threads);
    if (pool->threads == NULL) return -1;
    pool->nthreads = 0;
    pool->queue    = q;
    pool->handler  = handler;
    pool->ctx      = ctx;

    for (size_t i = 0; i < n; i++) {
        int rc = pthread_create(&pool->threads[i], NULL, worker_main, pool);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s (running with %zu workers)\n",
                    strerror(rc), pool->nthreads);
            break;
        }
        pool->nthreads++;
    }
    return pool->nthreads > 0 ? 0 : -1;
}

void pool_join(thread_pool_t *pool) {
    for (size_t i = 0; i < pool->nthreads; i++)
        pthread_join(pool->threads[i], NULL);
    free(pool->threads);
    pool->threads  = NULL;
    pool->nthreads = 0;
}
