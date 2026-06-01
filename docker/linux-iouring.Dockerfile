# Build & test SwiftNet on Linux with the io_uring backend.
#
#   docker build -f docker/linux-iouring.Dockerfile -t swiftnet-linux .
#   docker run --rm --security-opt seccomp=unconfined swiftnet-linux
#
# --security-opt seccomp=unconfined is needed because Docker's default seccomp
# profile may block the io_uring_setup/enter/register syscalls.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates curl liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Configure + build (Linux => SWIFTNET_BACKEND_IOURING, links liburing).
RUN rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j

# Run the test suite, then a live io_uring smoke test.
CMD set -e; \
    echo "=== backend macro ==="; grep -h SWIFTNET_BACKEND build/CMakeCache.txt 2>/dev/null || true; \
    echo "=== ctest ==="; ctest --test-dir build --output-on-failure; \
    echo "=== io_uring live smoke ==="; \
    ./build/examples/basic_server & SRV=$!; sleep 2; \
    curl -s -m5 -o /dev/null -w "GET /        -> %{http_code}\n" http://127.0.0.1:8080/; \
    curl -s -m5 -w "\nGET /user/77 -> " http://127.0.0.1:8080/user/77; echo; \
    kill $SRV 2>/dev/null; \
    echo "=== io_uring backend OK ==="
