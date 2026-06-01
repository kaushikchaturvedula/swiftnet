#!/bin/bash
# SwiftNet vs Node.js/Express benchmark.
#
# Starts both servers, runs wrk against a set of endpoints, and prints a
# comparison table. Portable to bash 3.2 (macOS default).

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SWIFTNET_BIN="$ROOT/build/examples/basic_server"

SWIFTNET_PORT=8080
NODEJS_PORT=3000
DURATION="${DURATION:-8s}"
THREADS="${THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-64}"
ENDPOINTS=("/" "/user/123")

command -v wrk  >/dev/null 2>&1 || { echo "error: wrk not installed (brew install wrk)"; exit 1; }
command -v node >/dev/null 2>&1 || { echo "error: node not installed"; exit 1; }
[ -x "$SWIFTNET_BIN" ] || { echo "error: build SwiftNet first: cmake -S . -B build && cmake --build build -j"; exit 1; }

echo "=================================================="
echo " SwiftNet vs Node.js  (t=$THREADS c=$CONNECTIONS d=$DURATION)"
echo " $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown CPU)"
echo "=================================================="

# Start servers
SPDLOG_LEVEL=off "$SWIFTNET_BIN" >/tmp/swiftnet_bench.log 2>&1 &
SN_PID=$!
( cd "$ROOT/benchmark" && node nodejs-server.js >/tmp/nodejs_bench.log 2>&1 ) &
NODE_PID=$!
trap 'kill $SN_PID $NODE_PID 2>/dev/null; pkill -f nodejs-server.js 2>/dev/null' EXIT
sleep 3

rps() { wrk -t"$THREADS" -c"$CONNECTIONS" -d"$DURATION" "$1" 2>/dev/null | awk '/Requests\/sec/{print $2}'; }

printf "\n%-14s %14s %14s %12s\n" "Endpoint" "SwiftNet RPS" "Node.js RPS" "Ratio"
printf -- "-------------------------------------------------------------\n"
for ep in "${ENDPOINTS[@]}"; do
    sn=$(rps "http://localhost:$SWIFTNET_PORT$ep")
    nd=$(rps "http://localhost:$NODEJS_PORT$ep")
    ratio=$(awk -v a="$sn" -v b="$nd" 'BEGIN{ if (b>0) printf "%.2fx", a/b; else print "n/a" }')
    printf "%-14s %14s %14s %12s\n" "$ep" "${sn:-n/a}" "${nd:-n/a}" "$ratio"
done
echo
echo "Done. (server logs: /tmp/swiftnet_bench.log, /tmp/nodejs_bench.log)"
