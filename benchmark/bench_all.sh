#!/bin/bash
# Deep benchmark: SwiftNet vs Node.js/Express vs Spring Boot (virtual threads).
#
# Starts all three servers, warms each (important for the JVM's JIT), then runs
# wrk against each endpoint and prints a comparison table. Raw wrk output is
# saved under benchmark/results/. Portable to bash 3.2 (macOS default).
#
# Tunables (env): T=threads C=connections D=duration WARM=warmup-duration
#   e.g.  T=12 C=1000 D=20s WARM=8s ./benchmark/bench_all.sh

set -u
# wrk -c1000 and three servers each accepting ~1000 connections need many fds;
# macOS defaults to 256. Raise it for this process tree (servers + wrk inherit).
ulimit -n 1048576 2>/dev/null || ulimit -n 65536 2>/dev/null || ulimit -n 10240 2>/dev/null || true
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SN_BIN="$ROOT/build/examples/basic_server"
SPRING_JAR="$ROOT/benchmark/springboot/build/libs/swiftnet-bench-0.0.1.jar"
OUT="$ROOT/benchmark/results"
mkdir -p "$OUT"

T="${T:-12}"; C="${C:-1000}"; D="${D:-20s}"; WARM="${WARM:-8s}"
ENDPOINTS=("/" "/user/123")

command -v wrk  >/dev/null 2>&1 || { echo "error: wrk not installed (brew install wrk)"; exit 1; }
command -v node >/dev/null 2>&1 || { echo "error: node not installed"; exit 1; }
command -v java >/dev/null 2>&1 || { echo "error: java not installed"; exit 1; }
[ -x "$SN_BIN" ]     || { echo "error: build SwiftNet: cmake --build build -j"; exit 1; }
[ -f "$SPRING_JAR" ] || { echo "error: build Spring Boot: (cd benchmark/springboot && gradle bootJar)"; exit 1; }

echo "=================================================================="
echo " SwiftNet vs Node.js vs Spring Boot   (wrk t=$T c=$C d=$D warm=$WARM)"
echo " $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown CPU), $(uname -m)"
echo "=================================================================="

field() { awk -v k="$2" '$0 ~ k {print $2; exit}' "$1"; }       # Requests/sec / Transfer/sec
lat_avg() { awk '/Thread Stats/{s=1} s && /^[ ]*Latency/{print $2; exit}' "$1"; }
lat_p99() { awk '/Latency Distribution/{d=1} d && /99%/{print $2; exit}' "$1"; }

wait_ready() {
    local port=$1 name=$2
    for i in $(seq 1 60); do
        if curl -s -m2 -o /dev/null "http://127.0.0.1:$port/"; then return 0; fi
        sleep 1
    done
    echo "  $name FAILED to start (:$port)"; return 1
}

# Benchmark a single server in ISOLATION (started, measured, stopped) so it has
# the whole machine -- running all servers at once oversubscribes the cores and
# distorts results.
bench_server() { # name port start_cmd...
    local name=$1 port=$2; shift 2
    "$@" >/tmp/bench_${name}.log 2>&1 & local pid=$!
    if ! wait_ready "$port" "$name"; then kill $pid 2>/dev/null; return 1; fi
    sleep 2 # settle
    local slugbase
    for ep in "${ENDPOINTS[@]}"; do
        slugbase=$(echo "$ep" | sed 's#[^a-zA-Z0-9]#_#g')
        wrk -t"$T" -c"$C" -d"$WARM" "http://127.0.0.1:$port$ep" >/dev/null 2>&1            # warm (JIT)
        wrk -t"$T" -c"$C" -d"$D" --latency "http://127.0.0.1:$port$ep" >"$OUT/${name}${slugbase}.txt" 2>&1
        echo "  measured $name $ep"
    done
    kill $pid 2>/dev/null
    pkill -f nodejs-server.js 2>/dev/null; pkill -f swiftnet-bench 2>/dev/null
    sleep 1
}

echo "Benchmarking each server in isolation..."
bench_server SwiftNet   8080 env SPDLOG_LEVEL=off "$SN_BIN"
bench_server Node.js    3000 node "$ROOT/benchmark/nodejs-server.js"
bench_server SpringBoot 8090 java -jar "$SPRING_JAR"

for ep in "${ENDPOINTS[@]}"; do
    slugbase=$(echo "$ep" | sed 's#[^a-zA-Z0-9]#_#g')
    echo
    echo "### Endpoint: $ep"
    printf "%-12s %14s %12s %12s %12s\n" "Server" "Req/sec" "Lat avg" "Lat p99" "Transfer/s"
    printf -- "----------------------------------------------------------------------\n"
    for name in SwiftNet Node.js SpringBoot; do
        f="$OUT/${name}${slugbase}.txt"
        [ -f "$f" ] || continue
        printf "%-12s %14s %12s %12s %12s\n" "$name" \
            "$(field "$f" 'Requests/sec')" "$(lat_avg "$f")" "$(lat_p99 "$f")" "$(field "$f" 'Transfer/sec')"
    done
done
echo
echo "Raw wrk output saved under benchmark/results/."
