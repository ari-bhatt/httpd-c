#include "http.h"
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Set by the signal handler, read by the accept loop. sig_atomic_t + volatile is
   the only kind of variable a handler may safely write. */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;            /* nothing else here: almost no function is async-signal-safe */
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-p port] [-t threads] [-q queue_cap] [-r docroot] [-s]\n"
            "  -p  port to listen on            (default 8080)\n"
            "  -t  worker threads               (default: number of CPU cores)\n"
            "  -q  job queue capacity           (default 256)\n"
            "  -r  document root                (default ./www)\n"
            "  -s  silent: no access log, for benchmarks\n",
            prog);
}

static int parse_int(const char *s, int min, int max, const char *name) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
        fprintf(stderr, "invalid %s: '%s' (expected %d-%d)\n", name, s, min, max);
        exit(2);
    }
    return (int)v;
}

static void parse_args(int argc, char **argv, server_config_t *cfg) {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    const char *docroot = "./www";

    cfg->port      = 8080;
    cfg->threads   = cores > 0 ? (int)cores : 1;
    cfg->queue_cap = 256;
    cfg->silent    = false;

    int opt;
    while ((opt = getopt(argc, argv, "p:t:q:r:sh")) != -1) {
        switch (opt) {
        case 'p': cfg->port      = parse_int(optarg, 1, 65535, "port");     break;
        case 't': cfg->threads   = parse_int(optarg, 1, 1024, "threads");   break;
        case 'q': cfg->queue_cap = parse_int(optarg, 1, 1000000, "queue");  break;
        case 'r': docroot = optarg;                                         break;
        case 's': cfg->silent = true;                                       break;
        case 'h': usage(argv[0]); exit(0);
        default:  usage(argv[0]); exit(2);
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        exit(2);
    }

    /* Resolve the docroot once; path-safety checks compare against this. */
    if (realpath(docroot, cfg->docroot) == NULL) {
        perror(docroot);
        exit(1);
    }
    struct stat st;
    if (stat(cfg->docroot, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "docroot is not a directory: %s\n", cfg->docroot);
        exit(1);
    }
}

static void install_signal_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;       /* no SA_RESTART: a blocked wait must return EINTR */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);   /* what `docker stop` and `kill` send */
}

int main(int argc, char **argv) {
    /* Writing to a socket the client already closed raises SIGPIPE, which would
       kill the whole process by default. */
    signal(SIGPIPE, SIG_IGN);
    install_signal_handlers();

    /* Line-buffer the access log so `docker logs` / `tail -f` see lines immediately. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    server_t srv = {0};
    parse_args(argc, argv, &srv.cfg);

    srv.listen_fd = server_listen(srv.cfg.port);
    if (srv.listen_fd < 0) return 1;
    /* Non-blocking listen socket: if a client vanishes between poll() saying
       "ready" and our accept(), accept() returns EAGAIN instead of hanging. */
    fcntl(srv.listen_fd, F_SETFL, fcntl(srv.listen_fd, F_GETFL) | O_NONBLOCK);

    if (queue_init(&srv.queue, (size_t)srv.cfg.queue_cap) < 0) {
        fprintf(stderr, "failed to allocate job queue\n");
        return 1;
    }

    /*
     * A process-directed signal can be delivered to ANY thread that hasn't blocked
     * it. Block SIGINT/SIGTERM before creating the workers (threads inherit the
     * mask), so they can never steal the signal. The main thread keeps them
     * blocked too, and unblocks them only while waiting inside ppoll() below.
     */
    sigset_t stop_signals, orig_mask;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, &orig_mask);

    if (pool_start(&srv.pool, (size_t)srv.cfg.threads, &srv.queue,
                   server_handle_connection, &srv) < 0) {
        fprintf(stderr, "failed to start worker threads\n");
        return 1;
    }

    fprintf(stderr, "httpd-c listening on 0.0.0.0:%d (docroot %s, %zu threads, queue %d)\n",
            srv.cfg.port, srv.cfg.docroot, srv.pool.nthreads, srv.cfg.queue_cap);

    /*
     * Producer side: the main thread only accepts connections and hands them to
     * the workers. If the queue is full, the server is overloaded: answer 503
     * right away instead of letting work and latency pile up (load shedding).
     */
    while (!g_stop) {
        /*
         * ppoll() atomically swaps in orig_mask (signals unblocked) for the
         * duration of the wait. A signal can therefore only arrive while we are
         * waiting, never between the g_stop check and the wait, so a Ctrl+C can't
         * slip through and leave us blocked until the next connection.
         */
        struct pollfd pfd = { .fd = srv.listen_fd, .events = POLLIN };
        int ready = ppoll(&pfd, 1, NULL, &orig_mask);
        if (ready < 0) {
            if (errno != EINTR) perror("ppoll");
            continue;                              /* loop re-checks g_stop */
        }

        job_t job;
        socklen_t len = sizeof job.addr;
        job.fd = accept(srv.listen_fd, (struct sockaddr *)&job.addr, &len);
        if (job.fd < 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ECONNABORTED)
                continue;
            perror("accept");
            if (err == EMFILE || err == ENFILE) {
                /* Out of file descriptors: back off briefly instead of spinning. */
                struct timespec pause = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
                nanosleep(&pause, NULL);
            }
            continue;
        }
        /* On Linux, accepted sockets do NOT inherit O_NONBLOCK, so client
           sockets stay blocking (with timeouts), as the workers expect. */
        clock_gettime(CLOCK_MONOTONIC, &job.accepted_at);
        server_set_timeouts(job.fd, CLIENT_TIMEOUT_SECS);

        if (!queue_try_push(&srv.queue, job)) {
            http_send_error(job.fd, 503, false);
            atomic_fetch_add(&srv.stats.rejected, 1);
            close(job.fd);
        }
    }

    /*
     * Graceful shutdown:
     *   1. stop accepting new connections,
     *   2. tell the queue to shut down and wake every sleeping worker,
     *   3. join the workers; they finish every job already queued first,
     *   4. free everything and exit 0.
     */
    fprintf(stderr, "\nshutdown requested, finishing queued connections...\n");
    close(srv.listen_fd);
    queue_shutdown(&srv.queue);
    pool_join(&srv.pool);
    queue_destroy(&srv.queue);

    fprintf(stderr, "shutting down: served=%lu rejected_503=%lu errors=%lu\n",
            atomic_load(&srv.stats.served), atomic_load(&srv.stats.rejected),
            atomic_load(&srv.stats.errors));
    return 0;
}
