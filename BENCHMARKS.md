# SwiftNet Benchmarks

Throughput/latency comparison of **SwiftNet** (this project) against **Node.js/Express**
and **Spring Boot (Java virtual threads)** on identical HTTP workloads.

> These are real measurements from a single representative machine. Numbers vary with
> hardware, kernel/OS, build flags, and especially with running the load generator on the
> same host as the server (see *Threats to validity*). Reproduce with the commands below.

## Test environment

| | |
|---|---|
| **Machine** | Apple M1 Pro, 10 cores (8 performance + 2 efficiency), 16 GB |
| **OS** | macOS 26 (Darwin 25.5), kqueue I/O backend |
| **Build (SwiftNet)** | Apple clang, C++20, `-O3`, **native arm64** release |
| **Load generator** | [`wrk`](https://github.com/wg/wrk) 4.x, HTTP/1.1 keep-alive |
| **fd limit** | `ulimit -n` raised to ≥ 1,048,576 (for `-c1000`) |

### Servers under test

| Server | Stack | Version | Notes |
|---|---|---|---|
| **SwiftNet** | C++20 coroutine virtual threads + kqueue reactor | this repo | `examples/basic_server`, port 8080 |
| **Node.js** | Express | Node 22.11, Express 4 | `benchmark/nodejs-server.js`, port 3000, single event loop |
| **Spring Boot** | Spring MVC + embedded Tomcat, **virtual threads on** (`spring.threads.virtual.enabled=true`) | Spring Boot 3.4, JDK 23 | `benchmark/springboot/`, port 8090 |

All three are native arm64 (`java` and `node` are universal binaries; SwiftNet built `-DCMAKE_OSX_ARCHITECTURES=arm64`).

### Methodology

- **Isolation**: each server is started **alone**, measured, then stopped. Running all three at
  once oversubscribes the 10 cores and distorts results (SwiftNet's 10 workers + reactor, the
  JVM's carrier pool, and `wrk`'s threads all contend).
- **Warm-up**: a full `wrk` run is executed and discarded before each measured run (critical for
  the JVM's JIT; the warm-up also primes SwiftNet/Node).
- **Endpoints** (equivalent work on all three):
  - `GET /` — small static response body.
  - `GET /user/123` — path parameter + small JSON response.
- Driver: `benchmark/bench_all.sh` (one server at a time, warm-up + measure, percentiles via `wrk --latency`).

---

## Results

### High concurrency — `wrk -t12 -c1000 -d20s`

**`GET /`**

| Server | Requests/sec | Latency avg | p50 | p90 | p99 | Throughput vs Node |
|---|--:|--:|--:|--:|--:|--:|
| **SwiftNet** | **67,355** | 17.0 ms | 13.6 ms | 35.4 ms | 64.4 ms | **3.9×** |
| Node.js/Express | 17,086 | 45.9 ms | 34.2 ms | 46.0 ms | 643 ms¹ | 1.0× |
| Spring Boot (vthreads) | 122,831 | 8.1 ms | 7.6 ms | 10.1 ms | 21.6 ms | 7.2× |

**`GET /user/123`**

| Server | Requests/sec | Latency avg | p50 | p90 | p99 | Throughput vs Node |
|---|--:|--:|--:|--:|--:|--:|
| **SwiftNet** | **53,995** | 19.0 ms | 18.9 ms | 31.5 ms | 39.8 ms | **3.4×** |
| Node.js/Express | 16,034 | 48.7 ms | 37.1 ms | 50.5 ms | 618 ms¹ | 1.0× |
| Spring Boot (vthreads) | 101,475 | 9.5 ms | 9.5 ms | 11.6 ms | 16.9 ms | 6.3× |

¹ Node.js showed socket errors (read resets + timeouts) at `-c1000`, inflating its tail latency — its single event loop is saturated well below 1000 connections.

### Moderate concurrency — `wrk -t8 -c64 -d12s`

**`GET /`**

| Server | Requests/sec | Latency avg | p99 |
|---|--:|--:|--:|
| **SwiftNet** | 72,742 | 0.93 ms | 2.98 ms |
| Node.js/Express | 18,045 | 3.65 ms | 6.26 ms |
| Spring Boot (vthreads) | 113,729 | 0.55 ms | 2.14 ms |

**`GET /user/123`**

| Server | Requests/sec | Latency avg | p99 |
|---|--:|--:|--:|
| **SwiftNet** | 53,867 | 1.30 ms | 4.41 ms |
| Node.js/Express | 15,004 | 4.24 ms | 7.23 ms |
| Spring Boot (vthreads) | 89,961 | 0.95 ms | 8.62 ms |

---

## Analysis

**Ranking (this workload, this host): Spring Boot > SwiftNet > Node.js.**

- **SwiftNet vs Node.js — SwiftNet wins decisively (~3.4–3.9× throughput).** Node's single-threaded
  event loop cannot use the 10 cores; it saturates at ~16–18k rps and, at `-c1000`, sheds
  connections (hence the ~600 ms p99). SwiftNet spreads requests across all cores via its
  work-stealing virtual-thread scheduler and keeps p99 in the tens of ms.
- **SwiftNet vs Spring Boot — Spring Boot is ~1.8× ahead.** Spring Boot's MVC stack on the JVM is
  extremely mature: a JIT-optimized hot path, Tomcat's multi-threaded NIO acceptor/poller (several
  I/O threads), and Loom virtual threads. SwiftNet currently uses a **single reactor thread** for
  all readiness events and pays per-request costs that Spring Boot's stack has optimized away
  (`std::regex` route matching, several coroutine-frame heap allocations per request, an MPSC node
  allocation per cross-thread enqueue). These are the headroom items below.
- **Latency** scales gracefully for SwiftNet: p99 stays ~3–4 ms at `c64` and ~40–64 ms at `c1000`.
  (An earlier LIFO work-stealing scheduler produced multi-second p99 under load; switching the
  per-core deque to **FIFO** removed that — see *Optimizations applied*.)

## Threats to validity

- **Single-host load**: `wrk` runs on the same machine as the server, competing for the 10 cores
  (12 `wrk` threads + 11 SwiftNet threads, etc.). Absolute numbers would be higher with a dedicated
  load box; relative ordering is the reliable takeaway.
- **Workload shape**: tiny CPU-light responses stress framework/runtime overhead, not application
  logic. A workload with real async I/O (DB calls) would shift the picture toward whichever runtime
  unmounts most cheaply.
- **macOS/arm64 only**: the Linux `io_uring` and Windows `IOCP` backends are not exercised here.
- Node/Express is intentionally a baseline (single process; no `cluster`).

## Optimizations applied to SwiftNet for these runs

- Lock-free per-core **work-stealing** (Chase-Lev deque) + a lock-free **MPSC inbox** for
  cross-thread injection; **FIFO** dequeue for latency fairness.
- **Atomic** scheduler statistics (no mutex on the hot path).
- `TCP_NODELAY` on accepted sockets; **round-robin** work placement.
- Native **arm64** build. Verified ThreadSanitizer- and AddressSanitizer-clean under load.

## Headroom / future work (to close the Spring Boot gap)

- **Multiple reactor threads** (per-core kqueue/`io_uring` with `SO_REUSEPORT`) — the single reactor
  is the most likely throughput ceiling.
- Replace `std::regex` routing with a compiled trie / radix matcher.
- Pool coroutine frames and the MPSC inbox nodes to cut per-request allocations.
- Collapse the request coroutine chain (client_task → catch-all → handler) to fewer hops.

---

## Reproduce

```bash
# 1. Build SwiftNet (native arm64 release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build -j

# 2. Node reference deps
(cd benchmark && npm install)

# 3. Spring Boot reference (Java 21+; here JDK 23)
(cd benchmark/springboot && gradle bootJar)

# 4. Run the full comparison (one server at a time, with warm-up)
T=12 C=1000 D=20s WARM=8s ./benchmark/bench_all.sh
# raw wrk output is written to benchmark/results/
```
