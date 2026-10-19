#!/usr/bin/env bash
# Benchmark httpd-c with wrk and write the results to bench/results.md and README.md.
#
#   sudo apt install wrk
#   bash bench/run.sh            # about 4 minutes
#
# Method (keeps the numbers honest):
#   - release build (-O2), access log off (-s)
#   - 5 s warm-up, then 3 runs per configuration; the MEDIAN run is reported
#   - wrk runs on the same machine and competes with the server for CPU
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=${PORT:-18090}
DURATION=${DURATION:-10s}
RUNS=${RUNS:-3}
THREAD_COUNTS=${THREAD_COUNTS:-"1 2 4 8 16"}
URL="http://127.0.0.1:$PORT"
OUT=bench/results.md
SERVER_PID=""

command -v wrk >/dev/null || { echo "wrk not found: sudo apt install wrk"; exit 1; }

cleanup() { [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true; }
trap cleanup EXIT

start_server() {
  ./httpd -p "$PORT" -s "$@" 2>/dev/null &
  SERVER_PID=$!
  for _ in $(seq 50); do
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && return 0
    sleep 0.1
  done
  echo "server failed to start"; exit 1
}

stop_server() {
  kill -TERM "$SERVER_PID"
  wait "$SERVER_PID" || true
  SERVER_PID=""
}

# Run wrk once; print "req_per_sec p50 p99 socket_errors non2xx transfer".
run_wrk() {
  wrk "$@" --latency -H "Connection: close" 2>&1 | awk '
    /^ +50%/            { p50 = $2 }
    /^ +99%/            { p99 = $2 }
    /^Requests\/sec:/   { rps = $2 }
    /^Transfer\/sec:/   { xfer = $2 }
    /^ +Socket errors:/ { gsub(",", ""); errs = $4 + $6 + $8 + $10 }
    /^ +Non-2xx/        { non2xx = $5 }
    END { printf "%s %s %s %d %d %s\n", rps, p50, p99, errs, non2xx, xfer }'
}

# Run wrk RUNS times and print the run with the median requests/sec.
median_run() {
  for _ in $(seq "$RUNS"); do run_wrk "$@"; done | sort -n -k1,1 | sed -n "$(( (RUNS + 1) / 2 ))p"
}

echo "Building release binary..."
make -s clean all

CPU=$(lscpu | sed -n 's/^Model name: *//p' | head -1)
CORES=$(nproc)
echo "Machine: $CPU, $CORES cores"

# --- 1. Thread-count sweep -----------------------------------------------------
echo "Thread sweep (small file, $RUNS x $DURATION per config)..."
SWEEP=""
BASE_RPS=""
for t in $THREAD_COUNTS; do
  start_server -t "$t"
  wrk -t4 -c100 -d5s -H "Connection: close" "$URL/index.html" >/dev/null 2>&1   # warm-up
  read -r rps p50 p99 errs _ _ < <(median_run -t4 -c100 -d"$DURATION" "$URL/index.html")
  stop_server
  [[ -z "$BASE_RPS" ]] && BASE_RPS=$rps
  speedup=$(awk -v a="$rps" -v b="$BASE_RPS" 'BEGIN { printf "%.2f", a / b }')
  printf '  %2s threads: %10s req/s  p50 %-8s p99 %-8s errors %s\n' "$t" "$rps" "$p50" "$p99" "$errs"
  SWEEP+="| $t | $(printf '%.0f' "$rps") | $p50 | $p99 | ${speedup}x | $errs |"$'\n'
done

# --- 2. Large-file throughput (shows off sendfile) -----------------------------
echo "Large file throughput (1 MB, sendfile)..."
head -c 1048576 /dev/urandom > www/big.bin
start_server -t "$CORES"
read -r big_rps big_p50 big_p99 big_errs _ big_xfer < <(median_run -t2 -c16 -d"$DURATION" "$URL/big.bin")
stop_server
rm -f www/big.bin
echo "  $big_xfer/s, $big_rps req/s, p99 $big_p99"

# --- 3. Overload: tiny pool + tiny queue vs 1000 connections -------------------
echo "Overload test (-t 2 -q 16 vs 1000 connections)..."
ulimit -n 4096 2>/dev/null || ulimit -n "$(ulimit -Hn)"
start_server -t 2 -q 16
read -r ov_rps ov_p50 ov_p99 ov_errs ov_non2xx _ < <(run_wrk -t4 -c1000 -d"$DURATION" "$URL/index.html")
alive=no
curl -s -o /dev/null --max-time 5 "$URL/" && alive=yes
stop_server
echo "  $ov_rps req/s, $ov_non2xx 503s, p99 $ov_p99, alive after: $alive"

# --- 4. Docker image size (if Docker is available) -----------------------------
IMAGE="not measured (Docker not available)"
if command -v docker >/dev/null && docker info >/dev/null 2>&1; then
  echo "Building Docker image..."
  if docker build -q -t httpd-c . >/dev/null 2>&1; then
    IMAGE=$(docker image inspect httpd-c --format '{{.Size}}' | awk '{ printf "%.1f MB", $1 / 1000000 }')
  else
    IMAGE="not measured (docker build failed)"
  fi
  echo "  image size: $IMAGE"
fi

# --- Write results ---------------------------------------------------------------
RESULTS=$(cat <<EOF
Measured $(date -u +%Y-%m-%d) on **$CPU ($CORES cores)**. Release build (\`-O2\`), access log off (\`-s\`),
\`wrk\` running on the same machine. Each row is the median of $RUNS runs of $DURATION after a 5 s warm-up.
Every request opens a new connection (\`Connection: close\`).

**Thread-count sweep** (\`wrk -t4 -c100 -d$DURATION --latency -H "Connection: close" /index.html\`)

| Server threads | Requests/sec | p50 latency | p99 latency | vs. 1 thread | Socket errors |
|---|---|---|---|---|---|
$SWEEP
**Large file** (1 MB, zero-copy \`sendfile\`, \`wrk -t2 -c16\`): **$big_xfer/s** ($(printf '%.0f' "$big_rps") req/s, p99 $big_p99)

**Overload / load shedding** (\`-t 2 -q 16\` vs \`wrk -t4 -c1000\`): $(printf '%.0f' "$ov_rps") req/s,
$ov_non2xx requests shed with \`503\`, p99 latency $ov_p99, server still responding afterwards: **$alive**

**Docker image:** $IMAGE
EOF
)

printf '# Benchmark results\n\nGenerated by `bash bench/run.sh`.\n\n%s\n' "$RESULTS" > "$OUT"

# Replace the section between the BENCH markers in README.md.
if grep -q '<!-- BENCH:START -->' README.md; then
  printf '%s\n' "$RESULTS" > bench/.section.tmp
  awk '
    /<!-- BENCH:START -->/ { print; while ((getline line < "bench/.section.tmp") > 0) print line; skip = 1; next }
    /<!-- BENCH:END -->/   { skip = 0 }
    !skip
  ' README.md > README.md.tmp && mv README.md.tmp README.md
  rm -f bench/.section.tmp
fi

echo
echo "Wrote $OUT and updated the Benchmarks section of README.md."
