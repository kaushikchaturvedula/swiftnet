#!/usr/bin/env bash
# Why is throughput ~100-105K? Is the SERVER the bottleneck, or the co-located
# load generator / loopback? Sweep (server engines) x (wrk threads, connections)
# on a 10-core M1 Pro and watch RPS + latency. If RPS is flat while latency rises
# with connections, the system is saturated; if RPS rises when we stop
# oversubscribing cores, the cap was co-location, not server CPU.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/swiftnet_bench"   # default build (frame pool OFF)
PORT=8080
ROUTE="/json"
DUR=10
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/benchmark/results/ceiling-$STAMP"; mkdir -p "$OUT"

run() { # $1 server_threads  $2 wrk_threads  $3 conns
  local st="$1" wt="$2" cc="$3"
  "$BIN" "$PORT" "$st" >/tmp/ceil_srv.log 2>&1 & local SRV=$!
  for _ in $(seq 1 50); do curl -s "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break; sleep 0.1; done
  wrk -t4 -c100 -d2s "http://127.0.0.1:$PORT$ROUTE" >/dev/null 2>&1  # warm
  local f="$OUT/srv${st}_wrk${wt}_c${cc}.txt"
  wrk -t"$wt" -c"$cc" -d"${DUR}s" --latency "http://127.0.0.1:$PORT$ROUTE" >"$f" 2>&1
  local rps lat; rps=$(grep "Requests/sec" "$f" | awk '{print $2}')
  lat=$(grep -A1 "Thread Stats" "$f" | tail -1 | awk '{print $2}')
  printf "  server=%-2s wrk=-t%-2s -c%-4s  => RPS %-10s  avg-lat %s\n" "$st" "$wt" "$cc" "${rps:-NA}" "${lat:-NA}"
  kill -INT "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; sleep 0.4
}

echo "host: $(sysctl -n machdep.cpu.brand_string) / $(sysctl -n hw.ncpu) cores  route=$ROUTE dur=${DUR}s" | tee "$OUT/meta.txt"
echo "=== A) default 10 engines, increasing client load (is it client-bound?) ===" | tee -a "$OUT/meta.txt"
run 10 2  200
run 10 4  500
run 10 8  500
run 10 12 1000
echo "=== B) match server+client to 10 cores (kill oversubscription) ===" | tee -a "$OUT/meta.txt"
run 5  4  500
run 6  4  500
run 4  4  400
echo "=== C) does server scale with engines at fixed heavy client? ===" | tee -a "$OUT/meta.txt"
run 2  8  500
run 4  8  500
run 8  8  500
echo "raw dir: $OUT"