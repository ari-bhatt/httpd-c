# httpd-c


A multithreaded HTTP/1.1 static-file server written in C, built on a fixed POSIX thread pool with a bounded job queue that sheds load with `503` instead of collapsing under overload.

## Features

- **Fixed thread pool + bounded producer/consumer queue** (mutex + condition variable)
- **Load shedding:** when the queue is full, new clients get an immediate `503` with `Retry-After`
- **Zero-copy file transfer** with `sendfile()`
- **Path-traversal protection:** `realpath()` plus a docroot boundary check (also catches `%2e%2e` and symlinks)
- **Slow-client protection:** 5 s socket timeouts, answered with `408`
- **Graceful shutdown** on `SIGINT`/`SIGTERM`: stop accepting, drain the queue, join the workers, exit 0
- **Access log** with per-request latency, safe against log injection; **C11 atomic** stats
- **GET** and **HEAD**; correct `400`, `403`, `404`, `405`, `408`, `414`, `431`, `503` and `505` responses
- **Tested:** 70 unit checks, end-to-end smoke tests, and CI runs under **AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer**
- **Docker:** small multi-stage Alpine image, runs as a non-root user

## Architecture

```
              accept() loop (main thread)
                         │
           queue_try_push ──── queue full? → send 503 + close  (load shedding)
                         ▼
   ┌────────── bounded circular queue (mutex + condition variable) ──────────┐
   └──────────────────────────────────────────────────────────────────────────┘
        │             │             │                    │
    worker 1      worker 2      worker 3       …     worker N
   (read → parse → resolve → respond → log → close)
```

| File | Responsibility |
|---|---|
| `src/main.c` | Argument parsing, signal setup, accept loop, shutdown sequence |
| `src/threadpool.c` | Bounded job queue and worker threads (knows nothing about HTTP) |
| `src/server.c` | Listening socket, timeouts, per-connection handler, access log |
| `src/http.c` | Request reading and parsing, response headers, safe file serving |
| `src/util.c` | `write_all`, URL decoding, MIME types, HTTP dates |

## Design decisions

**Thread pool, not thread-per-connection.** Creating a thread for every request costs time on every connection, and a connection flood would create unbounded threads, memory and context switching. A fixed pool caps resource use.

**Bounded queue with `503` when full.** An unbounded queue turns overload into ever-growing latency and memory use until clients time out with no feedback. A bounded queue fails fast with a clear `503 Retry-After: 1`, so latency stays bounded for the requests that are accepted. The benchmark's overload test shows this.

**`while`, not `if`, around `pthread_cond_wait`.** Condition variables can wake spuriously, and another worker may take the job first, so a woken worker always re-checks the condition.

**`sendfile()`.** File data moves from the page cache to the socket inside the kernel, with no `read()` into a user-space buffer and back. That means fewer copies and fewer system calls.

**Looping on `recv` and `send`.** TCP is a byte stream, so a single call may return or accept any number of bytes. `read_request` loops until the blank line that ends the headers, with an 8 KB cap (`431` beyond it), and `write_all` loops over partial writes.

**Path safety.** The path is URL-decoded first, then resolved with `realpath()` (which collapses `..` and symlinks), then checked to be inside the docroot. The boundary check (`resolved[root_len]` must be `/` or end of string) stops `/var/www-secret` from passing a plain prefix check against `/var/www`. Directory `index.html` files are re-checked in case they are symlinks.

**Timeouts.** Without `SO_RCVTIMEO`, a client that connects and sends nothing would pin a worker forever, and a few hundred of them (a Slowloris attack) would freeze the whole pool.

**Signals and shutdown.** The handler only sets a `volatile sig_atomic_t` flag. `SIGINT` and `SIGTERM` are blocked before the workers are created (threads inherit the mask), so a worker can never receive them. The main thread waits in `ppoll()`, which unblocks the signals only for the duration of the wait. That closes the race where a signal arrives between checking the flag and blocking in `accept()`. On shutdown the queue keeps handing out already-accepted jobs until it is empty, so in-flight clients are still served.

**`SIGPIPE` ignored.** Writing to a socket the client already closed raises `SIGPIPE`, whose default action kills the whole process.

**Thread-safe library calls.** `strtok_r` instead of `strtok`, and `gmtime_r` instead of `gmtime`, because the non-`_r` versions keep hidden shared state. Each access-log line is formatted first and written with a single call, so lines from different workers never interleave.

## Benchmarks

<!-- BENCH:START -->
Run `bash bench/run.sh` to measure on your machine. It fills in this section automatically.
<!-- BENCH:END -->

Full results and method: [`bench/results.md`](bench/results.md).

## Build, run, test

Requires Linux (or WSL2) and `gcc`. `sendfile()` has a different signature on macOS.

```bash
make                         # release build with -Wall -Wextra -Wpedantic -Werror
./httpd -p 8080              # serves ./www on http://localhost:8080
```

| Flag | Meaning | Default |
|---|---|---|
| `-p` | port | 8080 |
| `-t` | worker threads | number of CPU cores |
| `-q` | queue capacity | 256 |
| `-r` | document root | `./www` |
| `-s` | silent mode (no access log; for benchmarks) | off |

```bash
make test                    # 70 unit checks: parser, URL decoding, MIME, write_all, queue
make smoke                   # end-to-end tests against the real binary
make asan && bash tests/smoke.sh    # AddressSanitizer + UBSan
make tsan && bash tests/smoke.sh    # ThreadSanitizer
bash bench/run.sh            # benchmarks (needs wrk)
```

### Docker

```bash
docker build -t httpd-c .
docker run --rm -p 8080:8080 --name web httpd-c
docker stop web              # exits immediately and cleanly via SIGTERM
```

## Limitations and future work

- **No keep-alive:** one request per connection (`Connection: close`). Keep-alive with an idle timeout is the next step and would raise throughput a lot.
- **No TLS:** run it behind a TLS-terminating proxy.
- **GET and HEAD only**, static files only; no range requests or conditional `304` responses.
- **Thread-per-active-connection model:** an `epoll` event loop (the nginx model) would handle many more idle connections, and `SO_REUSEPORT` would allow several accept threads.
- **Fuzzing:** the parser is a pure function, so it is ready for libFuzzer or AFL++.
