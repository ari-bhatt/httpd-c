/*
 * Unit tests for the pure, socket-free parts of httpd-c: the request parser,
 * URL decoding, MIME lookup, write_all and the bounded job queue.
 * No framework needed: CHECK counts passes and prints the line of each failure.
 *
 * Run with: make test
 */
#include "http.h"
#include "threadpool.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int passed = 0, failed = 0;

#define CHECK(cond) do {                                                      \
        if (cond) passed++;                                                   \
        else { failed++; fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                                 __FILE__, __LINE__, #cond); }                \
    } while (0)

/* Parse a raw request and return the status (0 = OK). */
static int parse(const char *raw, http_request_t *r) {
    return http_parse(raw, r);
}

static void test_parse_valid(void) {
    http_request_t r;

    CHECK(parse("GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n", &r) == 0);
    CHECK(strcmp(r.method, "GET") == 0);
    CHECK(strcmp(r.path, "/index.html") == 0);
    CHECK(strcmp(r.version, "HTTP/1.1") == 0);
    CHECK(r.head == false);

    CHECK(parse("HEAD / HTTP/1.1\r\n\r\n", &r) == 0);
    CHECK(r.head == true);
    CHECK(strcmp(r.path, "/") == 0);

    CHECK(parse("GET / HTTP/1.0\r\n\r\n", &r) == 0);          /* HTTP/1.0 accepted */
    CHECK(strcmp(r.version, "HTTP/1.0") == 0);
}

static void test_parse_methods(void) {
    http_request_t r;
    CHECK(parse("POST / HTTP/1.1\r\n\r\n", &r) == 405);
    CHECK(parse("PUT / HTTP/1.1\r\n\r\n", &r) == 405);
    CHECK(parse("DELETE / HTTP/1.1\r\n\r\n", &r) == 405);
    CHECK(parse("get / HTTP/1.1\r\n\r\n", &r) == 405);        /* methods are case-sensitive */
    CHECK(strcmp(r.method, "get") == 0);                       /* kept for the access log */
}

static void test_parse_malformed(void) {
    http_request_t r;
    CHECK(parse("", &r) == 400);                               /* empty request */
    CHECK(parse("\r\n\r\n", &r) == 400);                       /* blank request line */
    CHECK(parse("GET /\r\n\r\n", &r) == 400);                  /* missing version */
    CHECK(parse("GET\r\n\r\n", &r) == 400);                    /* missing path */
    CHECK(parse("GET / HTTP/1.1 extra\r\n\r\n", &r) == 400);   /* too many tokens */
    CHECK(parse("GET / HTTP/1.1", &r) == 400);                 /* no CRLF at all */
    CHECK(parse("GET index.html HTTP/1.1\r\n\r\n", &r) == 400);/* path must start with / */
    CHECK(parse("GET http://x/ HTTP/1.1\r\n\r\n", &r) == 400); /* absolute-form not supported */
    CHECK(parse("GET / FTP/1.0\r\n\r\n", &r) == 400);          /* not HTTP at all */
}

static void test_parse_versions(void) {
    http_request_t r;
    CHECK(parse("GET / HTTP/2.0\r\n\r\n", &r) == 505);
    CHECK(parse("GET / HTTP/0.9\r\n\r\n", &r) == 505);
    CHECK(parse("GET / HTTP/1.2\r\n\r\n", &r) == 505);
}

