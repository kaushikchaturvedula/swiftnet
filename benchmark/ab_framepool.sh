#!/usr/bin/env bash
# A/B micro-benchmark: coroutine-frame pool ON vs OFF, same machine, back-to-back.
#
# Isolates ONE change (vthread promise operator new/delete -> thread-local frame
# pool) by running two binaries that differ only in -DSWIFTNET_NO_FRAME_POOL.
# Variants are interleaved within each rep to cancel out time/thermal drift, and
# every wrk invocation's RAW stdout is saved (no summarizing).
#
# Usage: benchmark/ab_framepool.sh [reps] [duration_s]
set -u
REPS="${1:-3}"
DUR="${2:-15}"
PORT=8080
THREADS=12
CONNS=1000
ROUTES=("/" "/json")

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
POOL_BIN="$ROOT/build-pool/swiftnet_bench"
NOPOOL_BIN="$ROOT/build/swiftnet_bench"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/benchmark/results/framepool-ab-$STAMP"
mkdir -p "$OUT"

for b in "$POOL_BIN" "$NOPOOL_BIN"; do
  [ -x "$b" ] || { echo "missing binary: $b" >&2; exit 1; }
done

{
  echo "host: $(uname -msr)"
  sysctl -n machdep.cpu.brand_string 2>/dev/null
  echo "cores: $(sysctl -n hw.ncpu) (perf=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null) eff=$(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null))"
  echo "wrk: $(wrk --version 2>&1 | head -1)"
  echo "load: wrk -t$THREADS -c$CONNS -d${DUR}s --latency  | reps=$REPS  routes=${ROUTES[*]}"
  echo "pool_bin:   $POOL_BIN"
  echo "nopool_bin: $NOPOOL_BIN"
} | tee "$OUT/meta.txt"

start_server() { # $1 = binary
  "$1" "$PORT" >/tmp/ab_server.log 2>&1 &
  SRV=$!
  for _ in $(seq 1 50); do
    curl -s "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
    sleep 0.1
  done
}
stop_server() {
  kill -INT "$SRV" 2>/dev/null
  wait "$SRV" 2>/dev/null
  sleep 0.5  # let the listener fds fully close before the next bind
}

run_one() { # $1 variant label, $2 binary, $3 rep
  local label="$1" bin="$2" rep="$3"
  start_server "$bin"
  # warm up: prime keep-alive conns + the frame pool to steady state
  wrk -t4 -c100 -d3s "http://127.0.0.1:$PORT/" >/dev/null 2>&1
  for route in "${ROUTES[@]}"; do
    local tag; tag="$(echo "$route" | sed 's#/#_#g; s#^_$#root#')"
    local f="$OUT/${label}_rep${rep}_${tag}.txt"
    echo ">>> [$label rep$rep] wrk $route"
    {
      echo "# variant=$label rep=$rep route=$route"
      wrk -t"$THREADS" -c"$CONNS" -d"${DUR}s" --latency "http://127.0.0.1:$PORT$route"
    } | tee "$f"
    echo
  done
  stop_server
}

for rep in $(seq 1 "$REPS"); do
  # interleave variants each rep so drift hits both equally
  run_one nopool "$NOPOOL_BIN" "$rep"
  run_one pool   "$POOL_BIN"   "$rep"
done

echo "=================== SUMMARY (Requests/sec per run) ==================="
for route in "${ROUTES[@]}"; do
  tag="$(echo "$route" | sed 's#/#_#g; s#^_$#root#')"
  echo "route $route:"
  for label in nopool pool; do
    printf "  %-7s" "$label"
    for rep in $(seq 1 "$REPS"); do
      rps=$(grep "Requests/sec" "$OUT/${label}_rep${rep}_${tag}.txt" 2>/dev/null | awk '{print $2}')
      printf " %10s" "${rps:-NA}"
    done
    echo
  done
done | tee "$OUT/summary.txt"
echo
echo "raw output dir: $OUT"
