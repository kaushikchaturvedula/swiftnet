#!/usr/bin/env bash
# Work-stealing VALVE experiment (SCHEDULER experiment -- NOT a web-throughput claim).
#
# Imbalanced workload on one machine: a few CPU-heavy connections (GET /compute,
# which offloads a stealable compute task) running ALONGSIDE many light I/O-bound
# connections (GET /json). Because /compute makes the SERVER the bottleneck (unlike
# the loopback-bound plain-HTTP case), this is where the valve can matter.
#
# Compares pure per-core (valve OFF) vs per-core + valve (valve ON) on the SAME
# workload. Hypothesis: the valve lets idle engines steal compute off the engines
# that own the heavy connections, so the LIGHT connections on those engines keep
# low tail latency. We measure: light /json RPS + p50/p99, heavy /compute RPS,
# per-core compute-queue depth over time, and total steal count.
#
# Co-location caveat: wrk shares the box with the server, so absolute light-latency
# is inflated; the OFF-vs-ON DELTA on the identical workload is the signal.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/swiftnet_bench"
[ -x "$BIN" ] || { echo "build: cmake --build build --target swiftnet_bench -j"; exit 1; }

N="${N:-8000}"            # compute size (~10ms/req on M1 Pro); tune per machine
HEAVY_C="${HEAVY_C:-6}"   # heavy connections (land on ~this many of 10 engines)
HEAVY_T="${HEAVY_T:-2}"
LIGHT_C="${LIGHT_C:-100}"
LIGHT_T="${LIGHT_T:-4}"
DUR="${DUR:-15}"
THRESH="${THRESH:-1}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/benchmark/results/valve-$STAMP"; mkdir -p "$OUT"

{ echo "host: $(sysctl -n machdep.cpu.brand_string) / $(sysctl -n hw.ncpu) cores (kqueue)"; \
  echo "compute n=$N (~per-req CPU), heavy=-t$HEAVY_T -c$HEAVY_C /compute, light=-t$LIGHT_T -c$LIGHT_C /json, dur=${DUR}s, threshold=$THRESH"; \
  echo "SCHEDULER experiment (compute-bound) -- not a web-throughput number."; } | tee "$OUT/meta.txt"

run() { # $1=tag  $2=valve(0|1)  $3=heavy-route
  local tag="$1" v="$2" route="$3"
  pkill -INT swiftnet_bench 2>/dev/null; sleep 0.6
  SWIFTNET_STEAL="$v" SWIFTNET_STEAL_THRESHOLD="$THRESH" SWIFTNET_BENCH_STATS="$OUT/stats_$tag.txt" \
    "$BIN" 8080 >/tmp/valve_srv_$tag.log 2>&1 & local SRV=$!
  for _ in $(seq 1 50); do curl -s -m2 -o /dev/null http://127.0.0.1:8080/json && break; sleep 0.1; done
  grep -m1 "steal=" /tmp/valve_srv_$tag.log
  # warm both paths
  wrk -t2 -c20 -d3s "http://127.0.0.1:8080${route}?n=$N" >/dev/null 2>&1
  wrk -t2 -c50 -d3s "http://127.0.0.1:8080/json" >/dev/null 2>&1
  echo ">>> $tag (valve=$v, heavy=$route): heavy load running, measuring LIGHT /json ..."
  wrk -t"$HEAVY_T" -c"$HEAVY_C" -d"$((DUR+2))s" "http://127.0.0.1:8080${route}?n=$N" \
      >"$OUT/heavy_$tag.txt" 2>&1 &
  local HPID=$!
  sleep 1
  wrk -t"$LIGHT_T" -c"$LIGHT_C" -d"${DUR}s" --latency "http://127.0.0.1:8080/json" \
      >"$OUT/light_$tag.txt" 2>&1
  wait $HPID 2>/dev/null
  kill -INT $SRV 2>/dev/null; wait $SRV 2>/dev/null
  echo "   light /json : RPS $(awk '/Requests\/sec/{print $2}' "$OUT/light_$tag.txt")  p50 $(awk '/Latency Distribution/{d=1} d&&/50%/{print $2;exit}' "$OUT/light_$tag.txt")  p99 $(awk '/Latency Distribution/{d=1} d&&/99%/{print $2;exit}' "$OUT/light_$tag.txt")"
  echo "   heavy RPS $(awk '/Requests\/sec/{print $2}' "$OUT/heavy_$tag.txt")   steals $(awk 'END{for(i=1;i<=NF;i++) if($i ~ /steals=/){sub("steals=","",$i); print $i}}' "$OUT/stats_$tag.txt" 2>/dev/null)   peakdepth $(awk 'NR>1{for(i=2;i<NF;i++) if($i>m)m=$i} END{print m+0}' "$OUT/stats_$tag.txt" 2>/dev/null)"
}

echo "===== A) VALVE OFF, offloaded compute (pure per-core baseline) ====="; run OFF 0 /compute
echo "===== B) VALVE ON,  offloaded compute (per-core + steal) ====="; run ON 1 /compute
echo "===== C) VALVE ON,  INLINE compute (unstealable: shows offload is required) ====="; run INLINE 1 /compute_inline
pkill -INT swiftnet_bench 2>/dev/null

echo
echo "================= COMPARISON ================="
for tag in OFF ON INLINE; do
  printf "valve %-3s | light /json: RPS %-10s p50 %-8s p90 %-8s p99 %-8s | heavy RPS %-8s | steals %s\n" \
    "$tag" \
    "$(awk '/Requests\/sec/{print $2}' "$OUT/light_$tag.txt")" \
    "$(awk '/Latency Distribution/{d=1} d&&/50%/{print $2;exit}' "$OUT/light_$tag.txt")" \
    "$(awk '/Latency Distribution/{d=1} d&&/90%/{print $2;exit}' "$OUT/light_$tag.txt")" \
    "$(awk '/Latency Distribution/{d=1} d&&/99%/{print $2;exit}' "$OUT/light_$tag.txt")" \
    "$(awk '/Requests\/sec/{print $2}' "$OUT/heavy_$tag.txt")" \
    "$(awk 'END{for(i=1;i<=NF;i++) if($i ~ /steals=/){sub("steals=","",$i);print $i}}' "$OUT/stats_$tag.txt")"
done | tee "$OUT/summary.txt"
echo "raw dir: $OUT"