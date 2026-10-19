#!/usr/bin/env bash
# End-to-end smoke tests: start the real binary, hit it with curl, check status codes.
# Works for normal, ASan and TSan builds:  make && ./tests/smoke.sh
set -euo pipefail

cd "$(dirname "$0")/.."
PORT=${PORT:-18080}
SHED_PORT=$((PORT + 1))
BASE="http://127.0.0.1:$PORT"
PIDS=()

cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
}
trap cleanup EXIT

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; exit 1; }

# Start ./httpd in the background and wait until it accepts connections.
# (A fixed sleep is flaky under sanitizers, which make startup much slower.)
start_server() {
  local port=$1; shift
  ./httpd -p "$port" -r ./www -s "$@" &
  PIDS+=("$!")
  for _ in $(seq 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  fail "server on port $port did not start"
}

check() {
  local want=$1; shift
  local got
  got=$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$@")
  local label="$*"
  (( ${#label} > 90 )) && label="${label:0:87}..."      # keep long URLs readable
  if [[ "$got" == "$want" ]]; then pass "$want $label"; else fail "$label (got $got, want $want)"; fi
}

[[ -x ./httpd ]] || fail "./httpd not found; run make first"
start_server "$PORT"
MAIN_PID=${PIDS[0]}

# --- Basic serving -----------------------------------------------------------
check 200 "$BASE/"
check 200 "$BASE/style.css"
check 200 "$BASE/hello.txt"
check 200 "$BASE/docs/"
check 404 "$BASE/nope.html"

ctype=$(curl -s -o /dev/null -w '%{content_type}' "$BASE/style.css")
[[ "$ctype" == "text/css; charset=utf-8" ]] && pass "Content-Type $ctype" || fail "Content-Type $ctype"

body=$(curl -s "$BASE/hello.txt")
[[ "$body" == "Hello from httpd-c." ]] && pass "body matches file" || fail "body: $body"

# --- Methods -----------------------------------------------------------------
check 405 -X POST "$BASE/"
allow=$(curl -s -D - -o /dev/null -X DELETE "$BASE/" | tr -d '\r' | grep -i '^Allow:' || true)
[[ "$allow" == "Allow: GET, HEAD" ]] && pass "405 has $allow" || fail "405 Allow header: '$allow'"

check 200 -I "$BASE/"
head_bytes=$(curl -s -X HEAD --max-time 5 -o /dev/null -w '%{size_download}' "$BASE/" || true)
[[ "$head_bytes" == "0" ]] && pass "HEAD has no body" || fail "HEAD body was $head_bytes bytes"

# --- Bad requests ------------------------------------------------------------
check 400 "$BASE/%zz"
check 400 "$BASE/a%00b"
check 414 "$BASE/$(head -c 1100 /dev/zero | tr '\0' a)"
ver=$(printf 'GET / HTTP/2.0\r\n\r\n' | curl -s --max-time 5 telnet://127.0.0.1:$PORT 2>/dev/null | head -1 | tr -d '\r' || true)
[[ "$ver" == "HTTP/1.1 505 HTTP Version Not Supported" ]] && pass "505 for HTTP/2.0" || fail "505: '$ver'"

# --- Path traversal (enough ../ to reach / from any docroot) -----------------
check 403 --path-as-is "$BASE/../../../../../../../../etc/passwd"
check 403 --path-as-is "$BASE/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/etc/passwd"

# --- Slow client: connect, send nothing, expect 408 after the 5 s timeout ----
resp=$(timeout 10 bash -c "exec 3<>/dev/tcp/127.0.0.1/$PORT; head -1 <&3" | tr -d '\r' || true)
[[ "$resp" == "HTTP/1.1 408 Request Timeout" ]] && pass "408 for idle client" || fail "408: '$resp'"

# --- Concurrency: 500 requests, 50 at a time, all must be 200 ----------------
fails=$(seq 500 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code}\n' --max-time 10 "$BASE/" \
        | grep -vc '^200$' || true)
[[ "$fails" == 0 ]] && pass "500 concurrent requests" || fail "concurrency: $fails non-200"

# --- Load shedding: 1 worker, queue of 1, both occupied -> 503 ---------------
start_server "$SHED_PORT" -t 1 -q 1
SHED_PID=${PIDS[1]}
(exec 3<>"/dev/tcp/127.0.0.1/$SHED_PORT"; sleep 3) &   # occupies the only worker
sleep 0.3
(exec 3<>"/dev/tcp/127.0.0.1/$SHED_PORT"; sleep 3) &   # fills the queue
sleep 0.3
retry=$(curl -s -D - -o /dev/null --max-time 5 "http://127.0.0.1:$SHED_PORT/" | tr -d '\r' | grep -i '^Retry-After:' || true)
check 503 --max-time 5 "http://127.0.0.1:$SHED_PORT/"
[[ "$retry" == "Retry-After: 1" ]] && pass "503 has $retry" || fail "503 Retry-After: '$retry'"
wait $(jobs -p | grep -v -e "$MAIN_PID" -e "$SHED_PID") 2>/dev/null || true

# --- Graceful shutdown must exit 0 (sanitizer builds exit non-zero on errors) -
for pid in "$SHED_PID" "$MAIN_PID"; do
  kill -TERM "$pid"
  if wait "$pid"; then pass "graceful shutdown (pid $pid)"; else fail "server exited with status $?"; fi
done
PIDS=()

echo "All smoke tests passed."