static void test_parse_paths(void) {
    http_request_t r;

    CHECK(parse("GET /a.html?x=1&y=2 HTTP/1.1\r\n\r\n", &r) == 0);   /* query stripped */
    CHECK(strcmp(r.path, "/a.html") == 0);

    CHECK(parse("GET /a.html#top HTTP/1.1\r\n\r\n", &r) == 0);       /* fragment stripped */
    CHECK(strcmp(r.path, "/a.html") == 0);

    CHECK(parse("GET /my%20file.txt HTTP/1.1\r\n\r\n", &r) == 0);    /* %20 -> space */
    CHECK(strcmp(r.path, "/my file.txt") == 0);

    CHECK(parse("GET /%2e%2e/%2E%2E/etc HTTP/1.1\r\n\r\n", &r) == 0);/* decoded, then caught by realpath */
    CHECK(strcmp(r.path, "/../../etc") == 0);

    CHECK(parse("GET /%zz HTTP/1.1\r\n\r\n", &r) == 400);            /* invalid hex */
    CHECK(parse("GET /a%2 HTTP/1.1\r\n\r\n", &r) == 400);            /* truncated escape */
    CHECK(parse("GET /a% HTTP/1.1\r\n\r\n", &r) == 400);
    CHECK(parse("GET /a%00b HTTP/1.1\r\n\r\n", &r) == 400);          /* decoded NUL byte */

    /* 1024-byte path buffer: 1023 characters fit, 1024 do not. */
    char req[2100], path[1100];
    memset(path, 'a', sizeof path);
    path[0] = '/';
    path[1023] = '\0';
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\n\r\n", path);
    CHECK(parse(req, &r) == 0);
    path[1023] = 'a';
    path[1024] = '\0';
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\n\r\n", path);
    CHECK(parse(req, &r) == 414);
}

static void test_url_decode(void) {
    char out[8];
    CHECK(url_decode("/abc", out, sizeof out) == 0 && strcmp(out, "/abc") == 0);
    CHECK(url_decode("%41%62", out, sizeof out) == 0 && strcmp(out, "Ab") == 0);
    CHECK(url_decode("/1234567", out, sizeof out) == -2);   /* 8 chars + NUL won't fit */
    CHECK(url_decode("%g1", out, sizeof out) == -1);
}

static void test_mime(void) {
    CHECK(strcmp(mime_type("/index.html"), "text/html; charset=utf-8") == 0);
    CHECK(strcmp(mime_type("/STYLE.CSS"), "text/css; charset=utf-8") == 0);  /* case-insensitive */
    CHECK(strcmp(mime_type("/a/b/logo.png"), "image/png") == 0);
    CHECK(strcmp(mime_type("/photo.JPEG"), "image/jpeg") == 0);
    CHECK(strcmp(mime_type("/logo.svg"), "image/svg+xml") == 0);
    CHECK(strcmp(mime_type("/Makefile"), "application/octet-stream") == 0);  /* no extension */
    CHECK(strcmp(mime_type("/archive.xyz"), "application/octet-stream") == 0);
    CHECK(strcmp(mime_type("/v1.2/README"), "application/octet-stream") == 0); /* dot in dir */
}

static void test_write_all(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    char out[4096], in[4096];
    for (size_t i = 0; i < sizeof out; i++) out[i] = (char)(i * 31);
    CHECK(write_all(sv[0], out, sizeof out) == (ssize_t)sizeof out);

    size_t got = 0;
    while (got < sizeof in) {
        ssize_t n = read(sv[1], in + got, sizeof in - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    CHECK(got == sizeof in);
    CHECK(memcmp(out, in, sizeof out) == 0);

    close(sv[1]);                                    /* peer gone: EPIPE, not SIGPIPE */
    CHECK(write_all(sv[0], out, sizeof out) == -1);
    close(sv[0]);
}

static void test_queue(void) {
    job_queue_t q;
    job_t job = {0}, out;
    CHECK(queue_init(&q, 3) == 0);

    for (int i = 0; i < 3; i++) {
        job.fd = 100 + i;
        CHECK(queue_try_push(&q, job));
    }
    job.fd = 999;
    CHECK(!queue_try_push(&q, job));                 /* full: caller would send 503 */

    CHECK(queue_pop(&q, &out) && out.fd == 100);     /* FIFO order */
    job.fd = 103;
    CHECK(queue_try_push(&q, job));                  /* space again; tail wraps around */

    queue_shutdown(&q);
    CHECK(!queue_try_push(&q, job));                 /* no new work after shutdown */
    CHECK(queue_pop(&q, &out) && out.fd == 101);     /* ...but queued jobs still drain */
    CHECK(queue_pop(&q, &out) && out.fd == 102);
    CHECK(queue_pop(&q, &out) && out.fd == 103);
    CHECK(!queue_pop(&q, &out));                     /* drained: workers exit */
    queue_destroy(&q);
}

int main(void) {
    test_parse_valid();
    test_parse_methods();
    test_parse_malformed();
    test_parse_versions();
    test_parse_paths();
    test_url_decode();
    test_mime();
    test_write_all();
    test_queue();

    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
