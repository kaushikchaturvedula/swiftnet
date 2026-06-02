#!/usr/bin/env bash
# A/B: lazy request-header handling vs eager per-request header-map copy.
# Two binaries differ ONLY in src/swiftnet.cpp Request header handling:
#   eager = copy all headers into an unordered_map in the Request ctor
#   lazy  = scan the parser's header vector on demand; build the map only if
#           Request::headers() is called (the bench handlers never read headers)
# Measured two ways: wrk's minimal default headers, and 10 headers/request
# (realistic-client load) which amplifies the per-request copy cost.
set -u
REPS="${1:-3}"; DUR="${2:-12}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EAGER="/tmp/bench_eager_headers"          # built before the lazy-headers edit
LAZY="$ROOT/build/swiftnet_bench"         # current (lazy)
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/benchmark/results/lazyhdr-ab-$STAMP"; mkdir -p "$OUT"
HDRS10=(-H "X-A: 1" -H "X-B: 22" -H "X-C: 333" -H "X-D: 4444" -H "X-E: 55555" -H "X-F: 666666" -H "X-G: 7" -H "X-H: 8")

[ -x "$EAGER" ] || { echo "missing $EAGER (preserve the pre-change binary first)"; exit 1; }

bench() { # $1 label $2 bin $3 rep $4 case  (rest = extra headers)
  local label="$1" bin="$2" rep="$3" case="$4"; shift 4
  "$bin" 8080 >/tmp/ablh.log 2>&1 & local S=$!
  for _ in $(seq 1 40); do curl -s http://127.0.0.1:8080/ >/dev/null 2>&1 && break; sleep 0.1; done
  wrk -t4 -c100 -d3s "$@" http://127.0.0.1:8080/json >/dev/null 2>&1
  local f="$OUT/${label}_${case}_rep${rep}.txt"
  { echo "# $label $case rep$rep"; wrk -t8 -c500 -d"${DUR}s" --latency "$@" http://127.0.0.1:8080/json; } | tee "$f" >/dev/null
  kill -INT $S 2>/dev/null; wait $S 2>/dev/null; sleep 0.4
}

{ echo "host: $(sysctl -n machdep.cpu.brand_string) / $(sysctl -n hw.ncpu) cores"; echo "load: wrk -t8 -c500 -d${DUR}s --latency /json; reps=$REPS"; } | tee "$OUT/meta.txt"

for rep in $(seq 1 "$REPS"); do
  bench eager "$EAGER" "$rep" minhdr
  bench lazy  "$LAZY"  "$rep" minhdr
  bench eager "$EAGER" "$rep" hdr10 "${HDRS10[@]}"
  bench lazy  "$LAZY"  "$rep" hdr10 "${HDRS10[@]}"
done

echo "============== Requests/sec (eager -> lazy) =============="
for case in minhdr hdr10; do
  echo "case=$case:"
  for label in eager lazy; do
    printf "  %-6s" "$label"
    for rep in $(seq 1 "$REPS"); do
      printf " %10s" "$(grep Requests/sec "$OUT/${label}_${case}_rep${rep}.txt" | awk '{print $2}')"
    done; echo
  done
done | tee "$OUT/summary.txt"
echo "raw dir: $OUT"