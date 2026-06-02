#!/usr/bin/env bash
# Run from the LOAD box (a SEPARATE machine from the server) to drive wrk over the
# real network at the server under test. Sweeps connection counts to find the true
# peak, warms up first (critical for the JVM), and saves RAW wrk stdout per run.
#
# Usage:  SERVER=<server-ip> ./load_box.sh <swiftnet|node|fastify|spring>
#   env:  PORT (default per server)  T=wrk-threads  D=duration  ROUTE  CONNS="..."
#
# Run this once per server (switch the server on the server box between runs).
# Fair 3-way: identical server hardware, identical wrk command from this box.
set -u
NAME="${1:?usage: SERVER=<ip> load_box.sh <swiftnet|node|fastify|spring>}"
SERVER="${SERVER:?set SERVER=<server-box-ip>}"
case "$NAME" in
  swiftnet) PORT="${PORT:-8080}" ;;
  node|fastify) PORT="${PORT:-3000}" ;;
  spring) PORT="${PORT:-8090}" ;;
  *) echo "unknown server: $NAME"; exit 1 ;;
esac
ROUTE="${ROUTE:-/user/123}"     # JSON route present on all four servers
T="${T:-$(nproc 2>/dev/null || echo 16)}"
D="${D:-20s}"
CONNS="${CONNS:-50 100 200 500 1000 2000}"
ulimit -n 1048576 2>/dev/null || ulimit -n 65536 2>/dev/null || true

STAMP="$(date +%Y%m%d-%H%M%S)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/benchmark/results/offhost-$NAME-$STAMP"; mkdir -p "$OUT"
URL="http://$SERVER:$PORT$ROUTE"

command -v wrk >/dev/null || { echo "install wrk on this load box"; exit 1; }
{ echo "server=$NAME  url=$URL"; echo "load box: $(uname -msr), $(nproc 2>/dev/null) cores"; \
  echo "wrk: $(wrk --version 2>&1 | head -1)"; echo "protocol: warm 10s, then -d$D per connection count"; \
  echo "conns swept: $CONNS"; } | tee "$OUT/meta.txt"

echo "warming up (JIT / connection pool)..."
wrk -t"$T" -c200 -d10s "$URL" >/dev/null 2>&1

echo "============== $NAME  ($URL) =============="
best_rps=0; best_c=0
for c in $CONNS; do
  f="$OUT/c${c}.txt"
  { echo "# server=$NAME route=$ROUTE conns=$c threads=$T dur=$D"; \
    wrk -t"$T" -c"$c" -d"$D" --latency "$URL"; } | tee "$f"
  rps=$(awk '/Requests\/sec/{print $2}' "$f")
  echo "  -> c=$c : ${rps:-NA} req/s"; echo
  awk -v r="${rps:-0}" -v c="$c" -v br="$best_rps" 'BEGIN{exit !(r>br)}' && { best_rps=$rps; best_c=$c; }
done
echo "PEAK: $best_rps req/s at c=$best_c  (raw: $OUT)" | tee -a "$OUT/meta.txt"
echo
echo ">>> On the SERVER box, read the cpu_watch summary for c=$best_c to confirm"
echo ">>> the server was CPU-saturated (idle% near 0). If idle% is still high, the"
echo ">>> LOAD box or the network is the bottleneck -- use a bigger/closer load box."
