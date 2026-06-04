# Benchmarks

This page reports only what is genuinely measured. Every number here comes from one machine — an Apple M1 Pro running the `kqueue` backend — and the authoritative source, with raw `wrk`/`sample` output and reproduction scripts, is [`BENCHMARKS.md`](https://github.com/kaushikchaturvedula/swiftnet/blob/main/BENCHMARKS.md) in the repository root. When in doubt, that file wins.

> The single most important finding is methodological: on a single host, HTTP throughput over loopback is bound by the co-located load generator and the TCP loopback path, not by SwiftNet. At the measured ceiling the server sits about 92% idle. So single-host throughput measures the *harness*, not the server. The host-independent server metrics on this box are **per-request latency** and **CPU-per-request** — those are what this page reports.

> ⚠️ This page contains **no cross-framework throughput comparison** (no SwiftNet-vs-Node, vs-Spring-Boot, etc.) and **no requests-per-second ranking**. Single-machine loopback benchmarking is harness-bound and cannot fairly compare servers. Those numbers await a proper two-machine setup (an off-host load generator); a setup-only kit lives under `benchmark/remote/`, but it produces no numbers until two boxes are provisioned.

## Test environment

All measured numbers below were taken on this configuration.

| | |
|---|---|
| Machine | Apple M1 Pro, 10 cores (8 performance + 2 efficiency), 16 GB |
| OS | macOS 26 (Darwin 25.5), `kqueue` I/O backend |
| Build | Apple clang 21, C++23, `-O3`, native arm64 (`-DCMAKE_OSX_ARCHITECTURES=arm64`) |
| Load generator | `wrk` 4.2.0, HTTP/1.1 keep-alive, **same host** as the server |
| Server under test | `benchmark/swiftnet_bench` — a minimal server (no logging/CORS/body middleware) so the tool measures framework cost, not app code |

Routes exercised: `GET /` (plaintext `Hello, World!`), `GET /json` (`{"message":"Hello, World!"}`), and `GET /user/:id` (router param + small JSON).

## Quick start

Build the minimal benchmark server and measure the two host-independent metrics yourself.

```bash
# Build the native arm64 release benchmark target
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"
cmake --build build --target swiftnet_bench -j

# Start the server
build/swiftnet_bench 8080 &

# Per-request latency: one connection, no queueing
wrk -t1 -c1 -d8s --latency http://127.0.0.1:8080/json

# CPU-per-request: drive the ceiling, then sample the server's on-CPU fraction
wrk -t2 -c200 -d12s http://127.0.0.1:8080/json &
sample $(pgrep swiftnet_bench) 6
```

The minimal benchmark server is roughly the following SwiftNet app:

```cpp
#include "swiftnet.hpp"
using namespace swiftnet;

int main(int argc, char** argv) {
    SwiftNet app(argc > 1 ? std::atoi(argv[1]) : 8080);

    app.get("/", [](Request& req, Response& res) {
        res.text("Hello, World!");
    });

    app.get("/json", [](Request& req, Response& res) {
        res.json(Json{{"message", "Hello, World!"}});
    });

    app.get("/user/:id", [](Request& req, Response& res) {
        res.json(Json{{"id", req.param("id")}});
    });

    app.listen([] { /* ready */ });
}
```

## The metrics we report

### Per-request latency (`wrk -t1 -c1`)

One connection, no queueing — this is the honest "how fast is one request" number, the full loopback round-trip of accept → parse → route → serialize → write → read on the client.

| Route | p50 | p75 | p90 | p99 | avg |
|---|--:|--:|--:|--:|--:|
| `GET /` (plaintext) | 25 µs | 28 µs | 33 µs | 64 µs | 27 µs |
| `GET /json` | 26 µs | 30 µs | 36 µs | 104 µs | — |

The ~25 µs median is where the per-core engine model pays off: a connection is pinned to one engine and never hops threads, so the per-request I/O path takes no locks.

### CPU-per-request (server sampled at the ceiling)

The server process was sampled for 6 s while serving ~116,647 req/s (`wrk -t2 -c200`). Leaf-sample breakdown:

```
total leaf samples : 55879
idle (kevent/cvwait): 51410  (92.0%)   <- engine threads parked waiting for I/O
ON-CPU             :  4469  ( 8.0%)
   read   (syscall): 1949  ┐ 82% of all on-CPU time is the unavoidable
   write  (syscall): 1728  ┘ one read + one write per request
   malloc family   :  ~230  (libmalloc tiny/free)
   nlohmann json   :   ~55  (dump/dump_escaped/destroy)
   std::string     :   ~45  (append/push_back, mostly inside json dump)
   hash tables     :   ~15  (io_ops slot + Request headers)
   router/route    :   ~14
```

Reading: at the ceiling the server burns about 8% of the 10 cores, roughly **~7 µs of CPU per request**, and **82% of that is the two socket syscalls** (one `read`, one `write`). Everything allocation- and serialization-related is the remaining ~1.4% of total CPU — which is why allocation-focused optimizations barely move the needle on this machine.

## How it works (why throughput here is the harness)

The sweep in `benchmark/ceiling_sweep.sh` varies server engines against client load on the 10-core box. Two signatures show a saturated *pipe*, not a saturated *server*:

- **RPS is flat (~100–118K) while latency rises linearly with connections.** That is Little's law for a saturated link: `RPS ≈ conns / latency`. Five times the connections gives roughly the same RPS at roughly six times the latency.
- **Engine count is irrelevant.** Under a fixed heavy client, 2 engines serve as much as 8. A CPU-bound server scales with cores; this one does not, because it isn't CPU-bound — the on-CPU profile shows it sitting ~92% idle at the ceiling.
- **The consequence:** server-side throughput optimizations cannot raise the measured RPS on a single-machine loopback setup. A real server-vs-server throughput comparison needs an off-host load generator, which this environment does not have.

> Single-host loopback throughput (~100–118K req/s on this box) is reported in `BENCHMARKS.md` only to *demonstrate* that it is harness-bound. It is **not** a SwiftNet web-throughput claim and should not be quoted as one.

## Optimization experiments

Each change was measured and kept only if it showed a real, reproducible gain on macOS/`kqueue`; negatives are reported honestly.

| Experiment | Result on macOS/kqueue | Status |
|---|---|---|
| Coroutine-frame pool (`include/detail/frame_pool.hpp`) | ~2% slower / neutral — macOS `libmalloc` already serves same-size, same-thread frames well | OFF by default on Apple; kept ON for non-Apple (UNVERIFIED there) |
| Lazy request headers (header vector scanned on demand) | Neutral with minimal headers; clean **+6%** with a realistic 10 headers/request, and strictly less work for handlers that don't read headers | Kept |
| Per-core engine scheduler (reactor + run-queue + `SO_REUSEPORT` per pinned thread) | Source of the 25 µs p50; throughput-neutral on this harness-bound box; TSan/ASan clean under load | Kept (latency/correctness, not throughput) |
| Native CPU tuning + flag-gating bug fix (`STREQUAL "Clang"` → `MATCHES "Clang"`) | Does not move the harness-bound number on this box | Kept |

## Work-stealing valve (scheduler experiment)

> ⚠️ This is a **compute-bound scheduler result**, not a web-throughput claim. The plain-HTTP workload above is loopback-bound and cannot exercise the scheduler, so the valve is tested with a workload where the *server* is the bottleneck.

The workload: a CPU-bound route `GET /compute` that does `co_await swiftnet::offload(...)` of ~10 ms of work as a **stealable compute task**, run alongside many light `GET /json` connections. The valve (off by default; `SWIFTNET_STEAL=1`, `SWIFTNET_STEAL_THRESHOLD=N`) lets an idle engine steal a compute task off the engine that owns a heavy connection, so that engine's light connections keep low tail latency. Only compute tasks are stealable; pinned I/O coroutines are never moved.

Run on M1 Pro (10 engines, kqueue): 6 heavy `/compute` connections + 100 light `/json` connections, 15 s, threshold = 1.

| Mode | light /json RPS | light p50 | light p99 | heavy RPS | steals | peak compute depth |
|---|--:|--:|--:|--:|--:|--:|
| A. valve OFF (offload, pure per-core) | 8,880 | 10.98 ms | 20.27 ms | 89 | 0 | 18 |
| B. valve ON (offload + steal) | 93,234 | 1.06 ms | 9.42 ms | 387 | 8,102 | 9 |
| C. valve ON, INLINE (no offload) | 1,573 | 62.35 ms | 84.21 ms | 95 | 0 | 0 |

- **B vs A:** light p50 drops 10.98 ms → 1.06 ms (~10×), light RPS rises 8.9K → 93K, heavy RPS rises 89 → 387 (~4.3×). With the valve off, a backlog piles onto one engine (depth → 18, 0 steals); with it on, depth stays near 0–1 while steals climb continuously. The valve drains the hotspot onto the idle engines.
- **C is the control:** running the work *inline* in the pinned connection coroutine makes it unstealable (0 steals), and light latency is the worst of the three. The win comes from the `offload` primitive making compute stealable — the valve cannot rescue inline/pinned work.
- **Honest bounds:** with 12 heavy connections the valve still helps, but valve-ON light throughput falls from 93K (6 heavy) to ~49.5K (12 heavy). The valve *redistributes* spare capacity, it does not create it, so the benefit shrinks as every engine becomes compute-busy. The magnitude also depends on connection placement.

Because `wrk` is co-located, the OFF/ON/INLINE *delta* on the identical workload is the signal here, not the absolute numbers. See [Work-stealing valve](architecture/work-stealing-valve.md) for the design.

## Backend status

Only the macOS/`kqueue` backend is performance-measured. The others are functionally implemented and tested but carry **no speed claim**.

| Backend | Platform | Status |
|---|---|---|
| `kqueue` | macOS/BSD | VERIFIED — measured (everything on this page); ctest; TSan-clean |
| `io_uring` | Linux | Implemented, functionally verified (compile + full `ctest` + live requests in a Linux container), throughput UNVERIFIED |
| `epoll` | Linux | Implemented, functionally verified (auto-selected when the io_uring probe fails), throughput UNVERIFIED |
| IOCP | Windows | Skeleton behind the backend interface, UNVERIFIED — no real `OVERLAPPED` + `WSARecv`/`WSASend` completion path yet |

See [Platform support](reference/platform-support.md) for the full backend and auto-detection details.

## Threats to validity

- **Single-host load (dominant).** `wrk` runs on the same 10 cores as the server over loopback, which sets the ~100–118K ceiling — not the server. Latency and CPU-per-request are the host-independent metrics.
- **Workload shape.** Tiny CPU-light responses stress framework overhead; a workload with real async I/O (e.g. DB calls) would shift the picture.
- **Apple Silicon / kqueue only.** io_uring, epoll, and IOCP are not performance-measured.
- **No cross-framework throughput claim is made here.** The correct way to compare servers is a dedicated off-host load box (kit under `benchmark/remote/`); until two boxes are provisioned, no comparison numbers exist.

## Common pitfalls

- **Quoting the loopback RPS as throughput.** The ~100–118K figure measures the harness. Do not report it as SwiftNet's web throughput or rank it against another framework.
- **Benchmarking with the full middleware stack on.** The measured numbers use a minimal server (no logging/CORS/body middleware) so the tool measures framework cost, not app code. Adding middleware measures your app, which is fine — just don't compare it to these figures.
- **Reading the valve result as web throughput.** It is a compute-bound scheduler experiment; the meaningful comparison is the OFF/ON/INLINE delta on the same imbalanced workload.
- **Expecting the valve to help inline work.** Only `co_await swiftnet::offload(...)` tasks are stealable. Heavy work run inline in a pinned connection coroutine cannot be moved.
- **Expecting more engines to raise loopback RPS.** On this harness-bound box, 2 engines serve as much as 8.

## See also

- [Work-stealing valve](architecture/work-stealing-valve.md)
- [Platform support](reference/platform-support.md)
