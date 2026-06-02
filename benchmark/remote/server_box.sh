#!/usr/bin/env bash
# Run ONE server under test on the SERVER box, bound to all interfaces (so the
# separate LOAD box can reach it over the real network), using all cores, with
# the CPU-saturation monitor running alongside.
#
# Usage:  ./server_box.sh <swiftnet|node|fastify|spring> [port]
# Then, on the LOAD box:  SERVER=<this-box-ip> ./load_box.sh <same-name>
# Ctrl-C here when the load run finishes -> prints the saturation summary.
#
# Backend note: on a Linux server box SwiftNet uses the io_uring backend; these
# would be the FIRST real-hardware io_uring throughput numbers (currently
# unverified -- see BENCHMARKS.md). On a macOS server box it uses kqueue.
set -u
NAME="${1:?usage: server_box.sh <swiftnet|node|fastify|spring> [port]}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
NCPU=$( (nproc 2>/dev/null) || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Raise fd limit for many concurrent connections.
ulimit -n 1048576 2>/dev/null || ulimit -n 65536 2>/dev/null || true

case "$NAME" in
  swiftnet)
    PORT="${2:-8080}"
    BIN="$ROOT/build/swiftnet_bench"
    [ -x "$BIN" ] || { echo "build first: cmake --build build --target swiftnet_bench -j"; exit 1; }
    echo "starting SwiftNet (all $NCPU cores) on 0.0.0.0:$PORT"
    "$BIN" "$PORT" >/tmp/srv_swiftnet.log 2>&1 & SRV=$! ;;
  node)
    PORT="${2:-3000}"
    echo "starting Node cluster ($NCPU workers) on 0.0.0.0:$PORT"
    ( cd "$ROOT/benchmark" && node nodejs-cluster.js ) >/tmp/srv_node.log 2>&1 & SRV=$! ;;
  fastify)
    PORT="${2:-3000}"
    echo "starting Fastify cluster ($NCPU workers) on 0.0.0.0:$PORT"
    # NOTE: ensure fastify listens on host '0.0.0.0' (Fastify defaults to localhost).
    ( cd "$ROOT/benchmark" && node fastify-cluster.js ) >/tmp/srv_fastify.log 2>&1 & SRV=$! ;;
  spring)
    PORT="${2:-8090}"
    JAR="$ROOT/benchmark/springboot/build/libs/swiftnet-bench-0.0.1.jar"
    [ -f "$JAR" ] || { echo "build first: (cd benchmark/springboot && ./gradlew bootJar)"; exit 1; }
    echo "starting Spring Boot (Loom vthreads) on 0.0.0.0:$PORT"
    java -jar "$JAR" --server.address=0.0.0.0 --server.port="$PORT" >/tmp/srv_spring.log 2>&1 & SRV=$! ;;
  *) echo "unknown server: $NAME"; exit 1 ;;
esac

# Wait until it answers locally.
for _ in $(seq 1 60); do curl -s -m2 -o /dev/null "http://127.0.0.1:${PORT}/" && break; sleep 1; done
echo "server pid=$SRV port=$PORT  (listening on all interfaces)"
echo "now run the load from the OTHER box:  SERVER=<this-ip> ./load_box.sh $NAME"
echo "-------------------------------------------------------------------"
trap 'echo; echo "stopping server $SRV"; kill -INT "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; exit 0' INT
bash "$HERE/cpu_watch.sh" "$SRV" 1   # blocks, prints saturation until Ctrl-C
