#include "server.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

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

int main(int argc, char **argv) {
    /* Writing to a socket the client already closed raises SIGPIPE, which would
       kill the whole process by default. */
    signal(SIGPIPE, SIG_IGN);

    server_t srv = {0};
    parse_args(argc, argv, &srv.cfg);

    srv.listen_fd = server_listen(srv.cfg.port);
    if (srv.listen_fd < 0) return 1;

    fprintf(stderr, "httpd-c listening on 0.0.0.0:%d (docroot %s)\n",
            srv.cfg.port, srv.cfg.docroot);

    /* Single-threaded baseline: accept, handle, close, repeat. */
    for (;;) {
        struct sockaddr_in client;
        socklen_t len = sizeof client;
        int fd = accept(srv.listen_fd, (struct sockaddr *)&client, &len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        server_handle_connection(&srv, fd);
        close(fd);
    }
}
