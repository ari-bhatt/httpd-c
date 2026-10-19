#include "server.h"
#include "http.h"
#include "util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
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

void server_handle_connection(server_t *srv, int fd) {
    (void)srv;                                  /* docroot is used once files are served */

    char raw[MAX_REQ + 1];
    http_request_t req = {0};
    int status;

    int rc = read_request(fd, raw);
    if (rc == READ_CLOSED || rc == READ_ERROR) return;   /* nothing to answer */
    if (rc == READ_TIMEOUT) {
        status = 408;
    } else if (rc == READ_TOO_LARGE) {
        status = 431;
    } else if ((status = http_parse(raw, &req)) == 0) {
        /* Temporary: echo the parsed request until file serving is added. */
        char body[1200];
        int len = snprintf(body, sizeof body, "httpd-c parsed: %s %s %s\n",
                           req.method, req.path, req.version);
        if (len < 0) return;
        if ((size_t)len >= sizeof body) len = (int)sizeof body - 1;
        if (http_send_headers(fd, 200, "text/plain; charset=utf-8", len, NULL) == 0 && !req.head)
            write_all(fd, body, (size_t)len);
        return;
    }
    http_send_error(fd, status, req.head);
}
